// Copyright (c) 2025 redis-lite authors. MIT License.
//
// zset.h — 有序集合（Redis ZSET 的等价实现）
//
// ============================ 设计：双索引 ============================
//
// ZSET 的三类操作对复杂度要求不同，没有任何**单一**数据结构能同时满足：
//
//   1) ZSCORE / ZINCRBY  按 member 查 score     → 期望 O(1)
//   2) ZRANGE / ZRANGEBYSCORE 范围遍历           → O(log N + M)
//   3) ZRANK 按 member 查排名                    → O(log N)
//
// 因此采用与 Redis 相同的「字典 + 跳表」双索引：
//
//   index_ : unordered_map<member, score>   负责 1)；同时承担 member 存在性判断
//   zsl_   : SkipList（带 span）            负责 2) 与 3)
//
// ============================ 关于 ZRANK 的演进 ============================
//
// 早期版本用 std::set 做有序索引，ZRANK 只能 std::distance(begin, it)，
// 即 O(rank) ≈ O(N/2) —— 那是当时项目里唯一一处复杂度未达 Redis 水平的地方。
//
// 现已换成**带 span 的跳表**：每条前向指针附带「它跨过多少个节点」，
// 搜索路径上顺带累加即得排名，ZRANK 降为 O(log N)。
//
// 实现过程并不顺利：span 的增量更新在多种层数分布下反复出错（rank 时对时错），
// 连续调试 11 轮未收敛。最终靠两件事解决：
//   ① 把 span 的定义收敛成唯一一条式子：span[i](x) = rank(x->forward[i]) - rank(x)
//   ② 写**穷举测试**：枚举「N 个元素各自层数」的全部组合（数千组），
//      让机器指出哪一组开始错 —— 第一次运行就定位到根因
//      （累加时误读了后继的 span 而非自己的 span，且该错误在 3 处复制）
// 完整记录见 docs/IMPLEMENTATION_LOG.md。

#ifndef RL_ZSET_H
#define RL_ZSET_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "rl/skip_list.h"

namespace rl {

using ZScore = double;

// 对外暴露的节点类型就是跳表节点 —— 命令层只需要 member 与 score 两个字段，
// 不需要为每种查询再包一层结构体。
using ZSetNode = SkipNode;

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
    ZScore IncrBy(const std::string& member, ZScore delta, bool* was_present);

    void Clear();

    // ---------------------------------------------------------------- 读
    size_t Size() const { return index_.size(); }

    // ZSCORE。**O(1)**（走字典）。
    bool GetScore(const std::string& member, ZScore* score) const;

    // ZRANK，**0-based**（与 Redis 一致）。不存在返回 false。
    // ★ O(log N)：字典给出 score，跳表带 span 的搜索给出排名。
    bool Rank(const std::string& member, size_t* rank) const;

    // ZRANGE start..stop，**0-based 闭区间**（命令层负责负数与越界归一化）。
    // 结果按 score 升序。
    void RangeByRank(size_t start, size_t stop, std::vector<const ZSetNode*>* out) const;

    // ZRANGEBYSCORE。exclusive 对应 "(1.5" 这种排除写法。
    void RangeByScore(ZScore min_score, ZScore max_score, bool min_exclusive,
                      bool max_exclusive, std::vector<const ZSetNode*>* out) const;

    const ZSetNode* First() const { return zsl_.first(); }
    const ZSetNode* Last() const { return zsl_.Last(); }

    // 按序导出（score 升序）。AOF 重写 / 调试使用。
    void ForEach(const std::function<void(const std::string&, ZScore)>& fn) const;

    size_t MemoryUsage() const;

private:
    std::unordered_map<std::string, ZScore> index_;
    SkipList zsl_;
};

// score ↔ 字符串。整数不带小数点，非整数用 %.17g 保证往返精度。
std::string ZScoreToString(ZScore score);
bool ParseZScore(const char* data, size_t len, ZScore* out);

}  // namespace rl

#endif  // RL_ZSET_H
