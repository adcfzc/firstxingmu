// Copyright (c) 2025 redis-lite authors. MIT License.
//
// server.cpp

#include "rl/server.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "rl/log.h"

namespace rl {

Server::Server(const Config& config) : config_(config) {}

Server::~Server() { Shutdown(); }

bool Server::Init(std::string* err) {
    stats_.reset(new Stats());
    stats_->SetUptimeStart();

    store_.reset(new Store());
    // 显式注入 Stats，而不是让 Store 去拿全局单例 —— 便于单元测试替换。
    if (!store_->Init(config_.shard_count, stats_.get())) {
        if (err != nullptr) *err = "store init failed";
        return false;
    }

    aof_.reset(new Aof());
    if (config_.aof_enabled) {
        if (!aof_->Open(config_.aof_path, config_.aof_policy, err)) {
            return false;
        }
    }

    // 线程池必须先起来，才能知道实际的 Sub Loop 数量（--threads 0 表示自动）。
    pool_.reset(new EventLoopPool());
    if (!pool_->InitAndStart(config_.loop_count, err)) {
        return false;
    }

    // Dispatcher 持有 Store / Aof / Stats 的引用，构造一次复用。
    ServerContext ctx;
    ctx.store = store_.get();
    ctx.aof = aof_.get();
    ctx.stats = stats_.get();
    ctx.version = "1.0.0";
    ctx.port = config_.port;
    ctx.shard_count = config_.shard_count;
    // ★ 必须用 pool_ 实际启动的数量，而不是 config_.loop_count ——
    // 后者在「0 = 自动」时是 0，会让 INFO 里的 reactor_loops 永远显示 0。
    ctx.loop_count = pool_->sub_loop_count();
    dispatcher_.reset(new Dispatcher(ctx));

    // ★ 先重放 AOF，再启动线程池。
    // 顺序很重要：重放期间没有并发连接，Store 处在「单线程独占」状态，
    // 不需要担心重放与在线请求交错导致的状态不一致。
    //
    // 注意：AOF 重放本身不依赖线程池，但 Store 的分片锁必须已经就绪 ——
    // 因此 Init 里先 Init store，再起线程池，最后重放。
    if (config_.aof_enabled) {
        int n = dispatcher_->LoadAof(config_.aof_path);
        if (n < 0) {
            if (err != nullptr) *err = "AOF replay failed";
            return false;
        }
        RL_INFO("AOF replay done: %d commands", n);
    }

    conn_manager_.reset(new ConnectionManager());
    conn_manager_->Init(pool_.get(), dispatcher_.get(), stats_.get());
    conn_manager_->StartPeriodicCheck(pool_->main_loop());

    store_->StartBackground();
    if (config_.aof_enabled) {
        FlushAofPeriodically();
    }

    listener_.reset(new Listener());
    return true;
}

void Server::FlushAofPeriodically() {
    if (aof_flusher_running_.exchange(true)) return;

    // everysec 模式：每 100ms 批量落盘一次，而不是每条命令一次 write+fsync。
    // 这把 fsync 的次数从「QPS 次/秒」降到「10 次/秒」，
    // 用最多丢 100ms 数据换取数量级的吞吐提升。
    aof_flusher_ = std::thread([this] {
#if RL_LINUX
        pthread_setname_np(pthread_self(), "rl-aof");
#endif
        RL_DEBUG("AOF flusher thread started");
        while (aof_flusher_running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (aof_ && aof_->is_open()) {
                aof_->FlushIfNeeded();
                stats_->SetAofSize(aof_->file_size());
            }
        }
        RL_DEBUG("AOF flusher thread exiting");
    });
}

bool Server::Start(std::string* err) {
    // Listener::Start 内部会注册 accept fd。它必须在主 loop 线程上执行 ——
    // 而本函数由控制线程（main）调用，所以通过 QueueWork 投递过去。
    //
    // 这是 one-loop-per-thread 模型的必然结果：任何碰 loop 内部状态的操作
    // 都要么在 loop 线程里、要么投递进去，没有第三条路。
    bool started = false;
    EventLoop* main_loop = pool_->main_loop();
    EventLoop::QueueToLoop(main_loop, [&] {
        started = listener_->Start(pool_.get(), conn_manager_.get(), config_.bind_addr,
                                   config_.port, err);
    });

    // 等待投递的任务在 main loop 线程上执行完，才能知道监听是否成功。
    for (int i = 0; i < 500 && !started; ++i) {
        if (listener_->fd() != kInvalidSocket) {
            started = true;
            break;
        }
        if (err != nullptr && !err->empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!started) {
        if (err != nullptr && err->empty()) *err = "listener start timed out";
        return false;
    }

    RL_INFO("redis-lite %s ready — port=%u loops=%s shards=%s aof=%s", "1.0.0",
            static_cast<unsigned>(config_.port), Num(pool_->sub_loop_count()).c_str(),
            Num(config_.shard_count).c_str(),
            config_.aof_enabled ? AofPolicyName(config_.aof_policy) : "off");
    return true;
}

void Server::Run(const std::function<bool()>& on_tick) {
    EventLoop* main_loop = pool_->main_loop();

    // 主 loop 跑在自己的线程上（见 EventLoopPool::InitAndStart），
    // 这里只是「控制线程」：等待退出条件成立，然后停掉主 loop。
    constexpr int kTickMs = 50;
    while (main_loop->IsRunning() && !shutdown_requested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
        if (on_tick && !on_tick()) {
            shutdown_requested_.store(true, std::memory_order_release);
            break;
        }
    }

    // Stop() 只置标志并唤醒，跨线程安全；join 交给 EventLoopPool::Stop()。
    main_loop->Stop();
    RL_INFO("shutdown requested, stopping...");
}

void Server::Shutdown() {
    if (listener_) listener_->Stop();

    if (conn_manager_) {
        conn_manager_->StopPeriodicCheck();
        conn_manager_->CloseAll();
    }

    // 顺序：先停生产者的后台线程，再停 loop 线程，最后落盘。
    // 反过来的话，后台线程可能在 Aof 已销毁后访问它。
    if (store_) store_->Shutdown();

    aof_flusher_running_.store(false, std::memory_order_release);
    if (aof_flusher_.joinable()) aof_flusher_.join();

    if (pool_) pool_->Stop();

    // 到这里所有写路径都已停止，做最后一次同步落盘。
    if (aof_ && aof_->is_open()) {
        aof_->SyncNow();
        stats_->SetAofSize(aof_->file_size());
        aof_->Close();
    }

    if (conn_manager_) conn_manager_.reset();
    if (dispatcher_) dispatcher_.reset();
    if (pool_) pool_.reset();
    if (aof_) aof_.reset();
    if (store_) store_.reset();

    RL_INFO("server stopped");
}

}  // namespace rl
