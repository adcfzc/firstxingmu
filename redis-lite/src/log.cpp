// Copyright (c) 2025 redis-lite authors. MIT License.
//
// log.cpp — 日志实现。

#include "rl/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "rl/platform.h"

namespace rl {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::kInfo)};
std::mutex g_log_mutex;

const char* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug:
            return "DEBUG";
        case LogLevel::kInfo:
            return "INFO ";
        case LogLevel::kWarn:
            return "WARN ";
        case LogLevel::kError:
            return "ERROR";
    }
    return "?????";
}

// 只保留文件名，去掉冗长的绝对路径。
const char* ShortName(const char* path) {
    const char* slash = std::strrchr(path, '/');
    const char* bslash = std::strrchr(path, '\\');
    const char* best = path;
    if (slash != nullptr && slash + 1 > best) best = slash + 1;
    if (bslash != nullptr && bslash + 1 > best) best = bslash + 1;
    return best;
}
}  // namespace

void SetLogLevel(LogLevel level) { g_level.store(static_cast<int>(level)); }

LogLevel GetLogLevel() { return static_cast<LogLevel>(g_level.load()); }

bool ParseLogLevel(const std::string& s, LogLevel* out) {
    if (s == "debug") {
        *out = LogLevel::kDebug;
        return true;
    }
    if (s == "info") {
        *out = LogLevel::kInfo;
        return true;
    }
    if (s == "warn" || s == "warning") {
        *out = LogLevel::kWarn;
        return true;
    }
    if (s == "error") {
        *out = LogLevel::kError;
        return true;
    }
    return false;
}

void LogMessage(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    std::string ts = NowString();
    std::lock_guard<std::mutex> lk(g_log_mutex);
    std::fprintf(stderr, "[%s] [%s] %s:%d %s\n", ts.c_str(), LevelTag(level), ShortName(file),
                 line, msg);
    std::fflush(stderr);
}

void LogFatal(const char* file, int line, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    std::string ts = NowString();
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        std::fprintf(stderr, "[%s] [FATAL] %s:%d %s\n", ts.c_str(), ShortName(file), line, msg);
        std::fflush(stderr);
    }
    std::abort();
}

}  // namespace rl
