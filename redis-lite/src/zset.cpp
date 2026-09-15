// Copyright (c) 2025 redis-lite authors. MIT License.
//
// zset.cpp — 有序集合实现
//
// 双索引的一致性是本文件的核心关注点：
//   任何写操作都必须同时更新 index_ 与 sorted_，且两者的 score 必须一致。
//   一旦不一致，就会出现「ZSCORE 查得到但 ZRANGE 看不到」这类幽灵 bug。
//   因此所有写路径都集中在本文件，且每个写操作都显式处理两个索引。

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
        // 新增：两个索引都要插。
        index_.emplace(member, score);
        sorted_.insert(ZSetNode{score, member});
        return true;
    }

    // 已存在：score 变了才需要调整 sorted_。
    // score 未变时什么都不做 —— 这个短路很关键，否则高频重复 ZADD
    // 会反复做「删 + 插」的有序集操作，白白付出 O(log N)。
    if (it->second == score) {
        return false;
    }

    // ★ 必须先删旧再插新，不能原地改：ZSetNode::score 是 std::set 的排序键，
    //   改它会破坏红黑树的有序性（与 ZSET 的「member 唯一」约束无关，
    //   纯粹是容器层面的要求）。std::set 的元素是 const 的，编译器也会拦住。
    sorted_.erase(ZSetNode{it->second, member});
    it->second = score;
    sorted_.insert(ZSetNode{score, member});
    return false;
}

bool ZSet::Remove(const std::string& member) {
    auto it = index_.find(member);
    if (it == index_.end()) return false;

    sorted_.erase(ZSetNode{it->second, member});
    index_.erase(it);
    return true;
}

ZScore ZSet::IncrBy(const std::string& member, ZScore delta, bool* was_present) {
    auto it = index_.find(member);

    if (it == index_.end()) {
        if (was_present != nullptr) *was_present = false;
        index_.emplace(member, delta);
        sorted_.insert(ZSetNode{delta, member});
        return delta;
    }

    if (was_present != nullptr) *was_present = true;
    const ZScore new_score = it->second + delta;
    if (new_score == it->second) return new_score;  // delta 为 0

    sorted_.erase(ZSetNode{it->second, member});
    it->second = new_score;
    sorted_.insert(ZSetNode{new_score, member});
    return new_score;
}

void ZSet::Clear() {
    index_.clear();
    sorted_.clear();
}

// ================================================================ 读路径
bool ZSet::GetScore(const std::string& member, ZScore* score) const {
    auto it = index_.find(member);
    if (it == index_.end()) return false;
    if (score != nullptr) *score = it->second;
    return true;
}

bool ZSet::Rank(const std::string& member, size_t* rank) const {
    auto it = index_.find(member);
    if (it == index_.end()) return false;

    // ⚠️ O(rank)：std::set 节点不含排名信息，只能从头数。
    //    这是本项目已知的性能缺口，详见 zset.h 文件头。
    auto sit = sorted_.find(ZSetNode{it->second, member});
    if (sit == sorted_.end()) {
        // 双索引不一致 —— 这是不该发生的状态。明确报错而不是静默返回错误排名。
        RL_ERROR("ZSet::Rank: index/sorted 不一致，member=%s score=%g", member.c_str(),
                 it->second);
        return false;
    }

    if (rank != nullptr) {
        *rank = static_cast<size_t>(std::distance(sorted_.begin(), sit));
    }
    return true;
}

void ZSet::RangeByRank(size_t start, size_t stop, std::vector<const ZSetNode*>* out) const {
    if (out == nullptr || start > stop || start >= sorted_.size()) return;

    auto it = sorted_.begin();
    std::advance(it, static_cast<long>(start));

    for (size_t i = start; i <= stop && it != sorted_.end(); ++i, ++it) {
        out->push_back(&(*it));
    }
}

void ZSet::RangeByScore(ZScore min_score, ZScore max_score, bool min_exclusive,
                        bool max_exclusive, std::vector<const ZSetNode*>* out) const {
    if (out == nullptr) return;

    // 用 std::set 的有序性做 O(log N) 定位，再线性收集 O(M)。
    //
    // 起点由「是否排除 min 端点」决定：
    //   包含 min → lower_bound({min, ""})，取第一个 score >= min 的元素
    //   排除 min → 从该位置起跳过所有 score == min 的元素
    //
    // 注：这里刻意不用 upper_bound({min, +inf}) 那种技巧 ——
    // member 是 std::string，构造不出「字典序最大的字符串」，
    // 强行用哨兵值会让代码依赖「空字符串是最小 member」这种隐式假设。
    // 显式跳过虽然多几行，但语义一眼可见。
    auto it = sorted_.lower_bound(ZSetNode{min_score, std::string()});
    if (min_exclusive) {
        while (it != sorted_.end() && it->score <= min_score) ++it;
    }

    for (; it != sorted_.end(); ++it) {
        if (it->score > max_score) break;
        if (max_exclusive && it->score == max_score) break;
        out->push_back(&(*it));
    }
}

void ZSet::ForEach(const std::function<void(const std::string&, ZScore)>& fn) const {
    for (const ZSetNode& node : sorted_) {
        fn(node.member, node.score);
    }
}

size_t ZSet::MemoryUsage() const {
    size_t total = sizeof(ZSet);
    for (const auto& kv : index_) {
        // 桶开销 + key 字符串 + score
        total += kv.first.capacity() + sizeof(ZScore) + 2 * sizeof(void*);
    }
    for (const ZSetNode& node : sorted_) {
        // 红黑树节点：颜色 + 三个指针 + 载荷
        total += node.member.capacity() + sizeof(ZScore) + 4 * sizeof(void*);
    }
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
    // 先做一次小写化检查，避免大小写混写绕过。
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
