// Copyright (c) 2025 redis-lite authors. MIT License.
//
// event_loop.h — Reactor 核心
//
// 一个 EventLoop 绑定一个线程，遵循 "one loop per thread" 模型：
//   - 保证同一连接的所有读写都在同一线程内串行发生 → 连接对象无需加锁
//   - 跨线程只能通过 QueueWork() 投递任务，配合 Notifier 唤醒
//
// 线程安全矩阵：
//   Start/Stop/QueueWork/RunAfter   : 任意线程
//   其余所有方法                     : 仅 loop 线程（Debug 下由 AssertInLoopThread 校验）

#ifndef RL_EVENT_LOOP_H
#define RL_EVENT_LOOP_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rl/platform.h"
#include "rl/poller.h"

#if RL_LINUX
#include <pthread.h>
#endif

namespace rl {

// fd 就绪回调。参数为统一位掩码，见 EventFlags。
using EventCallback = std::function<void(uint32_t events)>;

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 在调用线程上跑事件循环，直到 Stop() 被调用。
    // 阻塞语义：Start() 会一直阻塞，因此通常在新线程中调用。
    void Start();

    // 线程安全。可从任意线程调用，唤醒并终止循环。
    void Stop();

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

    // ------------------------------------------------- fd 事件注册（loop 线程）
    bool Register(Socket fd, uint32_t events, EventCallback cb, std::string* err = nullptr);
    bool Modify(Socket fd, uint32_t events);
    void Unregister(Socket fd);

    // ------------------------------------------------- 跨线程任务投递
    // 若已在 loop 线程内，直接同步执行（避免无谓的唤醒开销）。
    void QueueWork(std::function<void()> task);

    // ------------------------------------------------- 定时任务（loop 线程）
    // 延迟 delay_ms 执行一次。
    void RunAfter(int64_t delay_ms, std::function<void()> task);
    // 每 interval_ms 执行一次，直到 Stop()。回调内不要做长时间阻塞操作。
    void RunEvery(int64_t interval_ms, std::function<void()> task);

    // 把任务投递到指定 loop；target 为空则同步执行。
    static void QueueToLoop(EventLoop* target, std::function<void()> task);

    void AssertInLoopThread() const;
    bool IsInLoopThread() const;

    int64_t pending_task_count() const {
        std::lock_guard<std::mutex> lk(task_mutex_);
        return static_cast<int64_t>(pending_tasks_.size());
    }

private:
    struct Timer {
        int64_t deadline_ms;             // 下次触发的绝对时间
        int64_t interval_ms;             // 0 表示一次性
        uint64_t seq;                    // 相同 deadline 时保证 FIFO
        std::function<void()> task;
    };

    struct TimerLater {
        // std::priority_queue 是最大堆，这里反向比较得到最小堆。
        bool operator()(const Timer& a, const Timer& b) const {
            if (a.deadline_ms != b.deadline_ms) return a.deadline_ms > b.deadline_ms;
            return a.seq > b.seq;
        }
    };

    void RunPendingTasks();
    void RunExpiredTimers();
    void Wakeup();
    void HandleWakeupRead();

    Poller poller_;
    Notifier notifier_;

    std::atomic<bool> running_{false};
    std::thread::id loop_thread_id_;

    // 本线程内的 fd → 回调。只在 loop 线程访问。
    std::unordered_map<Socket, EventCallback> callbacks_;

    // 跨线程任务队列，由 task_mutex_ 保护。
    mutable std::mutex task_mutex_;
    std::vector<std::function<void()>> pending_tasks_;

    // 定时器最小堆，只在 loop 线程访问。
    std::priority_queue<Timer, std::vector<Timer>, TimerLater> timers_;
    uint64_t timer_seq_ = 0;
};

}  // namespace rl

#endif  // RL_EVENT_LOOP_H
