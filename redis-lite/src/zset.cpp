// Copyright (c) 2025 redis-lite authors. MIT License.
//
// zset.cpp — 有序集合实现
//
// 本文件的核心关注点是**双索引一致性**：
//   index_（字典）与 zsl_（跳表）必须始终描述同一份数据。
//   一旦不一致，就会出现「ZSCORE 查得到但 ZRANGE 看不到」这类幽灵 bug ——
//   难复现、难定位。因此所有写路径都集中在此，且每个写操作都显式更新两者。
//
// 为什么 member 的存在性判断走字典而不是跳表：
//   跳表按 (score, member) 排序，只有 member 无法直接定位，只能线性扫 —— O(N)。
//   字典把这件事做成 O(1)，这正是 Redis 同时维护两份索引的原因。
//   正因如此，跳表的插入入口用 InsertNew（不判重），ZADD 全程保持 O(log N)。

#include "rl/zset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "rl/log.h"

namespace rl {

// ================================================================ 写路径
bool ZSet::Add(const std::string& member, ZScore score) {
    auto it = index_.find(member);

    if (it == index_.end()) {
        // 新增：字典 O(1) 判重已完成，跳表直接插（InsertNew 不重复扫描）
        index_.emplace(member, score);
        zsl_.InsertNew(score, member);
        return true;
    }

    // 已存在：score 没变就什么都不做。
    // 这个短路很关键，否则高频重复 ZADD 会白做一次「删 + 插」的 O(log N)。
    if (it->second == score) return false;

    // score 变了：跳表按 (score, member) 排序，改 score 等于换位置，
    // 必须「删旧 + 插新」。
    zsl_.Delete(it->second, member);
    it->second = score;
    zsl_.InsertNew(score, member);
    return false;
}

bool ZSet::Remove(const std::string& member) {
    auto it = index_.find(member);
    if (it == index_.end()) return false;

    zsl_.Delete(it->second, member);
    index_.erase(it);
    return true;
}

ZScore ZSet::IncrBy(const std::string& member, ZScore delta, bool* was_present) {
    auto it = index_.find(member);

    if (it == index_.end()) {
        if (was_present != nullptr) *was_present = false;
        index_.emplace(member, delta);
        zsl_.InsertNew(delta, member);
        return delta;
    }

    if (was_present != nullptr) *was_present = true;
    const ZScore new_score = it->second + delta;
    if (new_score == it->second) return new_score;  // delta 为 0，无需动跳表

    zsl_.Delete(it->second, member);
    it->second = new_score;
    zsl_.InsertNew(new_score, member);
    return new_score;
}

void ZSet::Clear() {
    index_.clear();
    zsl_.Clear();
}

// ================================================================ 读路径
bool ZSet::GetScore(const std::string& member, ZScore* score) const {
    auto it = index_.find(member);
    if (it == index_.end()) return false;
    if (score != nullptr) *score = it->second;
    return true;
}

bool ZSet::Rank(const std::string& member, size_t* rank) const {
    // 第一步 O(1)：从字典拿到 score（跳表搜索需要完整的排序键）。
    auto it = index_.find(member);
    if (it == index_.end()) return false;

    // 第二步 O(log N)：跳表在搜索路径上累加 span 得到排名。
    unsigned long r = 0;
    if (!zsl_.Rank(it->second, member, &r)) {
        // 双索引不一致 —— 不该发生的状态，明确报错而不是返回错误排名。
        RL_ERROR("ZSet::Rank: index/zset inconsistent, member=%s score=%g", member.c_str(),
                 it->second);
        return false;
    }

    if (rank != nullptr) *rank = static_cast<size_t>(r);
    return true;
}

void ZSet::RangeByRank(size_t start, size_t stop, std::vector<const ZSetNode*>* out) const {
    if (out == nullptr || start > stop || start >= zsl_.size()) return;

    // 跳表的 RangeByRank 用 1-based 闭区间（内部以此表达「第几名」），
    // 而 ZSET 的 ZRANGE 语义是 0-based —— 这里做一次转换。
    zsl_.RangeByRank(static_cast<unsigned long>(start) + 1,
                     static_cast<unsigned long>(stop) + 1, out);
}

void ZSet::RangeByScore(ZScore min_score, ZScore max_score, bool min_exclusive,
                        bool max_exclusive, std::vector<const ZSetNode*>* out) const {
    if (out == nullptr) return;

    // O(log N) 定位起点，再线性收集 O(M)。
    //
    // 刻意不用「构造最大 member 做 upper_bound」那种技巧：
    // member 是 std::string，构造不出字典序最大的字符串，
    // 强行用哨兵值会让代码依赖「空字符串是最小 member」这种隐式假设。
    // 显式跳过虽然多几行，但语义一眼可见。
    const SkipNode* x = zsl_.LowerBoundByScore(min_score);
    if (min_exclusive) {
        while (x != nullptr && x->score <= min_score) x = x->forward[0];
    }

    for (; x != nullptr; x = x->forward[0]) {
        if (x->score > max_score) break;
        if (max_exclusive && x->score == max_score) break;
        out->push_back(x);
    }
}

void ZSet::ForEach(const std::function<void(const std::string&, ZScore)>& fn) const {
    for (const SkipNode* x = zsl_.first(); x != nullptr; x = x->forward[0]) {
        fn(x->member, x->score);
    }
}

size_t ZSet::MemoryUsage() const {
    size_t total = sizeof(ZSet);
    for (const auto& kv : index_) {
        total += kv.first.capacity() + sizeof(ZScore) + 2 * sizeof(void*);
    }
    total += zsl_.MemoryUsage();
    return total;
}

// ================================================================ score 格式化
std::string ZScoreToString(ZScore score) {
    char buf[64];
    if (std::isfinite(score) && score == static_cast<ZScore>(static_cast<long long>(score))) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(score));
    } else {
        // 17 位有效数字是 double 往返所需的最小值。
        std::snprintf(buf, sizeof(buf), "%.17g", score);
    }
    return std::string(buf);
}

bool ParseZScore(const char* data, size_t len, ZScore* out) {
    if (data == nullptr || len == 0 || len >= 64) return false;

    // 不接受 inf / nan，与 Redis 的 ZADD 行为一致。
    // 先小写化再检查，避免大小写混写绕过。
    char lower[64];
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    lower[len] = '\0';
    if (std::strstr(lower, "inf") != nullptr || std::strstr(lower, "nan") != nullptr) {
        return false;
    }

    char buf[64];
    std::memcpy(buf, data, len);
    buf[len] = '\0';

    char* end = nullptr;
    const double v = std::strtod(buf, &end);
    // 必须消费完全部字符，否则 "1.5abc" 会被误判为合法。
    if (end == nullptr || *end != '\0' || end == buf) return false;
    if (!std::isfinite(v)) return false;

    if (out != nullptr) *out = v;
    return true;
}

}  // namespace rl
