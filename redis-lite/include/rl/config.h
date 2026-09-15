// Copyright (c) 2025 redis-lite authors. MIT License.
//
// config.h — 命令行 / 配置文件解析
//
// 刻意不引第三方库（gflags、CLI11），保持零依赖：
//   编译即用，面试时也能完整解释每一行参数处理逻辑。

#ifndef RL_CONFIG_H
#define RL_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "rl/aof.h"
#include "rl/log.h"

namespace rl {

struct Config {
    std::string bind_addr = "127.0.0.1";
    uint16_t port = 6379;

    // 0 = 按硬件线程数自动决定。
    size_t loop_count = 0;
    // 分片数，内部会向上取整到 2 的幂。
    size_t shard_count = 64;

    bool aof_enabled = false;
    std::string aof_path = "appendonly.aof";
    AofSyncPolicy aof_policy = AofSyncPolicy::kEverySec;

    LogLevel log_level = LogLevel::kInfo;

    // 作为命令行工具运行（解析失败时返回 false 并打印 usage）。
    static bool Parse(int argc, char** argv, Config* out, bool* show_help);

    // 同时支持 --key=value 与 --key value 两种写法。
    static std::string Usage(const char* prog);
};

}  // namespace rl

#endif  // RL_CONFIG_H
