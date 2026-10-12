// SPDX-License-Identifier: Apache-2.0
//
// Minimal self-contained test harness.
//
// Phase 0 deliberately vendors NO third-party testing framework:
//   * Configuring must work air-gapped (see docs/research/licenses.md).
//   * The build must remain auditable end to end.
//
// If P5EMU later adopts GoogleTest / Catch2, the switch happens in tests/CMakeLists.txt
// and this file is deleted. Until then, keep this file smaller than the thing
// it replaces.

#ifndef P5EMU_TESTS_TEST_HARNESS_H
#define P5EMU_TESTS_TEST_HARNESS_H

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace p5emu::test {

struct TestCase {
  const char* suite;
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& Registry();

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)()) {
    Registry().push_back(TestCase{suite, name, fn});
  }
};

// Set by the CHECK/REQUIRE macros. Deliberately file/line granular so a failure
// names the exact assertion.
struct CurrentTest {
  int failures = 0;
};

CurrentTest& Current();

void ReportFailure(const char* file, int line, const std::string& message);

int RunAll(int argc, char** argv);

}  // namespace p5emu::test

#define P5EMU_TEST(suite, name)                                             \
  static void suite##_##name##_body();                                      \
  static const ::p5emu::test::Registrar suite##_##name##_registrar(         \
      #suite, #name, &suite##_##name##_body);                               \
  static void suite##_##name##_body()

// CHECK: records a failure and keeps going.
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ::p5emu::test::ReportFailure(__FILE__, __LINE__,                      \
                                   "CHECK failed: " #cond);                  \
    }                                                                        \
  } while (false)

#define CHECK_EQ(actual, expected)                                           \
  do {                                                                       \
    const auto p5emu_a = (actual);                                           \
    const auto p5emu_b = (expected);                                         \
    if (!(p5emu_a == p5emu_b)) {                                             \
      ::p5emu::test::ReportFailure(                                          \
          __FILE__, __LINE__,                                                \
          std::string("CHECK_EQ failed: " #actual " == " #expected));       \
    }                                                                        \
  } while (false)

// REQUIRE: records a failure and aborts the current test case. Use when
// continuing would crash or produce noise.
#define REQUIRE(cond)                                                        \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ::p5emu::test::ReportFailure(__FILE__, __LINE__,                      \
                                   "REQUIRE failed: " #cond);                \
      return;                                                                \
    }                                                                        \
  } while (false)

#endif  // P5EMU_TESTS_TEST_HARNESS_H
