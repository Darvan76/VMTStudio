#pragma once

// ut.h - a tiny, self-contained, header-only test framework for VMTViewer.
//
// Rationale (Phase 1): the project must configure and build from a clean
// checkout with no network access and no third-party download step. doctest /
// Catch2 are the intended long-term choice, but they cannot be vendored here
// as a complete file in this phase, so the skeleton ships this ~90-line
// harness. It can be replaced by doctest later without touching test bodies
// (only the TEST_CASE / CHECK macro spellings would change).
//
// Usage:
//   #include "ut.h"
//   UT_TEST_CASE("name") { UT_CHECK(1 + 1 == 2); }
//   // and one translation unit provides:
//   int main() { return ut::run_all(); }

#include <cstddef>
#include <cstdio>
#include <vector>

namespace ut {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) {
        registry().push_back(TestCase{ name, fn });
    }
};

inline int g_checks = 0;
inline int g_failures = 0;

inline int run_all() {
    const std::size_t total = registry().size();
    std::printf("vtm test harness: running %zu test case(s)\n", total);

    int failed_cases = 0;
    for (const TestCase& test : registry()) {
        const int failures_before = g_failures;
        test.fn();
        const bool passed = (g_failures == failures_before);
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", test.name);
        if (!passed) {
            ++failed_cases;
        }
    }

    std::printf("vtm test harness: %zu test case(s), %d check(s), %d failure(s)\n",
                total, g_checks, g_failures);
    return (failed_cases == 0 && g_failures == 0) ? 0 : 1;
}

} // namespace ut

#define UT_CONCAT_INNER(a, b) a##b
#define UT_CONCAT(a, b) UT_CONCAT_INNER(a, b)

#define UT_TEST_CASE(name)                                                    \
    static void UT_CONCAT(ut_test_fn_, __LINE__)();                           \
    static ::ut::Registrar UT_CONCAT(ut_test_reg_, __LINE__)(                 \
        name, &UT_CONCAT(ut_test_fn_, __LINE__));                             \
    static void UT_CONCAT(ut_test_fn_, __LINE__)()

#define UT_CHECK(expr)                                                        \
    do {                                                                      \
        ++::ut::g_checks;                                                     \
        if (!(expr)) {                                                        \
            ++::ut::g_failures;                                               \
            std::printf("  [FAIL] %s:%d: CHECK(%s)\n",                        \
                        __FILE__, __LINE__, #expr);                           \
        }                                                                     \
    } while (0)

#define UT_CHECK_EQ(a, b)                                                     \
    do {                                                                      \
        ++::ut::g_checks;                                                     \
        if (!((a) == (b))) {                                                  \
            ++::ut::g_failures;                                               \
            std::printf("  [FAIL] %s:%d: CHECK_EQ(%s, %s)\n",                 \
                        __FILE__, __LINE__, #a, #b);                          \
        }                                                                     \
    } while (0)
