// Copyright (c) 2025 redis-lite authors. MIT License.
//
// store.cpp — 分片存储引擎实现。

#include "rl/store.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "rl/aof.h"
#include "rl/log.h"
#include "rl/platform.h"

namespace rl {

const char* ValueTypeName(ValueType t) {
    switch (t) {
        case ValueType::kString:
            return "string";
        case ValueType::kInt:
            return "string";  // 对外语义仍是 string，只是内部编码不同
        case ValueType::kZSet:
            return "zset";
        case ValueType::kNone:
            return "none";
    }
    return "none";
}

// ==================================================================== Value
const std::string& Value::AsString() const {
    if (type_ == ValueType::kString) return str_;
    if (type_ == ValueType::kInt) {
        if (cache_.empty()) {
            char buf[32];
            int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_));
            cache_.assign(buf, static_cast<size_t>(n));
        }
        return cache_;
    }
    // kZSet 没有单一字符串表示。返回空串而不是崩溃，让调用方的类型检查
    // 成为唯一的正确性依赖 —— 所有调用点都必须先判类型。
    cache_.clear();
    return cache_;
}

size_t Value::MemoryUsage() const {
    // 粗略估算：tag + 数值 + 字符串容量 + zset 结构。
    // 目的是量级正确，不必精确到字节。
    size_t base = sizeof(Value);
    if (type_ == ValueType::kString) base += str_.capacity();
    if (type_ == ValueType::kZSet && zset_ != nullptr) base += zset_->MemoryUsage();
    return base;
}

// ==================================================================== Impl
namespace {

// FNV-1a 64 位。选它是因为短 key 上很快，且分布对哈希表友好。
inline uint64_t HashKey(const char* data, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

struct Entry {
    Value value;
    // 0 表示永不过期。用绝对单调时刻而非剩余时长，
    // 避免每次读取都要做时间换算，也避免系统时间被改动导致 TTL 错乱。
    int64_t expire_at_ms = 0;

    bool HasExpire() const { return expire_at_ms != 0; }
    bool IsExpired(int64_t now_ms) const {
        return expire_at_ms != 0 && expire_at_ms <= now_ms;
    }
};

struct Shard {
    // W4 深挖位 ①：把这里换成自研的渐进式 rehash 哈希表。
    std::unordered_map<std::string, Entry> map;
    mutable std::mutex mutex;

    // 带 TTL 的 key 索引，用于主动过期的随机采样。
    // 采样而非全量扫描：全量扫描的耗时随 key 数线性增长，会拖垮后台线程。
    std::vector<std::string> ttl_keys;
    size_t ttl_scan_cursor = 0;
};

}  // namespace

struct Store::Impl {
    std::vector<std::unique_ptr<Shard>> shards;
    size_t shard_mask = 0;
    Stats* stats = nullptr;

    std::atomic<bool> background_running{false};
    std::thread background_thread;

    // 全局原子计数，避免为了 INFO 去遍历所有分片加锁。
    std::atomic<int64_t> key_count{0};
    std::atomic<int64_t> expires_count{0};

    Shard& GetShard(const std::string& key) {
        uint64_t h = HashKey(key.data(), key.size());
        return *shards[h & shard_mask];
    }

    static void AddTtlKey(Shard& sh, const std::string& key) {
        sh.ttl_keys.push_back(key);
    }

    // 从 ttl_keys 里移除指定 key。O(n) 但只在删除时发生，
    // 且 ttl_keys 只包含带过期时间的 key，规模远小于全量数据。
    static void RemoveTtlKey(Shard& sh, const std::string& key) {
        for (size_t i = 0; i < sh.ttl_keys.size(); ++i) {
            if (sh.ttl_keys[i] == key) {
                sh.ttl_keys[i] = std::move(sh.ttl_keys.back());
                sh.ttl_keys.pop_back();
                if (sh.ttl_scan_cursor >= sh.ttl_keys.size()) sh.ttl_scan_cursor = 0;
                return;
            }
        }
    }
};

// ==================================================================== 生命周期
Store::Store() : impl_(new Impl()) {}

Store::~Store() { Shutdown(); }

bool Store::Init(size_t shard_count, Stats* stats) {
    impl_->stats = stats;

    // 强制 2 的幂：让 GetShard 里的 & 代替 %，省掉一次除法。
    if (shard_count < 1) shard_count = 1;
    size_t n = 1;
    while (n < shard_count) n <<= 1;

    impl_->shards.clear();
    impl_->shards.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        impl_->shards.emplace_back(new Shard());
    }
    impl_->shard_mask = n - 1;

    RL_INFO("Store initialized with %s shards", Num(n).c_str());
    return true;
}

void Store::Shutdown() {
    if (impl_->background_running.exchange(false)) {
        if (impl_->background_thread.joinable()) impl_->background_thread.join();
    }
    if (!impl_->shards.empty()) {
        impl_->shards.clear();
        impl_->key_count.store(0);
        impl_->expires_count.store(0);
    }
}

// ==================================================================== 读路径
bool Store::Get(const std::string& key, Value* out) {
    const int64_t now = NowMs();
    Shard& sh = impl_->GetShard(key);

    std::lock_guard<std::mutex> lk(sh.mutex);
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return false;

    // 惰性过期：命中即检查。这样即使主动过期还没扫到，也不会读到脏数据。
    if (it->second.IsExpired(now)) {
        sh.map.erase(it);
        impl_->RemoveTtlKey(sh, key);
        impl_->key_count.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    *out = it->second.value;
    return true;
}

bool Store::Exists(const std::string& key) {
    Value v;
    return Get(key, &v);
}

ValueType Store::TypeOf(const std::string& key) {
    Value v;
    if (!Get(key, &v)) return ValueType::kNone;
    return v.type();
}

int64_t Store::TtlMs(const std::string& key) {
    const int64_t now = NowMs();
    Shard& sh = impl_->GetShard(key);

    std::lock_guard<std::mutex> lk(sh.mutex);
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return -2;
    if (it->second.IsExpired(now)) {
        sh.map.erase(it);
        impl_->RemoveTtlKey(sh, key);
        impl_->key_count.fetch_sub(1, std::memory_order_relaxed);
        return -2;
    }
    if (!it->second.HasExpire()) return -1;

    int64_t remain = it->second.expire_at_ms - now;
    return remain < 0 ? 0 : remain;
}

// ==================================================================== ZSET
bool Store::GetZSet(const std::string& key, std::shared_ptr<ZSet>* out, bool* wrong_type) {
    if (wrong_type != nullptr) *wrong_type = false;
    const int64_t now = NowMs();
    Shard& sh = impl_->GetShard(key);

    std::lock_guard<std::mutex> lk(sh.mutex);
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return false;

    if (it->second.IsExpired(now)) {
        sh.map.erase(it);
        impl_->RemoveTtlKey(sh, key);
        impl_->key_count.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    if (!it->second.value.IsZSet()) {
        // 类型不匹配必须与「不存在」区分开 —— 前者要回 -WRONGTYPE 错误，
        // 后者要回空值。混在一起会让客户端无法判断该怎么处理。
        if (wrong_type != nullptr) *wrong_type = true;
        return false;
    }

    if (out != nullptr) *out = it->second.value.AsZSet();
    return true;
}

bool Store::GetOrCreateZSet(const std::string& key, std::shared_ptr<ZSet>* out,
                            bool* wrong_type) {
    if (wrong_type != nullptr) *wrong_type = false;
    const int64_t now = NowMs();
    Shard& sh = impl_->GetShard(key);

    std::lock_guard<std::mutex> lk(sh.mutex);
    auto it = sh.map.find(key);

    if (it != sh.map.end() && it->second.IsExpired(now)) {
        sh.map.erase(it);
        impl_->RemoveTtlKey(sh, key);
        impl_->key_count.fetch_sub(1, std::memory_order_relaxed);
        it = sh.map.end();
    }

    if (it != sh.map.end()) {
        if (!it->second.value.IsZSet()) {
            if (wrong_type != nullptr) *wrong_type = true;
            return false;
        }
        if (out != nullptr) *out = it->second.value.AsZSet();
        return true;
    }

    // 新建。TTL 不继承（key 本来就是新的）。若调用方需要保留 TTL，
    // 应先查后建 —— 当前命令语义（ZADD 到不存在的 key）不需要。
    auto zs = std::make_shared<ZSet>();
    Entry e;
    e.value = Value(zs);
    e.expire_at_ms = 0;
    sh.map.emplace(key, std::move(e));
    impl_->key_count.fetch_add(1, std::memory_order_relaxed);

    if (out != nullptr) *out = std::move(zs);
    return true;
}

void Store::RemoveIfEmptyZSet(const std::string& key) {
    Shard& sh = impl_->GetShard(key);
    std::lock_guard<std::mutex> lk(sh.mutex);

    auto it = sh.map.find(key);
    if (it == sh.map.end()) return;
    if (!it->second.value.IsZSet()) return;
    const std::shared_ptr<ZSet>& zs = it->second.value.AsZSet();
    if (zs == nullptr || zs->Size() != 0) return;

    // 空集合要从 keyspace 里删掉，否则 DBSIZE 会统计出一堆空 zset，
    // 且 KEYS * 会返回「存在但 ZCARD 为 0」的 key —— 与 Redis 行为不符。
    const bool had_ttl = it->second.HasExpire();
    sh.map.erase(it);
    impl_->key_count.fetch_sub(1, std::memory_order_relaxed);
    if (had_ttl) {
        size_t before = sh.ttl_keys.size();
        impl_->RemoveTtlKey(sh, key);
        if (sh.ttl_keys.size() != before) {
            impl_->expires_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

// ==================================================================== 写路径
SetResult Store::Set(const std::string& key, Value value, const SetOptions& opts) {
    const int64_t now = NowMs();
    Shard& sh = impl_->GetShard(key);

    std::lock_guard<std::mutex> lk(sh.mutex);

    auto it = sh.map.find(key);
    // 先把「已过期」视为不存在，否则 NX/XX 语义会在过期 key 上出错。
    if (it != sh.map.end() && it->second.IsExpired(now)) {
        sh.map.erase(it);
        impl_->RemoveTtlKey(sh, key);
        impl_->key_count.fetch_sub(1, std::memory_order_relaxed);
        it = sh.map.end();
    }

    const bool exists = (it != sh.map.end());
    if (opts.nx && exists) return SetResult::kNotSetNx;
    if (opts.xx && !exists) return SetResult::kNotSetXx;

    int64_t expire_at = 0;
    if (opts.has_expire) {
        expire_at = opts.expire_at_ms;
    } else if (opts.keep_ttl && exists) {
        expire_at = it->second.expire_at_ms;
    }

    if (!exists) {
        Entry e;
        e.value = std::move(value);
        e.expire_at_ms = expire_at;
        sh.map.emplace(key, std::move(e));
        impl_->key_count.fetch_add(1, std::memory_order_relaxed);
        // 新建且带 TTL：登记到主动过期索引。
        if (expire_at != 0) {
            impl_->AddTtlKey(sh, key);
            impl_->expires_count.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // 覆盖写：TTL 索引需要同步维护，否则主动过期会漏掉这个 key。
        // O(1) 判断，不扫描整个 ttl_keys —— 这条路径在每个 SET 上都会走。
        const bool had_ttl = it->second.HasExpire();
        it->second.value = std::move(value);
        it->second.expire_at_ms = expire_at;
        const bool has_ttl = it->second.HasExpire();

        if (!had_ttl && has_ttl) {
            impl_->AddTtlKey(sh, key);
            impl_->expires_count.fetch_add(1, std::memory_order_relaxed);
        } else if (had_ttl && !has_ttl) {
            size_t before = sh.ttl_keys.size();
            impl_->RemoveTtlKey(sh, key);
            if (sh.ttl_keys.size() != before) {
                impl_->expires_count.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }

    return SetResult::kOk;
}

void Store::SetRaw(const std::string& key, Value value, int64_t expire_at_ms) {
    Shard& sh = impl_->GetShard(key);
    std::lock_guard<std::mutex> lk(sh.mutex);

    auto it = sh.map.find(key);
    const bool existed = (it != sh.map.end());

    Entry e;
    e.value = std::move(value);
    e.expire_at_ms = expire_at_ms;

    if (existed) {
        it->second = std::move(e);
    } else {
        sh.map.emplace(key, std::move(e));
        impl_->key_count.fetch_add(1, std::memory_order_relaxed);
    }

    if (expire_at_ms != 0) {
        bool registered = false;
        for (const std::string& k : sh.ttl_keys) {
            if (k == key) {
                registered = true;
                break;
            }
        }
        if (!registered) impl_->AddTtlKey(sh, key);
    }
}

bool Store::Del(const std::string& key) {
    Shard& sh = impl_->GetShard(key);
    std::lock_guard<std::mutex> lk(sh.mutex);

    auto it = sh.map.find(key);
    if (it == sh.map.end()) return false;

    const bool had_ttl = it->second.HasExpire();
    sh.map.erase(it);
    impl_->key_count.fetch_sub(1, std::memory_order_relaxed);

    if (had_ttl) {
        // 只有在确实登记过 TTL 时才需要同步索引。
        size_t before = sh.ttl_keys.size();
        impl_->RemoveTtlKey(sh, key);
        if (sh.ttl_keys.size() != before) {
            impl_->expires_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    return true;
}

int64_t Store::DelMulti(const std::vector<std::string>& keys) {
    int64_t n = 0;
    for (const std::string& k : keys) {
        if (Del(k)) ++n;
    }
    return n;
}

ExpireResult Store::ExpireAt(const std::string& key, int64_t expire_at_ms) {
    const int64_t now = NowMs();
    Shard& sh = impl_->GetShard(key);

    std::lock_guard<std::mutex> lk(sh.mutex);
    auto it = sh.map.find(key);
    if (it == sh.map.end()) return ExpireResult{false, true};

    if (it->second.IsExpired(now)) {
        sh.map.erase(it);
        impl_->RemoveTtlKey(sh, key);
        impl_->key_count.fetch_sub(1, std::memory_order_relaxed);
        return ExpireResult{false, true};
    }

    // 过期时刻已过：立即删除，语义等价于 Redis 的 EXPIRE 负数。
    if (expire_at_ms != 0 && expire_at_ms <= now) {
        const bool had_ttl = it->second.HasExpire();
        sh.map.erase(it);
        impl_->key_count.fetch_sub(1, std::memory_order_relaxed);
        if (had_ttl) {
            size_t before = sh.ttl_keys.size();
            impl_->RemoveTtlKey(sh, key);
            if (sh.ttl_keys.size() != before) {
                impl_->expires_count.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        return ExpireResult{true, true};
    }

    const bool had_ttl = it->second.HasExpire();
    it->second.expire_at_ms = expire_at_ms;
    const bool has_ttl = it->second.HasExpire();

    if (!had_ttl && has_ttl) {
        impl_->AddTtlKey(sh, key);
        impl_->expires_count.fetch_add(1, std::memory_order_relaxed);
    } else if (had_ttl && !has_ttl) {
        impl_->RemoveTtlKey(sh, key);
        impl_->expires_count.fetch_sub(1, std::memory_order_relaxed);
    }

    return ExpireResult{true, false};
}

bool Store::IncrBy(const std::string& key, int64_t delta, int64_t* new_value) {
    const int64_t now = NowMs();
    Shard& sh = impl_->GetShard(key);

    std::lock_guard<std::mutex> lk(sh.mutex);

    auto it = sh.map.find(key);
    if (it != sh.map.end() && it->second.IsExpired(now)) {
        sh.map.erase(it);
        impl_->RemoveTtlKey(sh, key);
        impl_->key_count.fetch_sub(1, std::memory_order_relaxed);
        it = sh.map.end();
    }

    if (it == sh.map.end()) {
        Entry e;
        e.value = Value(delta);
        sh.map.emplace(key, std::move(e));
        impl_->key_count.fetch_add(1, std::memory_order_relaxed);
        *new_value = delta;
        return true;
    }

    Entry& e = it->second;
    int64_t cur = 0;
    if (e.value.IsInt()) {
        cur = e.value.AsInt();
    } else {
        // 字符串编码的值也允许自增，但必须是合法整数，否则与 Redis 一样报错。
        if (!ParseInt64(e.value.AsString().data(), e.value.AsString().size(), &cur)) {
            return false;
        }
    }

    // 溢出检查：C++ 有符号溢出是 UB，必须先判后算。
    if ((delta > 0 && cur > INT64_MAX - delta) || (delta < 0 && cur < INT64_MIN - delta)) {
        return false;
    }

    cur += delta;

    // 编码转换：原值在 int64 范围内就保持 kInt 编码，省内存也省格式化开销。
    e.value = Value(cur);
    *new_value = cur;
    return true;
}

// ==================================================================== 遍历
void Store::ForEach(const std::function<void(const std::string&, const Value&, int64_t)>& fn) {
    const int64_t now = NowMs();
    for (auto& shp : impl_->shards) {
        Shard& sh = *shp;
        // 逐个分片加锁，而不是一次性全锁：
        // 遍历期间其他分片仍可正常读写，把长耗时的 KEYS 影响面降到 1/64。
        std::lock_guard<std::mutex> lk(sh.mutex);
        for (auto& kv : sh.map) {
            if (kv.second.IsExpired(now)) continue;
            fn(kv.first, kv.second.value, kv.second.expire_at_ms);
        }
    }
}

size_t Store::Size() {
    size_t n = 0;
    for (auto& shp : impl_->shards) {
        Shard& sh = *shp;
        std::lock_guard<std::mutex> lk(sh.mutex);
        n += sh.map.size();
    }
    return n;
}

size_t Store::ExpiresCount() {
    size_t n = 0;
    for (auto& shp : impl_->shards) {
        Shard& sh = *shp;
        std::lock_guard<std::mutex> lk(sh.mutex);
        n += sh.ttl_keys.size();
    }
    return n;
}

size_t Store::MemoryUsage() {
    size_t total = 0;
    for (auto& shp : impl_->shards) {
        Shard& sh = *shp;
        std::lock_guard<std::mutex> lk(sh.mutex);
        for (const auto& kv : sh.map) {
            total += kv.first.capacity() + kv.second.value.MemoryUsage() + sizeof(Entry);
        }
    }
    return total;
}

void Store::FlushAll() {
    for (auto& shp : impl_->shards) {
        Shard& sh = *shp;
        std::lock_guard<std::mutex> lk(sh.mutex);
        sh.map.clear();
        sh.ttl_keys.clear();
        sh.ttl_scan_cursor = 0;
    }
    impl_->key_count.store(0);
    impl_->expires_count.store(0);
}

// ==================================================================== 过期
size_t Store::ExpireCycle() {
    const int64_t now = NowMs();
    size_t removed = 0;

    // 每轮只采样 20 个 key：把后台清理的成本控制在常数级别。
    // 这是「主动过期 + 惰性过期」组合里主动那一半的核心取舍。
    constexpr size_t kSamplePerCycle = 20;

    for (auto& shp : impl_->shards) {
        Shard& sh = *shp;
        std::vector<std::string> expired;

        {
            std::lock_guard<std::mutex> lk(sh.mutex);
            if (sh.ttl_keys.empty()) continue;

            const size_t total = sh.ttl_keys.size();
            const size_t budget = std::min(kSamplePerCycle, total);

            for (size_t i = 0; i < budget; ++i) {
                if (sh.ttl_keys.empty()) break;
                if (sh.ttl_scan_cursor >= sh.ttl_keys.size()) sh.ttl_scan_cursor = 0;

                const std::string& k = sh.ttl_keys[sh.ttl_scan_cursor];
                auto it = sh.map.find(k);
                if (it == sh.map.end()) {
                    // 索引与实际数据不一致（例如被 Del 又重设），顺手清理。
                    sh.ttl_keys[sh.ttl_scan_cursor] = std::move(sh.ttl_keys.back());
                    sh.ttl_keys.pop_back();
                    continue;
                }
                if (it->second.IsExpired(now)) {
                    expired.push_back(k);
                    sh.map.erase(it);
                    sh.ttl_keys[sh.ttl_scan_cursor] = std::move(sh.ttl_keys.back());
                    sh.ttl_keys.pop_back();
                    if (sh.ttl_scan_cursor >= sh.ttl_keys.size()) sh.ttl_scan_cursor = 0;
                } else {
                    ++sh.ttl_scan_cursor;
                }
            }
        }

        if (!expired.empty()) {
            removed += expired.size();
            impl_->key_count.fetch_sub(static_cast<int64_t>(expired.size()),
                                      std::memory_order_relaxed);
            impl_->expires_count.fetch_sub(static_cast<int64_t>(expired.size()),
                                           std::memory_order_relaxed);
        }
    }

    return removed;
}

void Store::StartBackground() {
    if (impl_->background_running.exchange(true)) return;

    impl_->background_thread = std::thread([this] {
#if RL_LINUX
        pthread_setname_np(pthread_self(), "rl-bg");
#endif
        RL_DEBUG("Store background thread started");
        while (impl_->background_running.load(std::memory_order_acquire)) {
            // 100ms 一跳：过期精度对缓存场景足够，又不会空转浪费 CPU。
            // 用 sleep 而不是条件变量，是为了让这个线程自身保持极简。
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            size_t n = ExpireCycle();
            if (n > 0 && impl_->stats != nullptr) {
                impl_->stats->IncExpiredKeys();
            }
        }
        RL_DEBUG("Store background thread exiting");
    });
}

}  // namespace rl
