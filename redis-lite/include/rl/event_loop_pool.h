// Copyright (c) 2025 redis-lite authors. MIT License.
//
// event_loop_pool.h — 主从 Reactor 线程池
//
//   Main Loop : 只 accept，不处理业务
//   Sub Loops : 每个线程一个 EventLoop，承担读写与命令执行
//
// 新连接按 round-robin 分给 Sub Loop。这一层是 W4 性能对比的主角：
// 把 sub_loop_count 设为 1 即退化为单 Reactor，压测数据可直接对比。

#ifndef RL_EVENT_LOOP_POOL_H
#define RL_EVENT_LOOP_POOL_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "rl/event_loop.h"

namespace rl {

class EventLoopPool {
public:
    EventLoopPool() = default;
    ~EventLoopPool();

    EventLoopPool(const EventLoopPool&) = delete;
    EventLoopPool& operator=(const EventLoopPool&) = delete;

    // loop_count 为 Sub Loop 数量。0 表示按硬件线程数自动选择。
    // 每个 Sub Loop 会启动一个独立线程。
    bool InitAndStart(size_t loop_count, std::string* err);

    void Stop();

    // round-robin 选一个 Sub Loop。loop_count == 0 时返回 main_loop()，
    // 这样单线程模式下所有逻辑仍走同一条路径，便于对比测试。
    EventLoop* NextLoop();

    EventLoop* main_loop() { return main_loop_.get(); }
    size_t sub_loop_count() const { return sub_loops_.size(); }
    size_t next_index() const { return next_.load(std::memory_order_relaxed); }

    // 在任意 Sub Loop 上执行任务，主要用于后台线程通知全部 loop。
    void Broadcast(std::function<void()> task);

private:
    std::unique_ptr<EventLoop> main_loop_;
    std::vector<std::unique_ptr<EventLoop>> sub_loops_;
    std::vector<std::thread> threads_;
    std::atomic<size_t> next_{0};
    std::atomic<bool> started_{false};
};

}  // namespace rl

#endif  // RL_EVENT_LOOP_POOL_H
