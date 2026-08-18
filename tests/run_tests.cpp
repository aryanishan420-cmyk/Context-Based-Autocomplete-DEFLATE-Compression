#include <cstdio>
#include <exception>
#include <string>

#include "test_framework.hpp"

int main(int argc, char** argv) {
  const std::string filter = argc > 1 ? argv[1] : "";
  int passed = 0, failed = 0, skipped = 0;

  for (const testing::TestCase& test : testing::registry()) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    try {
      test.body();
      std::printf("  ok    %s\n", full.c_str());
      ++passed;
    } catch (const testing::Failure& f) {
      std::printf("  FAIL  %s\n        %s\n", full.c_str(), f.message.c_str());
      ++failed;
    } catch (const std::exception& e) {
      std::printf("  FAIL  %s\n        unexpected exception: %s\n", full.c_str(), e.what());
      ++failed;
    }
  }

  std::printf("\n%d passed, %d failed", passed, failed);
  if (skipped) std::printf(", %d filtered out", skipped);
  std::printf("\n");
  return failed == 0 ? 0 : 1;
}
