// Copyright (c) 2025 redis-lite authors. MIT License.
//
// command.h — 命令分派层
//
// 职责边界：只做「命令名 → 处理函数」的映射与参数校验，不关心网络。
// 这让命令层可以直接被单元测试驱动，也是 AOF 重放能复用它的原因 ——
// 恢复路径与线上路径执行的是同一份代码。

#ifndef RL_COMMAND_H
#define RL_COMMAND_H

#include <string>
#include <vector>

#include "rl/aof.h"
#include "rl/store.h"

namespace rl {

class Stats;

// 一次命令执行所需的全部依赖。构造开销极小（几个引用），每次执行现构造即可。
// 显式传递而不是全局单例：命令层因此可以脱离整个服务端单测。
struct ServerContext {
    Store* store = nullptr;
    Aof* aof = nullptr;
    Stats* stats = nullptr;
    std::string version = "1.0.0";
    std::string config_path = "";
    int64_t port = 0;
    size_t shard_count = 0;
    size_t loop_count = 0;
};

class Dispatcher {
public:
    explicit Dispatcher(ServerContext ctx) : ctx_(ctx) {}

    // 执行一条命令，把 RESP 编码结果追加到 out。
    // 无论成功失败都会写入一段合法的 RESP —— 调用方不需要处理异常。
    // args 非 const：命令名会被归一化成小写，省掉每个 handler 重复转换。
    void Execute(std::vector<std::string>* args, std::string* out);

    // 重放 AOF 中的历史命令。与 Execute 的唯一区别是「不再写回 AOF」，
    // 否则启动重放会把文件写成自己的两倍长。
    void Replay(const std::vector<std::vector<std::string>>& commands);

    // 从磁盘加载 AOF 并重放。返回成功重放的命令数，-1 表示加载失败。
    int LoadAof(const std::string& path);

    ServerContext* context() { return &ctx_; }

private:
    ServerContext ctx_;
};

}  // namespace rl

#endif  // RL_COMMAND_H
