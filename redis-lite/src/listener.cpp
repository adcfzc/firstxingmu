// Copyright (c) 2025 redis-lite authors. MIT License.
//
// listener.cpp

#include "rl/listener.h"

#include <cerrno>
#include <cstring>

#include "rl/event_loop.h"
#include "rl/log.h"
#include "rl/stats.h"

namespace rl {

namespace {
constexpr int kListenBacklog = 1024;

// EMFILE / ENFILE 判断。Windows 上对应 WSAEMFILE。
bool IsFdExhausted() {
#if RL_WINDOWS
    return WSAGetLastError() == WSAEMFILE;
#else
    return errno == EMFILE || errno == ENFILE;
#endif
}
}  // namespace

Listener::Listener() = default;

Listener::~Listener() { Stop(); }

bool Listener::OpenReserveFd() {
    // 预留一个 fd 但从不使用。它的唯一用途是在 EMFILE 时被临时牺牲。
    reserve_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if RL_WINDOWS
    if (reserve_fd_ == INVALID_SOCKET) reserve_fd_ = kInvalidSocket;
#endif
    return reserve_fd_ != kInvalidSocket;
}

bool Listener::Start(EventLoopPool* pool, ConnectionManager* manager,
                     const std::string& bind_addr, uint16_t port, std::string* err) {
    if (pool == nullptr || manager == nullptr) {
        if (err != nullptr) *err = "listener: null pool or manager";
        return false;
    }
    pool_ = pool;
    manager_ = manager;
    bind_addr_ = bind_addr;

    listen_fd_ = CreateListener(bind_addr, port, kListenBacklog, err);
    if (listen_fd_ == kInvalidSocket) return false;
    port_ = port;

    OpenReserveFd();

    EventLoop* main_loop = pool_->main_loop();
    if (main_loop == nullptr) {
        if (err != nullptr) *err = "listener: main loop is null";
        return false;
    }

    // 回调捕获 this：Listener 的生命周期由 Server 持有，且长于所有连接。
    if (!main_loop->Register(
            listen_fd_, kEventReadable, [this](uint32_t events) {
                if (events & kEventReadable) OnReadable();
            },
            err)) {
        return false;
    }

    RL_INFO("listening on %s:%u (backlog=%d)", bind_addr.c_str(), static_cast<unsigned>(port),
            kListenBacklog);
    return true;
}

void Listener::Stop() {
    if (listen_fd_ != kInvalidSocket) {
        EventLoop* main_loop = (pool_ != nullptr) ? pool_->main_loop() : nullptr;
        const Socket fd = listen_fd_;
        listen_fd_ = kInvalidSocket;

        if (main_loop != nullptr) {
            // ★ 必须投递到主 loop 线程执行 Unregister，不能在这里直接调。
            //
            // Stop() 由控制线程（Server 的退出流程）调用，而 Unregister 内部有
            // AssertInLoopThread 断言 —— 跨线程直接调用会导致进程 abort。
            // 这个 bug 在压测中完全没暴露（正常流量下连接都是客户端先断开），
            // 直到在 AddressSanitizer 下测「优雅退出」才现形：
            //   [FATAL] EventLoop called from wrong thread
            //
            // 由于 Unregister 只做「从回调表移除 + epoll_ctl DEL」这类
            // 非阻塞操作，即使主 loop 线程此刻在 epoll_wait 中，
            // 投递任务也会立刻唤醒它并很快执行完。
            EventLoop::QueueToLoop(main_loop, [main_loop, fd] {
                main_loop->Unregister(fd);
                // 真正的 close 放在 unregister 之后执行，保证顺序：
                // 先从 epoll 摘掉，再关 fd。反过来的话，如果主 loop 正卡在
                // epoll_wait 上且事件已就绪，会对已关闭的 fd 调用回调。
                CloseSocket(fd);
            });
        } else {
            CloseSocket(fd);
        }
    }

    if (reserve_fd_ != kInvalidSocket) {
        CloseSocket(reserve_fd_);
        reserve_fd_ = kInvalidSocket;
    }
}

void Listener::OnReadable() {
    // ---- 循环 accept 直到 EAGAIN ----
    // 这是 ET 模式必须做的第一件事。漏掉一次 accept 就是一个永久积压的连接。
    for (;;) {
        sockaddr_in peer;
        SockLen len = sizeof(peer);

        Socket client = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);

        if (client == kInvalidSocket) {
            if (WouldBlock()) break;  // 队列已空 —— 正常出口
            if (Interrupted()) continue;
            if (IsFdExhausted()) {
                HandleAcceptError(0);
                break;
            }
            RL_WARN("accept failed: %s", LastSocketError().c_str());
            break;
        }

        // 必要的 socket 选项：不设 TCP_NODELAY 会让小请求被 Nagle 算法延迟 40ms。
        SetNoDelay(client);

        std::string err;
        if (!SetNonBlocking(client, &err)) {
            RL_ERROR("failed to set client non-blocking: %s", err.c_str());
            CloseSocket(client);
            continue;
        }

        accepted_.fetch_add(1, std::memory_order_relaxed);

        // 分发到某个 Sub Loop。round-robin 让负载在 loop 间均匀分布。
        EventLoop* target = pool_->NextLoop();
        if (target == nullptr) {
            CloseSocket(client);
            continue;
        }

        // 跨线程投递：Connection 的创建必须发生在目标 loop 线程内，
        // 否则 Register 里的 AssertInLoopThread 会失败，也避免数据竞争。
        ConnectionManager* mgr = manager_;
        EventLoop::QueueToLoop(target, [target, mgr, client] {
            mgr->CreateConnection(target, client);
        });
    }
}

void Listener::HandleAcceptError(int /*err_code*/) {
    // EMFILE 处理：先把预留 fd 关掉，腾出一个名额；
    // 再 accept 一次把内核队列头部的连接取出来并立刻关闭（这条连接必然失败，
    // 但能防止「队列非空 → epoll 持续可读 → 忙等」的死循环）；
    // 最后重新申请预留 fd。
    RL_WARN("accept: out of file descriptors, recycling reserve fd");

    if (reserve_fd_ != kInvalidSocket) {
        CloseSocket(reserve_fd_);
        reserve_fd_ = kInvalidSocket;

        sockaddr_in peer;
        SockLen len = sizeof(peer);
        Socket victim = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (victim != kInvalidSocket) {
            CloseSocket(victim);
        }
    }

    OpenReserveFd();
}

}  // namespace rl
