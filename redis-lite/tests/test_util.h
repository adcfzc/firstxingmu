// Copyright (c) 2025 redis-lite authors. MIT License.
//
// test_util.h — 极简测试框架的公共头
//
// 用法：
//   每个测试文件顶部 #include "test_util.h"，用 RL_TEST(name) { ... } 定义用例。
//   只有一个文件（test_main.cc）在包含前定义 RL_TEST_MAIN 来生成 main()。

#ifndef RL_TEST_UTIL_H
#define RL_TEST_UTIL_H

#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace rl {
namespace test {

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>* TestRegistry();
int& FailureCount();
int& CheckCount();
void ReportFailure(const char* file, int line, const std::string& msg);

// 注册器：利用静态对象的构造函数在 main 之前完成注册。
struct Registrar {
    Registrar(const char* name, void (*fn)()) { TestRegistry()->push_back(TestCase{name, fn}); }
};

// 统一的比较包装：
//   算术类型 → 提升为 int64_t，避免 RL_CHECK_EQ(size, 3) 的无符号比较噪音
//   其他类型 → 按**值**返回
//
// ★ 必须按值返回，不能返回 const T&。
// 宏里写的是 `const auto& lhs = Num(a);`，若 Num 返回引用，
// 它会绑定到 Num 内部构造的临时对象上；该临时对象在语句结束即析构，
// lhs 随即悬垂 —— 实测表现为断言里读到一堆 \xb0\xf7 垃圾字节，
// 而真正的字符串比较本身是对的。这类 bug 极难从失败信息反推，务必避免。
template <typename T>
typename std::enable_if<std::is_arithmetic<T>::value, int64_t>::type Num(T v) {
    return static_cast<int64_t>(v);
}

template <typename T>
typename std::enable_if<!std::is_arithmetic<T>::value, T>::type Num(const T& v) {
    return v;
}

template <typename T>
std::string ToStr(const T& v) {
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

// 字符串专用：把 \r \n \0 转义出来。
// 不加这个，断言失败信息里的 RESP 报文会因为真实换行而"断成好几行"，
// 根本看不出差异在哪 —— 调试协议解析器时这点非常要命。
inline std::string ToStr(const std::string& v) {
    std::string out;
    out.reserve(v.size() + 8);
    out.push_back('"');
    for (char c : v) {
        switch (c) {
            case '\r':
                out += "\\r";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\0':
                out += "\\0";
                break;
            case '"':
                out += "\\\"";
                break;
            default:
                // 可见 ASCII 原样输出，其余用 \xHH。
                if (static_cast<unsigned char>(c) >= 0x20 &&
                    static_cast<unsigned char>(c) < 0x7f) {
                    out.push_back(c);
                } else {
                    static char hex[] = "0123456789abcdef";
                    out += "\\x";
                    out.push_back(hex[(static_cast<unsigned char>(c) >> 4) & 0xf]);
                    out.push_back(hex[static_cast<unsigned char>(c) & 0xf]);
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace test
}  // namespace rl

#define RL_TEST(name)                                                         \
    static void rl_test_##name();                                             \
    static ::rl::test::Registrar rl_registrar_##name(#name, &rl_test_##name); \
    static void rl_test_##name()

#define RL_CHECK(cond)                                                            \
    do {                                                                          \
        ++::rl::test::CheckCount();                                               \
        if (!(cond)) {                                                            \
            ::rl::test::ReportFailure(__FILE__, __LINE__, "CHECK failed: " #cond); \
        }                                                                         \
    } while (0)

#define RL_CHECK_EQ(a, b)                                                                \
    do {                                                                                 \
        ++::rl::test::CheckCount();                                                      \
        const auto rl_lhs_ = ::rl::test::Num(a);                                         \
        const auto rl_rhs_ = ::rl::test::Num(b);                                         \
        if (!(rl_lhs_ == rl_rhs_)) {                                                     \
            ::rl::test::ReportFailure(                                                   \
                __FILE__, __LINE__,                                                      \
                std::string("CHECK_EQ failed: " #a " == " #b "\n       got " +           \
                            ::rl::test::ToStr(rl_lhs_) + "\n       exp " +               \
                            ::rl::test::ToStr(rl_rhs_)));                                \
        }                                                                                \
    } while (0)

#define RL_CHECK_MSG(cond, msg)                                                \
    do {                                                                       \
        ++::rl::test::CheckCount();                                            \
        if (!(cond)) {                                                         \
            ::rl::test::ReportFailure(__FILE__, __LINE__,                      \
                                      std::string("CHECK failed: ") + (msg));  \
        }                                                                      \
    } while (0)

// 比较字符串、或需要自定义消息时使用。
#define RL_CHECK_STR(a, b)                                                    \
    do {                                                                      \
        ++::rl::test::CheckCount();                                           \
        const std::string rl_a_ = (a);                                        \
        const std::string rl_b_ = (b);                                        \
        if (rl_a_ != rl_b_) {                                                 \
            ::rl::test::ReportFailure(                                        \
                __FILE__, __LINE__,                                           \
                std::string("CHECK_STR failed: " #a " == " #b "\n       got " + \
                            ::rl::test::ToStr(rl_a_) + "\n       exp " +      \
                            ::rl::test::ToStr(rl_b_)));                       \
        }                                                                     \
    } while (0)

#endif  // RL_TEST_UTIL_H
