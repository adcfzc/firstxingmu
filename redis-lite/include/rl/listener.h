// Copyright (c) 2025 redis-lite authors. MIT License.
//
// listener.h — 监听 socket 的 accept 循环
//
// ET 模式下的 accept 有两个必须处理的细节（面试常考）：
//   1) 必须循环 accept 直到 EAGAIN。否则多个连接同时到达时，
//      只处理第一个，剩下的因为「状态没再变化」而永远得不到通知。
//   2) 必须处理 EMFILE（fd 用尽）。此时 accept 会失败，但连接仍在
//      内核的 accept 队列里，epoll 会持续报告可读 → 忙等 100% CPU。
//      标准解法：预留一个空闲 fd，遇到 EMFILE 时关掉它、accept 一个连接、
//      立刻再关掉，从而把这个「毒丸」连接摘掉，然后重新打开预留 fd。

#ifndef RL_LISTENER_H
#define RL_LISTENER_H

#include <atomic>
#include <cstdint>
#include <string>

#include "rl/connection_manager.h"
#include "rl/event_loop_pool.h"
#include "rl/platform.h"

namespace rl {

class Listener {
public:
    Listener();
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // 必须在主 loop 线程内调用（注册事件时需要 AssertInLoopThread 通过）。
    bool Start(EventLoopPool* pool, ConnectionManager* manager, const std::string& bind_addr,
               uint16_t port, std::string* err);
    void Stop();

    Socket fd() const { return listen_fd_; }
    uint16_t port() const { return port_; }
    uint64_t accepted() const { return accepted_.load(std::memory_order_relaxed); }

private:
    void OnReadable();
    // fd 耗尽时的应急处理：牺牲一条连接换取 accept 队列排空。
    void HandleAcceptError(int err_code);
    bool OpenReserveFd();

    Socket listen_fd_ = kInvalidSocket;
    Socket reserve_fd_ = kInvalidSocket;
    uint16_t port_ = 0;
    std::string bind_addr_;

    EventLoopPool* pool_ = nullptr;
    ConnectionManager* manager_ = nullptr;
    std::atomic<uint64_t> accepted_{0};
};

}  // namespace rl

#endif  // RL_LISTENER_H
