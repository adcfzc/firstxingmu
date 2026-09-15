// Copyright (c) 2025 redis-lite authors. MIT License.
//
// aof.h — Append Only File 持久化
//
// 核心设计（面试重点）：
//   1) AOF 内容就是 RESP 命令流，与客户端请求格式完全一致
//      → 恢复时可以复用同一个 RespParser，不需要第二套解析逻辑
//      → 「线上路径」与「恢复路径」共享代码，是消灭一类 bug 的有效手段
//   2) 追加缓冲 + 后台批量落盘，而不是每条命令一次 write
//      → 把 write 系统调用的次数从「每命令一次」降到「每 100ms 一次」
//   3) appendfsync 三档策略：always / everysec / no，对应不同的持久性-性能取舍
//   4) 有界缓冲：缓冲超过阈值时同步落盘，防止内存无上限增长
//
// 【W4 扩展点】AOF 重写（compaction）：
//   当前实现只追加不整理，文件会随写入次数单调增长。
//   实现方式：fork 子进程遍历 Store::ForEach 生成最小命令集，主进程期间
//   的双写进 AOF 重写缓冲，完成后合并。这是最容易讲出深度的一个扩展。

#ifndef RL_AOF_H
#define RL_AOF_H

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace rl {

enum class AofSyncPolicy : uint8_t {
    kAlways = 0,    // 每条命令都落盘并 fsync：最安全，最慢
    kEverySec = 1,  // 后台每 100ms 批量 fsync：最多丢 1 秒数据（默认）
    kNo = 2,        // 只交给 OS，进程崩溃不丢，断电可能丢
};

class Aof {
public:
    Aof();
    ~Aof();

    Aof(const Aof&) = delete;
    Aof& operator=(const Aof&) = delete;

    // 以追加模式打开 aof 文件。不存在则创建。
    bool Open(const std::string& path, AofSyncPolicy policy, std::string* err);
    // 落盘剩余缓冲并关闭。
    void Close();

    // 追加一条写命令。线程安全，由多个 loop 线程并发调用。
    // 用原始指针而不是 vector<string>，是为了让调用方免于构造临时 vector。
    void Append(const std::vector<std::string>& args);
    void AppendArgs(std::initializer_list<std::string> args);

    // 从文件读取全部命令。启动时由 Server 调用，然后逐条重放。
    // 允许尾部存在「写了一半」的命令：截断到最后一个完整命令，不算错误。
    static bool LoadAll(const std::string& path, std::vector<std::vector<std::string>>* out,
                        std::string* err, size_t* truncated_bytes);

    // 把缓冲刷到用户态，必要时 fsync。后台线程周期性调用（everysec 模式）。
    void FlushIfNeeded();
    // 同步落盘并 fsync。优雅退出、always 模式、缓冲超限时调用。
    void SyncNow();

    bool is_open() const { return fp_ != nullptr; }
    AofSyncPolicy policy() const { return policy_; }
    const std::string& path() const { return path_; }
    int64_t file_size() const { return file_size_; }
    uint64_t pending_bytes();
    uint64_t total_appended() const { return total_appended_; }

    // 供测试使用的缓冲阈值（默认 4MB）。
    void set_flush_threshold(uint64_t bytes) { flush_threshold_ = bytes; }

private:
    bool WriteToFile(const char* data, size_t len, std::string* err);

    std::mutex mutex_;
    std::FILE* fp_ = nullptr;
    std::string path_;
    AofSyncPolicy policy_ = AofSyncPolicy::kEverySec;

    // 待落盘缓冲。命令线程只往这里追加，不碰文件 IO。
    std::string buffer_;
    uint64_t flush_threshold_ = 4 * 1024 * 1024;
    int64_t file_size_ = 0;
    uint64_t total_appended_ = 0;
};

const char* AofPolicyName(AofSyncPolicy p);
bool ParseAofPolicy(const std::string& s, AofSyncPolicy* out);

}  // namespace rl

#endif  // RL_AOF_H
