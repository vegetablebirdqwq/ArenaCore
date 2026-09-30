#pragma once

// 极简单测框架：不引入任何第三方依赖（本机网络很慢，拉 GTest/Catch2 不现实）。
// 用法：
//     TEST(ring_buffer_append_and_consume) { ... CHECK(...); }
// 全部用例注册到一个静态表，由 main 调用 test::run_all()。

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back(Case{name, std::move(fn)}); }
};

inline int run_all() {
    int total = 0;
    for (const auto& c : registry()) {
        const int before = failure_count();
        std::printf("[ RUN      ] %s\n", c.name);
        c.fn();
        ++total;
        std::printf("[ %s ] %s\n", failure_count() == before ? "    OK   " : "  FAILED ", c.name);
    }
    std::printf("\n%d test case(s), %d assertion failure(s)\n", total, failure_count());
    return failure_count() == 0 ? 0 : 1;
}

}  // namespace test

#define TEST(test_name)                                                  \
    static void test_name();                                             \
    static ::test::Registrar registrar_##test_name(#test_name, test_name); \
    static void test_name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++::test::failure_count();                                            \
            std::printf("  [FAIL] %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                    \
    do {                                                                                              \
        const auto& a_ = (actual);                                                                    \
        const auto& e_ = (expected);                                                                  \
        if (!(a_ == e_)) {                                                                            \
            ++::test::failure_count();                                                                \
            std::printf("  [FAIL] %s:%d  %s != %s\n", __FILE__, __LINE__, #actual, #expected);        \
        }                                                                                             \
    } while (0)
