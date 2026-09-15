// Copyright (c) 2025 redis-lite authors. MIT License.
//
// log.h — 极简线程安全日志
//
// 刻意不引入 glog/spdlog：这个项目的卖点是"自己实现"，依赖越少越好，
// 面试时也更容易解释每一行的存在理由。

#ifndef RL_LOG_H
#define RL_LOG_H

// MinGW 默认的 msvcrt printf 不认 %zu / %lld / %llu，而 glibc 认。
// 与其在两边写两套格式串，不如统一把整数转成字符串再用 %s。
// 代价是几十字节的临时分配，但日志本来就不在热路径上；
// 收益是「同一份代码在 Windows 与 Linux 上输出完全一致」。
#include <string>

namespace rl {
inline std::string Num(long long v) { return std::to_string(v); }
inline std::string Num(unsigned long long v) { return std::to_string(v); }
inline std::string Num(long v) { return std::to_string(v); }
inline std::string Num(unsigned long v) { return std::to_string(v); }
inline std::string Num(int v) { return std::to_string(v); }
inline std::string Num(unsigned int v) { return std::to_string(v); }
}  // namespace rl

#include <cstdio>
#include <string>

namespace rl {

enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();
bool ParseLogLevel(const std::string& s, LogLevel* out);

// 线程安全，带时间戳与级别前缀。
void LogMessage(LogLevel level, const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

// 输出到 stderr 的致命错误，随后应终止进程。
void LogFatal(const char* file, int line, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

}  // namespace rl

#define RL_DEBUG(...) ::rl::LogMessage(::rl::LogLevel::kDebug, __FILE__, __LINE__, __VA_ARGS__)
#define RL_INFO(...) ::rl::LogMessage(::rl::LogLevel::kInfo, __FILE__, __LINE__, __VA_ARGS__)
#define RL_WARN(...) ::rl::LogMessage(::rl::LogLevel::kWarn, __FILE__, __LINE__, __VA_ARGS__)
#define RL_ERROR(...) ::rl::LogMessage(::rl::LogLevel::kError, __FILE__, __LINE__, __VA_ARGS__)
#define RL_FATAL(...) ::rl::LogFatal(__FILE__, __LINE__, __VA_ARGS__)

#endif  // RL_LOG_H
