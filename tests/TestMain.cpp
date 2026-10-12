// SPDX-License-Identifier: Apache-2.0

#include "TestHarness.h"

#include <cstdio>
#include <cstring>

namespace p5emu::test {

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

CurrentTest& Current() {
  static CurrentTest current;
  return current;
}

void ReportFailure(const char* file, int line, const std::string& message) {
  ++Current().failures;
  std::fprintf(stderr, "    %s:%d: %s\n", file, line, message.c_str());
}

int RunAll(int argc, char** argv) {
  const char* filter = (argc > 1) ? argv[1] : nullptr;

  int executed = 0;
  int failed_cases = 0;

  for (const TestCase& test : Registry()) {
    const std::string full = std::string(test.suite) + "." + test.name;
    if (filter != nullptr && full.find(filter) == std::string::npos) {
      continue;
    }

    std::fprintf(stderr, "[ RUN      ] %s\n", full.c_str());
    Current() = CurrentTest{};
    test.fn();
    ++executed;

    if (Current().failures != 0) {
      ++failed_cases;
      std::fprintf(stderr, "[   FAILED ] %s (%d assertion failure(s))\n",
                   full.c_str(), Current().failures);
    } else {
      std::fprintf(stderr, "[       OK ] %s\n", full.c_str());
    }
  }

  std::fprintf(stderr, "\n%d test(s) executed, %d failed\n", executed,
               failed_cases);
  return failed_cases == 0 ? 0 : 1;
}

}  // namespace p5emu::test

int main(int argc, char** argv) { return p5emu::test::RunAll(argc, argv); }
