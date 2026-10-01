// tests/unit/test.h — minimal dependency-free test harness for vmtouch core.
//
// Design notes:
//  * Zero dependencies: the whole unit-test suite builds with the same
//    toolchain as the library (CI installs nothing extra).
//  * Each test is a function registered via TEST(name) { ... }.
//  * ASSERT_* aborts the current test on failure; EXPECT_* records and
//    continues.
//  * Setup helpers shared by several suites live in helpers.h.
//
// Scenario lens (see docs/embedder-scenarios.md):
//  * unit tests here serve S1-S3; friction with the current API found while
//    writing them is tagged "// scenario:" for the Phase-3 API discussion.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace unit {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

inline const char*& current_test() {
    static const char* c = "?";
    return c;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int run_all() {
    int failed_tests = 0;
    for (const auto& tc : registry()) {
        current_test() = tc.name;
        int before = failures();
        tc.fn();
        if (failures() > before) {
            failed_tests++;
            std::printf("FAIL %s\n", tc.name);
        } else {
            std::printf("ok   %s\n", tc.name);
        }
    }
    std::printf("\n%zu checks failed in %d/%zu tests\n",
                static_cast<size_t>(failures()), failed_tests, registry().size());
    return failed_tests == 0 ? 0 : 1;
}

} // namespace unit

#define UNIT_CONCAT_(a, b) a##b
#define UNIT_CONCAT(a, b) UNIT_CONCAT_(a, b)

#define TEST(name)                                                        \
    static void UNIT_CONCAT(unit_test_fn_, __LINE__)();                   \
    static ::unit::Registrar UNIT_CONCAT(unit_reg_, __LINE__)(            \
        #name, &UNIT_CONCAT(unit_test_fn_, __LINE__));                    \
    static void UNIT_CONCAT(unit_test_fn_, __LINE__)()

#define UNIT_FAIL(fmt, ...)                                               \
    do {                                                                  \
        ::unit::failures()++;                                             \
        std::printf("  at %s:%d: " fmt "\n", __FILE__, __LINE__,          \
                    __VA_ARGS__);                                         \
    } while (0)

#define EXPECT_EQ(a, b)                                                   \
    do {                                                                  \
        auto va_ = (a);                                                   \
        auto vb_ = (b);                                                   \
        if (!(va_ == vb_))                                                \
            UNIT_FAIL("EXPECT_EQ failed: '%s' != '%s'",                   \
                      std::to_string(va_).c_str(),                        \
                      std::to_string(vb_).c_str());                       \
    } while (0)

#define EXPECT_TRUE(x)                                                    \
    do {                                                                  \
        if (!(x)) UNIT_FAIL("EXPECT_TRUE failed: %s", #x);                \
    } while (0)

#define EXPECT_FALSE(x) EXPECT_TRUE(!(x))

#define EXPECT_STREQ(a, b)                                                \
    do {                                                                  \
        std::string sa_ = (a);                                            \
        std::string sb_ = (b);                                            \
        if (sa_ != sb_)                                                   \
            UNIT_FAIL("EXPECT_STREQ failed: '%s' != '%s'",                \
                      sa_.c_str(), sb_.c_str());                          \
    } while (0)

#define EXPECT_THROWS_AS(stmt, ExType)                                    \
    do {                                                                  \
        bool threw_ = false;                                              \
        try { stmt; }                                                     \
        catch (const ExType&) { threw_ = true; }                          \
        catch (...) {}                                                    \
        if (!threw_) UNIT_FAIL("expected %s was not thrown", #ExType);    \
    } while (0)
