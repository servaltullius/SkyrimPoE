#include "test_harness.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

int WritePassiveParseReport(const std::filesystem::path& tree, const std::filesystem::path& output,
    const std::filesystem::path& mixedOutput);
int WritePassiveUiFixture(const std::filesystem::path& output);

int wmain(int argc, wchar_t** argv)
{
    if ((argc == 4 || argc == 5) && std::wstring_view(argv[1]) == L"--write-passive-parse-report") {
        return WritePassiveParseReport(argv[2], argv[3], argc == 5 ? argv[4] : L"");
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--write-passive-ui-fixture") {
        return WritePassiveUiFixture(argv[2]);
    }
    if (argc != 1) {
        std::cerr << "Usage: skyrimpoe_tests [--write-passive-parse-report tree.json output.json [mixed.json]"
                     " | --write-passive-ui-fixture output.json]\n";
        return 2;
    }
    const auto& tests = SkyrimPoE::Tests::Registry();
    if (tests.empty()) {
        std::cerr << "[FAIL] No tests registered.\n";
        return 1;
    }
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
