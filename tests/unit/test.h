// tests/unit/test.h — minimal dependency-free test harness for vmtouch core.
//
// Design notes:
//  * Zero dependencies: the whole unit-test suite builds with the same
//    toolchain as the library (CI installs nothing extra).
//  * Each test is a function registered via TEST(name) { ... }.
//  * EXPECT_* records a failure and continues; SKIP ends the current test.
//  * Setup helpers shared by several suites live in helpers.h.
//
// Scenario lens for the in-tree API (see DESIGN.md):
//  * unit tests here serve S1-S3; friction with the current API found while
//    writing them is tagged "// scenario:" for the Phase-3 API discussion.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace unit {

// Stringify a value for failure messages: numbers via std::to_string,
// strings/paths verbatim, enums via their underlying integer.
template <class T>
std::string to_display(const T &v) {
  return std::to_string(v);
}

template <class E>
  requires std::is_enum_v<E>
std::string to_display(const E &v) {
  return std::to_string(static_cast<int>(v));
}

inline std::string to_display(const std::string &v) {
  return v;
}
inline std::string to_display(const char *v) {
  return v;
}
inline std::string to_display(char v) {
  return std::string(1, v);
}
inline std::string to_display(bool v) {
  return v ? "true" : "false";
}

struct TestCase {
  const char *name;
  void (*fn)();
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> r;
  return r;
}

inline int &failures() {
  static int f = 0;
  return f;
}

inline const char *&skip_reason() {
  static const char *reason = nullptr;
  return reason;
}

struct Registrar {
  Registrar(const char *name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int run_all() {
  int failed_tests = 0;
  int skipped_tests = 0;
  int passed_tests = 0;
  for (const auto &tc : registry()) {
    skip_reason() = nullptr;
    int before = failures();
    tc.fn();
    if (failures() > before) {
      failed_tests++;
      std::printf("FAIL %s\n", tc.name);
    } else if (skip_reason()) {
      skipped_tests++;
      std::printf("SKIP %s (%s)\n", tc.name, skip_reason());
    } else {
      passed_tests++;
      std::printf("ok   %s\n", tc.name);
    }
  }
  std::printf("\n%d passed, %d failed, %d skipped (%d failed checks)\n", passed_tests, failed_tests,
              skipped_tests, failures());
  return failed_tests == 0 ? 0 : 1;
}

} // namespace unit

#define UNIT_CONCAT_(a, b) a##b
#define UNIT_CONCAT(a, b) UNIT_CONCAT_(a, b)

#define TEST(name)                                                                                 \
  static void UNIT_CONCAT(unit_test_fn_, __LINE__)();                                              \
  static ::unit::Registrar UNIT_CONCAT(unit_reg_,                                                  \
                                       __LINE__)(#name, &UNIT_CONCAT(unit_test_fn_, __LINE__));    \
  static void UNIT_CONCAT(unit_test_fn_, __LINE__)()

// reason must remain valid until the runner prints it (use a string literal).
#define SKIP(reason)                                                                               \
  do {                                                                                             \
    ::unit::skip_reason() = (reason);                                                              \
    return;                                                                                        \
  } while (0)

#define UNIT_FAIL(fmt, ...)                                                                        \
  do {                                                                                             \
    ::unit::failures()++;                                                                          \
    std::printf("  at %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__);                         \
  } while (0)

#define EXPECT_EQ(a, b)                                                                            \
  do {                                                                                             \
    const auto &va_ = (a);                                                                         \
    const auto &vb_ = (b);                                                                         \
    if (!(va_ == vb_))                                                                             \
      UNIT_FAIL("EXPECT_EQ failed: '%s' != '%s'", ::unit::to_display(va_).c_str(),                 \
                ::unit::to_display(vb_).c_str());                                                  \
  } while (0)

#define EXPECT_TRUE(x)                                                                             \
  do {                                                                                             \
    if (!(x))                                                                                      \
      UNIT_FAIL("EXPECT_TRUE failed: %s", #x);                                                     \
  } while (0)

#define EXPECT_FALSE(x) EXPECT_TRUE(!(x))

#define EXPECT_STREQ(a, b)                                                                         \
  do {                                                                                             \
    std::string sa_ = (a);                                                                         \
    std::string sb_ = (b);                                                                         \
    if (sa_ != sb_)                                                                                \
      UNIT_FAIL("EXPECT_STREQ failed: '%s' != '%s'", sa_.c_str(), sb_.c_str());                    \
  } while (0)

#define EXPECT_THROWS_AS(stmt, ExType)                                                             \
  do {                                                                                             \
    bool threw_ = false;                                                                           \
    try {                                                                                          \
      stmt;                                                                                        \
    } catch (const ExType &) {                                                                     \
      threw_ = true;                                                                               \
    } catch (...) {                                                                                \
    }                                                                                              \
    if (!threw_)                                                                                   \
      UNIT_FAIL("expected %s was not thrown", #ExType);                                            \
  } while (0)
