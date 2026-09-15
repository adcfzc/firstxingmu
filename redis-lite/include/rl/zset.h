// Copyright (c) 2025 redis-lite authors. MIT License.
//
// zset.h — 有序集合
//
// ============================ 设计取舍（面试重点） ============================
//
// ZSET 需要同时支持三类操作，且要求不同的复杂度：
//   1) ZSCORE / ZINCRBY 按 member 查 score        → 期望 O(1)
//   2) ZRANGE / ZRANGEBYSCORE 按 score 范围遍历   → 期望 O(log N + M)
//   3) ZRANK 按 member 查排名                     → 期望 O(log N)
//
// 没有任何**单一**数据结构能同时把三者做到最优，所以这里采用双索引：
//
//   index_ : std::unordered_map<member, score>   负责 1)
//   sorted_: std::set<ZSetNode>                  负责 2)，并提供有序性
//
// ============================ 关于 ZRANK 的诚实说明 ============================
//
// std::set 的节点**不知道自己的排名**，所以 ZRANK 只能退化为
// std::distance(begin, it)，即 O(rank) —— 平均 O(N/2)。
//
// 这是本次开发中一个明确的取舍，也是项目里唯一一处「复杂度未达 Redis 水平」
// 的地方，必须如实说明：
//
//   Redis 用**带 span 的跳表**把 ZRANK 做成 O(log N)。span 记录每个指针
//   「覆盖了多少个节点」，因此沿途累加即可得到排名。
//   我实现了这个结构，但 span 的增量更新在多种层数分布下反复出错
//   （详见 docs/IMPLEMENTATION_LOG.md），在交付压力下最终改为
//   「保证正确 + 明确记录缺口」，而不是带着未验证的复杂度上线。
//
//   影响面很窄：只有 ZRANK / ZREVRANK 退化；
//   ZADD、ZSCORE、ZCARD、ZRANGE、ZRANGEBYSCORE 都仍是
//   期望 O(log N) 或 O(log N + M)，而这些才是 ZSET 的主要用途。
//
// 面试时这个问题值得**主动**讲，因为它比「我实现了跳表」更能体现工程判断：
// 知道取舍点在哪、知道缺口的边界在哪、知道补上的路径是什么。

#ifndef RL_ZSET_H
#define RL_ZSET_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace rl {

using ZScore = double;

// 有序集合的排序键：先比 score，score 相同再比 member 字典序。
// 这个全序与 Redis 完全一致，是 ZRANGE 结果稳定可预期的基础。
struct ZSetNode {
    ZScore score = 0.0;
    std::string member;

    bool operator<(const ZSetNode& o) const {
        if (score != o.score) return score < o.score;
        return member < o.member;
    }
};

class ZSet {
public:
    ZSet() = default;
    ~ZSet() = default;

    ZSet(const ZSet&) = delete;
    ZSet& operator=(const ZSet&) = delete;

    // ---------------------------------------------------------------- 写
    // ZADD 核心：member 不存在则插入，存在则只更新 score。
    // 返回是否**新增**了元素 —— 与 Redis 的 ZADD 返回值一致（更新不计入）。
    bool Add(const std::string& member, ZScore score);

    // 删除 member。返回是否真的删掉了。
    bool Remove(const std::string& member);

    // ZINCRBY：给 member 的 score 加 delta；member 不存在时按 0 起算。
    // 返回新 score，并通过 was_present 告知调用方它原本是否存在。
    ZScore IncrBy(const std::string& member, ZScore delta, bool* was_present);

    void Clear();

    // ---------------------------------------------------------------- 读
    size_t Size() const { return index_.size(); }

    // ZSCORE。不存在返回 false。**O(1)**。
    bool GetScore(const std::string& member, ZScore* score) const;

    // ZRANK，**0-based**（与 Redis 一致；ZRANK 返回 0 表示最小元素）。
    // 不存在返回 false。
    // ⚠️ 复杂度 O(rank)，原因见文件头说明。
    bool Rank(const std::string& member, size_t* rank) const;

    // ZRANGE start..stop，**0-based 闭区间**（已由命令层归一化）。
    // 结果按 score 升序压入 out。start > stop 或越界时返回空。
    void RangeByRank(size_t start, size_t stop, std::vector<const ZSetNode*>* out) const;

    // ZRANGEBYSCORE。min/max 的排除语义由 exclusive 标记表达
    //（对应 "(1.5" 这种写法）。
    void RangeByScore(ZScore min_score, ZScore max_score, bool min_exclusive,
                      bool max_exclusive, std::vector<const ZSetNode*>* out) const;

    const ZSetNode* First() const { return sorted_.empty() ? nullptr : &(*sorted_.begin()); }
    const ZSetNode* Last() const { return sorted_.empty() ? nullptr : &(*sorted_.rbegin()); }

    // 按序导出全部元素（score 升序）。AOF 重写 / 调试使用。
    void ForEach(const std::function<void(const std::string&, ZScore)>& fn) const;

    // 估算内存占用，计入 INFO 的 used_memory。
    size_t MemoryUsage() const;

private:
    std::unordered_map<std::string, ZScore> index_;
    std::set<ZSetNode> sorted_;
};

// score ↔ 字符串转换。
// 整数值不带小数点（避免用户看到 "1.0000000000000000"），
// 非整数用 %.17g 保证 ParseZScore(ZScoreToString(x)) == x。
std::string ZScoreToString(ZScore score);
bool ParseZScore(const char* data, size_t len, ZScore* out);

}  // namespace rl

#endif  // RL_ZSET_H
