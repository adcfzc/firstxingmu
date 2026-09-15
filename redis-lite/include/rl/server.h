// Copyright (c) 2025 redis-lite authors. MIT License.
//
// server.h — 组装所有子系统的顶层对象
//
// 依赖方向（严格单向，禁止反向依赖）：
//   Server → Listener → ConnectionManager → Connection → Dispatcher
//                                                   ↓
//                                          Store / Aof / Stats
//   网络层不知道存储层的存在，存储层也不知道自己被网络层调用。

#ifndef RL_SERVER_H
#define RL_SERVER_H

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "rl/aof.h"
#include "rl/command.h"
#include "rl/config.h"
#include "rl/connection_manager.h"
#include "rl/event_loop_pool.h"
#include "rl/listener.h"
#include "rl/stats.h"
#include "rl/store.h"

namespace rl {

class Server {
public:
    explicit Server(const Config& config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // 初始化各子系统。失败返回 false 并写入 err。
    bool Init(std::string* err);

    // 启动监听，并在主事件循环上注册一个 100ms 的 tick。
    bool Start(std::string* err);

    // 阻塞运行，直到 RequestShutdown() 被调用、或 on_tick 返回 false。
    // on_tick 可传 nullptr。把「何时退出」交给调用方判断，
    // 是为了让信号处理（main.cc）与事件循环实现完全解耦。
    void Run(const std::function<bool()>& on_tick = nullptr);

    // 线程安全：从信号处理线程请求退出。
    void RequestShutdown() { shutdown_requested_.store(true); }

    void Shutdown();

    const Config& config() const { return config_; }
    Store* store() { return store_.get(); }
    Stats* stats() { return stats_.get(); }
    // 启动后可用于确认监听是否就绪（例如测试时取实际端口）。
    Socket listen_fd() const { return listener_ ? listener_->fd() : kInvalidSocket; }

private:
    void FlushAofPeriodically();

    Config config_;
    std::unique_ptr<Stats> stats_;
    std::unique_ptr<Store> store_;
    std::unique_ptr<Aof> aof_;
    std::unique_ptr<EventLoopPool> pool_;
    std::unique_ptr<Dispatcher> dispatcher_;
    std::unique_ptr<ConnectionManager> conn_manager_;
    std::unique_ptr<Listener> listener_;

    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> aof_flusher_running_{false};
    std::thread aof_flusher_;
};

}  // namespace rl

#endif  // RL_SERVER_H
