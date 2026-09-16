// Copyright (c) 2025 redis-lite authors. MIT License.
//
// bench_zrank.cc — 验证 ZRANK 的复杂度从 O(N) 降到 O(log N)
//
// 为什么单独写这个工具：
//   「我把 ZRANK 从 O(N) 优化到 O(log N)」是一句需要证据的话。
//   复杂度是渐进行为，不能用单个规模的耗时证明 —— 必须看**随规模的增长趋势**：
//
//     O(N)     ：规模 ×10 → 耗时 ×10
//     O(log N) ：规模 ×10 → 耗时约 +log(10) ≈ ×1.3~1.5（且绝对值极小）
//
//   所以本工具在多个规模上测同一个操作，打印耗时比值，让趋势自己说话。
//
// 用法:
//   ./rl-bench-zrank            # 默认 1k / 10k / 100k / 1M
//   ./rl-bench-zrank 1000 100000

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "rl/zset.h"

using namespace rl;

namespace {

// 自实现 LCG：保证可复现（std::rand 跨平台实现不同）
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t Next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return s >> 33;
    }
    double NextDouble() { return static_cast<double>(Next() % 1000000) / 1000.0; }
};

struct Row {
    size_t n = 0;
    double add_ns = 0;     // 每次 ZADD 平均耗时
    double rank_ns = 0;    // 每次 ZRANK 平均耗时
    double score_ns = 0;   // 每次 ZSCORE 平均耗时
    double range_ns = 0;   // 每次 ZRANGE(取 100 个) 平均耗时
};

Row RunSize(size_t n, size_t queries) {
    Row row;
    row.n = n;

    ZSet z;
    Rng rng(12345);

    std::vector<std::string> members;
    members.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        members.push_back("member:" + std::to_string(i));
    }

    // ---- 建表：ZADD ----
    {
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < n; ++i) {
            z.Add(members[i], rng.NextDouble());
        }
        auto t1 = std::chrono::steady_clock::now();
        row.add_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() /
            static_cast<double>(n);
    }

    // 随机挑查询目标（覆盖整张表，而非总是查同几个）
    std::vector<size_t> picks;
    picks.reserve(queries);
    for (size_t i = 0; i < queries; ++i) picks.push_back(static_cast<size_t>(rng.Next() % n));

    // ---- ZRANK（本工具的主角）----
    {
        volatile size_t sink = 0;  // 防止被优化掉
        size_t acc = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < queries; ++i) {
            size_t r = 0;
            if (z.Rank(members[picks[i]], &r)) acc += r;
        }
        auto t1 = std::chrono::steady_clock::now();
        sink = acc;
        (void)sink;
        row.rank_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() /
            static_cast<double>(queries);
    }

    // ---- ZSCORE（字典，O(1) 参照）----
    {
        ZScore acc = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < queries; ++i) {
            ZScore s = 0;
            if (z.GetScore(members[picks[i]], &s)) acc += s;
        }
        auto t1 = std::chrono::steady_clock::now();
        (void)acc;
        row.score_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() /
            static_cast<double>(queries);
    }

    // ---- ZRANGE 取 100 个（O(log N + M) 参照）----
    {
        const size_t want = std::min<size_t>(100, n);
        volatile size_t sink = 0;
        size_t acc = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < queries / 10 + 1; ++i) {
            const size_t from = static_cast<size_t>(rng.Next() % std::max<size_t>(1, n - want));
            std::vector<const ZSetNode*> out;
            z.RangeByRank(from, from + want - 1, &out);
            acc += out.size();
        }
        auto t1 = std::chrono::steady_clock::now();
        sink = acc;
        (void)sink;
        const double iters = static_cast<double>(queries / 10 + 1);
        row.range_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / iters;
    }

    return row;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<size_t> sizes;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            sizes.push_back(static_cast<size_t>(std::strtoull(argv[i], nullptr, 10)));
        }
    } else {
        sizes = {1000, 10000, 100000, 1000000};
    }

    std::printf("redis-lite ZRANK complexity check\n");
    std::printf("(ZSet = unordered_map + skip list with span)\n\n");
    std::printf("%-12s %-14s %-16s %-14s %-16s\n", "N", "ZADD/op", "ZRANK/op", "ZSCORE/op",
                "ZRANGE(100)/op");
    std::printf("%-12s %-14s %-16s %-14s %-16s\n", "-------------", "--------------",
                "----------------", "--------------", "----------------");

    std::vector<Row> rows;
    for (size_t n : sizes) {
        // 查询次数随规模略增，但控制总量
        const size_t queries = (n >= 1000000) ? 200000 : (n >= 100000 ? 100000 : 50000);
        rows.push_back(RunSize(n, queries));

        const Row& r = rows.back();
        auto us = [](double ns) { return ns / 1000.0; };
        std::printf("%-12zu %-14s %-16s %-14s %-16s\n", r.n,
                    (std::to_string(us(r.add_ns)) + " us").c_str(),
                    (std::to_string(us(r.rank_ns)) + " us").c_str(),
                    (std::to_string(us(r.score_ns)) + " us").c_str(),
                    (std::to_string(us(r.range_ns)) + " us").c_str());
    }

    // 趋势分析：这是本工具的结论部分
    std::printf("\n--- scaling (ratio vs smallest N) ---\n");
    std::printf("%-12s %-16s %-16s %-10s\n", "N", "ZRANK ratio", "N ratio", "verdict");
    const Row& base = rows.front();
    for (const Row& r : rows) {
        const double rank_ratio = r.rank_ns / base.rank_ns;
        const double n_ratio = static_cast<double>(r.n) / static_cast<double>(base.n);

        // 若 ZRANK 是 O(N)，rank_ratio 应≈ n_ratio；
        // 若是 O(log N)，rank_ratio 应远小于 n_ratio。
        const char* verdict = "?";
        if (n_ratio > 1.5) {
            verdict = (rank_ratio < n_ratio / 3.0) ? "sub-linear (consistent with O(log N))"
                                                   : "close to linear (would indicate O(N))";
        }
        std::printf("%-12zu %-16s %-16s %-10s\n", r.n,
                    (std::to_string(rank_ratio) + "x").c_str(),
                    (std::to_string(n_ratio) + "x").c_str(), verdict);
    }

    std::printf(
        "\n注：早期版本用 std::set 实现，ZRANK 是 O(rank)≈O(N/2)，\n"
        "    在 N=100 万时单次查询需扫描约 50 万个节点。\n"
        "    现在的数字应与 ZSCORE（纯字典 O(1)）同一量级。\n");
    return 0;
}
