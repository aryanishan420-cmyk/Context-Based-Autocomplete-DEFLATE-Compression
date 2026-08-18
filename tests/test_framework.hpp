// A very small test registry -- enough to keep the suite dependency-free.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body) {
    registry().push_back({suite, name, std::move(body)});
  }
};

struct Failure {
  std::string message;
};

inline void fail(const char* file, int line, const std::string& message) {
  throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + message};
}

}  // namespace testing

#define TEST(suite, name)                                                        \
  static void suite##_##name##_body();                                           \
  static testing::Registrar suite##_##name##_registrar(#suite, #name,            \
                                                       suite##_##name##_body);   \
  static void suite##_##name##_body()

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) testing::fail(__FILE__, __LINE__, "CHECK failed: " #cond);      \
  } while (0)

#define CHECK_EQ(a, b)                                                           \
  do {                                                                           \
    const auto& _a = (a);                                                        \
    const auto& _b = (b);                                                        \
    if (!(_a == _b))                                                             \
      testing::fail(__FILE__, __LINE__,                                          \
                    "CHECK_EQ failed: " #a " != " #b);                           \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                    \
  do {                                                                           \
    if (std::fabs((a) - (b)) > (tol))                                            \
      testing::fail(__FILE__, __LINE__, "CHECK_NEAR failed: " #a " vs " #b);     \
  } while (0)

#define CHECK_THROWS(expr)                                                       \
  do {                                                                           \
    bool _threw = false;                                                         \
    try {                                                                        \
      (void)(expr);                                                              \
    } catch (const std::exception&) {                                            \
      _threw = true;                                                             \
    }                                                                            \
    if (!_threw)                                                                 \
      testing::fail(__FILE__, __LINE__, "CHECK_THROWS failed: " #expr);          \
  } while (0)
