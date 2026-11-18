#include "AutoTest.h"
#include "ProjectileObservation.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    using namespace ProjectileObservation;
    using namespace std::chrono_literals;

    void Require(bool passed, std::string_view check)
    {
        if (!passed) {
            std::cerr << "FAIL: " << check << '\n';
            std::exit(EXIT_FAILURE);
        }
    }

    void VerifyDirectCall()
    {
        constexpr std::uintptr_t site = 0x1405C00B4;
        Require(!DirectCallTarget(site, { 0xE9, 0x10, 0, 0, 0 }), "E9 is not a direct call");
        Require(!DirectCallTarget(site, { 0xFF, 0x15, 0, 0, 0 }), "FF 15 is not an E8 call");
        Require(DirectCallTarget(site, { 0xE8, 0x78, 0x56, 0x34, 0x12 }) == site + 5 + 0x12345678,
            "positive rel32 with all byte positions");
        Require(DirectCallTarget(site, { 0xE8, 0, 0, 0, 0 }) == site + 5, "zero rel32");
        Require(DirectCallTarget(site, { 0xE8, 0xF0, 0xFF, 0xFF, 0xFF }) == site + 5 - 16,
            "negative rel32");
        Require(DirectCallTarget(site, { 0xE8, 0, 0, 0, 0x80 }) == site + 5 - 0x80000000,
            "minimum rel32");
        Require(DirectCallTarget(site, { 0xE8, 0x10, 0, 0, 0 }) != site + 5 + 0x11,
            "valid E8 can have the wrong destination");
    }

    void VerifyRegistry()
    {
        Registry registry;
        const Clock::time_point start{};
        const Key authorized{ 1, 2 };
        const Key unauthorized{ 1, 3 };
        registry.Insert(authorized, true, start);
        registry.Insert(unauthorized, false, start + 1s);
        auto match = registry.Find(authorized, start + 2s);
        Require(match && match->authorized && match->elapsed == 2s, "authorized launch and elapsed time");
        match = registry.Find(unauthorized, start + 2s);
        Require(match && !match->authorized && match->elapsed == 1s, "unauthorized launch with a shared handle");
        Require(!registry.Find({ 4, 2 }, start + 2s), "form ID alone is insufficient");
        Require(registry.Find(authorized, start + Registry::lifetime - 1ms).has_value(), "repeat hit before expiration");
        Require(!registry.Find(authorized, start + Registry::lifetime), "expiration at the lifetime boundary");
        Require(registry.Find(unauthorized, start + Registry::lifetime).has_value(), "later launch still present");
        registry.Insert(unauthorized, true, start + Registry::lifetime);
        match = registry.Find(unauthorized, start + Registry::lifetime + 1s);
        Require(match && match->authorized && match->elapsed == 1s, "replacement resets authorization and time");
    }

    void VerifyCapacity()
    {
        Registry registry;
        const Clock::time_point start{};
        const auto limit = static_cast<std::uint32_t>(Registry::capacity);
        for (std::uint32_t handle = 1; handle <= limit; ++handle) {
            registry.Insert({ handle, 1 }, false, start);
        }
        registry.Insert({ 1, 1 }, true, start + 1s);
        Require(registry.Find({ 2, 1 }, start + 1s).has_value(), "replacement does not consume a second entry");
        registry.Insert({ limit + 1, 1 }, false, start + 2s);
        Require(!registry.Find({ 2, 1 }, start + 2s), "capacity evicts the oldest remaining entry");
        const auto updated = registry.Find({ 1, 1 }, start + 2s);
        Require(updated && updated->authorized, "replacement belongs to the newest entries");
        Require(registry.Find({ limit + 1, 1 }, start + 2s).has_value(), "new entry survives eviction");
        registry.Insert({ limit + 2, 1 }, true, start + 2s + Registry::lifetime);
        Require(!registry.Find({ limit + 1, 1 }, start + 2s + Registry::lifetime), "insert prunes expired entries");
        Require(registry.Find({ limit + 2, 1 }, start + 2s + Registry::lifetime).has_value(), "insert after expiration");
    }

    void VerifyAttempt()
    {
        Require(AutoTest::FormatAttempt("console-slot2-a", "slot2", "console", 100, 60.25f, 65, 69.5f,
                    { 7, 8, 9 }, 2.75, 0, 4294967296) ==
                "console-slot2-a\tslot2\tconsole\t100.000\t60.250\t65.000\t69.500\t7\t8\t9\t2.750\t0\t4294967296",
            "ATTEMPT fields, precision, and appended 64-bit launch counts");
        Require(AutoTest::FormatAttempt("dual", "slot2", "both", 100, 6, 6, 6, { 1, 1, 0 }, 3, 2, 0) ==
                "dual\tslot2\tboth\t100.000\t6.000\t6.000\t6.000\t1\t1\t0\t3.000\t2\t0",
            "authorized launch count precedes unauthorized launch count");
    }
}

int main()
{
    VerifyDirectCall();
    std::cout << "PASS: E8 opcode and rel32 destination\n";
    VerifyRegistry();
    VerifyCapacity();
    std::cout << "PASS: launch registry key, authorization, repeated lookup, lifetime, and capacity\n";
    VerifyAttempt();
    std::cout << "PASS: ATTEMPT field order and precision\n";
}
