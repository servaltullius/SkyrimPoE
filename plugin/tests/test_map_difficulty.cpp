#include "test_harness.h"

#include "core/DamageConversion.h"
#include "core/MapSystem.h"
#include "core/SkillSystem.h"

#include <array>
#include <filesystem>

using namespace SkyrimPoE::Core;

namespace
{
    DataPack LoadPack()
    {
        DataPack pack;
        std::string error;
        POE_CHECK(pack.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "core_3_30.json", &error));
        return pack;
    }
}

POE_TEST("Stage6 map difficulty: PoB T1-T16 life and damage multipliers, Arena4 uses T8")
{
    const auto pack = LoadPack();
    // 결정 170 U-3: PoB 3.30 원표에서 계산한 생명·피해 기대값.
    constexpr std::array<MapMonsterMultipliers, kMaxMapTier> expected{{
        { 1.000000, 1.000000 }, { 1.104681, 1.051559 }, { 1.218465, 1.105635 }, { 1.365105, 1.162308 },
        { 1.513053, 1.221711 }, { 1.686897, 1.283951 }, { 1.876372, 1.349217 }, { 2.097427, 1.417588 },
        { 2.323004, 1.489225 }, { 2.568335, 1.564315 }, { 2.834927, 1.642966 }, { 3.143072, 1.725338 },
        { 3.518072, 1.811672 }, { 3.947918, 1.902075 }, { 4.439600, 1.996761 }, { 4.976678, 2.095944 }
    }};
    for (std::uint8_t tier = kMinMapTier; tier <= kMaxMapTier; ++tier) {
        const auto actual = MapSystem::GetMapMonsterMultipliers(MapSystem::MonsterLevelForTier(tier), pack);
        const auto& row = expected[static_cast<std::size_t>(tier - kMinMapTier)];
        POE_CHECK_NEAR(actual.lifeMultiplier, row.lifeMultiplier, 1e-4);
        POE_CHECK_NEAR(actual.damageMultiplier, row.damageMultiplier, 1e-4);
    }

    auto arena = MapSystem::CreateArena4Run(100, 0, 0, {}, {});
    // 결투장 플래그가 있으면 일반 지도 티어 입력과 관계없이 T8을 사용한다.
    arena.tier = kMaxMapTier;
    arena.bossRefFormId = 0xFF000001;
    MapActorRegistry actors;
    actors.Rebuild(arena, pack);
    const auto scaling = actors.Find(arena.bossRefFormId);
    POE_CHECK(scaling.has_value());
    POE_CHECK(MapSystem::RewardMonsterLevel(arena) == kArena4MonsterLevel);
    POE_CHECK_NEAR(scaling->lifeMultiplier, expected[7].lifeMultiplier, 1e-4);
    POE_CHECK_NEAR(scaling->damageMultiplier, expected[7].damageMultiplier, 1e-4);
}

// 결정 169 ③: 지도 레벨은 스냅숏에 들어가지 않으므로, 오픈월드와 T16 지도 셀 안(플레이어 쪽 어픽스 없음)의
// 환산 결과가 같아야 한다. 회차 상태가 다시 환산에 끼어드는 회귀를 잡는다.
POE_TEST("Stage6 map difficulty: open world and a T16 map cell use the same exile level for combat conversion")
{
    const auto pack = LoadPack();
    const Config cfg;
    StatEngine engine;
    Layer1Input l1;
    l1.exileLevel = 50;
    l1.armour = 2000.0;
    l1.fireResist = 80.0;
    l1.chaosResist = 40.0;
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 1;
    l1.slots[0].gem.gemId = "Fireball";
    engine.SetLayer1(l1);

    const auto snapshotFor = [&](bool inMapCell, const MapModSummary& mods) {
        Layer2Input l2;
        l2.inActiveMap = inMapCell;
        l2.activeMapMods = mods;
        engine.SetLayer2(l2);
        POE_CHECK(engine.RebuildIfDirty(pack, cfg));
        const auto snap = engine.GetSnapshot();
        POE_CHECK(snap != nullptr);
        POE_CHECK(snap->inActiveMap == inMapCell);
        POE_CHECK(snap->exileLevel == l1.exileLevel);
        return snap;
    };
    const auto t16 = MapSystem::CreateNormalMapRun(2, kMaxMapTier, {}, 0, 0, {}, {});
    POE_CHECK(MapSystem::RewardMonsterLevel(t16) == 83u);
    const auto first = snapshotFor(false, MapModSummary{});
    const auto last = snapshotFor(true, t16.summary);

    SkillSystem firstSkills;
    SkillSystem lastSkills;
    const auto firstCast = firstSkills.TryBuildHandCastSpell(0, *first, l1.slots, 200.0, 200.0, pack, cfg);
    const auto lastCast = lastSkills.TryBuildHandCastSpell(0, *last, l1.slots, 200.0, 200.0, pack, cfg);
    POE_CHECK(firstCast && lastCast);
    POE_CHECK(firstCast->exileLevel == l1.exileLevel && lastCast->exileLevel == l1.exileLevel);
    POE_CHECK(firstCast->skyrimHitDamage > 0.0);
    POE_CHECK_NEAR(firstCast->skyrimHitDamage, lastCast->skyrimHitDamage, 1e-9);
    POE_CHECK_NEAR(firstCast->skyrimHitDamage,
        ConvertOffenseHit(firstCast->poeBaseHitDamage.TotalAverage(), l1.exileLevel, pack, cfg), 1e-9);
    POE_CHECK_NEAR(ConvertDefensePhysicalHit(50.0, first->exileLevel, *first, pack, cfg),
        ConvertDefensePhysicalHit(50.0, last->exileLevel, *last, pack, cfg), 1e-9);
    for (const auto type : { DamageType::Fire, DamageType::Chaos }) {
        POE_CHECK_NEAR(ConvertDefenseResistDamage(50.0, type, first->exileLevel, *first, pack, cfg),
            ConvertDefenseResistDamage(50.0, type, last->exileLevel, *last, pack, cfg), 1e-9);
    }
    POE_CHECK_NEAR(pack.MonsterAccuracy(first->exileLevel), pack.MonsterAccuracy(last->exileLevel), 1e-9);
}

POE_TEST("Stage6 map difficulty: all maximum resistances have a 50 percent floor only inside the map")
{
    const auto pack = LoadPack();
    const Config cfg;
    StatEngine engine;
    Layer1Input l1;
    l1.passives.maxFireResistBonus = -30.0;
    l1.passives.maxColdResistBonus = -35.0;
    l1.passives.maxLightningResistBonus = -40.0;
    l1.passives.maxChaosResistBonus = -100.0;
    engine.SetLayer1(l1);

    const auto caps = [](const StatSnapshot& snap) {
        return std::array{ snap.maxFireResist, snap.maxColdResist, snap.maxLightningResist, snap.maxChaosResist };
    };
    constexpr std::array outside{ 45.0, 40.0, 35.0, 0.0 };
    constexpr std::array inside{ 50.0, 50.0, 50.0, 50.0 };

    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    POE_CHECK(caps(*engine.GetSnapshot()) == outside);

    Layer2Input l2;
    l2.inActiveMap = true;
    l2.activeMapMods.playerMaxResPenalty = 10.0;
    engine.SetLayer2(l2);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    POE_CHECK(caps(*engine.GetSnapshot()) == inside);

    l2.inActiveMap = false;
    engine.SetLayer2(l2);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    POE_CHECK(caps(*engine.GetSnapshot()) == outside);
}

POE_TEST("Stage6 map actor registry: registration, neutral outside, and idempotent restore by run")
{
    const auto pack = LoadPack();
    constexpr std::uint32_t boss = 0xFF000001;
    constexpr std::uint32_t packActor = 0xFF000002;
    constexpr std::uint32_t nextActor = 0xFF000003;
    MapActorRegistry actors;

    actors.Register(boss, { 15, 2.0, 1.5 });
    const auto registered = actors.Find(boss);
    POE_CHECK(registered.has_value());
    POE_CHECK(registered->runId == 15 && registered->lifeMultiplier == 2.0 && registered->damageMultiplier == 1.5);
    POE_CHECK(!actors.Find(packActor).has_value());
    const auto neutral = actors.Find(packActor).value_or(MapActorScaling{});
    POE_CHECK(neutral.lifeMultiplier == 1.0 && neutral.damageMultiplier == 1.0);

    auto saved = MapSystem::CreateNormalMapRun(15, kMaxMapTier, {}, 0, 0, {}, {});
    saved.bossRefFormId = boss;
    saved.spawnedRefFormIds = { packActor };
    for (int restore = 0; restore < 2; ++restore) {
        actors.Rebuild(saved, pack);
        for (const auto actorId : { boss, packActor }) {
            const auto found = actors.Find(actorId);
            POE_CHECK(found.has_value());
            POE_CHECK(found->runId == saved.runId);
            POE_CHECK_NEAR(found->lifeMultiplier, 4.976678, 1e-4);
            POE_CHECK_NEAR(found->damageMultiplier, 2.095944, 1e-4);
        }
    }

    auto next = MapSystem::CreateNormalMapRun(16, kMinMapTier, {}, 0, 0, {}, {});
    next.spawnedRefFormIds = { nextActor };
    actors.Rebuild(next, pack);
    POE_CHECK(!actors.Find(boss).has_value() && !actors.Find(packActor).has_value());
    const auto nextScaling = actors.Find(nextActor);
    POE_CHECK(nextScaling.has_value() && nextScaling->runId == next.runId);
    POE_CHECK(nextScaling->lifeMultiplier == 1.0 && nextScaling->damageMultiplier == 1.0);
    POE_CHECK(!actors.Find(0).has_value());

    actors.Clear();
    POE_CHECK(!actors.Find(nextActor).has_value());
}

POE_TEST("Stage6 map difficulty: life scales the ailment threshold and incoming damage while vanilla hit uses exile life")
{
    const auto pack = LoadPack();
    constexpr std::uint32_t exileLevel = 50;
    constexpr double rawHit = 63.0;
    const auto t1 = MapSystem::GetMapMonsterMultipliers(MapSystem::MonsterLevelForTier(kMinMapTier), pack);
    const auto t16 = MapSystem::GetMapMonsterMultipliers(MapSystem::MonsterLevelForTier(kMaxMapTier), pack);
    const double firstThreshold = MapSystem::AilmentThreshold(exileLevel, t1.lifeMultiplier, pack);
    const double lastThreshold = MapSystem::AilmentThreshold(exileLevel, t16.lifeMultiplier, pack);
    POE_CHECK_NEAR(firstThreshold, pack.MonsterLife(exileLevel), 1e-9);
    POE_CHECK_NEAR(lastThreshold / firstThreshold, 4.976678, 1e-4);
    POE_CHECK_NEAR(MapSystem::EstimateVanillaHitPoe(rawHit, exileLevel, pack),
        rawHit * pack.MonsterLife(exileLevel) / 100.0, 1e-9);

    const MapActorScaling target{ 1, t16.lifeMultiplier, t16.damageMultiplier };
    POE_CHECK_NEAR(target.ScaleIncomingDamage(rawHit), rawHit / 4.976678, 1e-4);
    POE_CHECK_NEAR(MapActorScaling{}.ScaleIncomingDamage(rawHit), rawHit, 1e-9);
}
