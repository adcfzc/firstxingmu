// Copyright (c) 2025 redis-lite authors. MIT License.
//
// bench.cc — 自研压测工具
//
// 为什么不用 redis-benchmark：
//   1) 自己写才能解释「瓶颈在哪」—— 压测工具本身往往比被测服务端更容易先成为瓶颈
//   2) 需要 pipeline 深度可控 + 尾延迟分位数统计，这是性能报告的核心数据
//   3) 简历上写「自研压测工具并完成性能验证」比「用 wrk 跑了个数」有说服力
//
// 方法论（面试可以展开讲）：
//   * 固定并发数，扫描 pipeline 深度，找出「延迟/吞吐」的拐点
//   * 统计 P50/P99/P999 而不只是平均值：平均值会掩盖长尾
//   * 先跑 warmup 轮次，排除首次分配、冷缓存、TCP 慢启动的影响
//   * 压测端与服务端不要放在同一台机器的同一核上，否则测的是抢核不是服务端

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "rl/platform.h"
#include "rl/resp.h"

namespace {

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
    int clients = 50;
    int64_t requests = 100000;   // 总请求数（所有 client 合计）
    int pipeline = 16;           // 每批未确认请求数
    size_t value_size = 64;      // SET 的 value 字节数
    std::string command = "set"; // set | get | incr
    int key_space = 10000;       // key 空间大小
    bool warmup = true;
    bool verbose = false;
    int64_t timeout_sec = 60;    // 整体超时保护
};

void PrintUsage(const char* prog) {
    std::printf(
        "redis-lite bench — 自研压测工具\n"
        "\n"
        "用法: %s [options]\n"
        "\n"
        "  -h, --host <ip>        服务端地址 (默认 127.0.0.1)\n"
        "  -p, --port <n>         服务端端口 (默认 6379)\n"
        "  -c, --clients <n>      并发连接数 (默认 50)\n"
        "  -n, --requests <n>     总请求数 (默认 100000)\n"
        "  -P, --pipeline <n>     流水线深度 (默认 16)\n"
        "  -d, --datasize <n>     SET 的 value 字节数 (默认 64)\n"
        "  -t, --command <cmd>    set | get | incr (默认 set)\n"
        "  -k, --keyspace <n>     key 空间 (默认 10000)\n"
        "      --no-warmup        跳过预热轮次\n"
        "  -v, --verbose          打印每线程结果\n"
        "\n"
        "示例:\n"
        "  %s -c 50 -n 200000 -P 16 -d 64 -t set\n"
        "  %s -c 8  -n 200000 -P 1  -t get\n"
        "\n"
        "注意: pipeline=1 测的是 RTT 受限下的延迟，pipeline 加大才能测出吞吐上限。\n",
        prog, prog, prog);
}

bool ParseInt(const char* s, int64_t* out) {
    char* end = nullptr;
    long long v = std::strtoll(s, &end, 10);
    if (end == nullptr || *end != '\0') return false;
    *out = static_cast<int64_t>(v);
    return true;
}

bool ParseArgs(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "option %s requires a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        int64_t v = 0;

        if (a == "-h" || a == "--host") {
            o->host = need_value("--host");
        } else if (a == "-p" || a == "--port") {
            if (!ParseInt(need_value("--port"), &v) || v <= 0 || v > 65535) {
                std::fprintf(stderr, "invalid port\n");
                return false;
            }
            o->port = static_cast<uint16_t>(v);
        } else if (a == "-c" || a == "--clients") {
            if (!ParseInt(need_value("--clients"), &v) || v <= 0 || v > 4096) {
                std::fprintf(stderr, "invalid clients\n");
                return false;
            }
            o->clients = static_cast<int>(v);
        } else if (a == "-n" || a == "--requests") {
            if (!ParseInt(need_value("--requests"), &v) || v <= 0) {
                std::fprintf(stderr, "invalid requests\n");
                return false;
            }
            o->requests = v;
        } else if (a == "-P" || a == "--pipeline") {
            if (!ParseInt(need_value("--pipeline"), &v) || v <= 0 || v > 4096) {
                std::fprintf(stderr, "invalid pipeline\n");
                return false;
            }
            o->pipeline = static_cast<int>(v);
        } else if (a == "-d" || a == "--datasize") {
            if (!ParseInt(need_value("--datasize"), &v) || v < 0 || v > 1024 * 1024) {
                std::fprintf(stderr, "invalid datasize\n");
                return false;
            }
            o->value_size = static_cast<size_t>(v);
        } else if (a == "-k" || a == "--keyspace") {
            if (!ParseInt(need_value("--keyspace"), &v) || v <= 0) {
                std::fprintf(stderr, "invalid keyspace\n");
                return false;
            }
            o->key_space = static_cast<int>(v);
        } else if (a == "-t" || a == "--command") {
            o->command = need_value("--command");
            if (o->command != "set" && o->command != "get" && o->command != "incr") {
                std::fprintf(stderr, "invalid command: %s\n", o->command.c_str());
                return false;
            }
        } else if (a == "--no-warmup") {
            o->warmup = false;
        } else if (a == "-v" || a == "--verbose") {
            o->verbose = true;
        } else if (a == "--help") {
            PrintUsage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

// ============================================================ 发送缓冲拼装
// 直接把整批命令拼进一个 string，一次 send。
// 这正是 pipeline 的收益来源：把 N 次 write 系统调用压成 1 次。
void BuildBatch(const Options& o, int64_t start, int batch, std::string* out,
                std::vector<std::string>* args_scratch) {
    out->clear();
    // 预留：每条命令约 "$3\r\nSET\r\n$9\r\nkey:12345\r\n$64\r\n<value>\r\n" ≈ value_size + 64
    out->reserve(static_cast<size_t>(batch) * (o.value_size + 64));

    std::string value;
    if (o.command == "set") {
        value.assign(o.value_size, 'v');
    }

    std::string key;
    for (int i = 0; i < batch; ++i) {
        const int64_t idx = (start + i) % o.key_space;
        key = "key:" + std::to_string(idx);

        args_scratch->clear();
        args_scratch->push_back(o.command);
        args_scratch->push_back(key);
        if (o.command == "set") {
            // 把序号编码进 value，避免不同 key 写入完全相同的字节，
            // 从而暴露「服务端把相同 value 共享内存」这类实现差异。
            value.resize(o.value_size, 'v');
            if (o.value_size >= 8) {
                char tail[16];
                int n = std::snprintf(tail, sizeof(tail), "%07lld",
                                      static_cast<long long>(idx % 10000000));
                std::memcpy(&value[o.value_size - static_cast<size_t>(n)], tail,
                            static_cast<size_t>(n));
            }
            args_scratch->push_back(value);
        }

        rl::EncodeCommand(*args_scratch, out);
    }
}

// ============================================================ 响应读取
// 只需要判断「收到了一条完整回复」，不需要解析内容。
// 但必须真的走完整个 RESP 结构，否则字节流会错位。
class ReplyReader {
public:
    explicit ReplyReader(rl::Socket fd) : fd_(fd) {}

    // 读满 count 条回复。返回实际读到的条数，错误时返回负数。
    int ReadReplies(int count, int64_t* errors_out, std::string* err) {
        int got = 0;
        while (got < count) {
            int r = ReadOne(errors_out, err);
            if (r == 0) return got;       // 对端关闭
            if (r < 0) return r;          // 出错
            ++got;
        }
        return got;
    }

private:
    // 保证 buf_ 中从 pos_ 起至少有 n 字节。返回 false 表示对端关闭或出错。
    bool Fill(size_t n, std::string* err) {
        while (buf_.size() - pos_ < n) {
            char tmp[64 * 1024];
            int r = rl::RecvSome(fd_, tmp, sizeof(tmp));
            if (r > 0) {
                buf_.append(tmp, static_cast<size_t>(r));
                continue;
            }
            if (r == 0) return false;
            if (rl::WouldBlock() || rl::Interrupted()) {
                // 压测工具用阻塞读更简单：这里退化成轮询等待。
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            if (err != nullptr) *err = "recv failed: " + rl::LastSocketError();
            return false;
        }
        return true;
    }

    // 1 = 成功读一条；0 = 对端关闭；-1 = 错误
    int ReadOne(int64_t* errors_out, std::string* err) {
        if (!Fill(1, err)) return err != nullptr && !err->empty() ? -1 : 0;

        const char type = buf_[pos_];

        if (type == '+' || type == '-' || type == ':') {
            // 简单字符串 / 错误 / 整数：读到 CRLF
            size_t nl = FindCRLF();
            if (nl == std::string::npos) {
                if (!FillLine(err)) return -1;
                nl = FindCRLF();
                if (nl == std::string::npos) {
                    if (err != nullptr) *err = "malformed reply (no CRLF)";
                    return -1;
                }
            }
            if (type == '-' && errors_out != nullptr) ++(*errors_out);
            pos_ = nl + 2;
            return 1;
        }

        if (type == '$') {
            size_t nl = FindCRLF();
            if (nl == std::string::npos) {
                if (!FillLine(err)) return -1;
                nl = FindCRLF();
                if (nl == std::string::npos) {
                    if (err != nullptr) *err = "malformed bulk header";
                    return -1;
                }
            }
            int64_t len = 0;
            if (!rl::ParseInt64(buf_.data() + pos_ + 1, nl - pos_ - 1, &len)) {
                if (err != nullptr) *err = "malformed bulk length";
                return -1;
            }
            pos_ = nl + 2;
            if (len < 0) return 1;  // null bulk
            if (!Fill(static_cast<size_t>(len) + 2, err)) {
                return err != nullptr && !err->empty() ? -1 : 0;
            }
            pos_ += static_cast<size_t>(len) + 2;
            return 1;
        }

        if (type == '*') {
            // 数组：递归读元素。本压测工具的命令回复不会用到，但保持正确性。
            size_t nl = FindCRLF();
            if (nl == std::string::npos) {
                if (!FillLine(err)) return -1;
                nl = FindCRLF();
                if (nl == std::string::npos) return -1;
            }
            int64_t n = 0;
            if (!rl::ParseInt64(buf_.data() + pos_ + 1, nl - pos_ - 1, &n)) return -1;
            pos_ = nl + 2;
            for (int64_t i = 0; i < n; ++i) {
                int r = ReadOne(errors_out, err);
                if (r <= 0) return r;
            }
            return 1;
        }

        if (err != nullptr) {
            *err = std::string("unknown reply type: ") + type;
        }
        return -1;
    }

    bool FillLine(std::string* err) {
        // 已经读到 CRLF 之前就没有更多数据了：再读一块。
        char c;
        for (;;) {
            if (buf_.size() - pos_ >= 2) {
                for (size_t i = pos_; i + 1 < buf_.size(); ++i) {
                    if (buf_[i] == '\r' && buf_[i + 1] == '\n') return true;
                }
            }
            int r = rl::RecvSome(fd_, &c, 1);
            if (r > 0) {
                buf_.push_back(c);
                continue;
            }
            if (r == 0) return false;
            if (rl::WouldBlock() || rl::Interrupted()) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            if (err != nullptr) *err = "recv failed while reading line";
            return false;
        }
    }

    // 从 pos_ 起找 CRLF 的绝对偏移
    size_t FindCRLF() const {
        for (size_t i = pos_; i + 1 < buf_.size(); ++i) {
            if (buf_[i] == '\r' && buf_[i + 1] == '\n') return i;
        }
        return std::string::npos;
    }

    rl::Socket fd_;
    std::string buf_;
    size_t pos_ = 0;
};

// ============================================================ 分位数
double Percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double idx = p * static_cast<double>(v.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return v[lo];
    double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

struct ThreadResult {
    int64_t completed = 0;
    int64_t errors = 0;      // 服务端返回的 RESP 错误条数
    uint64_t bytes_sent = 0;
    double elapsed_sec = 0.0;
    std::vector<double> batch_latency_us;
    std::string error;
};

// 每个线程独立连接、独立发送，互不干扰。
void ClientWorker(const Options& o, int id, int64_t my_requests, ThreadResult* result) {
    std::string err;
    rl::Socket fd = rl::ConnectTo(o.host, o.port, &err);
    if (fd == rl::kInvalidSocket) {
        result->error = err;
        return;
    }

    ReplyReader reader(fd);
    std::string batch;
    std::vector<std::string> args;

    // key 空间按线程切分，避免所有线程争抢同一批 key（会造成锁热点，
    // 测出来的就不是真实并发能力，而是单个分片的串行能力）。
    const int64_t key_base = static_cast<int64_t>(id) * (o.key_space / std::max(1, o.clients));

    int64_t sent = 0;
    while (sent < my_requests) {
        const int n = static_cast<int>(
            std::min<int64_t>(o.pipeline, my_requests - sent));

        BuildBatch(o, key_base + sent, n, &batch, &args);

        const auto t0 = std::chrono::steady_clock::now();

        // 一次 send 送出整批命令。
        size_t off = 0;
        bool send_ok = true;
        while (off < batch.size()) {
            int w = rl::SendSome(fd, batch.data() + off, batch.size() - off);
            if (w > 0) {
                off += static_cast<size_t>(w);
                continue;
            }
            if (w < 0 && (rl::WouldBlock() || rl::Interrupted())) {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                continue;
            }
            send_ok = false;
            break;
        }
        if (!send_ok) {
            result->error = "send failed: " + rl::LastSocketError();
            break;
        }
        result->bytes_sent += batch.size();

        const int got = reader.ReadReplies(n, &result->errors, &err);
        if (got != n) {
            result->error = err.empty() ? "short read from server" : err;
            break;
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double us =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1000.0;
        // 记的是「每批」延迟，最后换算成「每请求」。
        result->batch_latency_us.push_back(us / static_cast<double>(n));

        sent += n;
        result->completed += n;
    }

    rl::CloseSocket(fd);
}

// 跑一轮，返回结果。warmup 轮次的结果会被丢弃。
ThreadResult RunRound(const Options& o, int64_t total_requests) {
    const int per_client = static_cast<int>(total_requests / o.clients);
    const int64_t remainder = total_requests % o.clients;

    std::vector<ThreadResult> results(static_cast<size_t>(o.clients));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(o.clients));

    const auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < o.clients; ++i) {
        const int64_t mine = per_client + (i < static_cast<int>(remainder) ? 1 : 0);
        threads.emplace_back([&o, i, mine, &results] {
            ClientWorker(o, i, mine, &results[static_cast<size_t>(i)]);
        });
    }
    for (auto& t : threads) t.join();

    const auto t1 = std::chrono::steady_clock::now();

    ThreadResult merged;
    for (const ThreadResult& r : results) {
        merged.completed += r.completed;
        merged.errors += r.errors;
        merged.bytes_sent += r.bytes_sent;
        if (merged.error.empty() && !r.error.empty()) merged.error = r.error;
        merged.batch_latency_us.insert(merged.batch_latency_us.end(),
                                       r.batch_latency_us.begin(), r.batch_latency_us.end());
    }
    merged.elapsed_sec =
        std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();

    if (o.verbose) {
        for (size_t i = 0; i < results.size(); ++i) {
            if (!results[i].error.empty()) {
                std::printf("  [client %zu] error: %s\n", i, results[i].error.c_str());
            }
        }
    }
    return merged;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!ParseArgs(argc, argv, &o)) {
        PrintUsage(argv[0]);
        return 2;
    }

    std::string err;
    if (!rl::InitSockets(&err)) {
        std::fprintf(stderr, "socket init failed: %s\n", err.c_str());
        return 1;
    }

    // 连通性预检：早点报错，别让用户等一场全失败的压测。
    {
        rl::Socket probe = rl::ConnectTo(o.host, o.port, &err);
        if (probe == rl::kInvalidSocket) {
            std::fprintf(stderr, "cannot connect to %s:%u — %s\n", o.host.c_str(),
                         static_cast<unsigned>(o.port), err.c_str());
            rl::CleanupSockets();
            return 1;
        }
        rl::CloseSocket(probe);
    }

    std::printf("redis-lite bench\n");
    std::printf("  target    : %s:%u\n", o.host.c_str(), static_cast<unsigned>(o.port));
    std::printf("  command   : %s (value_size=%zu, keyspace=%d)\n", o.command.c_str(),
                o.value_size, o.key_space);
    std::printf("  clients   : %d, pipeline: %d, requests: %lld\n", o.clients, o.pipeline,
                static_cast<long long>(o.requests));
    std::printf("\n");

    if (o.warmup) {
        // 预热：让 TCP 慢启动、内存分配器、分支预测器都进入稳定状态。
        std::printf("warmup...\n");
        const int64_t warm = std::max<int64_t>(o.clients * 100, o.requests / 20);
        RunRound(o, warm);
    }

    std::printf("running...\n");
    ThreadResult r = RunRound(o, o.requests);

    if (!r.error.empty()) {
        std::fprintf(stderr, "\nbench error: %s\n", r.error.c_str());
    }
    if (r.completed == 0) {
        rl::CleanupSockets();
        return 1;
    }

    const double qps = static_cast<double>(r.completed) / r.elapsed_sec;
    const double avg_us = r.elapsed_sec * 1e6 / static_cast<double>(r.completed);

    std::vector<double> lat = r.batch_latency_us;
    const double p50 = Percentile(lat, 0.50);
    const double p90 = Percentile(lat, 0.90);
    const double p99 = Percentile(lat, 0.99);
    const double p999 = Percentile(lat, 0.999);
    double max_us = lat.empty() ? 0.0 : lat.back();

    std::printf("\n================ results ================\n");
    std::printf("  completed      : %lld requests in %.3f s\n",
                static_cast<long long>(r.completed), r.elapsed_sec);
    std::printf("  throughput     : %.0f ops/sec\n", qps);
    std::printf("  net tx         : %.2f MB (%.1f MB/s)\n",
                static_cast<double>(r.bytes_sent) / 1048576.0,
                static_cast<double>(r.bytes_sent) / 1048576.0 / r.elapsed_sec);
    std::printf("  --- per-request latency ---\n");
    std::printf("  avg            : %.1f us\n", avg_us);
    std::printf("  p50            : %.1f us\n", p50);
    std::printf("  p90            : %.1f us\n", p90);
    std::printf("  p99            : %.1f us   <-- 尾延迟，比均值更重要\n", p99);
    std::printf("  p99.9          : %.1f us\n", p999);
    std::printf("  max            : %.1f us\n", max_us);
    if (r.errors > 0) {
        std::printf("  server errors  : %lld   <-- 服务端返回了 RESP 错误\n",
                    static_cast<long long>(r.errors));
    }
    std::printf("=========================================\n");

    rl::CleanupSockets();
    return r.errors > 0 ? 1 : 0;
}
