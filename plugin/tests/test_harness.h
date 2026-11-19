#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace SkyrimPoE::Tests
{
    struct TestCase
    {
        std::string name;
        std::function<void()> fn;
    };

    inline std::vector<TestCase>& Registry()
    {
        static std::vector<TestCase> cases;
        return cases;
    }

    struct AutoRegistrar
    {
        AutoRegistrar(const char* name, std::function<void()> fn)
        {
            Registry().push_back(TestCase{ name, std::move(fn) });
        }
    };

    inline void Fail(const char* expr, const char* file, int line, const std::string& detail = {})
    {
        std::ostringstream oss;
        oss << file << ":" << line << ": assertion failed: " << expr;
        if (!detail.empty()) {
            oss << " (" << detail << ")";
        }
        throw std::runtime_error(oss.str());
    }
}

#define POE_TEST_CONCAT_IMPL(a, b) a##b
#define POE_TEST_CONCAT(a, b) POE_TEST_CONCAT_IMPL(a, b)

#define POE_TEST(name)                                                                            \
    static void POE_TEST_CONCAT(poe_test_fn_, __LINE__)();                                        \
    static const ::SkyrimPoE::Tests::AutoRegistrar POE_TEST_CONCAT(poe_test_reg_, __LINE__)(      \
        name, POE_TEST_CONCAT(poe_test_fn_, __LINE__));                                           \
    static void POE_TEST_CONCAT(poe_test_fn_, __LINE__)()

#define POE_CHECK(cond)                                                                           \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            ::SkyrimPoE::Tests::Fail(#cond, __FILE__, __LINE__);                                  \
        }                                                                                         \
    } while (false)

#define POE_CHECK_NEAR(actual, expected, eps)                                                     \
    do {                                                                                          \
        const double _a = static_cast<double>(actual);                                            \
        const double _e = static_cast<double>(expected);                                          \
        if (!std::isfinite(_a) || std::abs(_a - _e) > (eps)) {                                    \
            std::ostringstream _oss;                                                              \
            _oss << "actual=" << _a << ", expected=" << _e << ", eps=" << (eps);                  \
            ::SkyrimPoE::Tests::Fail(#actual " ~= " #expected, __FILE__, __LINE__, _oss.str());   \
        }                                                                                         \
    } while (false)
