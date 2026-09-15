// Copyright (c) 2025 redis-lite authors. MIT License.
//
// main.cc — 进程入口
//
// 这一层只做三件事：解析参数、装信号处理、把控制权交给 Server。
// 任何业务逻辑都不应该出现在这里。
//
// 注意：MinGW 链接时需要显式要求 console 子系统，否则会去找 WinMain。
// 见 scripts/build.bat（-mconsole）。

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "rl/config.h"
#include "rl/log.h"
#include "rl/platform.h"
#include "rl/server.h"

namespace {

// 信号处理器里只能操作 volatile sig_atomic_t 或 lock-free 原子量，
// 不能加锁、不能分配内存、不能调用 printf。这里只置一个标志。
std::atomic<bool> g_stop{false};

void OnSignal(int sig) {
    (void)sig;
    g_stop.store(true, std::memory_order_relaxed);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace rl;

    Config config;
    bool show_help = false;
    if (!Config::Parse(argc, argv, &config, &show_help)) {
        std::fprintf(stderr, "\n%s", Config::Usage(argv[0]).c_str());
        return 1;
    }
    if (show_help) {
        std::printf("%s", Config::Usage(argv[0]).c_str());
        return 0;
    }

    SetLogLevel(config.log_level);

    // Windows 下必须 WSAStartup；Linux 下是空操作。
    std::string err;
    if (!InitSockets(&err)) {
        std::fprintf(stderr, "socket init failed: %s\n", err.c_str());
        return 1;
    }

    // SIGINT/SIGTERM：优雅退出，退出前把 AOF 缓冲刷干净。
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#if !RL_WINDOWS
    // 写已关闭的 socket 会收到 SIGPIPE，默认行为是直接杀进程。
    // 我们必须忽略它，让 send() 返回 EPIPE 从而走正常的错误处理路径。
    std::signal(SIGPIPE, SIG_IGN);
#endif

    Server server(config);
    if (!server.Init(&err)) {
        std::fprintf(stderr, "server init failed: %s\n", err.c_str());
        CleanupSockets();
        return 1;
    }
    if (!server.Start(&err)) {
        std::fprintf(stderr, "server start failed: %s\n", err.c_str());
        CleanupSockets();
        return 1;
    }

    // 每 100ms 检查一次信号标志。用回调而不是另起线程：
    // 唤醒频率相同，但少一个线程和一套同步。
    server.Run([] { return !g_stop.load(std::memory_order_relaxed); });
    server.Shutdown();

    CleanupSockets();
    RL_INFO("bye");
    return 0;
}
