// Copyright (c) 2025 redis-lite authors. MIT License.
//
// platform.h — 跨平台 socket / IO 抽象层
//
// 设计要点（面试可讲）：
//   本项目核心是 Linux epoll ET + 多 Reactor。但为了让代码能在 Windows 上
//   编译、调试、跑功能测试，这里做了一层薄抽象：
//     - Linux  : epoll，支持 EPOLLET（边缘触发）
//     - Windows: select()，只有电平触发语义
//   抽象的最小单位是 "Poller"，业务层（EventLoop / Connection）不出现任何
//   平台宏，新增平台只需实现一个 Poller 后端。
//
//   为什么 Windows 侧选 select() 而不是 WSAPoll：WSAPoll 需要 _WIN32_WINNT>=0x0600，
//   而 MinGW 的 <_mingw.h> 默认锁在 0x502（XP），依赖调用方记得加编译选项 ——
//   这是「构建脆弱性」。select() 零版本要求，代价是 FD_SETSIZE 上限（见 poller.cpp）。
//
//   注意：Windows 后端存在的意义是开发期验证正确性，不是生产形态。
//   压测数据一律以 Linux/epoll 为准（见 docs/PERF.md）。

#ifndef RL_PLATFORM_H
#define RL_PLATFORM_H

#include <cstdint>
#include <string>

// ★ Windows 开发构建必须带 -D_WIN32_WINNT=0x0600（Vista）。
//
// 原因：MinGW 的 <_mingw.h> 会在被包含时写入一个默认的 _WIN32_WINNT=0x502(XP)，
// 而它往往早于本头文件被拉入（经由 <cstdio> 等），此时再写 #ifndef 已经无效。
// 该默认值会让 inet_pton / inet_ntop 等 Vista 才引入的 API 变成"未声明"。
// 用命令行宏可以从一开始就压住默认值。
//
// 注意：事件多路复用后端**不再**依赖这个宏 —— Windows 侧改用 select()，
// 它对 Windows 版本没有任何要求。这个宏只为 inet_pton/inet_ntop 而留。
#if defined(_WIN32) && defined(_WIN32_WINNT) && (_WIN32_WINNT < 0x0600)
#  error "Windows 构建请加 -D_WIN32_WINNT=0x0600（inet_pton/inet_ntop 需要 Vista+）"
#endif

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <mstcpip.h>
#  include <windows.h>
#  define RL_WINDOWS 1
#  define RL_LINUX 0
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>          // addrinfo / getaddrinfo / freeaddrinfo / gai_strerror
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/epoll.h>
#  include <sys/eventfd.h>     // eventfd / EFD_NONBLOCK / EFD_CLOEXEC（唤醒管道）
#  include <sys/socket.h>
#  include <unistd.h>
#  define RL_WINDOWS 0
#  define RL_LINUX 1
#endif

namespace rl {

// ---------------------------------------------------------------- 类型别名
#if RL_WINDOWS
using Socket = SOCKET;
using SockLen = int;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
constexpr int kShutdownWrite = SD_SEND;
#else
using Socket = int;
using SockLen = socklen_t;
constexpr Socket kInvalidSocket = -1;
constexpr int kShutdownWrite = SHUT_WR;
#endif

// ------------------------------------------------------------------ 事件位
// 与 epoll 的 EPOLLIN/EPOLLOUT 数值保持一致，Linux 下可直接透传。
enum EventFlags : uint32_t {
    kEventNone = 0u,
    kEventReadable = 0x001u,
    kEventWritable = 0x004u,
    kEventError = 0x008u,
    kEventHup = 0x010u,
};

// 控制操作，语义对齐 epoll_ctl 的 op。
enum PollOp { kPollAdd = 1, kPollMod = 2, kPollDel = 3 };

// ------------------------------------------------------------- 初始化/清理
// Windows 下必须 WSAStartup，Linux 下为空操作。
// 进程启动即调用一次，见 src/main.cpp。
bool InitSockets(std::string* err);
void CleanupSockets();

// --------------------------------------------------------------- socket 工具
// 创建监听 socket：SO_REUSEADDR + 非阻塞 + 监听 backlog。
// 失败返回 kInvalidSocket，错误写入 err。
Socket CreateListener(const std::string& bind_addr, uint16_t port, int backlog,
                      std::string* err);

bool SetNonBlocking(Socket fd, std::string* err);
bool SetNoDelay(Socket fd);            // TCP_NODELAY，关掉 Nagle
bool SetReuseAddr(Socket fd);
void CloseSocket(Socket fd);
void ShutdownWrite(Socket fd);

// 返回 0 表示对端正常关闭；返回 -1 时用 WouldBlock/Interrupted 区分。
int RecvSome(Socket fd, void* buf, size_t len);
int SendSome(Socket fd, const void* buf, size_t len);
bool WouldBlock();
bool Interrupted();

// 客户端主动 connect，用于压测工具与测试代码。
// 失败返回 kInvalidSocket。成功返回的 fd 已设为非阻塞。
Socket ConnectTo(const std::string& host, uint16_t port, std::string* err);

// 域名 → IPv4 字面量。用 getaddrinfo，避免只支持纯 IP。
bool ResolveHost(const std::string& host, std::string* ip, std::string* err);

// 返回 "ip:port" 形式的对端/本端地址，失败返回 "?"。
std::string GetPeerName(Socket fd);
std::string GetSockName(Socket fd);

std::string LastSocketError();

// ----------------------------------------------------------- 单例唤醒管道
// eventfd / pipe / socketpair，用于跨线程唤醒事件循环。
class Notifier {
public:
    Notifier();
    ~Notifier();

    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    bool valid() const { return read_fd_ != kInvalidSocket; }
    Socket read_fd() const { return read_fd_; }

    // 线程安全：任何线程都可调用，向事件循环投递一次唤醒。
    void Notify();
    // 事件循环线程调用，排空计数。
    void Drain();

private:
    Socket read_fd_ = kInvalidSocket;
    Socket write_fd_ = kInvalidSocket;
#if RL_LINUX
    bool is_eventfd_ = false;
#endif
};

// 单调递增毫秒时间戳，用于 TTL。基于 CLOCK_MONOTONIC / QueryPerformanceCounter，
// 不受系统时间被修改影响（这是 TTL 正确性的关键）。
int64_t NowMs();
// 墙上时钟，仅用于日志。
std::string NowString();

}  // namespace rl

#endif  // RL_PLATFORM_H
