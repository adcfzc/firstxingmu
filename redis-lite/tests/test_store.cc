// Copyright (c) 2025 redis-lite authors. MIT License.
//
// test_store.cc — 存储引擎单元测试
//
// 重点验证三处最容易出错的语义：
//   1) NX/XX 必须把「已过期」的 key 视为不存在
//   2) TTL 的 -1 / -2 返回值区分
//   3) INCR 的溢出与「字符串编码值也能自增」

#include <string>
#include <thread>
#include <vector>

#include "rl/store.h"
#include "test_util.h"

using namespace rl;

namespace {

// 每个用例独立一份 Store，避免相互污染。
// 分片数故意用 1 和 8 各测一遍：验证分片逻辑不改变语义。
Store* MakeStore(size_t shards = 8) {
    Store* s = new Store();
    s->Init(shards, nullptr);
    return s;
}

}  // namespace

// ---------------------------------------------------------------- GET / SET
RL_TEST(store_set_get) {
    Store* s = MakeStore();
    SetOptions opts;
    RL_CHECK(s->Set("k", Value(std::string("v")), opts) == SetResult::kOk);

    Value out;
    RL_CHECK(s->Get("k", &out));
    RL_CHECK_STR(out.AsString(), std::string("v"));
    // enum class 不会隐式转 int，比较前必须显式转换。
    RL_CHECK_EQ(static_cast<int>(out.type()),
                static_cast<int>(ValueType::kString));

    delete s;
}

RL_TEST(store_get_missing) {
    Store* s = MakeStore();
    Value out;
    RL_CHECK(!s->Get("nope", &out));
    delete s;
}

RL_TEST(store_overwrite) {
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("k", Value(std::string("v1")), opts);
    s->Set("k", Value(std::string("v2")), opts);

    Value out;
    RL_CHECK(s->Get("k", &out));
    RL_CHECK_STR(out.AsString(), std::string("v2"));
    RL_CHECK_EQ(s->Size(), size_t(1));  // 覆盖不应增加 key 数
    delete s;
}

RL_TEST(store_empty_value) {
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("k", Value(std::string("")), opts);

    Value out;
    RL_CHECK(s->Get("k", &out));
    RL_CHECK_EQ(out.AsString().size(), size_t(0));
    delete s;
}

RL_TEST(store_single_shard_consistency) {
    // 分片数为 1 时退化为单表，语义必须与多分片完全一致。
    Store* s = MakeStore(1);
    SetOptions opts;
    for (int i = 0; i < 100; ++i) {
        s->Set("key" + std::to_string(i), Value(std::to_string(i)), opts);
    }
    RL_CHECK_EQ(s->Size(), size_t(100));

    Value out;
    RL_CHECK(s->Get("key42", &out));
    RL_CHECK_STR(out.AsString(), std::string("42"));
    delete s;
}

// ---------------------------------------------------------------- NX / XX
RL_TEST(store_set_nx) {
    Store* s = MakeStore();
    SetOptions nx;
    nx.nx = true;

    RL_CHECK(s->Set("k", Value(std::string("first")), nx) == SetResult::kOk);
    // 第二次必须失败，且不能覆盖原值。
    RL_CHECK(s->Set("k", Value(std::string("second")), nx) == SetResult::kNotSetNx);

    Value out;
    RL_CHECK(s->Get("k", &out));
    RL_CHECK_STR(out.AsString(), std::string("first"));
    delete s;
}

RL_TEST(store_set_xx) {
    Store* s = MakeStore();
    SetOptions xx;
    xx.xx = true;

    // key 不存在时 XX 必须失败。
    RL_CHECK(s->Set("k", Value(std::string("v")), xx) == SetResult::kNotSetXx);
    RL_CHECK(!s->Exists("k"));

    SetOptions plain;
    s->Set("k", Value(std::string("v1")), plain);
    RL_CHECK(s->Set("k", Value(std::string("v2")), xx) == SetResult::kOk);

    Value out;
    RL_CHECK(s->Get("k", &out));
    RL_CHECK_STR(out.AsString(), std::string("v2"));
    delete s;
}

// ---------------------------------------------------------------- DEL / EXISTS
RL_TEST(store_del) {
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("k", Value(std::string("v")), opts);

    RL_CHECK(s->Del("k"));
    RL_CHECK(!s->Del("k"));  // 第二次返回 false
    RL_CHECK_EQ(s->Size(), size_t(0));
    delete s;
}

RL_TEST(store_del_multi_counts_only_existing) {
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("a", Value(std::string("1")), opts);
    s->Set("b", Value(std::string("2")), opts);

    std::vector<std::string> keys = {"a", "b", "c", "a"};
    // 只有 a、b 存在；重复的 a 第二次已经不存在了。
    RL_CHECK_EQ(s->DelMulti(keys), int64_t(2));
    delete s;
}

RL_TEST(store_exists) {
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("k", Value(std::string("v")), opts);
    RL_CHECK(s->Exists("k"));
    RL_CHECK(!s->Exists("missing"));
    delete s;
}

// ---------------------------------------------------------------- TTL
RL_TEST(store_ttl_no_expire_returns_minus_one) {
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("k", Value(std::string("v")), opts);
    RL_CHECK_EQ(s->TtlMs("k"), int64_t(-1));
    delete s;
}

RL_TEST(store_ttl_missing_returns_minus_two) {
    Store* s = MakeStore();
    RL_CHECK_EQ(s->TtlMs("missing"), int64_t(-2));
    delete s;
}

RL_TEST(store_set_with_expire) {
    Store* s = MakeStore();
    SetOptions opts;
    opts.has_expire = true;
    opts.expire_at_ms = NowMs() + 10000;  // 10 秒后过期
    s->Set("k", Value(std::string("v")), opts);

    int64_t ttl = s->TtlMs("k");
    RL_CHECK_MSG(ttl > 9000 && ttl <= 10000, "ttl should be about 10000ms");

    Value out;
    RL_CHECK(s->Get("k", &out));
    delete s;
}

RL_TEST(store_lazy_expire_on_get) {
    Store* s = MakeStore();
    SetOptions opts;
    opts.has_expire = true;
    opts.expire_at_ms = NowMs() + 20;  // 20ms 后过期
    s->Set("k", Value(std::string("v")), opts);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // 惰性过期：读路径上必须直接判定为不存在。
    Value out;
    RL_CHECK(!s->Get("k", &out));
    RL_CHECK_EQ(s->TtlMs("k"), int64_t(-2));
    RL_CHECK_EQ(s->Size(), size_t(0));
    delete s;
}

RL_TEST(store_nx_sees_expired_key_as_absent) {
    // 这是最容易写错的一处：NX 判断必须基于「逻辑存在性」而非「表里有没有」。
    Store* s = MakeStore();
    SetOptions opts;
    opts.has_expire = true;
    opts.expire_at_ms = NowMs() + 20;
    s->Set("k", Value(std::string("old")), opts);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    SetOptions nx;
    nx.nx = true;
    RL_CHECK_MSG(s->Set("k", Value(std::string("new")), nx) == SetResult::kOk,
                 "NX must succeed on an expired key");

    Value out;
    RL_CHECK(s->Get("k", &out));
    RL_CHECK_STR(out.AsString(), std::string("new"));
    delete s;
}

RL_TEST(store_expire_at_in_past_deletes_key) {
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("k", Value(std::string("v")), opts);

    ExpireResult r = s->ExpireAt("k", NowMs() - 1000);
    RL_CHECK(r.changed);
    RL_CHECK(r.removed);
    RL_CHECK(!s->Exists("k"));
    delete s;
}

RL_TEST(store_expire_missing_key) {
    Store* s = MakeStore();
    ExpireResult r = s->ExpireAt("missing", NowMs() + 1000);
    RL_CHECK(!r.changed);
    RL_CHECK(r.removed);
    delete s;
}

RL_TEST(store_set_clears_ttl_by_default) {
    // 与 Redis 一致：不带 EX/PX 的 SET 会清掉原有 TTL。
    Store* s = MakeStore();
    SetOptions with_ttl;
    with_ttl.has_expire = true;
    with_ttl.expire_at_ms = NowMs() + 10000;
    s->Set("k", Value(std::string("v1")), with_ttl);
    RL_CHECK(s->TtlMs("k") > 0);

    SetOptions plain;
    s->Set("k", Value(std::string("v2")), plain);
    RL_CHECK_EQ(s->TtlMs("k"), int64_t(-1));
    delete s;
}

RL_TEST(store_set_keepttl_preserves_ttl) {
    Store* s = MakeStore();
    SetOptions with_ttl;
    with_ttl.has_expire = true;
    with_ttl.expire_at_ms = NowMs() + 10000;
    s->Set("k", Value(std::string("v1")), with_ttl);

    SetOptions keep;
    keep.keep_ttl = true;
    s->Set("k", Value(std::string("v2")), keep);

    RL_CHECK_MSG(s->TtlMs("k") > 9000, "KEEPTTL must preserve the original TTL");
    delete s;
}

RL_TEST(store_expire_cycle_removes_expired) {
    Store* s = MakeStore();
    for (int i = 0; i < 50; ++i) {
        SetOptions opts;
        opts.has_expire = true;
        opts.expire_at_ms = NowMs() + 20;
        s->Set("k" + std::to_string(i), Value(std::string("v")), opts);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // 主动过期一轮不一定全清（每分片每轮只采样 20 个），所以循环几轮。
    for (int i = 0; i < 10 && s->Size() > 0; ++i) {
        s->ExpireCycle();
    }
    RL_CHECK_MSG(s->Size() == 0, "active expire should eventually remove all expired keys");
    delete s;
}

// ---------------------------------------------------------------- INCR
RL_TEST(store_incr_new_key_starts_at_zero) {
    Store* s = MakeStore();
    int64_t v = 0;
    RL_CHECK(s->IncrBy("counter", 1, &v));
    RL_CHECK_EQ(v, int64_t(1));
    delete s;
}

RL_TEST(store_incr_repeated) {
    Store* s = MakeStore();
    int64_t v = 0;
    for (int i = 1; i <= 100; ++i) {
        RL_CHECK(s->IncrBy("c", 1, &v));
        RL_CHECK_EQ(v, int64_t(i));
    }
    delete s;
}

RL_TEST(store_incr_string_encoded_value) {
    // 用普通 SET 写入 "41"，INCR 之后应该是 42 而不是报错。
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("c", Value(std::string("41")), opts);

    int64_t v = 0;
    RL_CHECK(s->IncrBy("c", 1, &v));
    RL_CHECK_EQ(v, int64_t(42));

    Value out;
    RL_CHECK(s->Get("c", &out));
    RL_CHECK_STR(out.AsString(), std::string("42"));
    delete s;
}

RL_TEST(store_incr_non_numeric_fails) {
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("c", Value(std::string("hello")), opts);

    int64_t v = 0;
    RL_CHECK(!s->IncrBy("c", 1, &v));
    // 失败时不能修改原值。
    Value out;
    RL_CHECK(s->Get("c", &out));
    RL_CHECK_STR(out.AsString(), std::string("hello"));
    delete s;
}

RL_TEST(store_incr_overflow_detected) {
    Store* s = MakeStore();
    int64_t v = 0;
    RL_CHECK(s->IncrBy("c", INT64_MAX, &v));
    RL_CHECK_EQ(v, INT64_MAX);

    // INT64_MAX + 1 必须被拒绝，而不是 UB 回绕成负数。
    RL_CHECK_MSG(!s->IncrBy("c", 1, &v), "overflow must be rejected");
    delete s;
}

RL_TEST(store_incr_preserves_ttl) {
    Store* s = MakeStore();
    SetOptions opts;
    opts.has_expire = true;
    opts.expire_at_ms = NowMs() + 10000;
    s->Set("c", Value(std::string("1")), opts);

    int64_t v = 0;
    s->IncrBy("c", 1, &v);
    RL_CHECK_MSG(s->TtlMs("c") > 9000, "INCR must not clear TTL");
    delete s;
}

// ---------------------------------------------------------------- FOR EACH
RL_TEST(store_for_each) {
    Store* s = MakeStore();
    SetOptions opts;
    for (int i = 0; i < 200; ++i) {
        s->Set("k" + std::to_string(i), Value(std::to_string(i)), opts);
    }

    int counted = 0;
    bool all_prefixed = true;
    s->ForEach([&](const std::string& k, const Value&, int64_t) {
        ++counted;
        if (k.compare(0, 1, "k") != 0) all_prefixed = false;
    });
    RL_CHECK_EQ(counted, 200);
    RL_CHECK(all_prefixed);
    delete s;
}

RL_TEST(store_for_each_skips_expired) {
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("alive", Value(std::string("v")), opts);

    SetOptions exp;
    exp.has_expire = true;
    exp.expire_at_ms = NowMs() + 20;
    s->Set("dead", Value(std::string("v")), exp);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    int counted = 0;
    s->ForEach([&](const std::string&, const Value&, int64_t) { ++counted; });
    RL_CHECK_EQ(counted, 1);
    delete s;
}

// ---------------------------------------------------------------- FLUSH
RL_TEST(store_flush_all) {
    Store* s = MakeStore();
    SetOptions opts;
    for (int i = 0; i < 50; ++i) {
        s->Set("k" + std::to_string(i), Value(std::string("v")), opts);
    }
    RL_CHECK_EQ(s->Size(), size_t(50));

    s->FlushAll();
    RL_CHECK_EQ(s->Size(), size_t(0));
    RL_CHECK(!s->Exists("k0"));

    // flush 之后仍可正常写入。
    s->Set("new", Value(std::string("v")), opts);
    RL_CHECK_EQ(s->Size(), size_t(1));
    delete s;
}

// ---------------------------------------------------------------- 并发
RL_TEST(store_concurrent_disjoint_keys) {
    // 不同分片的并发写必须全部成功且不丢数据。
    // 这是「分片锁」设计的正确性验证：如果实现里用了共享的可变状态，
    // 这个用例会在高并发下随机失败。
    Store* s = MakeStore(64);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([s, t] {
            SetOptions opts;
            for (int i = 0; i < kPerThread; ++i) {
                s->Set("t" + std::to_string(t) + ":k" + std::to_string(i),
                       Value(std::to_string(i)), opts);
            }
        });
    }
    for (auto& th : threads) th.join();

    RL_CHECK_EQ(s->Size(), size_t(kThreads * kPerThread));
    Value out;
    RL_CHECK(s->Get("t7:k1999", &out));
    RL_CHECK_STR(out.AsString(), std::string("1999"));
    delete s;
}

RL_TEST(store_concurrent_incr_same_key) {
    // 同一 key 的并发自增必须无丢失：最终值 = 总次数。
    // 这验证的是「分片内互斥」是否真的生效。
    Store* s = MakeStore(16);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([s] {
            for (int i = 0; i < kPerThread; ++i) {
                int64_t v = 0;
                s->IncrBy("shared", 1, &v);
            }
        });
    }
    for (auto& th : threads) th.join();

    Value out;
    RL_CHECK(s->Get("shared", &out));
    RL_CHECK_STR(out.AsString(), std::to_string(kThreads * kPerThread));
    delete s;
}

// ---------------------------------------------------------------- 内存/统计
RL_TEST(store_memory_usage_positive) {
    Store* s = MakeStore();
    SetOptions opts;
    for (int i = 0; i < 100; ++i) {
        s->Set("key" + std::to_string(i), Value(std::string(64, 'x')), opts);
    }
    RL_CHECK_MSG(s->MemoryUsage() > 0, "memory usage should be measurable");
    delete s;
}

RL_TEST(store_type_of) {
    Store* s = MakeStore();
    RL_CHECK_EQ(static_cast<int>(s->TypeOf("missing")),
                static_cast<int>(ValueType::kNone));

    SetOptions opts;
    s->Set("k", Value(std::string("v")), opts);
    RL_CHECK_EQ(static_cast<int>(s->TypeOf("k")), static_cast<int>(ValueType::kString));

    // 对外逻辑类型：int 编码的值在协议层仍然是 string。
    s->Set("n", Value(int64_t(42)), opts);
    RL_CHECK_STR(ValueTypeName(s->TypeOf("n")), std::string("string"));
    delete s;
}

RL_TEST(store_setraw_overwrites_without_conditions) {
    // AOF 重放路径使用 SetRaw，必须绕过 NX/XX 与过期检查。
    Store* s = MakeStore();
    SetOptions opts;
    s->Set("k", Value(std::string("v1")), opts);
    s->SetRaw("k", Value(std::string("v2")), 0);

    Value out;
    RL_CHECK(s->Get("k", &out));
    RL_CHECK_STR(out.AsString(), std::string("v2"));
    delete s;
}
