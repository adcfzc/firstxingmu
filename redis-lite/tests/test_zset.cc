// Copyright (c) 2025 redis-lite authors. MIT License.
//
// test_zset.cc — 有序集合单元测试
//
// 测试重点：**双索引一致性**。
// ZSet 内部维护 index_(member→score) 与 sorted_(有序) 两份结构，
// 任何写操作都必须让两者同步。一旦不一致，就会出现
// 「ZSCORE 查得到、ZRANGE 看不到」这类幽灵 bug ——
// 所以这里的核心手法是：每次批量操作后，用「暴力重建参考答案」
// 与两份索引同时比对。

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "rl/zset.h"
#include "test_util.h"

using namespace rl;

namespace {

// 参考实现：把 (score, member) 排序后当标准答案。
struct Ref {
    std::vector<std::pair<ZScore, std::string>> items;

    void Add(const std::string& m, ZScore s) {
        for (auto& kv : items) {
            if (kv.second == m) {
                kv.first = s;
                Sort();
                return;
            }
        }
        items.emplace_back(s, m);
        Sort();
    }

    bool Remove(const std::string& m) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].second == m) {
                items.erase(items.begin() + static_cast<long>(i));
                return true;
            }
        }
        return false;
    }

    void Sort() {
        std::sort(items.begin(), items.end(),
                  [](const std::pair<ZScore, std::string>& a,
                     const std::pair<ZScore, std::string>& b) {
                      if (a.first != b.first) return a.first < b.first;
                      return a.second < b.second;
                  });
    }

    bool Rank(const std::string& m, size_t* out) const {
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].second == m) {
                *out = i;
                return true;
            }
        }
        return false;
    }
};

std::string JoinMembers(const std::vector<const ZSetNode*>& v) {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) r += ",";
        r += v[i]->member;
    }
    return r;
}

}  // namespace

// ---------------------------------------------------------------- 基本增删查
RL_TEST(zset_empty) {
    ZSet z;
    RL_CHECK_EQ(z.Size(), 0UL);
    RL_CHECK(z.First() == nullptr);
    RL_CHECK(z.Last() == nullptr);

    ZScore s = 0;
    RL_CHECK(!z.GetScore("nope", &s));
    size_t r = 0;
    RL_CHECK(!z.Rank("nope", &r));
}

RL_TEST(zset_add_and_get) {
    ZSet z;
    RL_CHECK(z.Add("a", 1.5));   // 新增 → true
    RL_CHECK_EQ(z.Size(), 1UL);

    ZScore s = 0;
    RL_CHECK(z.GetScore("a", &s));
    RL_CHECK(s == 1.5);
}

RL_TEST(zset_add_existing_updates_score) {
    ZSet z;
    z.Add("a", 1.0);
    RL_CHECK_MSG(!z.Add("a", 2.0), "ZADD on existing member must report 0 added");
    RL_CHECK_EQ(z.Size(), 1UL);

    ZScore s = 0;
    RL_CHECK(z.GetScore("a", &s));
    RL_CHECK(s == 2.0);
}

RL_TEST(zset_add_same_score_is_noop) {
    ZSet z;
    z.Add("a", 1.0);
    RL_CHECK(!z.Add("a", 1.0));
    RL_CHECK_EQ(z.Size(), 1UL);
    ZScore s = 0;
    RL_CHECK(z.GetScore("a", &s) && s == 1.0);
}

RL_TEST(zset_remove) {
    ZSet z;
    z.Add("a", 1.0);
    z.Add("b", 2.0);

    RL_CHECK(z.Remove("a"));
    RL_CHECK(!z.Remove("a"));  // 第二次返回 false
    RL_CHECK(!z.Remove("zzz"));
    RL_CHECK_EQ(z.Size(), 1UL);
}

// ---------------------------------------------------------------- 有序性
RL_TEST(zset_sorted_by_score) {
    ZSet z;
    z.Add("a", 3.0);
    z.Add("b", 1.0);
    z.Add("c", 2.0);

    std::vector<const ZSetNode*> v;
    z.RangeByRank(0, 2, &v);
    RL_CHECK_STR(JoinMembers(v), std::string("b,c,a"));
}

RL_TEST(zset_same_score_sorted_by_member) {
    // score 相同时按 member 字典序 —— ZSET 全序的关键
    ZSet z;
    z.Add("ccc", 1.0);
    z.Add("aaa", 1.0);
    z.Add("bbb", 1.0);

    std::vector<const ZSetNode*> v;
    z.RangeByRank(0, 2, &v);
    RL_CHECK_STR(JoinMembers(v), std::string("aaa,bbb,ccc"));
}

RL_TEST(zset_first_last) {
    ZSet z;
    z.Add("m", 5.0);
    z.Add("n", 1.0);
    z.Add("o", 9.0);

    RL_CHECK_STR(z.First()->member, std::string("n"));
    RL_CHECK_STR(z.Last()->member, std::string("o"));
}

// ---------------------------------------------------------------- ZRANK
RL_TEST(zset_rank_zero_based) {
    ZSet z;
    for (int i = 0; i < 10; ++i) {
        z.Add("m" + std::to_string(i), static_cast<ZScore>(i * 10));
    }
    // ZRANK 是 0-based，与 Redis 一致
    for (int i = 0; i < 10; ++i) {
        size_t r = 999;
        RL_CHECK(z.Rank("m" + std::to_string(i), &r));
        RL_CHECK_EQ(r, static_cast<size_t>(i));
    }
}

RL_TEST(zset_rank_after_score_update) {
    // 改分后排名必须跟着变 —— 这是双索引同步的关键考验
    ZSet z;
    for (int i = 0; i < 5; ++i) z.Add("m" + std::to_string(i), static_cast<ZScore>(i));

    z.Add("m0", 100.0);  // 最小的跑到最大
    size_t r = 0;
    RL_CHECK(z.Rank("m0", &r));
    RL_CHECK_EQ(r, 4UL);
    RL_CHECK(z.Rank("m1", &r));
    RL_CHECK_EQ(r, 0UL);
}

RL_TEST(zset_rank_after_removal) {
    ZSet z;
    for (int i = 0; i < 10; ++i) z.Add("m" + std::to_string(i), static_cast<ZScore>(i));

    for (int i = 0; i < 10; i += 2) z.Remove("m" + std::to_string(i));
    RL_CHECK_EQ(z.Size(), 5UL);

    for (int i = 1; i < 10; i += 2) {
        size_t r = 0;
        RL_CHECK(z.Rank("m" + std::to_string(i), &r));
        RL_CHECK_EQ(r, static_cast<size_t>((i - 1) / 2));
    }
}

// ---------------------------------------------------------------- 范围查询
RL_TEST(zset_range_by_rank) {
    ZSet z;
    for (int i = 0; i < 10; ++i) z.Add("m" + std::to_string(i), static_cast<ZScore>(i));

    std::vector<const ZSetNode*> v;
    z.RangeByRank(0, 2, &v);
    RL_CHECK_STR(JoinMembers(v), std::string("m0,m1,m2"));

    v.clear();
    z.RangeByRank(7, 100, &v);  // stop 越界自动截断
    RL_CHECK_STR(JoinMembers(v), std::string("m7,m8,m9"));

    v.clear();
    z.RangeByRank(5, 5, &v);
    RL_CHECK_STR(JoinMembers(v), std::string("m5"));

    v.clear();
    z.RangeByRank(3, 2, &v);  // start > stop
    RL_CHECK_EQ(v.size(), 0UL);

    v.clear();
    z.RangeByRank(100, 200, &v);  // start 越界
    RL_CHECK_EQ(v.size(), 0UL);
}

RL_TEST(zset_range_by_score) {
    ZSet z;
    for (int i = 0; i < 10; ++i) z.Add("m" + std::to_string(i), static_cast<ZScore>(i));

    std::vector<const ZSetNode*> v;
    z.RangeByScore(3, 5, false, false, &v);
    RL_CHECK_STR(JoinMembers(v), std::string("m3,m4,m5"));

    // 排除两端
    v.clear();
    z.RangeByScore(3, 5, true, true, &v);
    RL_CHECK_STR(JoinMembers(v), std::string("m4"));

    // 无穷端点
    v.clear();
    z.RangeByScore(-std::numeric_limits<ZScore>::infinity(),
                   std::numeric_limits<ZScore>::infinity(), false, false, &v);
    RL_CHECK_EQ(v.size(), 10UL);

    // 空区间
    v.clear();
    z.RangeByScore(100, 200, false, false, &v);
    RL_CHECK_EQ(v.size(), 0UL);
}

// ---------------------------------------------------------------- ZINCRBY
RL_TEST(zset_incrby_creates_missing) {
    ZSet z;
    bool present = true;
    const ZScore v = z.IncrBy("a", 5.0, &present);
    RL_CHECK(v == 5.0);
    RL_CHECK_MSG(!present, "member did not exist, was_present should be false");
    RL_CHECK_EQ(z.Size(), 1UL);
}

RL_TEST(zset_incrby_accumulates) {
    ZSet z;
    z.Add("a", 1.0);
    bool present = false;
    const ZScore v = z.IncrBy("a", 2.5, &present);
    RL_CHECK(v == 3.5);
    RL_CHECK(present);

    ZScore s = 0;
    RL_CHECK(z.GetScore("a", &s) && s == 3.5);
}

RL_TEST(zset_incrby_changes_order) {
    ZSet z;
    z.Add("a", 1.0);
    z.Add("b", 2.0);
    z.IncrBy("a", 10.0, nullptr);  // a 变成 11，应排到 b 后面

    std::vector<const ZSetNode*> v;
    z.RangeByRank(0, 1, &v);
    RL_CHECK_STR(JoinMembers(v), std::string("b,a"));
}

// ---------------------------------------------------------------- 双索引一致性
RL_TEST(zset_random_ops_consistency) {
    // 本文件最有价值的用例：随机混合操作后，
    // 用参考实现与两份索引同时交叉验证。
    ZSet z;
    Ref ref;

    uint64_t seed = 20250915;
    auto next_rand = [&seed]() -> uint64_t {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return seed >> 33;
    };

    constexpr int kMembers = 150;
    constexpr int kOps = 3000;

    for (int op = 0; op < kOps; ++op) {
        const int m = static_cast<int>(next_rand() % kMembers);
        const std::string member = "m" + std::to_string(m);
        const ZScore score = static_cast<ZScore>(next_rand() % 200) / 2.0;
        const int action = static_cast<int>(next_rand() % 3);

        if (action == 0) {
            z.Add(member, score);
            ref.Add(member, score);
        } else if (action == 1) {
            const bool a = z.Remove(member);
            const bool b = ref.Remove(member);
            RL_CHECK_MSG(a == b, "Remove mismatch at op " + std::to_string(op));
        } else {
            bool p1 = false, p2 = false;
            const bool existed = [&] {
                for (const auto& kv : ref.items) {
                    if (kv.second == member) return true;
                }
                return false;
            }();
            const ZScore v1 = z.IncrBy(member, score, &p1);
            if (existed) {
                ZScore old = 0;
                for (const auto& kv : ref.items) {
                    if (kv.second == member) old = kv.first;
                }
                ref.Add(member, old + score);
                p2 = true;
                (void)p2;
                ZScore expect = 0;
                for (const auto& kv : ref.items) {
                    if (kv.second == member) expect = kv.first;
                }
                RL_CHECK_MSG(v1 == expect,
                             "IncrBy value mismatch at op " + std::to_string(op));
            } else {
                ref.Add(member, score);
            }
            RL_CHECK_MSG(p1 == existed, "IncrBy was_present mismatch at op " +
                                            std::to_string(op));
        }

        if (op % 100 == 0 || op == kOps - 1) {
            // 1) size 一致
            RL_CHECK_MSG(z.Size() == ref.items.size(),
                         "size mismatch at op " + std::to_string(op) + ": got " +
                             std::to_string(z.Size()) + " exp " +
                             std::to_string(ref.items.size()));

            // 2) 全序一致（这一项同时验证 index_ 与 sorted_ 的 score 是否同步）
            std::vector<const ZSetNode*> all;
            z.RangeByRank(0, z.Size() == 0 ? 0 : z.Size() - 1, &all);
            std::string got;
            for (size_t i = 0; i < all.size(); ++i) {
                if (i) got += ",";
                got += all[i]->member;
            }
            std::string expect;
            for (size_t i = 0; i < ref.items.size(); ++i) {
                if (i) expect += ",";
                expect += ref.items[i].second;
            }
            RL_CHECK_MSG(got == expect, "order mismatch at op " + std::to_string(op));

            // 3) 每个元素的 rank 与 score 都一致
            int errors = 0;
            for (const auto& kv : ref.items) {
                size_t r = 0;
                ZScore s = 0;
                if (!z.Rank(kv.second, &r) || r != 0) {
                    // rank 单独比对
                }
                size_t expect_rank = 0;
                ref.Rank(kv.second, &expect_rank);
                if (!z.Rank(kv.second, &r) || r != expect_rank) ++errors;
                if (!z.GetScore(kv.second, &s) || s != kv.first) ++errors;
            }
            RL_CHECK_MSG(errors == 0, "rank/score errors at op " + std::to_string(op) +
                                          ": " + std::to_string(errors));
        }
    }
}

RL_TEST(zset_clear) {
    ZSet z;
    for (int i = 0; i < 50; ++i) z.Add("m" + std::to_string(i), static_cast<ZScore>(i));
    z.Clear();
    RL_CHECK_EQ(z.Size(), 0UL);
    RL_CHECK(z.First() == nullptr);

    // 清空后仍可正常使用
    z.Add("x", 1.0);
    RL_CHECK_EQ(z.Size(), 1UL);
    ZScore s = 0;
    RL_CHECK(z.GetScore("x", &s) && s == 1.0);
}

RL_TEST(zset_large_scale_rank) {
    ZSet z;
    constexpr int kN = 20000;
    for (int i = 0; i < kN; ++i) {
        z.Add("member:" + std::to_string(i), static_cast<ZScore>(i));
    }
    RL_CHECK_EQ(z.Size(), static_cast<size_t>(kN));

    size_t r = 0;
    RL_CHECK(z.Rank("member:0", &r) && r == 0UL);
    RL_CHECK(z.Rank("member:19999", &r) && r == static_cast<size_t>(kN - 1));
    RL_CHECK(z.Rank("member:12345", &r) && r == 12345UL);

    RL_CHECK_MSG(z.MemoryUsage() > 0, "memory usage should be measurable");
}

// ---------------------------------------------------------------- score 格式化
RL_TEST(zset_score_to_string_integer) {
    RL_CHECK_STR(ZScoreToString(0.0), std::string("0"));
    RL_CHECK_STR(ZScoreToString(1.0), std::string("1"));
    RL_CHECK_STR(ZScoreToString(-42.0), std::string("-42"));
    RL_CHECK_STR(ZScoreToString(1000000.0), std::string("1000000"));
}

RL_TEST(zset_score_to_string_fraction) {
    RL_CHECK_STR(ZScoreToString(1.5), std::string("1.5"));
    RL_CHECK_STR(ZScoreToString(-0.25), std::string("-0.25"));
}

RL_TEST(zset_parse_score_valid) {
    ZScore s = 0;
    RL_CHECK(ParseZScore("1.5", 3, &s) && s == 1.5);
    RL_CHECK(ParseZScore("0", 1, &s) && s == 0.0);
    RL_CHECK(ParseZScore("-3.25", 5, &s) && s == -3.25);
    RL_CHECK(ParseZScore("1e3", 3, &s) && s == 1000.0);
    RL_CHECK(ParseZScore("10", 2, &s) && s == 10.0);
}

RL_TEST(zset_parse_score_invalid) {
    ZScore s = 0;
    RL_CHECK(!ParseZScore("", 0, &s));
    RL_CHECK(!ParseZScore("abc", 3, &s));
    RL_CHECK(!ParseZScore("1.5abc", 6, &s));  // 尾部垃圾必须拒绝
    RL_CHECK(!ParseZScore("--1", 3, &s));
    // ZADD 不接受 inf / nan（与 ZRANGEBYSCORE 的端点不同）
    RL_CHECK(!ParseZScore("inf", 3, &s));
    RL_CHECK(!ParseZScore("-inf", 4, &s));
    RL_CHECK(!ParseZScore("+inf", 4, &s));
    RL_CHECK(!ParseZScore("nan", 3, &s));
    RL_CHECK(!ParseZScore("NaN", 3, &s));
    RL_CHECK(!ParseZScore("INF", 3, &s));
}

RL_TEST(zset_score_roundtrip) {
    // ZScoreToString → ParseZScore 必须无损，否则 AOF 恢复会改分数
    const ZScore values[] = {0.0,  1.0,  -1.0, 1.5,     -0.25, 3.14159265358979,
                             1e15, -1e15, 0.1, 1.0 / 3.0, 2.5,   -7.75};
    for (ZScore v : values) {
        const std::string text = ZScoreToString(v);
        ZScore back = 0;
        RL_CHECK_MSG(ParseZScore(text.data(), text.size(), &back),
                     "failed to parse back: " + text);
        RL_CHECK_MSG(back == v, "roundtrip mismatch: " + text);
    }
}
