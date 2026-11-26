#include "test_harness.h"

#include "core/ProjectileStats.h"
#include "core/SkillSystem.h"

#include <array>
#include <filesystem>

using namespace SkyrimPoE::Core;

POE_TEST("Projectile count rolls spell volleys and surpassing chance within the launch cap")
{
    auto success = [] { return 0.04; };
    auto missed = [] { return 0.10; };
    std::array<double, kMaxCastProjectiles> byAdditionalCount{};
    byAdditionalCount[2] = 6.0;
    POE_CHECK(RollCastProjectileCount(1, 0.0, byAdditionalCount, success) == 3);
    POE_CHECK(RollCastProjectileCount(1, 0.0, byAdditionalCount, missed) == 1);

    byAdditionalCount[3] = 100.0;
    POE_CHECK(RollCastProjectileCount(1, 205.0, byAdditionalCount, success) == 9);
    POE_CHECK(RollCastProjectileCount(15, 205.0, byAdditionalCount, success) == kMaxCastProjectiles);
}

POE_TEST("Projectile pierce keeps guarantees separate from random pierce and leaves legacy rolls untouched")
{
    int draws = 0;
    auto roll = [&] {
        ++draws;
        return 0.25;
    };

    const auto alwaysPierce = RollProjectilePierce(2, true, 0.0, roll);
    const auto countedPierce = RollProjectilePierce(2, false, 0.0, roll);
    const auto noPierce = RollProjectilePierce(0, false, 0.0, roll);
    POE_CHECK(alwaysPierce.pierced && alwaysPierce.remainingPierces == 2);
    POE_CHECK(countedPierce.pierced && countedPierce.remainingPierces == 1);
    POE_CHECK(!noPierce.pierced);
    POE_CHECK(draws == 0);

    const auto randomPierce = RollProjectilePierce(0, false, 30.0, roll);
    POE_CHECK(randomPierce.pierced && randomPierce.remainingPierces == 0);
    POE_CHECK(!RollProjectilePierce(0, false, 15.0, roll).pierced);
    POE_CHECK(draws == 2);
}

POE_TEST("A successful projectile passive roll adds one chain or one fragment when forking")
{
    auto roll = [] { return 0.5; };
    POE_CHECK(RollCastChainCount(3, 60.0, roll) == 4);
    POE_CHECK(RollCastChainCount(3, 40.0, roll) == 3);
    POE_CHECK(RollCastChainCount(kMaxCastChains, 100.0, roll) == kMaxCastChains);
    POE_CHECK(RollForkedProjectileCount(75.0, roll) == 3);
    POE_CHECK(RollForkedProjectileCount(25.0, roll) == 2);
}

POE_TEST("PoE2 projectile stats reach the cast and preserve the lightning and projectile restrictions")
{
    DataPack pack;
    std::string error;
    POE_CHECK(pack.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs/gen0/core.json", &error));
    Config config;
    config.enforceGemRequirements = false;
    Layer1Input input;
    const std::array ids{ "Fireball", "Spark", "Arc", "IceNova" };
    for (std::size_t i = 0; i < ids.size(); ++i) {
        input.slots[i].occupied = true;
        input.slots[i].gem.gemId = ids[i];
        input.slots[i].gem.instanceId = i + 1;
    }
    for (const auto* line : {
        "Skills fire an additional Projectile",
        "6% chance for Spell Skills to fire 2 additional Projectiles",
        "+8% Surpassing chance to fire an additional Projectile",
        "50% chance to Pierce an Enemy",
        "60% chance for Lightning Skills to Chain an additional time",
        "Projectiles have 75% chance for an additional Projectile when Forking",
        "20% increased Projectile Speed",
        "5% reduced Projectile Speed for Spell Skills",
        "10% increased chance to inflict Ailments with Projectiles",
    }) {
        POE_CHECK(PassiveStatParser::ParsePoe2Line(line, input.passives));
    }
    StatEngine engine;
    engine.SetLayer1(input);
    POE_CHECK(engine.RebuildIfDirty(pack, config));
    const auto snapshot = engine.GetSnapshot();
    SkillSystem skills;
    const auto fireball = skills.TryBuildHandCastSpell(0, *snapshot, input.slots, 100.0, 100.0, pack, config);
    const auto spark = skills.TryBuildHandCastSpell(1, *snapshot, input.slots, 100.0, 100.0, pack, config);
    const auto arc = skills.TryBuildHandCastSpell(2, *snapshot, input.slots, 100.0, 100.0, pack, config);
    const auto nova = skills.TryBuildHandCastSpell(3, *snapshot, input.slots, 100.0, 100.0, pack, config);
    POE_CHECK(fireball && spark && arc && nova);
    POE_CHECK(fireball->projectileCount == 2);
    POE_CHECK_NEAR(fireball->additionalSpellProjectilesChancePct[2], 6.0, 1e-9);
    POE_CHECK_NEAR(fireball->additionalProjectileChancePct, 8.0, 1e-9);
    POE_CHECK_NEAR(fireball->projectilePierceChancePct, 50.0, 1e-9);
    POE_CHECK_NEAR(fireball->additionalForkProjectileChancePct, 75.0, 1e-9);
    POE_CHECK_NEAR(fireball->projectileSpeedMult, 1.15, 1e-9);
    POE_CHECK_NEAR(fireball->projectileAilmentChanceIncPct, 10.0, 1e-9);
    POE_CHECK_NEAR(fireball->additionalChainChancePct, 0.0, 1e-9);
    POE_CHECK_NEAR(spark->additionalChainChancePct, 60.0, 1e-9);
    POE_CHECK_NEAR(arc->additionalChainChancePct, 60.0, 1e-9);
    POE_CHECK_NEAR(nova->projectilePierceChancePct, 0.0, 1e-9);
    POE_CHECK_NEAR(nova->projectileSpeedMult, 1.0, 1e-9);
    POE_CHECK_NEAR(nova->projectileAilmentChanceIncPct, 0.0, 1e-9);
}
