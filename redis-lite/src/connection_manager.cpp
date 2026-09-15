// Copyright (c) 2025 redis-lite authors. MIT License.
//
// connection_manager.cpp

#include "rl/connection_manager.h"

#include <chrono>
#include <thread>
#include <vector>

#include "rl/command.h"
#include "rl/log.h"

namespace rl {

namespace {

// 超时策略。取 5 分钟足够宽容（不会被正常的慢客户端触发），
// 又能清掉真正的死连接。生产环境应做成配置项。
constexpr int64_t kIdleTimeoutMs = 5 * 60 * 1000;

}  // namespace

ConnectionManager::ConnectionManager() = default;

ConnectionManager::~ConnectionManager() {
    StopPeriodicCheck();
    CloseAll();
}

void ConnectionManager::Init(EventLoopPool* pool, Dispatcher* dispatcher, Stats* stats) {
    pool_ = pool;
    dispatcher_ = dispatcher;
    stats_ = stats;
}

void ConnectionManager::CreateConnection(EventLoop* loop, Socket fd) {
    if (loop == nullptr || fd == kInvalidSocket) return;

    auto conn = std::make_shared<Connection>(loop, fd, dispatcher_);

    // 关闭回调只捕获裸指针，绝不捕获 shared_ptr：
    // 否则 manager → conn → 回调 → conn 形成引用环，连接永远不会被释放。
    ConnectionManager* self = this;
    conn->set_on_close([self](Socket dead_fd) { self->Remove(dead_fd); });

    {
        std::lock_guard<std::mutex> lk(mutex_);
        connections_[fd] = conn;
    }
    total_created_.fetch_add(1, std::memory_order_relaxed);
    if (stats_ != nullptr) {
        stats_->IncConnections();
        stats_->SetCurrentConnections(static_cast<int64_t>(count()));
        stats_->SetTotalConnections(total_created_.load(std::memory_order_relaxed));
    }

    std::string err;
    if (!conn->Start(&err)) {
        RL_ERROR("Connection::Start failed: %s", err.c_str());
        if (stats_ != nullptr) stats_->IncRejected();
        Remove(fd);
        conn->Close();
    }
}

void ConnectionManager::Remove(Socket fd) {
    ConnectionPtr victim;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        // 先把 shared_ptr 移出 map，再在锁外析构。
        // 若在持锁状态下析构，Connection 的析构函数可能再次调用本类 → 死锁。
        victim = std::move(it->second);
        connections_.erase(it);
    }
    victim.reset();  // 锁外析构

    if (stats_ != nullptr) {
        stats_->SetCurrentConnections(static_cast<int64_t>(count()));
    }
}

size_t ConnectionManager::count() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return connections_.size();
}

void ConnectionManager::CloseAll() {
    std::vector<ConnectionPtr> all;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        all.reserve(connections_.size());
        for (auto& kv : connections_) {
            all.push_back(std::move(kv.second));
        }
        connections_.clear();
    }

    // 锁外逐个关闭。注意 CloseAll 通常由 Server 的退出流程在主线程调用，
    // 而连接的 fd 注册在各自的 Sub Loop 上 —— Connection::Close() 内部
    // 会做线程亲和检查并投递到正确的 loop，这里不需要额外处理。
    for (auto& c : all) {
        if (c && !c->closed()) c->Close();
    }
    all.clear();

    // 优雅退出时不能立刻返回：Close() 是把关闭操作**投递**到各 Sub Loop 的，
    // 需要给它们一点时间真正执行完，否则 Server 紧接着 pool_->Stop() 会
    // 在连接还没清理完时停掉事件循环。
    //
    // 这里用一个短等待而不是精确同步：退出路径不在性能关键路径上，
    // 真正的一致性由后续 pool_->Stop() 的 join 保证。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (stats_ != nullptr) stats_->SetCurrentConnections(0);
}

void ConnectionManager::StartPeriodicCheck(EventLoop* ticker_loop) {
    if (ticker_loop == nullptr || checking_) return;
    ticker_loop_ = ticker_loop;
    checking_ = true;

    // 注册到主 loop 的定时器：每秒检查一次空闲连接。
    ticker_loop_->RunEvery(1000, [this] { CheckTimeouts(); });
}

void ConnectionManager::StopPeriodicCheck() { checking_ = false; }

void ConnectionManager::CheckTimeouts() {
    const int64_t now = NowMs();
    std::vector<ConnectionPtr> expired;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            const ConnectionPtr& c = it->second;
            if (!c || c->closed()) {
                it = connections_.erase(it);
                continue;
            }
            // 空闲判定：既没有待发送数据，也没有近期活动。
            // 注意这里只用「连接建立时刻」做粗略判断，精确实现应记录
            // last_active_ms 并在每次读事件时刷新。
            if (c->pending_output_bytes() == 0 &&
                now - c->connected_at_ms() > kIdleTimeoutMs && c->commands_handled() == 0) {
                expired.push_back(c);
                it = connections_.erase(it);
                continue;
            }
            ++it;
        }
    }

    for (auto& c : expired) {
        RL_INFO("closing idle connection: %s", c->peer().c_str());
        // 回到该连接自己的 loop 线程去关闭，避免跨线程操作 loop。
        EventLoop* loop = c->loop();
        ConnectionPtr keep = c;  // 让 lambda 持有，保证关闭期间对象存活
        EventLoop::QueueToLoop(loop, [keep] {
            if (!keep->closed()) keep->Close();
        });
    }

    if (!expired.empty() && stats_ != nullptr) {
        stats_->SetCurrentConnections(static_cast<int64_t>(count()));
    }
}

}  // namespace rl
