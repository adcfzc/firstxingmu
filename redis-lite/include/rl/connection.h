// Copyright (c) 2025 redis-lite authors. MIT License.
//
// connection.h — 连接状态机
//
// 每个 Connection 的生命周期完全由「拥有它的那个 EventLoop 线程」串行驱动，
// 因此对象本身不需要任何锁。跨线程只能通过 EventLoop::QueueWork 投递操作。
//
// ET 模式下的读循环是本文件最关键的部分：
//   epoll 边缘触发只在「状态变化」时通知一次。若只 recv 一次就返回，
//   剩余字节会永远留在内核缓冲里，而 epoll 不会再通知 —— 连接就此静默挂死。
//   所以必须循环 recv 直到 EAGAIN。

#ifndef RL_CONNECTION_H
#define RL_CONNECTION_H

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rl/platform.h"
#include "rl/resp.h"

namespace rl {

class EventLoop;
class Dispatcher;

// 单条命令的输出上限（协议回复），防止写缓冲把内存撑爆。
constexpr size_t kMaxOutputBuffer = 64 * 1024 * 1024;
// 单连接待解析输入上限。
constexpr size_t kMaxQueryBuffer = 64 * 1024 * 1024;

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(EventLoop* loop, Socket fd, Dispatcher* dispatcher);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 注册到事件循环并开始工作。必须在 loop 线程调用。
    bool Start(std::string* err);

    // 立即关闭。若还有未发送完的数据，会先尝试优雅关闭（FIN）。
    void Close();

    bool closed() const { return closed_; }
    Socket fd() const { return fd_; }
    EventLoop* loop() const { return loop_; }
    const std::string& peer() const { return peer_; }
    uint64_t commands_handled() const { return commands_handled_; }
    size_t pending_output_bytes() const { return output_bytes_; }
    int64_t connected_at_ms() const { return connected_at_ms_; }

    // 由 ConnectionManager 在关闭后回调，用于从连接表摘除。
    // 参数是被关闭的 fd（此时 socket 已关闭，仅作为身份标识）。
    void set_on_close(std::function<void(Socket)> cb) { on_close_ = std::move(cb); }

private:
    void HandleEvent(uint32_t events);
    // ET 语义：循环读到 EAGAIN。返回 false 表示连接已断开。
    bool HandleRead();
    void HandleWrite();
    // 解析并执行 in_buf_ 中所有完整命令，把回复排进输出队列。
    void ProcessInput();

    bool FlushOutput();         // 尽力发送，返回是否全部发完
    void EnableWriteIfNeeded();
    void DisableWriteIfIdle();
    void HandleClose();
    void MarkClosed(const char* reason);
    void UpdateEvents();

    EventLoop* loop_;
    Socket fd_;
    Dispatcher* dispatcher_;
    std::string peer_;

    RespParser parser_;

    // 输出队列：多段 buffer + 段内偏移。
    // 用 deque<string> 而不是单个大 string，避免每次发送都要 memmove 剩余数据。
    std::deque<std::string> output_;
    size_t output_front_offset_ = 0;
    size_t output_bytes_ = 0;

    uint32_t registered_events_ = 0;
    bool closed_ = false;
    bool close_after_write_ = false;  // 优雅关闭：发完再关
    uint64_t commands_handled_ = 0;
    int64_t connected_at_ms_ = 0;

    std::function<void(Socket)> on_close_;
};

using ConnectionPtr = std::shared_ptr<Connection>;

}  // namespace rl

#endif  // RL_CONNECTION_H
