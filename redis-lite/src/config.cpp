// Copyright (c) 2025 redis-lite authors. MIT License.
//
// config.cpp

#include "rl/config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rl {

namespace {

// 把 "--key=value" 或 "--key value" 拆成 key / value。
// 返回 false 表示该参数需要 value 但缺失。
bool SplitArg(const std::vector<std::string>& argv, size_t* i, std::string* key,
              std::string* value) {
    const std::string& arg = argv[*i];
    if (arg.size() < 2 || arg[0] != '-' || arg[1] != '-') return false;

    std::string body = arg.substr(2);
    size_t eq = body.find('=');
    if (eq != std::string::npos) {
        *key = body.substr(0, eq);
        *value = body.substr(eq + 1);
        return true;
    }

    *key = body;
    if (*i + 1 < argv.size()) {
        *value = argv[++(*i)];
        return true;
    }
    *value = "";
    return true;
}

bool ParseU64(const std::string& s, uint64_t* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    *out = static_cast<uint64_t>(v);
    return true;
}

}  // namespace

bool Config::Parse(int argc, char** argv, Config* out, bool* show_help) {
    *show_help = false;

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];

        if (a == "-h" || a == "--help") {
            *show_help = true;
            return true;
        }

        std::string key, value;
        if (!SplitArg(args, &i, &key, &value)) {
            std::fprintf(stderr, "unrecognized argument: %s\n", a.c_str());
            return false;
        }

        if (key == "port") {
            uint64_t v = 0;
            if (!ParseU64(value, &v) || v == 0 || v > 65535) {
                std::fprintf(stderr, "invalid --port: %s\n", value.c_str());
                return false;
            }
            out->port = static_cast<uint16_t>(v);
        } else if (key == "bind") {
            out->bind_addr = value;
        } else if (key == "threads" || key == "loops") {
            uint64_t v = 0;
            if (!ParseU64(value, &v) || v > 256) {
                std::fprintf(stderr, "invalid --threads: %s\n", value.c_str());
                return false;
            }
            out->loop_count = static_cast<size_t>(v);
        } else if (key == "shards") {
            uint64_t v = 0;
            if (!ParseU64(value, &v) || v == 0 || v > 65536) {
                std::fprintf(stderr, "invalid --shards: %s\n", value.c_str());
                return false;
            }
            out->shard_count = static_cast<size_t>(v);
        } else if (key == "aof") {
            out->aof_enabled = true;
            if (!value.empty() && value != "yes" && value != "true" && value != "1") {
                out->aof_path = value;
            }
        } else if (key == "aof-fsync") {
            if (!ParseAofPolicy(value, &out->aof_policy)) {
                std::fprintf(stderr, "invalid --aof-fsync: %s (always|everysec|no)\n",
                             value.c_str());
                return false;
            }
        } else if (key == "log-level") {
            if (!ParseLogLevel(value, &out->log_level)) {
                std::fprintf(stderr, "invalid --log-level: %s (debug|info|warn|error)\n",
                             value.c_str());
                return false;
            }
        } else {
            std::fprintf(stderr, "unknown option: --%s\n", key.c_str());
            return false;
        }
    }

    return true;
}

std::string Config::Usage(const char* prog) {
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
                  "redis-lite — 一个用于秋招简历的 C++ 高性能 KV 服务\n"
                  "\n"
                  "用法: %s [options]\n"
                  "\n"
                  "  --port <n>          监听端口 (默认 6379)\n"
                  "  --bind <ip>         绑定地址 (默认 127.0.0.1)\n"
                  "  --threads <n>       Sub Reactor 线程数, 0=按 CPU 核数 (默认 0)\n"
                  "  --shards <n>        存储分片数 (默认 64)\n"
                  "  --aof [path]        开启 AOF 持久化 (默认 appendonly.aof)\n"
                  "  --aof-fsync <mode>  always | everysec | no (默认 everysec)\n"
                  "  --log-level <lvl>   debug | info | warn | error (默认 info)\n"
                  "  -h, --help          显示本帮助\n"
                  "\n"
                  "示例:\n"
                  "  %s --port 7000 --threads 4 --aof data.aof\n",
                  prog, prog);
    return std::string(buf);
}

}  // namespace rl
