// Copyright (c) 2025 redis-lite authors. MIT License.
//
// command.cpp — 命令表与处理函数实现。
//
// 已实现命令：
//   PING ECHO INFO DBSIZE FLUSHALL KEYS TYPE EXISTS
//   SET GET DEL EXPIRE TTL PEXPIRE PTTL INCR DECR INCRBY DECRBY
//
// 未实现（README 中标注，作为 W4 扩展方向）：
//   HSET/HGET 哈希、ZADD/ZRANGE 跳表、MULTI/EXEC 事务、SUBSCRIBE 发布订阅、
//   主从复制 PSYNC、集群分片、SCAN 游标遍历

#include "rl/command.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <unordered_map>

#include "rl/log.h"
#include "rl/resp.h"
#include "rl/stats.h"
#include "rl/zset.h"

namespace rl {

namespace {

// ================================================================ 辅助函数
void ReplyError(std::string* out, const std::string& msg) { EncodeError(msg, out); }

void ReplyWrongArgs(std::string* out, const char* cmd) {
    ReplyError(out, std::string("ERR wrong number of arguments for '") + cmd + "' command");
}

void ReplyNotInt(std::string* out) {
    ReplyError(out, "ERR value is not an integer or out of range");
}

// 解析 EXPIRE/PEXPIRE 这类「key + 秒数/毫秒数」的第二个参数。
bool ParseExpireArg(const std::string& arg, int64_t multiplier, int64_t* out_ms) {
    int64_t v = 0;
    if (!ParseInt64(arg.data(), arg.size(), &v)) return false;
    // 溢出保护：EXPIRE 的秒数乘 1000 可能溢出。
    if (v > INT64_MAX / multiplier || v < INT64_MIN / multiplier) return false;
    *out_ms = v * multiplier;
    return true;
}

// 写命令落 AOF 的统一入口。
// 注意这里落的是「命令语义」而不是「执行结果」，所以 EXPIRE 必须转成
// 绝对时刻的 PEXPIREAT —— 否则重放时相对时间会被再次计算，语义就变了。
void Persist(ServerContext& ctx, const std::vector<std::string>& args) {
    if (ctx.aof != nullptr && ctx.aof->is_open()) {
        ctx.aof->Append(args);
    }
}

// ================================================================ PING / ECHO
void CmdPing(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    (void)ctx;
    if (args.size() > 2) {
        ReplyWrongArgs(out, "ping");
        return;
    }
    if (args.size() == 1) {
        EncodeSimpleString("PONG", out);
    } else {
        EncodeBulkString(args[1], out);
    }
}

void CmdEcho(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    (void)ctx;
    if (args.size() != 2) {
        ReplyWrongArgs(out, "echo");
        return;
    }
    EncodeBulkString(args[1], out);
}

// ================================================================ SET
// 语法：SET key value [EX s | PX ms | EXAT ts | PXAT ts | KEEPTTL] [NX | XX]
void CmdSet(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() < 3) {
        ReplyWrongArgs(out, "set");
        return;
    }

    const std::string& key = args[1];
    const std::string& value = args[2];

    SetOptions opts;
    bool expire_seconds = false;
    bool has_relative_expire = false;
    int64_t expire_value = 0;

    for (size_t i = 3; i < args.size(); ++i) {
        std::string opt = ToLowerAscii(args[i]);

        if (opt == "nx") {
            if (opts.xx) {
                ReplyError(out, "ERR syntax error");
                return;
            }
            opts.nx = true;
        } else if (opt == "xx") {
            if (opts.nx) {
                ReplyError(out, "ERR syntax error");
                return;
            }
            opts.xx = true;
        } else if (opt == "keepttl") {
            opts.keep_ttl = true;
        } else if (opt == "ex" || opt == "px" || opt == "exat" || opt == "pxat") {
            if (i + 1 >= args.size()) {
                ReplyError(out, "ERR syntax error");
                return;
            }
            if (opts.has_expire) {
                ReplyError(out, "ERR syntax error");
                return;
            }
            const std::string& raw = args[++i];
            int64_t v = 0;
            if (!ParseInt64(raw.data(), raw.size(), &v)) {
                ReplyNotInt(out);
                return;
            }
            if ((opt == "ex" || opt == "px") && v <= 0) {
                // 与 Redis 一致：相对过期时间必须为正。
                ReplyError(out, "ERR invalid expire time in 'set' command");
                return;
            }
            if (opt == "ex") {
                if (v > INT64_MAX / 1000) {
                    ReplyNotInt(out);
                    return;
                }
                expire_value = v * 1000;
                expire_seconds = true;
                has_relative_expire = true;
            } else if (opt == "px") {
                expire_value = v;
                has_relative_expire = true;
            } else if (opt == "exat") {
                if (v > INT64_MAX / 1000) {
                    ReplyNotInt(out);
                    return;
                }
                // EXAT 是「秒」，内部统一用「毫秒」。
                expire_value = v * 1000;
                has_relative_expire = false;
            } else {  // pxat
                expire_value = v;
                has_relative_expire = false;
            }

            opts.has_expire = true;
        } else {
            ReplyError(out, "ERR syntax error");
            return;
        }
    }
    (void)expire_seconds;

    if (opts.has_expire) {
        // 单调时钟 + 相对时长 = 绝对过期时刻。
        // 存绝对时刻而非剩余时长，是为了让 TTL 的读取变成一次减法，
        // 也避免每次读都要重新计算。
        opts.expire_at_ms = has_relative_expire ? (NowMs() + expire_value) : expire_value;
        if (opts.expire_at_ms <= NowMs()) {
            opts.expire_at_ms = NowMs() + 1;  // 已过期的瞬间值：立刻删
        }
    }

    SetResult r = ctx.store->Set(key, Value(value), opts);
    if (r == SetResult::kNotSetNx || r == SetResult::kNotSetXx) {
        // 与 Redis 一致：条件不满足时返回 nil，且不写 AOF。
        EncodeNullBulk(out);
        return;
    }

    // 落 AOF 时统一转成 SET + PXAT 绝对时刻：
    // 重放时不会因为「重放发生得更晚」而改变过期语义。
    if (ctx.aof != nullptr && ctx.aof->is_open()) {
        if (opts.expire_at_ms != 0) {
            std::vector<std::string> aof_args = {"SET", key, value, "PXAT",
                                                 std::to_string(opts.expire_at_ms)};
            if (opts.keep_ttl) aof_args.push_back("KEEPTTL");
            ctx.aof->Append(aof_args);
        } else {
            std::vector<std::string> aof_args = {"SET", key, value};
            if (opts.keep_ttl) aof_args.push_back("KEEPTTL");
            ctx.aof->Append(aof_args);
        }
    }

    EncodeSimpleString("OK", out);
}

// ================================================================ GET
void CmdGet(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 2) {
        ReplyWrongArgs(out, "get");
        return;
    }
    Value v;
    if (!ctx.store->Get(args[1], &v)) {
        if (ctx.stats != nullptr) ctx.stats->IncKeyspaceMisses();
        EncodeNullBulk(out);
        return;
    }
    if (ctx.stats != nullptr) ctx.stats->IncKeyspaceHits();
    // 无论内部是 kInt 还是 kString，对外都吐字节序列 —— 编码是内部实现细节。
    EncodeBulkString(v.AsString(), out);
}

// ================================================================ DEL / EXISTS
void CmdDel(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() < 2) {
        ReplyWrongArgs(out, "del");
        return;
    }
    // DEL 允许多 key，与 Redis 一致。
    std::vector<std::string> keys(args.begin() + 1, args.end());
    int64_t n = ctx.store->DelMulti(keys);
    if (n > 0) Persist(ctx, args);
    EncodeInteger(n, out);
}

void CmdExists(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    (void)ctx;
    if (args.size() < 2) {
        ReplyWrongArgs(out, "exists");
        return;
    }
    int64_t n = 0;
    // EXISTS 支持重复 key 计数，所以不能用去重后的集合。
    for (size_t i = 1; i < args.size(); ++i) {
        if (ctx.store->Exists(args[i])) ++n;
    }
    EncodeInteger(n, out);
}

// ================================================================ INCR / DECR
void CmdIncrBy(ServerContext& ctx, std::vector<std::string>& args, std::string* out,
               bool is_decr) {
    if (args.size() != 2 && args.size() != 3) {
        ReplyWrongArgs(out, is_decr ? "decr" : "incr");
        return;
    }

    int64_t delta = 1;
    if (args.size() == 3) {
        if (!ParseInt64(args[2].data(), args[2].size(), &delta)) {
            ReplyNotInt(out);
            return;
        }
    }
    if (is_decr) {
        if (delta == INT64_MIN) {
            // -INT64_MIN 会溢出，必须先挡掉。
            ReplyNotInt(out);
            return;
        }
        delta = -delta;
    }

    int64_t value = 0;
    if (!ctx.store->IncrBy(args[1], delta, &value)) {
        ReplyError(out, "ERR value is not an integer or out of range");
        return;
    }

    // INCR 在 Redis 里也写 AOF（等价于 SET 新值）。
    // 这里落 INCRBY 而不是 SET，保证重启后语义一致且文件更短。
    if (ctx.aof != nullptr && ctx.aof->is_open()) {
        ctx.aof->AppendArgs({"INCRBY", args[1], std::to_string(delta)});
    }

    EncodeInteger(value, out);
}

void CmdIncr(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdIncrBy(ctx, args, out, false);
}

void CmdDecr(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdIncrBy(ctx, args, out, true);
}

// ================================================================ EXPIRE / TTL
// 语法：EXPIRE key seconds
// 落 AOF 时转成 PEXPIREAT 绝对毫秒时刻。
void CmdExpireGeneric(ServerContext& ctx, std::vector<std::string>& args, std::string* out,
                      int64_t multiplier, const char* name) {
    if (args.size() != 3) {
        ReplyWrongArgs(out, name);
        return;
    }

    const std::string& key = args[1];
    int64_t ttl_value = 0;
    if (!ParseExpireArg(args[2], multiplier, &ttl_value)) {
        ReplyNotInt(out);
        return;
    }

    // 关键：相对时长 → 绝对时刻，只在这里换算一次。
    const int64_t at_ms = NowMs() + ttl_value;
    ExpireResult r = ctx.store->ExpireAt(key, at_ms);

    if (r.removed && !r.changed) {
        EncodeInteger(0, out);  // key 不存在
        return;
    }

    if (r.removed && r.changed) {
        // TTL 为负：key 被立刻删除，等价于 DEL。
        if (ctx.aof != nullptr && ctx.aof->is_open()) {
            ctx.aof->AppendArgs({"DEL", key});
        }
        EncodeInteger(1, out);
        return;
    }

    if (ctx.aof != nullptr && ctx.aof->is_open()) {
        ctx.aof->AppendArgs({"PEXPIREAT", key, std::to_string(at_ms)});
    }
    EncodeInteger(1, out);
}

void CmdExpire(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdExpireGeneric(ctx, args, out, 1000, "expire");
}

void CmdPExpire(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdExpireGeneric(ctx, args, out, 1, "pexpire");
}

// PEXPIREAT key ms —— AOF 重放的入口，参数本身就是绝对时刻。
void CmdPExpireAt(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 3) {
        ReplyWrongArgs(out, "pexpireat");
        return;
    }
    int64_t at_ms = 0;
    if (!ParseInt64(args[2].data(), args[2].size(), &at_ms)) {
        ReplyNotInt(out);
        return;
    }
    ExpireResult r = ctx.store->ExpireAt(args[1], at_ms);
    if (!r.changed && r.removed) {
        EncodeInteger(0, out);
        return;
    }
    if (ctx.aof != nullptr && ctx.aof->is_open()) {
        ctx.aof->Append(args);
    }
    EncodeInteger(1, out);
}

void CmdTtlGeneric(ServerContext& ctx, std::vector<std::string>& args, std::string* out,
                   int64_t divisor) {
    if (args.size() != 2) {
        ReplyWrongArgs(out, "ttl");
        return;
    }
    int64_t ms = ctx.store->TtlMs(args[1]);
    if (ms == -2 || ms == -1) {
        EncodeInteger(ms, out);  // -2 不存在 / -1 无 TTL
        return;
    }
    // 向上取整：Redis 里 TTL 不会因为不足 1 秒就返回 0。
    EncodeInteger((ms + divisor - 1) / divisor, out);
}

void CmdTtl(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdTtlGeneric(ctx, args, out, 1000);
}

void CmdPTtl(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdTtlGeneric(ctx, args, out, 1);
}

// ================================================================ TYPE
void CmdType(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 2) {
        ReplyWrongArgs(out, "type");
        return;
    }
    EncodeSimpleString(ValueTypeName(ctx.store->TypeOf(args[1])), out);
}

// ================================================================ KEYS
void CmdKeys(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 2) {
        ReplyWrongArgs(out, "keys");
        return;
    }

    const std::string& pattern = args[1];
    std::vector<std::string> matched;

    // 注意：ForEach 持分片锁调用回调，回调里绝不能再访问 Store。
    // 这里只做匹配并收集，编码放到锁外。
    ctx.store->ForEach([&](const std::string& k, const Value&, int64_t) {
        if (GlobMatch(pattern.data(), pattern.size(), k.data(), k.size())) {
            matched.push_back(k);
        }
    });

    EncodeArrayHeader(matched.size(), out);
    for (const std::string& k : matched) {
        EncodeBulkString(k, out);
    }
}

// ================================================================ DBSIZE
void CmdDbSize(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 1) {
        ReplyWrongArgs(out, "dbsize");
        return;
    }
    EncodeInteger(static_cast<int64_t>(ctx.store->Size()), out);
}

// ================================================================ FLUSHALL
void CmdFlushAll(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 1) {
        ReplyWrongArgs(out, "flushall");
        return;
    }
    ctx.store->FlushAll();

    // AOF 里的历史命令已全部失效。最诚实的做法是整体重写文件为一条 FLUSHALL，
    // 否则重启后旧数据会「复活」——这是很多自制 Redis 的隐藏 bug。
    if (ctx.aof != nullptr && ctx.aof->is_open()) {
        std::string path = ctx.aof->path();
        AofSyncPolicy policy = ctx.aof->policy();
        ctx.aof->Close();

        std::FILE* fp = std::fopen(path.c_str(), "wb");  // 截断重写
        if (fp != nullptr) {
            std::vector<std::string> cmd = {"FLUSHALL"};
            std::string encoded;
            EncodeCommand(cmd, &encoded);
            std::fwrite(encoded.data(), 1, encoded.size(), fp);
            std::fclose(fp);
        } else {
            RL_ERROR("FLUSHALL: failed to truncate AOF %s", path.c_str());
        }

        std::string err;
        if (!ctx.aof->Open(path, policy, &err)) {
            RL_ERROR("FLUSHALL: failed to reopen AOF: %s", err.c_str());
        }
    }

    EncodeSimpleString("OK", out);
}

// ================================================================ INFO
void CmdInfo(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    (void)args;

    char buf[1024];
    std::string info;
    info.reserve(1024);

    // 非 const：INFO 顺手把内存用量回填进 Stats，方便外部监控直接读。
    Stats* s = ctx.stats;
    info += "# Server\r\n";    std::snprintf(buf, sizeof(buf), "redis_lite_version:%s\r\n", ctx.version.c_str());
    info += buf;
    std::snprintf(buf, sizeof(buf), "uptime_in_seconds:%lld\r\n",
                  static_cast<long long>(s != nullptr ? s->uptime_seconds() : 0));
    info += buf;
    std::snprintf(buf, sizeof(buf), "process_id:%d\r\n", static_cast<int>(
#if RL_WINDOWS
                                                          GetCurrentProcessId()
#else
                                                          getpid()
#endif
                                                          ));
    info += buf;

    info += "\r\n# Clients\r\n";
    std::snprintf(buf, sizeof(buf), "connected_clients:%lld\r\n",
                  static_cast<long long>(s != nullptr ? s->current_connections() : 0));
    info += buf;
    std::snprintf(buf, sizeof(buf), "total_connections_received:%lld\r\n",
                  static_cast<long long>(s != nullptr ? s->connections_total() : 0));
    info += buf;
    std::snprintf(buf, sizeof(buf), "rejected_connections:%lld\r\n",
                  static_cast<long long>(s != nullptr ? s->rejected() : 0));
    info += buf;

    info += "\r\n# Memory\r\n";
    const size_t mem = ctx.store->MemoryUsage();
    std::snprintf(buf, sizeof(buf), "used_memory:%zu\r\nused_memory_human:%.2fK\r\n", mem,
                  static_cast<double>(mem) / 1024.0);
    info += buf;
    if (s != nullptr) {
        s->SetUsedMemory(static_cast<int64_t>(mem));
    }

    info += "\r\n# Stats\r\n";
    std::snprintf(buf, sizeof(buf), "total_commands_processed:%lld\r\n",
                  static_cast<long long>(s != nullptr ? s->commands() : 0));
    info += buf;
    std::snprintf(buf, sizeof(buf), "expired_keys:%lld\r\n",
                  static_cast<long long>(s != nullptr ? s->expired_keys() : 0));
    info += buf;
    std::snprintf(buf, sizeof(buf), "keyspace_hits:%s%%\r\n",
                  s != nullptr ? s->hit_rate_string().c_str() : "0.00");
    info += buf;
    std::snprintf(buf, sizeof(buf), "total_net_input_bytes:%lld\r\n",
                  static_cast<long long>(s != nullptr ? s->bytes_in() : 0));
    info += buf;
    std::snprintf(buf, sizeof(buf), "total_net_output_bytes:%lld\r\n",
                  static_cast<long long>(s != nullptr ? s->bytes_out() : 0));
    info += buf;
    std::snprintf(buf, sizeof(buf), "protocol_errors:%lld\r\n",
                  static_cast<long long>(s != nullptr ? s->protocol_errors() : 0));
    info += buf;

    info += "\r\n# Persistence\r\n";
    const bool aof_on = (ctx.aof != nullptr && ctx.aof->is_open());
    std::snprintf(buf, sizeof(buf), "aof_enabled:%d\r\n", aof_on ? 1 : 0);
    info += buf;
    if (aof_on) {
        std::snprintf(buf, sizeof(buf), "aof_path:%s\r\naof_current_size:%lld\r\n",
                      ctx.aof->path().c_str(), static_cast<long long>(ctx.aof->file_size()));
        info += buf;
        std::snprintf(buf, sizeof(buf), "aof_fsync_policy:%s\r\naof_pending_bytes:%llu\r\n",
                      AofPolicyName(ctx.aof->policy()),
                      static_cast<unsigned long long>(ctx.aof->pending_bytes()));
        info += buf;
    }

    info += "\r\n# Keyspace\r\n";
    const size_t keys = ctx.store->Size();
    const size_t exp = ctx.store->ExpiresCount();
    std::snprintf(buf, sizeof(buf), "db0:keys=%zu,expires=%zu,shards=%zu\r\n", keys, exp,
                  ctx.shard_count);
    info += buf;

    info += "\r\n# Architecture\r\n";
    std::snprintf(buf, sizeof(buf), "reactor_loops:%zu\r\nshard_count:%zu\r\n", ctx.loop_count,
                  ctx.shard_count);
    info += buf;
    std::snprintf(buf, sizeof(buf), "edge_trigger:%d\r\n",
#if RL_LINUX
                  1
#else
                  0
#endif
    );
    info += buf;

    EncodeBulkString(info, out);
}

// ================================================================ 命令表
// ================================================================ ZSET 命令
//
// 已实现：ZADD ZSCORE ZCARD ZINCRBY ZRANK ZREVRANK ZRANGE ZREVRANGE
//        ZRANGEBYSCORE ZCOUNT ZREM
//
// 与 Redis 的差异（如实记录，面试可讲）：
//   - ZRANK 是 O(N) 而非 O(log N)，原因见 zset.h 文件头
//   - 未支持 ZADD 的 GT/LT/CH/INCR 选项、ZRANGEBYLEX、ZUNIONSTORE 等

// 类型不符时的统一回复。抽出来是因为 11 个 zset 命令都要用它。
void ReplyWrongType(std::string* out) {
    ReplyError(out,
               "WRONGTYPE Operation against a key holding the wrong kind of value");
}

// 取 zset 并处理「类型错误」与「key 不存在」两种情况的样板逻辑。
// 返回值语义：
//   true  = 拿到了可用的 zset（存在或已创建），可以继续执行
//   false = 已经写好了回复（类型错误 / 未找到），调用方应直接返回
bool AcquireZSet(ServerContext& ctx, const std::string& key, bool create,
                 std::shared_ptr<ZSet>* out, std::string* reply) {
    bool wrong_type = false;
    const bool ok = create ? ctx.store->GetOrCreateZSet(key, out, &wrong_type)
                           : ctx.store->GetZSet(key, out, &wrong_type);
    if (ok) return true;

    if (wrong_type) {
        ReplyWrongType(reply);
    } else {
        // key 不存在：读命令回空，写命令走 create 分支不会到这里
        EncodeNullBulk(reply);
    }
    return false;
}

// ZADD key score member [score member ...]
void CmdZAdd(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() < 4 || (args.size() % 2) != 0) {
        ReplyWrongArgs(out, "zadd");
        return;
    }

    std::shared_ptr<ZSet> zs;
    if (!AcquireZSet(ctx, args[1], true, &zs, out)) return;

    // 先全部解析并与校验，再统一写入。
    // 这样「第 3 个参数非法」时不会留下「前 2 个已写入」的半成品状态 ——
    // 原子性是命令语义的一部分。
    struct Pair {
        ZScore score;
        const std::string* member;
    };
    std::vector<Pair> pairs;
    pairs.reserve(args.size() / 2);

    for (size_t i = 2; i + 1 < args.size(); i += 2) {
        ZScore score = 0;
        if (!ParseZScore(args[i].data(), args[i].size(), &score)) {
            ReplyError(out, "ERR value is not a valid float");
            return;
        }
        pairs.push_back(Pair{score, &args[i + 1]});
    }

    int64_t added = 0;
    for (const Pair& p : pairs) {
        if (zs->Add(*p.member, p.score)) ++added;
    }

    // 落 AOF：逐条记录，重放时语义与在线一致。
    if (ctx.aof != nullptr && ctx.aof->is_open()) {
        for (const Pair& p : pairs) {
            ctx.aof->AppendArgs({"ZADD", args[1], ZScoreToString(p.score), *p.member});
        }
    }

    EncodeInteger(added, out);
}

void CmdZScore(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 3) {
        ReplyWrongArgs(out, "zscore");
        return;
    }
    std::shared_ptr<ZSet> zs;
    if (!AcquireZSet(ctx, args[1], false, &zs, out)) return;

    ZScore score = 0;
    if (!zs->GetScore(args[2], &score)) {
        EncodeNullBulk(out);
        return;
    }
    EncodeBulkString(ZScoreToString(score), out);
}

void CmdZCard(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 2) {
        ReplyWrongArgs(out, "zcard");
        return;
    }
    // ZCARD 对不存在的 key 返回 0（而不是 nil），这是 Redis 的语义。
    bool wrong_type = false;
    std::shared_ptr<ZSet> zs;
    if (!ctx.store->GetZSet(args[1], &zs, &wrong_type)) {
        if (wrong_type) {
            ReplyWrongType(out);
        } else {
            EncodeInteger(0, out);
        }
        return;
    }
    EncodeInteger(static_cast<int64_t>(zs->Size()), out);
}

// ZINCRBY key increment member
void CmdZIncrBy(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 4) {
        ReplyWrongArgs(out, "zincrby");
        return;
    }
    ZScore delta = 0;
    if (!ParseZScore(args[2].data(), args[2].size(), &delta)) {
        ReplyError(out, "ERR value is not a valid float");
        return;
    }

    std::shared_ptr<ZSet> zs;
    if (!AcquireZSet(ctx, args[1], true, &zs, out)) return;

    bool was_present = false;
    const ZScore now = zs->IncrBy(args[3], delta, &was_present);

    if (ctx.aof != nullptr && ctx.aof->is_open()) {
        ctx.aof->Append(args);
    }
    EncodeBulkString(ZScoreToString(now), out);
}

// ZRANK / ZREVRANK key member
void CmdZRankGeneric(ServerContext& ctx, std::vector<std::string>& args, std::string* out,
                     bool reverse) {
    if (args.size() != 3) {
        ReplyWrongArgs(out, reverse ? "zrevrank" : "zrank");
        return;
    }
    std::shared_ptr<ZSet> zs;
    if (!AcquireZSet(ctx, args[1], false, &zs, out)) return;

    size_t rank = 0;
    if (!zs->Rank(args[2], &rank)) {
        EncodeNullBulk(out);
        return;
    }
    // ZRANK 是 0-based；ZREVRANK 从大到小数，同样 0-based。
    const int64_t result =
        reverse ? static_cast<int64_t>(zs->Size() - 1 - rank) : static_cast<int64_t>(rank);
    EncodeInteger(result, out);
}

void CmdZRank(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdZRankGeneric(ctx, args, out, false);
}

void CmdZRevRank(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdZRankGeneric(ctx, args, out, true);
}

// 把 start/stop 归一化成 0-based 闭区间。
// 支持负数（-1 表示最后一个元素），越界自动截断。
// 返回 false 表示区间为空。
bool NormalizeRange(int64_t start, int64_t stop, size_t size, size_t* out_start,
                    size_t* out_stop) {
    if (size == 0) return false;
    const int64_t n = static_cast<int64_t>(size);

    if (start < 0) start += n;
    if (stop < 0) stop += n;
    if (start < 0) start = 0;
    if (stop >= n) stop = n - 1;
    if (start > stop || start >= n) return false;

    *out_start = static_cast<size_t>(start);
    *out_stop = static_cast<size_t>(stop);
    return true;
}

// ZRANGE key start stop / ZREVRANGE key start stop
void CmdZRangeGeneric(ServerContext& ctx, std::vector<std::string>& args, std::string* out,
                      bool reverse) {
    if (args.size() != 4) {
        ReplyWrongArgs(out, reverse ? "zrevrange" : "zrange");
        return;
    }

    int64_t start = 0, stop = 0;
    if (!ParseInt64(args[2].data(), args[2].size(), &start) ||
        !ParseInt64(args[3].data(), args[3].size(), &stop)) {
        ReplyNotInt(out);
        return;
    }

    std::shared_ptr<ZSet> zs;
    if (!AcquireZSet(ctx, args[1], false, &zs, out)) return;

    size_t lo = 0, hi = 0;
    if (!NormalizeRange(start, stop, zs->Size(), &lo, &hi)) {
        // 空结果也要回合法的空数组，而不是 nil。
        EncodeArrayHeader(0, out);
        return;
    }

    std::vector<const ZSetNode*> nodes;
    zs->RangeByRank(lo, hi, &nodes);

    EncodeArrayHeader(nodes.size(), out);
    if (!reverse) {
        for (const ZSetNode* n : nodes) {
            EncodeBulkString(n->member, out);
            EncodeBulkString(ZScoreToString(n->score), out);
        }
    } else {
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            EncodeBulkString((*it)->member, out);
            EncodeBulkString(ZScoreToString((*it)->score), out);
        }
    }
}

void CmdZRange(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdZRangeGeneric(ctx, args, out, false);
}

void CmdZRevRange(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdZRangeGeneric(ctx, args, out, true);
}

// 解析 ZRANGEBYSCORE 的区间端点，支持 "(1.5" 这种排除写法，
// 以及 "-inf" / "+inf"。
bool ParseScoreBound(const std::string& text, ZScore* value, bool* exclusive) {
    *exclusive = false;
    if (text.empty()) return false;

    const char* p = text.data();
    size_t len = text.size();
    if (p[0] == '(') {
        *exclusive = true;
        ++p;
        --len;
        if (len == 0) return false;
    }

    std::string lower(p, len);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    // 无穷端点：ZRANGEBYSCORE 允许，与 ZADD 禁止 inf 不同。
    if (lower == "-inf") {
        *value = -std::numeric_limits<ZScore>::infinity();
        return true;
    }
    if (lower == "+inf" || lower == "inf") {
        *value = std::numeric_limits<ZScore>::infinity();
        return true;
    }

    return ParseZScore(p, len, value);
}

// ZRANGEBYSCORE key min max
void CmdZRangeByScore(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 4) {
        ReplyWrongArgs(out, "zrangebyscore");
        return;
    }
    ZScore min_s = 0, max_s = 0;
    bool min_ex = false, max_ex = false;
    if (!ParseScoreBound(args[2], &min_s, &min_ex) ||
        !ParseScoreBound(args[3], &max_s, &max_ex)) {
        ReplyError(out, "ERR min or max is not a float");
        return;
    }

    std::shared_ptr<ZSet> zs;
    if (!AcquireZSet(ctx, args[1], false, &zs, out)) return;

    std::vector<const ZSetNode*> nodes;
    zs->RangeByScore(min_s, max_s, min_ex, max_ex, &nodes);

    EncodeArrayHeader(nodes.size(), out);
    for (const ZSetNode* n : nodes) {
        EncodeBulkString(n->member, out);
        EncodeBulkString(ZScoreToString(n->score), out);
    }
}

// ZCOUNT key min max
void CmdZCount(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() != 4) {
        ReplyWrongArgs(out, "zcount");
        return;
    }
    ZScore min_s = 0, max_s = 0;
    bool min_ex = false, max_ex = false;
    if (!ParseScoreBound(args[2], &min_s, &min_ex) ||
        !ParseScoreBound(args[3], &max_s, &max_ex)) {
        ReplyError(out, "ERR min or max is not a float");
        return;
    }

    bool wrong_type = false;
    std::shared_ptr<ZSet> zs;
    if (!ctx.store->GetZSet(args[1], &zs, &wrong_type)) {
        if (wrong_type) {
            ReplyWrongType(out);
        } else {
            EncodeInteger(0, out);
        }
        return;
    }

    std::vector<const ZSetNode*> nodes;
    zs->RangeByScore(min_s, max_s, min_ex, max_ex, &nodes);
    EncodeInteger(static_cast<int64_t>(nodes.size()), out);
}

// ZREM key member [member ...]
void CmdZRem(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    if (args.size() < 3) {
        ReplyWrongArgs(out, "zrem");
        return;
    }
    std::shared_ptr<ZSet> zs;
    if (!AcquireZSet(ctx, args[1], false, &zs, out)) return;

    int64_t removed = 0;
    for (size_t i = 2; i < args.size(); ++i) {
        if (zs->Remove(args[i])) ++removed;
    }

    // 集合变空后要把 key 从 keyspace 摘掉，否则 DBSIZE / KEYS 会看到空壳。
    ctx.store->RemoveIfEmptyZSet(args[1]);

    if (removed > 0 && ctx.aof != nullptr && ctx.aof->is_open()) {
        ctx.aof->Append(args);
    }
    EncodeInteger(removed, out);
}

// ================================================================ 命令表
struct CommandSpec {
    const char* name;
    void (*handler)(ServerContext&, std::vector<std::string>&, std::string*);
    bool is_write;  // 是否需要落 AOF（写命令由各 handler 自行落盘，此字段供 INFO/扩展用）
};

void CmdDecrBy(ServerContext& ctx, std::vector<std::string>& args, std::string* out) {
    CmdIncrBy(ctx, args, out, true);
}

const CommandSpec kCommandTable[] = {
    {"ping", CmdPing, false},
    {"echo", CmdEcho, false},
    {"set", CmdSet, true},
    {"get", CmdGet, false},
    {"del", CmdDel, true},
    {"exists", CmdExists, false},
    {"incr", CmdIncr, true},
    {"decr", CmdDecr, true},
    {"incrby", [](ServerContext& c, std::vector<std::string>& a, std::string* o) {
         CmdIncrBy(c, a, o, false);
     }, true},
    {"decrby", CmdDecrBy, true},
    {"expire", CmdExpire, true},
    {"pexpire", CmdPExpire, true},
    {"pexpireat", CmdPExpireAt, true},
    {"ttl", CmdTtl, false},
    {"pttl", CmdPTtl, false},
    {"type", CmdType, false},
    {"keys", CmdKeys, false},
    {"dbsize", CmdDbSize, false},
    {"flushall", CmdFlushAll, true},
    {"info", CmdInfo, false},

    // ---- ZSET ----
    {"zadd", CmdZAdd, true},
    {"zscore", CmdZScore, false},
    {"zcard", CmdZCard, false},
    {"zincrby", CmdZIncrBy, true},
    {"zrank", CmdZRank, false},
    {"zrevrank", CmdZRevRank, false},
    {"zrange", CmdZRange, false},
    {"zrevrange", CmdZRevRange, false},
    {"zrangebyscore", CmdZRangeByScore, false},
    {"zcount", CmdZCount, false},
    {"zrem", CmdZRem, true},
};

// 命令名 → 表项。构建一次，之后只读。
const std::unordered_map<std::string, const CommandSpec*>& GetCommandMap() {
    static const std::unordered_map<std::string, const CommandSpec*>* map = [] {
        auto* m = new std::unordered_map<std::string, const CommandSpec*>();
        for (const CommandSpec& spec : kCommandTable) {
            (*m)[spec.name] = &spec;
        }
        return m;
    }();
    return *map;
}

}  // namespace

// ================================================================ Dispatcher
void Dispatcher::Execute(std::vector<std::string>* args, std::string* out) {
    out->clear();

    if (args == nullptr || args->empty()) {
        ReplyError(out, "ERR empty command");
        return;
    }

    if (ctx_.stats != nullptr) ctx_.stats->IncCommands();

    // 命令名大小写不敏感，与 Redis 一致（SET / set / Set 等价）。
    const std::string name = ToLowerAscii((*args)[0]);

    const auto& map = GetCommandMap();
    auto it = map.find(name);
    if (it == map.end()) {
        // 未知命令必须带原始命令名，方便客户端定位问题。
        ReplyError(out, "ERR unknown command '" + (*args)[0] + "'");
        return;
    }

    // 处理函数只读命令名，这里把 args[0] 归一化成小写，
    // 避免每个 handler 都要自己做一次 ToLower。
    (*args)[0] = name;
    it->second->handler(ctx_, *args, out);
}

void Dispatcher::Replay(const std::vector<std::vector<std::string>>& commands) {
    // 重放期间 AOF 必须关闭，否则会把历史命令再写一遍，文件长度翻倍。
    std::string sink;
    for (const std::vector<std::string>& cmd : commands) {
        std::vector<std::string> copy = cmd;
        Execute(&copy, &sink);
    }
}

int Dispatcher::LoadAof(const std::string& path) {
    std::vector<std::vector<std::string>> commands;
    std::string err;
    size_t truncated = 0;

    if (!Aof::LoadAll(path, &commands, &err, &truncated)) {
        RL_ERROR("AOF load failed: %s", err.c_str());
        return -1;
    }

    Replay(commands);
    return static_cast<int>(commands.size());
}

}  // namespace rl
