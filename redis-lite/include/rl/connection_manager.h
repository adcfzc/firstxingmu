// Copyright (c) 2025 redis-lite authors. MIT License.
//
// connection_manager.h — 连接表
//
// shared_ptr<Connection> 的唯一持有者就是这里。
// 连接一旦关闭，由 Connection 通过 on_close 回调通知本类摘除，交由
// shared_ptr 自动析构 —— 不手工 delete，避免「回调里销毁自己」的经典问题。

#ifndef RL_CONNECTION_MANAGER_H
#define RL_CONNECTION_MANAGER_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "rl/connection.h"
#include "rl/event_loop.h"
#include "rl/event_loop_pool.h"
#include "rl/stats.h"

namespace rl {

class Dispatcher;

class ConnectionManager {
public:
    ConnectionManager();
    ~ConnectionManager();

    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    void Init(EventLoopPool* pool, Dispatcher* dispatcher, Stats* stats);

    // 在指定 loop 上创建连接。必须在该 loop 线程内调用（由 Listener 保证）。
    void CreateConnection(EventLoop* loop, Socket fd);

    // 启动/停止周期检查器（每 1 秒）。
    void StartPeriodicCheck(EventLoop* ticker_loop);
    void StopPeriodicCheck();

    size_t count() const;
    int64_t total_created() const { return total_created_.load(std::memory_order_relaxed); }

    // 关闭全部连接。优雅退出时调用，确保缓冲数据尽量发完。
    void CloseAll();

private:
    void Remove(Socket fd);
    void CheckTimeouts();

    EventLoopPool* pool_ = nullptr;
    Dispatcher* dispatcher_ = nullptr;
    Stats* stats_ = nullptr;

    mutable std::mutex mutex_;
    // key = fd。每连接一个 shared_ptr。
    std::unordered_map<Socket, ConnectionPtr> connections_;
    std::atomic<int64_t> total_created_{0};

    EventLoop* ticker_loop_ = nullptr;
    bool checking_ = false;
};

}  // namespace rl

#endif  // RL_CONNECTION_MANAGER_H
