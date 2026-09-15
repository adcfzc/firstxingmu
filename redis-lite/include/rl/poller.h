// Copyright (c) 2025 redis-lite authors. MIT License.
//
// poller.h — 事件多路复用封装
//
// 统一 epoll(Linux) 与 WSAPoll(Windows) 的差异：
//   - 事件注册/修改/删除
//   - 就绪事件等待与遍历
//   - 边缘触发（ET）语义标记
//
// 业务层只使用本头文件暴露的 API，见 src/event_loop.cpp。

#ifndef RL_POLLER_H
#define RL_POLLER_H

#include <cstdint>
#include <string>
#include <vector>

#include "rl/platform.h"

namespace rl {

// 就绪事件，屏蔽底层结构体差异。
struct ReadyEvent {
    Socket fd = kInvalidSocket;
    uint32_t events = kEventNone;  // 位掩码，见 EventFlags
};

// ------------------------------------------------------------------ Poller
// 一个 Poller 绑定一个线程（事件循环线程），非线程安全。
class Poller {
public:
    Poller();
    ~Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    // 创建底层多路复用实例。失败返回 false 并写入 err。
    bool Init(std::string* err);

    // 注册 / 修改 / 删除 fd 的关注事件。
    // 注意：Linux 下 ET 模式在 Register 时通过 epoll_ctl 的 EPOLLET 生效。
    bool Register(Socket fd, uint32_t events, std::string* err);
    bool Modify(Socket fd, uint32_t events, std::string* err);
    bool Unregister(Socket fd, std::string* err);

    // 等待事件。timeout_ms < 0 表示永久阻塞。
    // 返回就绪事件数量，-1 表示出错（EINTR 已内部重试）。
    int Wait(std::vector<ReadyEvent>* out, int timeout_ms, std::string* err);

    // 本后端是否原生支持边缘触发。
    // true  -> 业务层读事件必须循环读到 EAGAIN（本项目的默认形态）
    // false -> 电平触发，读一次即可，循环读也不会错
    static bool SupportsEdgeTrigger();

private:
#if RL_LINUX
    int epfd_ = -1;
#else
    // select() 每次等待需要完整重建 fd_set，这里维护注册表。
    struct Entry {
        Socket fd;
        uint32_t events;
    };
    std::vector<Entry> entries_;
    // 可读/可写集合的容量上限。select 的 FD_SETSIZE 是编译期常量，
    // 这里仅用于提前拒绝超限注册，给出明确错误而不是静默失败。
    static constexpr size_t kMaxSelectFds = FD_SETSIZE;
#endif
};

}  // namespace rl

#endif  // RL_POLLER_H
