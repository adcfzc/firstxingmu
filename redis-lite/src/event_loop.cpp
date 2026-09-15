// Copyright (c) 2025 redis-lite authors. MIT License.
//
// event_loop.cpp — Reactor 实现。
//
// 核心循环：
//   1) 跑到期的定时器
//   2) 执行跨线程投递的任务
//   3) epoll_wait，把就绪 fd 的回调「拷贝后」在锁外执行
//
// 为什么回调必须在锁外执行：回调里很可能再次调用 QueueWork()，
// 若持锁调用就会自死锁。这是 Reactor 实现里最常见的一个坑。

#include "rl/event_loop.h"
#include "rl/event_loop_pool.h"

#include <algorithm>
#include <cstring>
#include <exception>

#include "rl/log.h"

namespace rl {

EventLoop::EventLoop() = default;

EventLoop::~EventLoop() {
    // 未显式 Stop 就析构时兜底：否则 loop 线程会访问已销毁的成员。
    if (running_.load(std::memory_order_acquire)) {
        Stop();
    }
}

bool EventLoop::IsInLoopThread() const {
    return std::this_thread::get_id() == loop_thread_id_;
}

void EventLoop::AssertInLoopThread() const {
    if (!IsInLoopThread()) {
        RL_FATAL("EventLoop called from wrong thread (expected loop thread)");
    }
}

void EventLoop::Start() {
    loop_thread_id_ = std::this_thread::get_id();

    // 先置位再初始化：否则 Stop() 在初始化窗口内调用会看到 false 而提前返回，
    // 导致 loop 线程永远停不下来。Stop() 里的 Wakeup() 对未初始化的 notifier
    // 是安全的（Notifier::Notify 内部判空）。
    running_.store(true, std::memory_order_release);

    std::string err;
    if (!poller_.Init(&err)) {
        RL_ERROR("EventLoop: poller init failed: %s", err.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }
    if (!notifier_.valid()) {
        RL_ERROR("EventLoop: notifier unavailable, cross-thread wakeup disabled");
        running_.store(false, std::memory_order_release);
        return;
    }

    // 唤醒管道注册到 poller：跨线程投递任务时靠它打断 epoll_wait 的阻塞。
    if (!poller_.Register(notifier_.read_fd(), kEventReadable, &err)) {
        RL_ERROR("EventLoop: register notifier failed: %s", err.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }

    const bool et = Poller::SupportsEdgeTrigger();
    RL_DEBUG("EventLoop started (fd=%d, edge_trigger=%d)", static_cast<int>(notifier_.read_fd()),
             et ? 1 : 0);

    std::vector<ReadyEvent> ready;
    while (running_.load(std::memory_order_acquire)) {
        // 有定时器时不要永久阻塞，否则定时器无法按时触发。
        int timeout_ms = 50;
        if (!timers_.empty()) {
            int64_t delta = timers_.top().deadline_ms - NowMs();
            if (delta < 0) delta = 0;
            if (delta < timeout_ms) timeout_ms = static_cast<int>(delta);
        }

        ready.clear();
        std::string perr;
        int n = poller_.Wait(&ready, timeout_ms, &perr);
        if (n < 0) {
            RL_ERROR("EventLoop: poll failed: %s", perr.c_str());
            break;
        }

        // 先处理到期定时器与投递任务，让它们在本次迭代内尽快生效。
        RunExpiredTimers();
        RunPendingTasks();

        for (const ReadyEvent& re : ready) {
            if (re.fd == notifier_.read_fd()) {
                // 唤醒管道只是"有活干了"的信号，统一在下一轮处理。
                HandleWakeupRead();
                continue;
            }

            auto it = callbacks_.find(re.fd);
            if (it == callbacks_.end()) continue;

            // 拷贝回调后解锁执行：回调内可能 Unregister 掉自己或别的 fd。
            EventCallback cb = it->second;
            cb(re.events);
        }

        RunPendingTasks();
    }

    running_.store(false, std::memory_order_release);
    RL_DEBUG("EventLoop stopped");
}

void EventLoop::Stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    // 标记停止 + 唤醒。先置标志再 Notify，保证 loop 线程一定能看到 false。
    running_.store(false, std::memory_order_release);
    Wakeup();
}

void EventLoop::Wakeup() {
    if (notifier_.valid()) notifier_.Notify();
}

void EventLoop::HandleWakeupRead() { notifier_.Drain(); }

bool EventLoop::Register(Socket fd, uint32_t events, EventCallback cb, std::string* err) {
    AssertInLoopThread();

    std::string local_err;
    if (!poller_.Register(fd, events, &local_err)) {
        if (err != nullptr) *err = local_err;
        return false;
    }
    callbacks_[fd] = std::move(cb);
    return true;
}

bool EventLoop::Modify(Socket fd, uint32_t events) {
    AssertInLoopThread();
    if (callbacks_.find(fd) == callbacks_.end()) return false;
    std::string err;
    return poller_.Modify(fd, events, &err);
}

void EventLoop::Unregister(Socket fd) {
    AssertInLoopThread();
    callbacks_.erase(fd);
    std::string err;
    poller_.Unregister(fd, &err);
}

void EventLoop::QueueWork(std::function<void()> task) {
    if (task == nullptr) return;

    // 已在 loop 线程：直接执行。省掉一次自唤醒，也避免任务顺序被推迟。
    if (IsRunning() && IsInLoopThread()) {
        task();
        return;
    }

    {
        std::lock_guard<std::mutex> lk(task_mutex_);
        pending_tasks_.push_back(std::move(task));
    }
    // 注意：即使在 loop 线程内也必须唤醒，因为调用点可能在 poll 之前。
    Wakeup();
}

void EventLoop::QueueToLoop(EventLoop* target, std::function<void()> task) {
    if (target == nullptr) {
        if (task != nullptr) task();
        return;
    }
    target->QueueWork(std::move(task));
}

void EventLoop::RunPendingTasks() {
    if (pending_tasks_.empty()) return;

    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lk(task_mutex_);
        if (pending_tasks_.empty()) return;
        batch.swap(pending_tasks_);
    }

    for (auto& t : batch) {
        if (t == nullptr) continue;
        // 单个任务抛异常不应拖垮整个 loop。
        try {
            t();
        } catch (const std::exception& e) {
            RL_ERROR("EventLoop: task threw: %s", e.what());
        } catch (...) {
            RL_ERROR("EventLoop: task threw unknown exception");
        }
    }
}

void EventLoop::RunAfter(int64_t delay_ms, std::function<void()> task) {
    if (delay_ms < 0) delay_ms = 0;
    timers_.push(Timer{NowMs() + delay_ms, 0, timer_seq_++, std::move(task)});
}

void EventLoop::RunEvery(int64_t interval_ms, std::function<void()> task) {
    if (interval_ms <= 0) interval_ms = 1;
    timers_.push(Timer{NowMs() + interval_ms, interval_ms, timer_seq_++, std::move(task)});
}

void EventLoop::RunExpiredTimers() {
    if (timers_.empty()) return;

    const int64_t now = NowMs();
    // 上限保护：防止回调疯狂 RunAfter(0) 导致本轮饿死 IO。
    constexpr int kMaxTimersPerRound = 256;

    int fired = 0;
    while (!timers_.empty() && timers_.top().deadline_ms <= now && fired < kMaxTimersPerRound) {
        Timer t = timers_.top();
        timers_.pop();
        ++fired;

        if (t.task != nullptr) {
            try {
                t.task();
            } catch (const std::exception& e) {
                RL_ERROR("EventLoop: timer threw: %s", e.what());
            } catch (...) {
                RL_ERROR("EventLoop: timer threw unknown exception");
            }
        }

        // 周期任务重新排队。用 deadline 累加而非 now 累加，
        // 避免回调耗时被不断累积成漂移。
        if (t.interval_ms > 0 && running_.load(std::memory_order_acquire)) {
            t.deadline_ms += t.interval_ms;
            if (t.deadline_ms <= now) {
                // 落后太多（例如进程被挂起），直接跳到下一个未来时刻。
                t.deadline_ms = now + t.interval_ms;
            }
            timers_.push(std::move(t));
        }
    }
}

// ======================================================= EventLoopPool

EventLoopPool::~EventLoopPool() { Stop(); }

bool EventLoopPool::InitAndStart(size_t loop_count, std::string* err) {
    if (started_.load(std::memory_order_acquire)) return true;

    main_loop_.reset(new EventLoop());

    // ★ 主 loop 也必须有自己专属的线程。
    //
    // 曾经的 bug：只给 Sub Loop 起了线程，主 loop 的 Start() 从未被调用，
    // 于是 loop_thread_id_ 一直是「默认构造的 std::thread::id」。之后主线程
    // 调用 main_loop()->Register()（注册 accept fd、注册周期定时器）时，
    // AssertInLoopThread 立刻失败 —— 服务在启动阶段就 FATAL 退出。
    //
    // 修正后主循环线程只做两件事：accept，以及跑主 loop 上的定时器。
    {
        EventLoop* raw = main_loop_.get();
        threads_.emplace_back([raw] {
#if RL_LINUX
            pthread_setname_np(pthread_self(), "rl-main");
#endif
            raw->Start();
        });
    }

    if (loop_count == 0) {
        unsigned hw = std::thread::hardware_concurrency();
        loop_count = (hw == 0) ? 4 : hw;
        if (loop_count > 16) loop_count = 16;  // 再多的 loop 收益递减
    }
    (void)err;

    for (size_t i = 0; i < loop_count; ++i) {
        std::unique_ptr<EventLoop> loop(new EventLoop());
        EventLoop* raw = loop.get();
        sub_loops_.push_back(std::move(loop));

        threads_.emplace_back([raw] {
            // 线程内命名，方便 perf top / gdb 时辨认。
#if RL_LINUX
            pthread_setname_np(pthread_self(), "rl-worker");
#endif
            raw->Start();
        });
    }

    started_.store(true, std::memory_order_release);
    RL_INFO("EventLoopPool started: %s sub loop(s) + 1 main loop",
            Num(sub_loops_.size()).c_str());
    return true;
}

void EventLoopPool::Stop() {
    if (!started_.exchange(false, std::memory_order_acq_rel)) return;

    // Stop() 内部只置标志 + 唤醒，可从任意线程调用。
    for (auto& loop : sub_loops_) {
        if (loop) loop->Stop();
    }
    if (main_loop_) main_loop_->Stop();

    // 注意：threads_ 里第 0 个是主 loop 线程，其余才是 Sub Loop 线程，
    // 所以统一 join 即可，不要按索引去对应 sub_loops_。
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    sub_loops_.clear();
    RL_INFO("EventLoopPool stopped");
}

EventLoop* EventLoopPool::NextLoop() {
    if (sub_loops_.empty()) return main_loop_.get();
    // round-robin，relaxed 足够：只是一个分发计数器。
    size_t idx = next_.fetch_add(1, std::memory_order_relaxed) % sub_loops_.size();
    return sub_loops_[idx].get();
}

void EventLoopPool::Broadcast(std::function<void()> task) {
    for (auto& loop : sub_loops_) {
        if (loop) loop->QueueWork(task);
    }
}

}  // namespace rl
