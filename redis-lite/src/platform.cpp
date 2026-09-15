// Copyright (c) 2025 redis-lite authors. MIT License.
//
// platform.cpp — 平台抽象的 Windows / Linux 双实现。

#include "rl/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#include "rl/log.h"

namespace rl {

// ============================================================ 初始化 / 清理
#if RL_WINDOWS
namespace {
std::once_flag g_wsa_once;
}  // namespace

bool InitSockets(std::string* err) {
    // WSAStartup 必须早于任何 socket 调用，且全进程只需一次。
    static bool ok = false;
    std::call_once(g_wsa_once, [] {
        WSADATA data;
        ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    });
    if (!ok && err != nullptr) {
        *err = "WSAStartup failed: " + LastSocketError();
    }
    return ok;
}
#else
bool InitSockets(std::string* /*err*/) { return true; }
#endif

void CleanupSockets() {
#if RL_WINDOWS
    WSACleanup();
#endif
}

// ================================================================= 错误处理
std::string LastSocketError() {
#if RL_WINDOWS
    int code = WSAGetLastError();
    char buf[256];
    buf[0] = '\0';
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf,
                   sizeof(buf), nullptr);
    // 去掉 FormatMessage 附带的换行。
    size_t n = std::strlen(buf);
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
        buf[--n] = '\0';
    }
    char out[320];
    std::snprintf(out, sizeof(out), "wsa=%d %s", code, buf);
    return std::string(out);
#else
    int code = errno;
    char out[256];
    std::snprintf(out, sizeof(out), "errno=%d %s", code, std::strerror(code));
    return std::string(out);
#endif
}

bool WouldBlock() {
#if RL_WINDOWS
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

bool Interrupted() {
#if RL_WINDOWS
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

// ============================================================== socket 工具
bool SetNonBlocking(Socket fd, std::string* err) {
#if RL_WINDOWS
    u_long mode = 1;
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) {
        if (err != nullptr) *err = "ioctlsocket(FIONBIO) failed: " + LastSocketError();
        return false;
    }
    return true;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        if (err != nullptr) *err = "fcntl(F_GETFL) failed: " + LastSocketError();
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        if (err != nullptr) *err = "fcntl(F_SETFL, O_NONBLOCK) failed: " + LastSocketError();
        return false;
    }
    return true;
#endif
}

bool SetNoDelay(Socket fd) {
    int on = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on),
                      sizeof(on)) == 0;
}

bool SetReuseAddr(Socket fd) {
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on),
                      sizeof(on)) == 0;
}

void CloseSocket(Socket fd) {
    if (fd == kInvalidSocket) return;
#if RL_WINDOWS
    closesocket(fd);
#else
    ::close(fd);
#endif
}

void ShutdownWrite(Socket fd) {
    if (fd == kInvalidSocket) return;
    shutdown(fd, kShutdownWrite);
}

Socket CreateListener(const std::string& bind_addr, uint16_t port, int backlog,
                      std::string* err) {
    Socket fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidSocket) {
        if (err != nullptr) *err = "socket() failed: " + LastSocketError();
        return kInvalidSocket;
    }

    SetReuseAddr(fd);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (bind_addr.empty() || bind_addr == "0.0.0.0" || bind_addr == "*") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (bind_addr == "127.0.0.1" || bind_addr == "localhost") {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        if (err != nullptr) *err = "invalid bind address: " + bind_addr;
        CloseSocket(fd);
        return kInvalidSocket;
    }

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (err != nullptr) {
            *err = "bind(" + bind_addr + ":" + std::to_string(port) + ") failed: " +
                   LastSocketError();
        }
        CloseSocket(fd);
        return kInvalidSocket;
    }

    if (listen(fd, backlog) != 0) {
        if (err != nullptr) *err = "listen() failed: " + LastSocketError();
        CloseSocket(fd);
        return kInvalidSocket;
    }

    if (!SetNonBlocking(fd, err)) {
        CloseSocket(fd);
        return kInvalidSocket;
    }

    return fd;
}

int RecvSome(Socket fd, void* buf, size_t len) {
    return static_cast<int>(recv(fd, static_cast<char*>(buf), static_cast<int>(len), 0));
}

int SendSome(Socket fd, const void* buf, size_t len) {
    return static_cast<int>(send(fd, static_cast<const char*>(buf), static_cast<int>(len), 0));
}

// ============================================================ 地址与连接
bool ResolveHost(const std::string& host, std::string* ip, std::string* err) {
    // 先试字面量，省掉一次 DNS 往返。
    in_addr probe;
    if (inet_pton(AF_INET, host.c_str(), &probe) == 1) {
        *ip = host;
        return true;
    }

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;  // 本项目只做 IPv4
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        if (err != nullptr) {
            *err = "getaddrinfo(" + host + ") failed: " +
#if RL_WINDOWS
                   std::to_string(rc);
#else
                   gai_strerror(rc);
#endif
        }
        if (res != nullptr) freeaddrinfo(res);
        return false;
    }

    char buf[INET_ADDRSTRLEN];
    buf[0] = '\0';
    auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    const char* ok = inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
    freeaddrinfo(res);

    if (ok == nullptr) {
        if (err != nullptr) *err = "inet_ntop failed: " + LastSocketError();
        return false;
    }
    *ip = buf;
    return true;
}

Socket ConnectTo(const std::string& host, uint16_t port, std::string* err) {
    std::string ip;
    if (!ResolveHost(host, &ip, err)) return kInvalidSocket;

    Socket fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidSocket) {
        if (err != nullptr) *err = "socket() failed: " + LastSocketError();
        return kInvalidSocket;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (err != nullptr) {
            *err = "connect(" + ip + ":" + std::to_string(port) + ") failed: " +
                   LastSocketError();
        }
        CloseSocket(fd);
        return kInvalidSocket;
    }

    // 压测工具自己也要走非阻塞路径，保证 RecvSome/SendSome 的 EAGAIN 语义一致。
    if (!SetNonBlocking(fd, err)) {
        CloseSocket(fd);
        return kInvalidSocket;
    }
    SetNoDelay(fd);
    return fd;
}

namespace {
// 注意参数不能是 const：MinGW 把 inet_ntop 的源地址声明为 PVOID（非 const），
// 传 const 指针在 Windows 上会编译失败。用非 const 引用最省事，也无需强转。
std::string FormatAddr(sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN];
    buf[0] = '\0';
    if (inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) == nullptr) {
        return "?";
    }
    return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}
}  // namespace

std::string GetPeerName(Socket fd) {
    sockaddr_in addr;
    SockLen len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "?";
    return FormatAddr(addr);
}

std::string GetSockName(Socket fd) {
    sockaddr_in addr;
    SockLen len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "?";
    return FormatAddr(addr);
}

// ============================================================== Notifier
#if RL_LINUX
Notifier::Notifier() {
    // 优先 eventfd：一次 8 字节累加，天然线程安全且可合并唤醒。
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd >= 0) {
        read_fd_ = fd;
        write_fd_ = fd;
        is_eventfd_ = true;
        return;
    }
    // 退化到 pipe + 非阻塞。
    int fds[2];
    if (pipe(fds) != 0) {
        RL_ERROR("pipe() failed: %s", LastSocketError().c_str());
        return;
    }
    read_fd_ = fds[0];
    write_fd_ = fds[1];
    std::string err;
    SetNonBlocking(read_fd_, &err);
    SetNonBlocking(write_fd_, &err);
    is_eventfd_ = false;
}

Notifier::~Notifier() {
    if (is_eventfd_) {
        if (read_fd_ != kInvalidSocket) ::close(read_fd_);
        return;
    }
    if (read_fd_ != kInvalidSocket) ::close(read_fd_);
    if (write_fd_ != kInvalidSocket) ::close(write_fd_);
}

void Notifier::Notify() {
    if (write_fd_ == kInvalidSocket) return;
    if (is_eventfd_) {
        uint64_t one = 1;
        ssize_t n = ::write(write_fd_, &one, sizeof(one));
        (void)n;
        return;
    }
    char c = 'x';
    ssize_t n = ::write(write_fd_, &c, 1);
    (void)n;
}

void Notifier::Drain() {
    if (read_fd_ == kInvalidSocket) return;
    if (is_eventfd_) {
        uint64_t v = 0;
        while (::read(read_fd_, &v, sizeof(v)) > 0) {
        }
        return;
    }
    char buf[64];
    while (::read(read_fd_, buf, sizeof(buf)) > 0) {
    }
}
#else  // ------------------------------------------------------------- Windows
Notifier::Notifier() {
    // Windows 下用 connected UDP socketpair 代替 pipe。
    Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        RL_ERROR("notifier: socket() failed: %s", LastSocketError().c_str());
        return;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // 让内核选端口

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, 1) != 0) {
        RL_ERROR("notifier: bind/listen failed: %s", LastSocketError().c_str());
        CloseSocket(listener);
        return;
    }

    SockLen alen = sizeof(addr);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
        RL_ERROR("notifier: getsockname failed: %s", LastSocketError().c_str());
        CloseSocket(listener);
        return;
    }

    write_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (write_fd_ == kInvalidSocket ||
        connect(write_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        RL_ERROR("notifier: connect failed: %s", LastSocketError().c_str());
        CloseSocket(listener);
        return;
    }

    read_fd_ = accept(listener, nullptr, nullptr);
    CloseSocket(listener);

    if (read_fd_ == kInvalidSocket) {
        RL_ERROR("notifier: accept failed: %s", LastSocketError().c_str());
        return;
    }

    std::string err;
    SetNonBlocking(read_fd_, &err);
    SetNonBlocking(write_fd_, &err);
}

Notifier::~Notifier() {
    if (read_fd_ != kInvalidSocket) CloseSocket(read_fd_);
    if (write_fd_ != kInvalidSocket) CloseSocket(write_fd_);
}

void Notifier::Notify() {
    if (write_fd_ == kInvalidSocket) return;
    char c = 'x';
    int n = SendSome(write_fd_, &c, 1);
    (void)n;
}

void Notifier::Drain() {
    if (read_fd_ == kInvalidSocket) return;
    char buf[128];
    while (RecvSome(read_fd_, buf, sizeof(buf)) > 0) {
    }
}
#endif

// ================================================================= 时间
int64_t NowMs() {
#if RL_WINDOWS
    // QueryPerformanceCounter 是 Windows 上的单调时钟。
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<int64_t>(now.QuadPart * 1000 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
#endif
}

std::string NowString() {
    std::time_t t = std::time(nullptr);
    std::tm tmv;
#if RL_WINDOWS
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

}  // namespace rl
