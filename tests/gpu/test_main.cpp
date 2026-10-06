#include "test_harness.h"

namespace TestHarness {
    int g_failures = 0;
}

int main() {
    int failedTests = 0;
    for (const TestHarness::TestCase& test : TestHarness::Registry()) {
        TestHarness::g_failures = 0;
        std::printf("[ RUN  ] %s\n", test.name);
        test.fn();
        if (TestHarness::g_failures == 0) {
            std::printf("[  OK  ] %s\n", test.name);
        } else {
            std::printf("[ FAIL ] %s (%d checks)\n", test.name, TestHarness::g_failures);
            ++failedTests;
        }
    }
    std::printf("\n%zu testes, %d falharam\n", TestHarness::Registry().size(), failedTests);
    return failedTests == 0 ? 0 : 1;
}
