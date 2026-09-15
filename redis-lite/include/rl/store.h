// Copyright (c) 2025 redis-lite authors. MIT License.
//
// store.h — 分片存储引擎
//
// 为什么是「分片 + 每分片独立锁」而不是「一把大锁」：
//   多 Reactor 模型下 N 个 loop 线程会并发执行命令。单把全局互斥锁会把
//   所有线程串行化，QPS 随核数增长很快撞墙；按 key 哈希到 64 个分片后，
//   不同分片的操作可以真正并行，锁冲突概率降到约 1/64。
//
// W4 深挖位（三选一，见 docs/INTERVIEW.md）：
//   ① 渐进式 rehash：把 Shard::map_ 换成自研双表哈希，扩容不再一次性阻塞
//   ② 跳表：增加 skiplist.h 实现 ZSET，支持 O(log N + M) 范围查询
//   ③ 紧凑编码：小整数/短字符串走 intset / ziplist 式编码，降低内存占用
//   三者的插入点都只在 GetShard / Shard 内部，不影响任何上层代码。

#ifndef RL_STORE_H
#define RL_STORE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rl/resp.h"
#include "rl/stats.h"
#include "rl/zset.h"

namespace rl {

// 值类型。
// 【TODO/W4】目前支持 string 与 zset；hash / list / set 可按同样的模式扩展。
enum class ValueType : uint8_t { kString = 0, kInt = 1, kZSet = 2, kNone = 3 };

const char* ValueTypeName(ValueType t);

// ---------------------------------------------------------------- Value
// 刻意不用 std::variant：编码种类很少，手写 tag + 成员更直白，
// 也更容易在面试里讲清内存布局与拷贝行为。
//
// 注意 zset 用 shared_ptr 持有：ZSet 不可拷贝（内含 std::set 与 hash map），
// 而 Value 需要能放进 unordered_map 并支持拷贝。shared_ptr 让拷贝变成
// 引用计数递增 —— 代价是「通过一个 Value 拷贝修改 zset 会影响另一个」，
// 这在当前设计里是安全的：所有 zset 修改都走 Store 的 GetZSet() 原地操作，
// 不存在「拷贝后独立修改」的语义。
class Value {
public:
    Value() = default;
    explicit Value(std::string s) : type_(ValueType::kString), str_(std::move(s)) {}
    explicit Value(int64_t v) : type_(ValueType::kInt), num_(v) {}
    explicit Value(std::shared_ptr<ZSet> z) : type_(ValueType::kZSet), zset_(std::move(z)) {}

    ValueType type() const { return type_; }
    bool IsString() const { return type_ == ValueType::kString; }
    bool IsInt() const { return type_ == ValueType::kInt; }
    bool IsZSet() const { return type_ == ValueType::kZSet; }

    // 统一视图：无论内部编码如何，都给出对外可见的字节序列。
    // GET / AOF / 复制都用它，保证「不同编码，同一语义」。
    // zset 没有「单一字符串表示」，调用方应先判类型。
    const std::string& AsString() const;

    int64_t AsInt() const { return num_; }
    const std::shared_ptr<ZSet>& AsZSet() const { return zset_; }

    // 估算内存占用，用于 INFO used_memory。key 的长度由调用方补上。
    size_t MemoryUsage() const;

private:
    ValueType type_ = ValueType::kNone;
    int64_t num_ = 0;
    std::string str_;
    std::shared_ptr<ZSet> zset_;
    // kInt 时惰性生成字符串视图，避免每次读都重新格式化。
    mutable std::string cache_;
};

// ---------------------------------------------------------------- ExpireResult
enum class SetResult { kOk, kNotSetNx, kNotSetXx };

struct SetOptions {
    bool nx = false;               // 仅当 key 不存在时设置
    bool xx = false;               // 仅当 key 存在时设置
    bool keep_ttl = false;         // 保留原有 TTL（默认 SET 会清除 TTL）
    bool has_expire = false;       // 是否带 EX/PX/EXAT/PXAT
    int64_t expire_at_ms = 0;      // 绝对过期时刻（单调时钟）
};

struct ExpireResult {
    bool changed = false;
    bool removed = false;  // 该 key 是否已被删除
};

// ---------------------------------------------------------------- Store
class Store {
public:
    Store();
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // shard_count 必须是 2 的幂（内部用位与代替取模）。
    // stats 可为 nullptr；显式注入而非单例，便于测试时替换实现。
    bool Init(size_t shard_count = 64, Stats* stats = nullptr);
    void Shutdown();

    // ---------------------------------------------------------- 基础命令
    // 命中返回 true 并填充 out；未命中或已过期返回 false。
    bool Get(const std::string& key, Value* out);

    SetResult Set(const std::string& key, Value value, const SetOptions& opts);

    // 返回是否真的删除了存在的 key（语义对齐 Redis DEL 的返回值）。
    bool Del(const std::string& key);
    int64_t DelMulti(const std::vector<std::string>& keys);

    bool Exists(const std::string& key);

    // 过期时间设置。expire_at_ms 为绝对时刻。
    ExpireResult ExpireAt(const std::string& key, int64_t expire_at_ms);
    // -2 = key 不存在；-1 = 存在但无 TTL；否则为剩余毫秒。
    int64_t TtlMs(const std::string& key);

    // 原子自增。key 不存在按 0 起算；值非整数返回 false。
    bool IncrBy(const std::string& key, int64_t delta, int64_t* new_value);

    // 覆盖式写入，仅用于 AOF 恢复与 FLUSHALL 之后的重建。
    void SetRaw(const std::string& key, Value value, int64_t expire_at_ms);

    // ---------------------------------------------------------- ZSET 支持
    // 取得已存在的 zset（只读）。
    //   key 不存在        → 返回 false，*out 不动
    //   key 存在但非 zset → 返回 false，*wrong_type 置 true（命令层据此回 -WRONGTYPE）
    bool GetZSet(const std::string& key, std::shared_ptr<ZSet>* out, bool* wrong_type);

    // 取得 zset，不存在则新建（并保留原有 TTL）。
    // 语义同上：类型不匹配时返回 false 且置 wrong_type。
    bool GetOrCreateZSet(const std::string& key, std::shared_ptr<ZSet>* out, bool* wrong_type);

    // 删除一个 zset 类型的 key（例如 ZREM 后集合变空、ZREMRANGEBYRANK 清空）。
    // 只在当前值确实是 zset 且已空时删除，避免误删其他类型。
    void RemoveIfEmptyZSet(const std::string& key);

    // ---------------------------------------------------------- 统计与遍历
    size_t Size();
    size_t ExpiresCount();
    size_t MemoryUsage();
    ValueType TypeOf(const std::string& key);
    void FlushAll();

    // 遍历全部数据。回调在持有分片锁期间被调用 —— 回调内不要再次访问 Store，
    // 否则可能自死锁。KEYS / AOF 重写 / INFO 使用此接口。
    void ForEach(const std::function<void(const std::string&, const Value&,
                                          int64_t /*expire_at_ms*/)>& fn);

    // ---------------------------------------------------------- 过期
    // 启动后台线程：每 100ms 采样检查过期 key，同时驱动 AOF 后台刷盘。
    void StartBackground();
    // 手动跑一轮（测试用），返回本轮清理掉的 key 数。
    size_t ExpireCycle();

    // 惰性过期：读路径上发现已过期立即删除。
    // 惰性 + 主动双管齐下，是 Redis 的原始设计，这里保持一致。

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rl

#endif  // RL_STORE_H
