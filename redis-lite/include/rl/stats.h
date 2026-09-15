// Copyright (c) 2025 redis-lite authors. MIT License.
//
// stats.h — 运行期计量，供 INFO 命令输出
//
// 全部用 std::atomic<uint64_t>，允许多个 loop 线程并发累加而不加锁。
// 这是 INFO 命令能有意义的前提，也是压测时观察 QPS 的依据。

#ifndef RL_STATS_H
#define RL_STATS_H

#include <atomic>
#include <cstdint>
#include <string>

#include "rl/platform.h"
#include "rl/log.h"

namespace rl {

class Stats {
public:
    void IncCommands() { commands_.fetch_add(1, std::memory_order_relaxed); }
    void IncConnections() { connections_.fetch_add(1, std::memory_order_relaxed); }
    void IncRejected() { rejected_.fetch_add(1, std::memory_order_relaxed); }
    void IncExpiredKeys() { expired_keys_.fetch_add(1, std::memory_order_relaxed); }
    void IncKeyspaceHits() { hits_.fetch_add(1, std::memory_order_relaxed); }
    void IncKeyspaceMisses() { misses_.fetch_add(1, std::memory_order_relaxed); }
    void AddBytesIn(uint64_t n) { bytes_in_.fetch_add(n, std::memory_order_relaxed); }
    void AddBytesOut(uint64_t n) { bytes_out_.fetch_add(n, std::memory_order_relaxed); }
    void IncProtocolErrors() { protocol_errors_.fetch_add(1, std::memory_order_relaxed); }

    void SetUptimeStart() { start_ms_ = NowMs(); }
    void SetCurrentConnections(int64_t n) {
        current_connections_.store(n, std::memory_order_relaxed);
    }
    void SetTotalConnections(int64_t n) {
        total_connections_.store(n, std::memory_order_relaxed);
    }
    void SetUsedMemory(int64_t bytes) { used_memory_.store(bytes, std::memory_order_relaxed); }
    void SetKeyspace(size_t keys, size_t expires) {
        keys_.store(static_cast<uint64_t>(keys), std::memory_order_relaxed);
        expires_.store(static_cast<uint64_t>(expires), std::memory_order_relaxed);
    }
    void SetAofSize(int64_t bytes) { aof_size_.store(bytes, std::memory_order_relaxed); }

    uint64_t commands() const { return commands_.load(std::memory_order_relaxed); }
    uint64_t connections_total() const {
        return total_connections_.load(std::memory_order_relaxed);
    }
    int64_t current_connections() const {
        return current_connections_.load(std::memory_order_relaxed);
    }
    uint64_t rejected() const { return rejected_.load(std::memory_order_relaxed); }
    uint64_t expired_keys() const { return expired_keys_.load(std::memory_order_relaxed); }
    uint64_t protocol_errors() const { return protocol_errors_.load(std::memory_order_relaxed); }
    uint64_t bytes_in() const { return bytes_in_.load(std::memory_order_relaxed); }
    uint64_t bytes_out() const { return bytes_out_.load(std::memory_order_relaxed); }
    int64_t used_memory() const { return used_memory_.load(std::memory_order_relaxed); }
    int64_t aof_size() const { return aof_size_.load(std::memory_order_relaxed); }
    uint64_t keys() const { return keys_.load(std::memory_order_relaxed); }
    uint64_t expires() const { return expires_.load(std::memory_order_relaxed); }

    int64_t uptime_seconds() const {
        int64_t start = start_ms_.load(std::memory_order_relaxed);
        if (start == 0) return 0;
        return (NowMs() - start) / 1000;
    }

    // hit_rate 用字符串返回，避免除零。
    std::string hit_rate_string() const {
        uint64_t h = hits_.load(std::memory_order_relaxed);
        uint64_t m = misses_.load(std::memory_order_relaxed);
        if (h + m == 0) return "0.00";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", 100.0 * static_cast<double>(h) /
                                                     static_cast<double>(h + m));
        return std::string(buf);
    }

private:
    std::atomic<uint64_t> commands_{0};
    std::atomic<uint64_t> connections_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> expired_keys_{0};
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
    std::atomic<uint64_t> bytes_in_{0};
    std::atomic<uint64_t> bytes_out_{0};
    std::atomic<uint64_t> protocol_errors_{0};
    std::atomic<uint64_t> keys_{0};
    std::atomic<uint64_t> expires_{0};
    std::atomic<uint64_t> total_connections_{0};
    std::atomic<int64_t> current_connections_{0};
    std::atomic<int64_t> used_memory_{0};
    std::atomic<int64_t> aof_size_{0};
    std::atomic<int64_t> start_ms_{0};
};

}  // namespace rl

#endif  // RL_STATS_H
