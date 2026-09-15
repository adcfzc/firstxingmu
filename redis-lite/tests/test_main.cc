// Copyright (c) 2025 redis-lite authors. MIT License.
//
// test_main.cc — 极简测试框架
//
// 不引 gtest：整个项目保持零第三方依赖，clone 下来就能编译。
// 面试时这一点可以主动说：「我刻意不引测试框架，因为核心被测对象是
// 自己实现的协议解析器，用框架反而掩盖了断言本身想验证的边界条件」。

// 本文件负责生成 main()，其余测试文件不要定义这个宏。
#define RL_TEST_MAIN 1

#include "test_util.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace rl {
namespace test {

std::vector<TestCase>* TestRegistry() {
    static std::vector<TestCase> registry;
    return &registry;
}

int& FailureCount() {
    static int count = 0;
    return count;
}

int& CheckCount() {
    static int count = 0;
    return count;
}

void ReportFailure(const char* file, int line, const std::string& msg) {
    ++FailureCount();
    std::fprintf(stderr, "  FAIL %s:%d\n       %s\n", file, line, msg.c_str());
}

}  // namespace test
}  // namespace rl

int main(int argc, char** argv) {
    using namespace rl::test;

    std::string filter;
    if (argc > 1) filter = argv[1];

    int ran = 0;
    int failed_cases = 0;

    for (const TestCase& tc : *TestRegistry()) {
        if (!filter.empty() && std::string(tc.name).find(filter) == std::string::npos) {
            continue;
        }

        const int before = FailureCount();
        std::printf("[ RUN  ] %s\n", tc.name);

        try {
            tc.fn();
        } catch (const std::exception& e) {
            ReportFailure(__FILE__, __LINE__,
                          std::string("uncaught exception: ") + e.what());
        } catch (...) {
            ReportFailure(__FILE__, __LINE__, "uncaught unknown exception");
        }

        ++ran;
        const int delta = FailureCount() - before;
        if (delta == 0) {
            std::printf("[  OK  ] %s\n", tc.name);
        } else {
            ++failed_cases;
            std::printf("[ FAIL ] %s (%d checks failed)\n", tc.name, delta);
        }
    }

    std::printf("\n========================================\n");
    std::printf("cases: %d run, %d failed\n", ran, failed_cases);
    std::printf("checks: %d executed, %d failed\n", CheckCount(), FailureCount());
    std::printf("========================================\n");

    return failed_cases == 0 ? 0 : 1;
}
