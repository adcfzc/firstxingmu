// Copyright (c) 2025 redis-lite authors. MIT License.
//
// test_skiplist.cc — 跳表单测
//
// ============================ 测试策略（本文件的核心） ============================
//
// 跳表的 span bug 有个恶劣特性：**不崩溃、不报错，只让 rank 慢慢漂移**。
// 手工挑几个用例根本抓不住（本项目为此连续调试 11 轮未收敛）。
//
// 所以这里用两种互补的手法：
//
//   ① 穷举层数组合
//      对 N 个元素，枚举「每个元素层数」的所有组合（层数取值 1..maxLevel）。
//      N=6、maxLevel=3 时是 3^6 = 729 组；N=7 时 2187 组。
//      每组都插入完毕并逐步校验。任何对「层数分布」敏感的 bug 都无处可藏。
//
//      —— 这是上一轮失败后总结出的方法：不再"改一版跑一次"，
//         而是让机器穷举，自己指出哪一组开始出错。
//
//   ② 与参考实现随机交叉验证
//     维护一个按 (score, member) 排序的 vector 作为标准答案，
//     随机混合 Insert/Delete 后全量比对 size / 顺序 / 每个元素的 rank。
//
// 配套：用自实现的 LCG 而非 std::rand，保证失败可复现（跨平台一致）。

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "rl/skip_list.h"
#include "test_util.h"

using namespace rl;

namespace {

// ---------------------------------------------------------------- 层数注入
// 用预设的层数序列驱动跳表，使层数分布完全可控、可复现。
struct LevelSeq {
    std::vector<int> levels;
    size_t idx = 0;
};

unsigned int GenLevel(void* ctx, int max_level) {
    auto* s = static_cast<LevelSeq*>(ctx);
    if (s->levels.empty()) return 1;
    int lv = s->levels[s->idx % s->levels.size()];
    ++s->idx;
    if (lv < 1) lv = 1;
    if (lv > max_level) lv = max_level;
    return static_cast<unsigned int>(lv);
}

// ---------------------------------------------------------------- 参考实现
struct Ref {
    std::vector<std::pair<SkipScore, std::string>> items;

    void Insert(SkipScore s, const std::string& m) {
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

    bool Delete(SkipScore s, const std::string& m) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].second == m && items[i].first == s) {
                items.erase(items.begin() + static_cast<long>(i));
                return true;
            }
        }
        return false;
    }

    void Sort() {
        std::sort(items.begin(), items.end(),
                  [](const std::pair<SkipScore, std::string>& a,
                     const std::pair<SkipScore, std::string>& b) {
                      if (a.first != b.first) return a.first < b.first;
                      return a.second < b.second;
                  });
    }

    bool Rank(const std::string& m, unsigned long* out) const {
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].second == m) {
                *out = static_cast<unsigned long>(i);
                return true;
            }
        }
        return false;
    }

    bool ScoreOf(const std::string& m, SkipScore* out) const {
        for (const auto& kv : items) {
            if (kv.second == m) {
                *out = kv.first;
                return true;
            }
        }
        return false;
    }
};

// 全量比对：size、顺序、每个元素的 rank、以及 ZRANGE 各区间。
// 返回错误描述，空串表示一致。
std::string CompareWithRef(const SkipList& sl, const Ref& ref, const char* where) {
    if (sl.size() != ref.items.size()) {
        return std::string(where) + ": size " + std::to_string(sl.size()) + " vs " +
               std::to_string(ref.items.size());
    }

    // 顺序
    std::string got, want;
    for (const SkipNode* n = sl.first(); n != nullptr; n = n->forward[0]) {
        got += n->member + ",";
    }
    for (const auto& kv : ref.items) want += kv.second + ",";
    if (got != want) {
        return std::string(where) + ": order mismatch\n  got " + got + "\n  exp " + want;
    }

    // 每个元素的 rank
    for (const auto& kv : ref.items) {
        unsigned long expect = 0;
        if (!ref.Rank(kv.second, &expect)) continue;

        unsigned long got_rank = 0;
        if (!sl.Rank(kv.first, kv.second, &got_rank)) {
            return std::string(where) + ": Rank(" + kv.second + ") not found";
        }
        if (got_rank != expect) {
            return std::string(where) + ": Rank(" + kv.second + ") = " +
                   std::to_string(got_rank) + ", expected " + std::to_string(expect);
        }
    }

    // ZRANGE 若干区间（含负数由调用方归一化，这里测 1-based 闭区间）
    const unsigned long n = static_cast<unsigned long>(ref.items.size());
    const unsigned long probes[][2] = {
        {1, n}, {1, 1}, {n, n}, {1, n / 2 + 1}, {n / 2 + 1, n},
    };
    for (const auto& pr : probes) {
        if (n == 0 || pr[0] > n) continue;
        unsigned long lo = pr[0], hi = std::min(pr[1], n);
        if (lo > hi) continue;

        std::vector<const SkipNode*> nodes;
        sl.RangeByRank(lo, hi, &nodes);

        std::string g, w;
        for (const SkipNode* x : nodes) g += x->member + ",";
        for (unsigned long i = lo; i <= hi; ++i) w += ref.items[i - 1].second + ",";
        if (g != w) {
            return std::string(where) + ": Range(" + std::to_string(lo) + "," +
                   std::to_string(hi) + ") mismatch\n  got " + g + "\n  exp " + w;
        }
    }

    return std::string();
}

// 每次结构变动后做一次不变式自检 + 与参考比对。
// 返回错误描述，空串表示通过。
std::string Check(const SkipList& sl, const Ref& ref, const char* where) {
    std::string err;
    if (!sl.Validate(&err)) return std::string(where) + ": Validate failed: " + err;
    return CompareWithRef(sl, ref, where);
}

}  // namespace

// ================================================================ 基础
RL_TEST(skiplist_empty) {
    SkipList sl;
    RL_CHECK_EQ(sl.size(), 0UL);
    RL_CHECK_EQ(sl.level(), 1);
    RL_CHECK(sl.first() == nullptr);

    unsigned long r = 0;
    RL_CHECK_MSG(!sl.Rank(1.0, "nope", &r), "rank of missing key must be false");
    RL_CHECK(!sl.Find(1.0, "nope"));

    std::string err;
    RL_CHECK_MSG(sl.Validate(&err), err);
}

RL_TEST(skiplist_single_insert_rank) {
    SkipList sl;
    sl.Insert(1.0, "a");
    RL_CHECK_EQ(sl.size(), 1UL);

    unsigned long r = 999;
    RL_CHECK(sl.Rank(1.0, "a", &r));
    RL_CHECK_EQ(r, 0UL);  // 0-based：唯壹元素排名 0

    std::string err;
    RL_CHECK_MSG(sl.Validate(&err), err);
}

RL_TEST(skiplist_delete_all_and_reuse) {
    SkipList sl;
    for (int i = 0; i < 200; ++i) sl.Insert(i, "m" + std::to_string(i));
    for (int i = 0; i < 200; ++i) {
        RL_CHECK(sl.Delete(i, "m" + std::to_string(i)));
    }
    RL_CHECK_EQ(sl.size(), 0UL);
    RL_CHECK_EQ(sl.level(), 1);
    RL_CHECK(sl.first() == nullptr);

    std::string err;
    RL_CHECK_MSG(sl.Validate(&err), err);

    // 清空后仍可正常使用
    sl.Insert(5.0, "x");
    unsigned long r = 0;
    RL_CHECK(sl.Rank(5.0, "x", &r) && r == 0);
}

// ================================================================ ① 穷举层数组合
//
// 本文件最有价值的测试。枚举「N 个元素各自层数」的全部组合，
// 每组逐步插入并校验。对层数分布敏感的 span bug 必然在此暴露。
//
// 上一轮失败的教训：靠人工构造用例 + 手工推演，11 轮都没收敛；
// 换成穷举后，问题第一次运行就会指出「第几个元素、哪个组合」出错。
static void ExhaustiveInsert(int n, int max_level) {
    uint64_t total = 1;
    for (int i = 0; i < n; ++i) total *= static_cast<uint64_t>(max_level);

    for (uint64_t code = 0; code < total; ++code) {
        // 解码出每个元素的层数（1..max_level）
        std::vector<int> levels(static_cast<size_t>(n), 1);
        uint64_t c = code;
        for (int i = 0; i < n; ++i) {
            levels[static_cast<size_t>(i)] = 1 + static_cast<int>(c % static_cast<uint64_t>(max_level));
            c /= static_cast<uint64_t>(max_level);
        }

        LevelSeq seq;
        seq.levels = levels;

        SkipList sl;
        sl.SetLevelGeneratorForTest(&GenLevel, &seq);
        Ref ref;

        for (int i = 0; i < n; ++i) {
            const SkipScore score = static_cast<SkipScore>(i * 3 % 7);  // 制造同分
            const std::string member = "m" + std::to_string(i);

            // 注意：不要在这里重置 seq.idx。
            // 生成器在每次 Insert 中被调用恰好一次（新节点取一次层数），
            // 因此让它自然推进，第 i 个元素才会拿到 levels[i]。
            // 重置会让所有元素都取 levels[0]，穷举就退化成了单一分布。
            sl.Insert(score, member);
            ref.Insert(score, member);

            const std::string err =
                Check(sl, ref, ("exhaustive-insert n=" + std::to_string(n) +
                                " maxLevel=" + std::to_string(max_level) +
                                " code=" + std::to_string(code) + " step=" + std::to_string(i))
                                   .c_str());
            if (!err.empty()) {
                // 打印层数分布，便于定位
                std::string lv;
                for (int v : levels) lv += std::to_string(v) + " ";
                RL_CHECK_MSG(false, err + "\n       levels = " + lv);
                return;  // 一个组合失败就返回，避免刷屏
            }
        }
    }
}

RL_TEST(skiplist_exhaustive_levels_n4) {
    ExhaustiveInsert(4, 3);  // 3^4 = 81 组
}

RL_TEST(skiplist_exhaustive_levels_n6) {
    ExhaustiveInsert(6, 3);  // 3^6 = 729 组
}

RL_TEST(skiplist_exhaustive_levels_n7_maxlevel3) {
    ExhaustiveInsert(7, 3);  // 3^7 = 2187 组
}

RL_TEST(skiplist_exhaustive_levels_n5_maxlevel5) {
    ExhaustiveInsert(5, 5);  // 5^5 = 3125 组，覆盖更宽的层数差异
}

// ================================================================ ② 随机交叉验证
RL_TEST(skiplist_random_ops_vs_reference) {
    SkipList sl;
    Ref ref;

    // 自实现 LCG：std::rand 跨平台实现不同，会让失败无法复现
    uint64_t seed = 20250916;
    auto next = [&seed]() -> uint64_t {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return seed >> 33;
    };

    constexpr int kMembers = 120;
    constexpr int kOps = 4000;

    for (int op = 0; op < kOps; ++op) {
        const int m = static_cast<int>(next() % kMembers);
        const std::string member = "m" + std::to_string(m);
        const SkipScore score = static_cast<SkipScore>(next() % 60) / 2.0;
        const int action = static_cast<int>(next() % 3);

        if (action == 0) {
            sl.Insert(score, member);
            ref.Insert(score, member);
        } else if (action == 1) {
            SkipScore old = 0;
            const bool existed = ref.ScoreOf(member, &old);
            const bool a = sl.Delete(existed ? old : score, member);
            const bool b = ref.Delete(existed ? old : score, member);
            RL_CHECK_MSG(a == b, "Delete mismatch at op " + std::to_string(op));
        } else {
            // 更新分数：先删后插（跳表 Insert 内部也这么实现）
            SkipScore old = 0;
            const bool existed = ref.ScoreOf(member, &old);
            if (existed) {
                sl.Delete(old, member);
            }
            sl.Insert(score, member);
            ref.Insert(score, member);
        }

        if (op % 50 == 0 || op == kOps - 1) {
            const std::string err = Check(sl, ref, ("random op=" + std::to_string(op)).c_str());
            if (!err.empty()) {
                RL_CHECK_MSG(false, err);
                return;
            }
        }
    }
}

// 删除密集场景：大量删除后 rank 必须全部正确
RL_TEST(skiplist_random_delete_heavy) {
    SkipList sl;
    Ref ref;
    for (int i = 0; i < 300; ++i) {
        sl.Insert(i, "m" + std::to_string(i));
        ref.Insert(i, "m" + std::to_string(i));
    }

    uint64_t seed = 777;
    auto next = [&seed]() -> uint64_t {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return seed >> 33;
    };

    for (int step = 0; step < 250; ++step) {
        const int m = static_cast<int>(next() % 300);
        const std::string member = "m" + std::to_string(m);
        SkipScore old = 0;
        const bool existed = ref.ScoreOf(member, &old);
        const bool a = sl.Delete(existed ? old : 0.0, member);
        const bool b = ref.Delete(existed ? old : 0.0, member);
        RL_CHECK_MSG(a == b, "delete-heavy mismatch at step " + std::to_string(step));

        const std::string err =
            Check(sl, ref, ("delete-heavy step=" + std::to_string(step)).c_str());
        if (!err.empty()) {
            RL_CHECK_MSG(false, err);
            return;
        }
    }
}

// ================================================================ ③ 排名正确性
RL_TEST(skiplist_rank_zero_based_dense) {
    SkipList sl;
    for (int i = 0; i < 500; ++i) sl.Insert(i * 10, "m" + std::to_string(i));

    for (int i = 0; i < 500; ++i) {
        unsigned long r = 999;
        RL_CHECK(sl.Rank(i * 10, "m" + std::to_string(i), &r));
        RL_CHECK_EQ(r, static_cast<unsigned long>(i));
    }
}

RL_TEST(skiplist_rank_same_score_ordered_by_member) {
    SkipList sl;
    sl.Insert(1.0, "ccc");
    sl.Insert(1.0, "aaa");
    sl.Insert(1.0, "bbb");

    unsigned long r = 0;
    RL_CHECK(sl.Rank(1.0, "aaa", &r) && r == 0);
    RL_CHECK(sl.Rank(1.0, "bbb", &r) && r == 1);
    RL_CHECK(sl.Rank(1.0, "ccc", &r) && r == 2);

    std::string err;
    RL_CHECK_MSG(sl.Validate(&err), err);
}

RL_TEST(skiplist_rank_missing_returns_false) {
    SkipList sl;
    sl.Insert(1.0, "a");
    unsigned long r = 0;
    RL_CHECK(!sl.Rank(2.0, "a", &r));   // score 不符
    RL_CHECK(!sl.Rank(1.0, "b", &r));   // member 不符
    RL_CHECK(!sl.Rank(9.0, "zzz", &r));
}

// ================================================================ ④ 范围查询
RL_TEST(skiplist_range_by_rank) {
    SkipList sl;
    for (int i = 0; i < 10; ++i) sl.Insert(i, "m" + std::to_string(i));

    auto collect = [&](unsigned long lo, unsigned long hi) {
        std::vector<const SkipNode*> v;
        sl.RangeByRank(lo, hi, &v);
        std::string s;
        for (const SkipNode* n : v) s += n->member + ",";
        return s;
    };

    // 1-based 闭区间
    RL_CHECK_STR(collect(1, 3), std::string("m0,m1,m2,"));
    RL_CHECK_STR(collect(1, 1), std::string("m0,"));
    RL_CHECK_STR(collect(5, 5), std::string("m4,"));
    RL_CHECK_STR(collect(10, 10), std::string("m9,"));
    RL_CHECK_STR(collect(1, 10),
                 std::string("m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,"));
    RL_CHECK_STR(collect(8, 100), std::string("m7,m8,m9,"));  // stop 越界截断
    RL_CHECK_STR(collect(3, 2), std::string(""));             // start > stop
    RL_CHECK_STR(collect(100, 200), std::string(""));         // start 越界
}

RL_TEST(skiplist_lower_bound_by_score) {
    SkipList sl;
    for (int i = 0; i < 10; ++i) sl.Insert(i * 2, "m" + std::to_string(i));

    RL_CHECK_STR(sl.LowerBoundByScore(0)->member, std::string("m0"));
    RL_CHECK_STR(sl.LowerBoundByScore(1)->member, std::string("m1"));   // 1 不存在，取 >= 的
    RL_CHECK_STR(sl.LowerBoundByScore(4)->member, std::string("m2"));
    RL_CHECK_STR(sl.LowerBoundByScore(18)->member, std::string("m9"));
    RL_CHECK(sl.LowerBoundByScore(19) == nullptr);
    RL_CHECK(sl.LowerBoundByScore(1000) == nullptr);
}

// ================================================================ ⑤ 规模
RL_TEST(skiplist_large_scale) {
    SkipList sl;
    constexpr int kN = 20000;
    for (int i = 0; i < kN; ++i) sl.Insert(i, "member:" + std::to_string(i));

    RL_CHECK_EQ(sl.size(), static_cast<size_t>(kN));
    // p=0.25，N=20000 的期望层数约 log4(20000) ≈ 7
    RL_CHECK_MSG(sl.level() >= 5 && sl.level() <= 20,
                 "unexpected level: " + std::to_string(sl.level()));

    unsigned long r = 0;
    RL_CHECK(sl.Rank(0, "member:0", &r) && r == 0);
    RL_CHECK(sl.Rank(19999, "member:19999", &r) && r == static_cast<unsigned long>(kN - 1));
    RL_CHECK(sl.Rank(12345, "member:12345", &r) && r == 12345UL);

    RL_CHECK_MSG(sl.MemoryUsage() > 0, "memory usage should be measurable");

    std::string err;
    RL_CHECK_MSG(sl.Validate(&err), err);
}

// ================================================================ ⑥ score 转换
RL_TEST(skiplist_score_roundtrip) {
    const SkipScore values[] = {0.0, 1.0, -1.0, 1.5, -0.25, 3.14159265358979,
                                1e15, -1e15, 0.1, 1.0 / 3.0};
    for (SkipScore v : values) {
        const std::string text = SkipScoreToString(v);
        SkipScore back = 0;
        RL_CHECK_MSG(ParseSkipScore(text.data(), text.size(), &back),
                     "failed to parse back: " + text);
        RL_CHECK_MSG(back == v, "roundtrip mismatch: " + text);
    }
}

RL_TEST(skiplist_parse_score_rejects_garbage) {
    SkipScore s = 0;
    RL_CHECK(!ParseSkipScore("", 0, &s));
    RL_CHECK(!ParseSkipScore("abc", 3, &s));
    RL_CHECK(!ParseSkipScore("1.5abc", 6, &s));
    RL_CHECK(!ParseSkipScore("inf", 3, &s));
    RL_CHECK(!ParseSkipScore("nan", 3, &s));
}
