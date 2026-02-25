#include "bbl.h"
#include <iostream>

int passed = 0, failed = 0;

#define TEST(name) static void name()
#define RUN(name) do { \
    std::cout << "  " << #name << std::endl; \
    name(); \
} while(0)
#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        std::cerr << "  FAIL: " << #a << " == " << #b \
                  << " (got '" << _a << "' vs '" << _b << "') at " \
                  << __FILE__ << ":" << __LINE__ << std::endl; \
        failed++; \
    } else { passed++; } \
} while(0)

// Tests go here.

int main() {
    std::cout << "=== bbl ===" << std::endl;

    // RUN(test_name);

    std::cout << "\nPassed: " << passed << "  Failed: " << failed << std::endl;
    return failed > 0 ? 1 : 0;
}
