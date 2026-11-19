#include "test_harness.h"

#include <exception>
#include <iostream>

int main()
{
    const auto& tests = SkyrimPoE::Tests::Registry();
    std::size_t passed = 0;
    std::size_t failed = 0;

    for (const auto& t : tests) {
        try {
            t.fn();
            ++passed;
            std::cout << "[PASS] " << t.name << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "[FAIL] " << t.name << " -> " << e.what() << "\n";
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << t.name << " -> unknown exception\n";
        }
    }

    std::cout << "Summary: " << passed << " passed, " << failed << " failed (total " << tests.size() << ").\n";
    return failed == 0 ? 0 : 1;
}
