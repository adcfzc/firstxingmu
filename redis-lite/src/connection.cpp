// Copyright (c) 2025 redis-lite authors. MIT License.
//
// connection.cpp — 连接状态机实现。
//
// 性能相关的三个关键决策，都在这里：
//   1) ET 读循环：一次可读事件把内核缓冲读干净，把 epoll_wait 的唤醒次数
//      从「每个 segment 一次」降到「每批数据一次」。
//   2) 写路径延迟注册 EPOLLOUT：空闲连接不订阅可写事件。若一直订阅，
//      epoll 会持续报告可写，造成 100% CPU 空转（这是新手最常见的坑）。
//   3) 发送队列用 deque + offset：部分写时不搬移剩余数据。

#include "rl/connection.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include "rl/command.h"
#include "rl/event_loop.h"
#include "rl/log.h"
#include "rl/stats.h"

namespace rl {

namespace {
// 单次 recv 的缓冲大小。64KB 是一个折中：
// 太小则 syscall 次数多，太大则栈占用高且缓存不友好。
constexpr size_t kReadChunk = 64 * 1024;
}  // namespace

Connection::Connection(EventLoop* loop, Socket fd, Dispatcher* dispatcher)
    : loop_(loop), fd_(fd), dispatcher_(dispatcher) {
    connected_at_ms_ = NowMs();
}

Connection::~Connection() {
    if (!closed_ && fd_ != kInvalidSocket) {
        loop_->Unregister(fd_);
        CloseSocket(fd_);
        fd_ = kInvalidSocket;
    }
}

bool Connection::Start(std::string* err) {
    peer_ = GetPeerName(fd_);
    registered_events_ = kEventReadable;

    // ★ 必须捕获 weak_ptr 而不是 this。
    //
    // 原因：EventLoop 在执行一批就绪事件时会拷贝回调列表。若回调 A 关闭了连接 B，
    // B 的 shared_ptr 可能已被 ConnectionManager 释放，此时列表中 B 的回调就是
    // 悬垂的 —— 典型的迭代器/生命周期 bug。持 weak_ptr 并在调用前 lock，
    // 对象已销毁时直接跳过。
    std::weak_ptr<Connection> weak = weak_from_this();
    return loop_->Register(
        fd_, registered_events_,
        [weak](uint32_t events) {
            if (auto self = weak.lock()) {
                self->HandleEvent(events);
            }
        },
        err);
}

void Connection::HandleEvent(uint32_t events) {
    if (closed_) return;

    // 错误与挂断优先处理，但不直接返回：
    // 可能还有已到达的数据需要读完再回复（半关闭场景）。
    if (events & (kEventError | kEventHup)) {
        // 先尝试把残留数据读出来并处理掉。
        if (events & kEventReadable) {
            if (!HandleRead()) return;
        }
        // 对端已关闭且没有残留数据：若还有回复没发完，标记优雅关闭。
        if (!output_.empty()) {
            close_after_write_ = true;
            HandleWrite();
            return;
        }
        MarkClosed("peer hung up");
        return;
    }

    if (events & kEventReadable) {
        if (!HandleRead()) return;
    }
    if (events & kEventWritable) {
        HandleWrite();
    }
}

bool Connection::HandleRead() {
    char buf[kReadChunk];

    // ---- ET 读循环 ----
    // 必须一直读到 EAGAIN 或读到 0。少读一次就会永久丢事件。
    for (;;) {
        int n = RecvSome(fd_, buf, sizeof(buf));

        if (n > 0) {
            if (dispatcher_ != nullptr && dispatcher_->context()->stats != nullptr) {
                dispatcher_->context()->stats->AddBytesIn(static_cast<uint64_t>(n));
            }
            parser_.Append(buf, static_cast<size_t>(n));
            continue;
        }

        if (n == 0) {
            // 对端写了 FIN。把已到达的数据处理完，再关闭。
            ProcessInput();
            if (!output_.empty()) {
                close_after_write_ = true;
                HandleWrite();
                return true;
            }
            MarkClosed("peer closed");
            return false;
        }

        // n < 0
        if (WouldBlock()) break;  // 内核缓冲已排空 —— ET 模式下正常出口
        if (Interrupted()) continue;
        MarkClosed("recv error");
        return false;
    }

    // 防御慢速攻击 / 超大命令：单连接待解析数据有上限。
    if (parser_.buffered() > kMaxQueryBuffer) {
        std::string err;
        EncodeError("ERR Protocol error: query buffer limit exceeded", &err);
        output_bytes_ += err.size();
        output_.push_back(std::move(err));
        close_after_write_ = true;
        HandleWrite();
        return true;
    }

    ProcessInput();

    // 低延迟优化：读完之后立刻尝试发送，而不是等 epoll 通知可写。
    // 省掉一次「注册 EPOLLOUT → epoll_wait → 回写」的往返。
    if (!output_.empty()) {
        HandleWrite();
    }
    return !closed_;
}

void Connection::ProcessInput() {
    std::vector<std::string> args;
    std::string reply;

    for (;;) {
        bool complete = false;
        try {
            complete = parser_.Next(&args);
        } catch (const ProtocolException& e) {
            if (dispatcher_ != nullptr && dispatcher_->context()->stats != nullptr) {
                dispatcher_->context()->stats->IncProtocolErrors();
            }
            RL_WARN("protocol error from %s: %s", peer_.c_str(), e.what());
            EncodeError(std::string("ERR Protocol error: ") + e.what(), &reply);
            output_.push_back(reply);
            output_bytes_ += reply.size();
            // 协议一旦错位，后续字节无法可靠解释，只能断开。
            close_after_write_ = true;
            return;
        }

        if (!complete) break;   // 半包：等下次可读
        if (args.empty()) continue;  // 空数组请求，忽略

        reply.clear();
        dispatcher_->Execute(&args, &reply);
        ++commands_handled_;

        if (dispatcher_->context()->stats != nullptr) {
            dispatcher_->context()->stats->AddBytesOut(
                static_cast<uint64_t>(reply.size()));
        }

        output_bytes_ += reply.size();
        output_.push_back(std::move(reply));

        // 输出缓冲超限：停止解析，让写路径先把数据送出去。
        if (output_bytes_ > kMaxOutputBuffer) {
            RL_WARN("output buffer limit exceeded for %s, closing", peer_.c_str());
            close_after_write_ = true;
            return;
        }
    }
}

void Connection::HandleWrite() {
    if (closed_) return;

    while (!output_.empty()) {
        std::string& front = output_.front();
        const char* data = front.data() + output_front_offset_;
        const size_t remaining = front.size() - output_front_offset_;

        int n = SendSome(fd_, data, remaining);

        if (n > 0) {
            output_front_offset_ += static_cast<size_t>(n);
            output_bytes_ -= static_cast<size_t>(n);
            if (output_front_offset_ >= front.size()) {
                output_.pop_front();
                output_front_offset_ = 0;
            }
            // 继续循环：可能还有后续 segment 要发。
            continue;
        }

        if (n < 0 && WouldBlock()) {
            // 内核发送缓冲满了：注册 EPOLLOUT，等下次可写再继续。
            EnableWriteIfNeeded();
            return;
        }

        if (n < 0 && Interrupted()) continue;

        // 真实的发送错误（EPIPE / ECONNRESET）。对端已经走了，
        // 剩下的数据没有必要再发。
        MarkClosed("send error");
        return;
    }

    // 全部发完：摘掉 EPOLLOUT 订阅，避免空转唤醒。
    DisableWriteIfIdle();

    if (close_after_write_) {
        MarkClosed("graceful close");
    }
}

bool Connection::FlushOutput() { return output_.empty(); }

void Connection::EnableWriteIfNeeded() {
    if (registered_events_ & kEventWritable) return;
    // 延迟注册：只有在真的写不下时才订阅可写事件。
    registered_events_ |= kEventWritable;
    UpdateEvents();
}

void Connection::DisableWriteIfIdle() {
    if (output_.empty() && (registered_events_ & kEventWritable)) {
        registered_events_ &= ~static_cast<uint32_t>(kEventWritable);
        UpdateEvents();
    }
}

void Connection::UpdateEvents() { loop_->Modify(fd_, registered_events_); }

void Connection::Close() {
    if (closed_) return;

    // ★ 线程亲和检查：连接的所有事件循环操作（Register/Modify/Unregister）
    //   必须发生在「拥有它的那个 loop 线程」上。
    //
    //   这个判断是必须的，因为 Close() 有两个来源：
    //     (a) 本连接自己的 loop 线程（读事件里发现对端关闭）—— 直接执行
    //     (b) 其他线程（Server 优雅退出时 ConnectionManager::CloseAll、
    //         或后台超时检查）—— 必须投递到目标 loop
    //
    //   漏掉这个检查的后果在测试中已实际发生：
    //   `redis-lite` 压测跑完全部请求都正常，但**收到退出信号时崩溃**，
    //   报 "EventLoop called from wrong thread"。因为平时连接都是
    //   客户端先断开的（走路径 a），优雅退出才第一次触发路径 b。
    //
    //   后来在 AddressSanitizer 下跑端到端测试才暴露 —— 这也说明
    //   「关闭路径」必须有独立测试，不能指望正常流量覆盖到。
    EventLoop* loop = loop_;
    if (loop != nullptr && !loop->IsInLoopThread()) {
        ConnectionPtr keep = shared_from_this();
        EventLoop::QueueToLoop(loop, [keep] {
            if (!keep->closed()) keep->Close();
        });
        return;
    }

    // 还有数据没发完时先优雅关闭，让对端能收到完整的错误回复。
    if (!output_.empty()) {
        close_after_write_ = true;
        HandleWrite();
        return;
    }
    MarkClosed("explicit close");
}

void Connection::MarkClosed(const char* reason) {
    if (closed_) return;
    closed_ = true;

    RL_DEBUG("connection closed: %s (%s, %s cmds)", peer_.c_str(), reason,
             Num(static_cast<unsigned long long>(commands_handled_)).c_str());

    const Socket dead_fd = fd_;
    if (fd_ != kInvalidSocket) {
        loop_->Unregister(fd_);
        // 先半关闭写方向，让对端立刻感知，再关 fd（避免 TIME_WAIT 堆积在服务端）。
        ShutdownWrite(fd_);
        CloseSocket(fd_);
        fd_ = kInvalidSocket;
    }

    if (on_close_) {
        // 回调会从 ConnectionManager 中摘除 shared_ptr，可能导致 this 析构。
        // 因此这是本函数里最后一个动作，之后不得再访问任何成员。
        // fd 已经被置为无效，所以把原值单独传出去作为身份标识。
        auto cb = std::move(on_close_);
        on_close_ = nullptr;
        cb(dead_fd);
    }
}

}  // namespace rl
