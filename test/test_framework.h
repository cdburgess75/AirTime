#pragma once
//
// Tiny dependency-free unit-test harness for the AirTime core.
//
// Deliberately zero third-party deps: it builds with nothing but a C++17
// compiler, so `make test` works offline in any environment. Tests self-register
// via AT_TEST(); main() calls airtime_test::run_all().

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace airtime_test {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}
inline int& check_count() {
  static int c = 0;
  return c;
}
inline int& fail_count() {
  static int f = 0;
  return f;
}
inline bool& current_failed() {
  static bool f = false;
  return f;
}

struct Reg {
  Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

inline void report_fail(const char* file, int line, const std::string& msg) {
  ++fail_count();
  current_failed() = true;
  std::printf("      FAIL %s:%d  %s\n", file, line, msg.c_str());
}

inline bool is_near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

inline int run_all() {
  int passed = 0, failed = 0;
  for (auto& c : registry()) {
    current_failed() = false;
    c.fn();
    if (current_failed()) {
      ++failed;
      std::printf("  [FAIL] %s\n", c.name);
    } else {
      ++passed;
      std::printf("  [ ok ] %s\n", c.name);
    }
  }
  std::printf("\n==== %d passed, %d failed, %d checks ====\n", passed, failed,
              check_count());
  return failed == 0 ? 0 : 1;
}

}  // namespace airtime_test

#define AT_TEST(name)                                                      \
  static void name();                                                      \
  static ::airtime_test::Reg at_reg_##name(#name, name);                   \
  static void name()

#define AT_CHECK(cond)                                                     \
  do {                                                                     \
    ++::airtime_test::check_count();                                       \
    if (!(cond))                                                           \
      ::airtime_test::report_fail(__FILE__, __LINE__,                      \
                                  std::string("AT_CHECK failed: " #cond)); \
  } while (0)

#define AT_CHECK_EQ(a, b)                                                       \
  do {                                                                          \
    ++::airtime_test::check_count();                                            \
    auto at_a = (a);                                                            \
    auto at_b = (b);                                                            \
    if (!(at_a == at_b)) {                                                      \
      char at_buf[256];                                                         \
      std::snprintf(at_buf, sizeof(at_buf),                                     \
                    "AT_CHECK_EQ failed: %s == %s  (%lld vs %lld)", #a, #b,     \
                    static_cast<long long>(at_a), static_cast<long long>(at_b));\
      ::airtime_test::report_fail(__FILE__, __LINE__, at_buf);                  \
    }                                                                           \
  } while (0)

#define AT_CHECK_NEAR(a, b, tol)                                                \
  do {                                                                          \
    ++::airtime_test::check_count();                                            \
    if (!::airtime_test::is_near((a), (b), (tol))) {                            \
      char at_buf[256];                                                         \
      std::snprintf(at_buf, sizeof(at_buf),                                     \
                    "AT_CHECK_NEAR failed: %s (%.6g) ~= %s (%.6g) tol %.3g",    \
                    #a, static_cast<double>(a), #b, static_cast<double>(b),     \
                    static_cast<double>(tol));                                  \
      ::airtime_test::report_fail(__FILE__, __LINE__, at_buf);                  \
    }                                                                           \
  } while (0)
