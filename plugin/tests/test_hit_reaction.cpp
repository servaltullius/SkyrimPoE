#include "test_harness.h"

#include "core/Config.h"
#include "core/HitReaction.h"

#include <limits>

using namespace SkyrimPoE::Core;

POE_TEST("Flinch needs a hit of at least the life ratio and respects the per-target cooldown (decision 237)")
{
    const FlinchRule rule{ 0.10, 1.0 };
    POE_CHECK(ShouldFlinch(10.0, 100.0, 5.0, std::nullopt, rule));
    POE_CHECK(!ShouldFlinch(9.9, 100.0, 5.0, std::nullopt, rule));
    POE_CHECK(!ShouldFlinch(50.0, 100.0, 5.5, 5.0, rule));
    POE_CHECK(ShouldFlinch(50.0, 100.0, 6.0, 5.0, rule));
    // 시계가 뒤로 가면(세이브 전환 등) 쉬는 시간으로 막지 않는다.
    POE_CHECK(ShouldFlinch(50.0, 100.0, 1.0, 5.0, rule));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    POE_CHECK(!ShouldFlinch(nan, 100.0, 5.0, std::nullopt, rule));
    POE_CHECK(!ShouldFlinch(10.0, 0.0, 5.0, std::nullopt, rule));
    POE_CHECK(!ShouldFlinch(0.0, 100.0, 5.0, std::nullopt, rule));
    POE_CHECK(!ShouldFlinch(50.0, 100.0, 6.0, std::nullopt, FlinchRule{ 0.10, nan }));
    POE_CHECK(!ShouldFlinch(50.0, 100.0, 6.0, std::nullopt, FlinchRule{ 0.10, -1.0 }));
    POE_CHECK(!ShouldFlinch(50.0, 100.0, 6.0, nan, rule));
    // 비율 0이면 피해가 있는 모든 타격이 움찔한다(쉬는 시간은 그대로).
    POE_CHECK(ShouldFlinch(0.1, 100.0, 5.0, std::nullopt, FlinchRule{ 0.0, 1.0 }));
}

POE_TEST("Reactions INI section parses its keys and rejects out-of-range values (decisions 237 and 238)")
{
    const auto defaults = LoadConfigFromString("");
    POE_CHECK(defaults.config.sendSkillHitEvents);
    POE_CHECK_NEAR(defaults.config.skillFlinchMinLifeRatio, 0.10, 1e-12);
    POE_CHECK_NEAR(defaults.config.skillFlinchCooldownSec, 1.0, 1e-12);
    POE_CHECK_NEAR(defaults.config.skillFlinchMagnitude, 0.25, 1e-12);
    POE_CHECK_NEAR(defaults.config.heavyStunStaggerMagnitude, 1.0, 1e-12);

    const auto parsed = LoadConfigFromString("[Reactions]\nSendSkillHitEvents = false\nSkillFlinchMinLifeRatio = 0.2\n"
                                             "SkillFlinchCooldownSec = 2.5\nSkillFlinchMagnitude = 0.3\nHeavyStunStaggerMagnitude = 0.75\n");
    POE_CHECK(parsed.warnings.empty());
    POE_CHECK(!parsed.config.sendSkillHitEvents);
    POE_CHECK_NEAR(parsed.config.skillFlinchMinLifeRatio, 0.2, 1e-12);
    POE_CHECK_NEAR(parsed.config.skillFlinchCooldownSec, 2.5, 1e-12);
    POE_CHECK_NEAR(parsed.config.skillFlinchMagnitude, 0.3, 1e-12);
    POE_CHECK_NEAR(parsed.config.heavyStunStaggerMagnitude, 0.75, 1e-12);

    const auto bad = LoadConfigFromString("[Reactions]\nSkillFlinchMinLifeRatio = 1.5\nSkillFlinchCooldownSec = -1\nUnknownKey = 1\n");
    POE_CHECK(bad.warnings.size() == 3);
    POE_CHECK_NEAR(bad.config.skillFlinchMinLifeRatio, 0.10, 1e-12);
    POE_CHECK_NEAR(bad.config.skillFlinchCooldownSec, 1.0, 1e-12);
}
