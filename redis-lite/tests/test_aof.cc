// Copyright (c) 2025 redis-lite authors. MIT License.
//
// test_aof.cc — AOF 持久化单元测试
//
// 核心验证「崩溃一致性」：文件尾部存在写了一半的命令时，
// 恢复必须能截断到最后一个完整命令，而不是启动失败或加载脏数据。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rl/aof.h"
#include "rl/command.h"
#include "rl/resp.h"
#include "rl/store.h"
#include "test_util.h"

using namespace rl;

namespace {

// 每个用例用独立文件名，避免测试之间互相干扰。
std::string TempPath(const char* tag) {
    return std::string("rl_test_") + tag + ".aof";
}

void RemoveFile(const std::string& p) { std::remove(p.c_str()); }

}  // namespace

RL_TEST(aof_append_and_load_roundtrip) {
    const std::string path = TempPath("roundtrip");
    RemoveFile(path);

    std::string err;
    {
        Aof aof;
        RL_CHECK_MSG(aof.Open(path, AofSyncPolicy::kAlways, &err), err);
        aof.AppendArgs({"SET", "k1", "v1"});
        aof.AppendArgs({"SET", "k2", "v2"});
        aof.AppendArgs({"DEL", "k1"});
        aof.Close();
    }

    std::vector<std::vector<std::string>> cmds;
    size_t truncated = 0;
    RL_CHECK_MSG(Aof::LoadAll(path, &cmds, &err, &truncated), err);
    RL_CHECK_EQ(cmds.size(), size_t(3));
    RL_CHECK_EQ(truncated, size_t(0));

    RL_CHECK_EQ(cmds[0].size(), size_t(3));
    RL_CHECK_STR(cmds[0][0], std::string("SET"));
    RL_CHECK_STR(cmds[0][1], std::string("k1"));
    RL_CHECK_STR(cmds[0][2], std::string("v1"));
    RL_CHECK_STR(cmds[2][0], std::string("DEL"));

    RemoveFile(path);
}

RL_TEST(aof_load_missing_file_is_not_an_error) {
    // 首次启动必然没有 AOF 文件，这不是错误。
    const std::string path = TempPath("nonexistent");
    RemoveFile(path);

    std::vector<std::vector<std::string>> cmds;
    std::string err;
    size_t truncated = 0;
    RL_CHECK_MSG(Aof::LoadAll(path, &cmds, &err, &truncated), err);
    RL_CHECK_EQ(cmds.size(), size_t(0));
}

RL_TEST(aof_truncates_incomplete_tail) {
    // 模拟崩溃：最后一条命令只写了一半。
    const std::string path = TempPath("truncated");
    RemoveFile(path);

    std::string err;
    {
        Aof aof;
        RL_CHECK(aof.Open(path, AofSyncPolicy::kNo, &err));
        aof.AppendArgs({"SET", "complete", "value"});
        aof.SyncNow();
        aof.Close();
    }

    // 手工在文件尾部追加半条命令，模拟掉电。
    {
        std::FILE* fp = std::fopen(path.c_str(), "ab");
        RL_CHECK(fp != nullptr);
        // *3\r\n$3\r\nSET\r\n$4\r\nhalf\r\n$10\r\nabc   <- body 只有 3 字节就断了
        const char* partial = "*3\r\n$3\r\nSET\r\n$4\r\nhalf\r\n$10\r\nabc";
        std::fwrite(partial, 1, std::strlen(partial), fp);
        std::fclose(fp);
    }

    std::vector<std::vector<std::string>> cmds;
    size_t truncated = 0;
    RL_CHECK_MSG(Aof::LoadAll(path, &cmds, &err, &truncated), err);

    // 只应保留那条完整的命令。
    RL_CHECK_EQ(cmds.size(), size_t(1));
    RL_CHECK_STR(cmds[0][1], std::string("complete"));
    RL_CHECK_MSG(truncated > 0, "truncated byte count should be reported");

    RemoveFile(path);
}

RL_TEST(aof_truncates_on_protocol_error_in_tail) {
    // 尾部字节不仅不完整，而且不是合法的 RESP 前缀。
    const std::string path = TempPath("garbage");
    RemoveFile(path);

    std::string err;
    {
        Aof aof;
        RL_CHECK(aof.Open(path, AofSyncPolicy::kAlways, &err));
        aof.AppendArgs({"SET", "a", "1"});
        aof.Close();
    }
    {
        std::FILE* fp = std::fopen(path.c_str(), "ab");
        RL_CHECK(fp != nullptr);
        const char* garbage = "*notanumber\r\n";
        std::fwrite(garbage, 1, std::strlen(garbage), fp);
        std::fclose(fp);
    }

    std::vector<std::vector<std::string>> cmds;
    size_t truncated = 0;
    RL_CHECK_MSG(Aof::LoadAll(path, &cmds, &err, &truncated), err);
    RL_CHECK_EQ(cmds.size(), size_t(1));  // 坏尾被丢弃，好数据保留

    RemoveFile(path);
}

RL_TEST(aof_preserves_binary_values) {
    // value 含 \r\n 与 \0：AOF 用长度前缀编码，必须无损。
    const std::string path = TempPath("binary");
    RemoveFile(path);

    const std::string binary("bin\r\n\0ary", 10);
    std::string err;
    {
        Aof aof;
        RL_CHECK(aof.Open(path, AofSyncPolicy::kAlways, &err));
        aof.AppendArgs({"SET", "k", binary});
        aof.Close();
    }

    std::vector<std::vector<std::string>> cmds;
    size_t truncated = 0;
    RL_CHECK(Aof::LoadAll(path, &cmds, &err, &truncated));
    RL_CHECK_EQ(cmds.size(), size_t(1));
    RL_CHECK_EQ(cmds[0][2].size(), size_t(10));
    RL_CHECK_MSG(cmds[0][2] == binary, "binary value must round-trip losslessly");

    RemoveFile(path);
}

RL_TEST(aof_empty_command_is_ignored) {
    const std::string path = TempPath("emptycmd");
    RemoveFile(path);

    std::string err;
    {
        Aof aof;
        RL_CHECK(aof.Open(path, AofSyncPolicy::kNo, &err));
        aof.Append(std::vector<std::string>());  // 空命令应被忽略
        aof.AppendArgs({"PING"});
        aof.Close();
    }

    std::vector<std::vector<std::string>> cmds;
    size_t truncated = 0;
    RL_CHECK(Aof::LoadAll(path, &cmds, &err, &truncated));
    RL_CHECK_EQ(cmds.size(), size_t(1));

    RemoveFile(path);
}

RL_TEST(aof_reopen_appends_not_truncates) {
    // 重新打开必须走追加模式，否则重启会把历史数据清掉。
    const std::string path = TempPath("reopen");
    RemoveFile(path);

    std::string err;
    {
        Aof aof;
        RL_CHECK(aof.Open(path, AofSyncPolicy::kAlways, &err));
        aof.AppendArgs({"SET", "first", "1"});
        aof.Close();
    }
    {
        Aof aof;
        RL_CHECK(aof.Open(path, AofSyncPolicy::kAlways, &err));
        aof.AppendArgs({"SET", "second", "2"});
        aof.Close();
    }

    std::vector<std::vector<std::string>> cmds;
    size_t truncated = 0;
    RL_CHECK(Aof::LoadAll(path, &cmds, &err, &truncated));
    RL_CHECK_EQ(cmds.size(), size_t(2));
    RL_CHECK_STR(cmds[0][1], std::string("first"));
    RL_CHECK_STR(cmds[1][1], std::string("second"));

    RemoveFile(path);
}

// ---------------------------------------------------------------- 端到端恢复
RL_TEST(aof_full_recovery_through_dispatcher) {
    // 这是最有价值的一个用例：模拟完整生命周期
    //   执行命令 → 落 AOF → 进程"重启" → 重放 → 数据必须一致
    const std::string path = TempPath("recovery");
    RemoveFile(path);

    std::string err;
    Stats stats;
    stats.SetUptimeStart();

    // ---- 第一个"进程" ----
    Store store1;
    store1.Init(8, &stats);
    Aof aof1;
    RL_CHECK(aof1.Open(path, AofSyncPolicy::kAlways, &err));

    ServerContext ctx1;
    ctx1.store = &store1;
    ctx1.aof = &aof1;
    ctx1.stats = &stats;
    Dispatcher d1(ctx1);

    std::string reply;
    auto run = [&](std::initializer_list<const char*> args) {
        std::vector<std::string> v(args.begin(), args.end());
        reply.clear();
        d1.Execute(&v, &reply);
    };

    run({"SET", "persistent", "hello"});
    RL_CHECK_STR(reply, std::string("+OK\r\n"));

    run({"SET", "counter", "10"});
    run({"INCR", "counter"});
    RL_CHECK_STR(reply, std::string(":11\r\n"));

    run({"SET", "doomed", "x"});
    run({"DEL", "doomed"});

    run({"SET", "withttl", "v", "EX", "10000"});
    RL_CHECK_STR(reply, std::string("+OK\r\n"));

    aof1.Close();
    store1.Shutdown();

    // ---- 第二个"进程"：从 AOF 恢复 ----
    Store store2;
    store2.Init(8, &stats);
    Aof aof2;

    ServerContext ctx2;
    ctx2.store = &store2;
    // 重放期间 AOF 故意不打开：否则会把历史命令再写一遍。
    ctx2.aof = nullptr;
    ctx2.stats = &stats;
    Dispatcher d2(ctx2);

    int replayed = d2.LoadAof(path);
    RL_CHECK_MSG(replayed > 0, "AOF replay should load commands");

    Value out;
    RL_CHECK_MSG(store2.Get("persistent", &out), "persistent key must survive restart");
    RL_CHECK_STR(out.AsString(), std::string("hello"));

    RL_CHECK_MSG(store2.Get("counter", &out), "counter must survive restart");
    RL_CHECK_STR(out.AsString(), std::string("11"));

    RL_CHECK_MSG(!store2.Exists("doomed"), "deleted key must stay deleted");
    RL_CHECK_MSG(store2.Exists("withttl"), "key with ttl must survive restart");
    RL_CHECK_MSG(store2.TtlMs("withttl") > 0, "TTL must survive restart as absolute time");

    store2.Shutdown();
    RemoveFile(path);
}

RL_TEST(aof_nx_failure_is_not_persisted) {
    // SET NX 失败时不能写 AOF，否则重启后行为会与在线时不一致。
    const std::string path = TempPath("nxfail");
    RemoveFile(path);

    std::string err;
    Stats stats;
    Store store;
    store.Init(4, &stats);
    Aof aof;
    RL_CHECK(aof.Open(path, AofSyncPolicy::kAlways, &err));

    ServerContext ctx;
    ctx.store = &store;
    ctx.aof = &aof;
    ctx.stats = &stats;
    Dispatcher d(ctx);

    std::string reply;
    std::vector<std::string> a1 = {"SET", "k", "v1", "NX"};
    d.Execute(&a1, &reply);
    RL_CHECK_STR(reply, std::string("+OK\r\n"));

    std::vector<std::string> a2 = {"SET", "k", "v2", "NX"};
    d.Execute(&a2, &reply);
    RL_CHECK_STR(reply, std::string("$-1\r\n"));  // nil

    aof.Close();
    store.Shutdown();

    // 恢复后 k 必须是 v1。
    std::vector<std::vector<std::string>> cmds;
    size_t truncated = 0;
    RL_CHECK(Aof::LoadAll(path, &cmds, &err, &truncated));
    RL_CHECK_EQ(cmds.size(), size_t(1));
    RL_CHECK_STR(cmds[0][2], std::string("v1"));

    RemoveFile(path);
}

RL_TEST(aof_expire_persisted_as_absolute_time) {
    // EXPIRE 落盘必须转成 PEXPIREAT，否则重放时相对时间会被重新计算。
    const std::string path = TempPath("expire");
    RemoveFile(path);

    std::string err;
    Stats stats;
    Store store;
    store.Init(4, &stats);
    Aof aof;
    RL_CHECK(aof.Open(path, AofSyncPolicy::kAlways, &err));

    ServerContext ctx;
    ctx.store = &store;
    ctx.aof = &aof;
    ctx.stats = &stats;
    Dispatcher d(ctx);

    std::string reply;
    std::vector<std::string> a1 = {"SET", "k", "v"};
    d.Execute(&a1, &reply);
    std::vector<std::string> a2 = {"EXPIRE", "k", "100"};
    d.Execute(&a2, &reply);
    RL_CHECK_STR(reply, std::string(":1\r\n"));

    aof.Close();
    store.Shutdown();

    std::vector<std::vector<std::string>> cmds;
    size_t truncated = 0;
    RL_CHECK(Aof::LoadAll(path, &cmds, &err, &truncated));
    RL_CHECK_EQ(cmds.size(), size_t(2));
    RL_CHECK_STR(cmds[1][0], std::string("PEXPIREAT"));
    RL_CHECK_STR(cmds[1][1], std::string("k"));

    RemoveFile(path);
}
