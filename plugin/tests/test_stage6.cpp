#include "test_harness.h"

#include "core/Config.h"
#include "core/DataPack.h"
#include "core/KillTracker.h"
#include "core/ItemCrafting.h"
#include "core/ItemData.h"
#include "core/ItemFormatter.h"
#include "core/LootTable.h"
#include "core/MapSystem.h"
#include "core/PassiveTree.h"
#include "core/PassiveTreeData.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"

#include <filesystem>
#include <random>

using namespace SkyrimPoE::Core;

namespace
{
    std::filesystem::path DataDir()
    {
        return std::filesystem::path(POE_TEST_DATA_DIR);
    }
}

POE_TEST("Stage6 MapSystem: T1-T16 monster levels and 16 map affixes")
{
    POE_CHECK(MapSystem::MonsterLevelForTier(1) == 68);
    POE_CHECK(MapSystem::MonsterLevelForTier(8) == 75);
    POE_CHECK(MapSystem::MonsterLevelForTier(16) == 83);
    POE_CHECK(MapSystem::MonsterLevelForTier(0) == 68);
    POE_CHECK(MapSystem::MonsterLevelForTier(99) == 83);

    ItemData items;
    std::string err;
    POE_CHECK(items.LoadFromFile(DataDir() / "packs" / "gen0" / "items.json", &err));

    // 지도 어픽스 16종 제작 격리 검증: 일반 장비 어픽스가 섞이지 않아야 함(검수 M-3)
    std::mt19937_64 rng(60601);
    PoEItemRecord mapRec;
    mapRec.baseFormId = 0xFE0008B2;
    mapRec.rarity = ItemRarity::Normal;
    mapRec.itemLevel = MapSystem::MonsterLevelForTier(5);
    mapRec.slotGroup = TokenSlotGroup::Map;

    const auto mapTags = ItemData::BuildItemTags(TokenSlotGroup::Map, DefenseSubtype::None, "", false, 0);
    const auto alch = ItemCrafting::ApplyOrb(CurrencyKind::Alchemy, mapRec, mapTags, items, rng);
    POE_CHECK(alch.IsOk());
    POE_CHECK(mapRec.rarity == ItemRarity::Rare);
    POE_CHECK(mapRec.affixes.size() >= 4 && mapRec.affixes.size() <= 6);
    for (const auto& a : mapRec.affixes) {
        POE_CHECK(a.affixId.starts_with("MapMod_"));
        POE_CHECK(!MapSystem::FormatMapModKoreanLine(a).empty());
    }

    const auto summary = MapSystem::AggregateMapMods(mapRec.affixes);
    POE_CHECK(summary.bonusIiqPct >= 32.0);
    POE_CHECK(summary.bonusIirPct >= 32.0);
    POE_CHECK(summary.packSizePct >= 16.0);

    // ItemFormatter가 지도 라벨 및 한글 어픽스 줄을 출력
    EquippedSlotInput dummySlot;
    dummySlot.itemLevel = mapRec.itemLevel;
    const auto card = ItemFormatter::FormatItem(&mapRec, "T5 지도", EquipSlot::MainHand, dummySlot, true, items);
    POE_CHECK(card.slotLabel == "지도");
    POE_CHECK(card.affixLines.size() == mapRec.affixes.size());
}

POE_TEST("Stage6 Atlas & PassiveTree: 123 normal points (99 + 16 atlas + 8 milestones) and 4th Ascendancy 8/8")
{
    PassiveTreeData tree;
    std::string err;
    POE_CHECK(tree.LoadFromFile(DataDir() / "packs" / "gen0" / "tree.json", &err));

    Config cfg;
    PassiveTreeState state;
    POE_CHECK(PassiveTree::SelectClass(tree, state, 1).ok);

    // Lv100, 이정표 4종(0xF), 아틀라스 T1~16 전체 완료(0xFFFF) => 99 + 16 + 8 = 123 패시브 포인트, 전직 8/8
    state.ascendancyMilestonesBitmask = 0xFu;
    AtlasState atlas;
    for (std::uint8_t t = 1; t <= 16; ++t) {
        POE_CHECK(MapSystem::RecordMapClear(atlas, t));   // 첫 클리어 true
        POE_CHECK(!MapSystem::RecordMapClear(atlas, t));  // 중복 클리어 false
    }
    POE_CHECK(!MapSystem::RecordMapClear(atlas, 0));
    POE_CHECK(!MapSystem::RecordMapClear(atlas, 17));
    POE_CHECK(atlas.completedTierMask == 0xFFFFu);
    POE_CHECK(atlas.highestTierCleared == 16);
    POE_CHECK(MapSystem::CountAtlasTierPoints(atlas.completedTierMask) == 16);
    POE_CHECK(MapSystem::CountMilestonePassivePoints(state.ascendancyMilestonesBitmask) == 8);
    POE_CHECK_NEAR(MapSystem::AtlasBonusIiqPct(atlas.completedTierMask), 32.0, 1e-6);
    POE_CHECK_NEAR(MapSystem::AtlasBonusIirPct(atlas.completedTierMask), 48.0, 1e-6);

    POE_CHECK(PassiveTree::ComputeEarnedAscendancyPoints(state, cfg) == 8);
    POE_CHECK(PassiveTree::ComputeAvailableNormalPoints(tree, state, 100, cfg, atlas.completedTierMask) == 123);

    // [Maps] INI 파싱 및 경계 검증 (MapBasePackCount 1..6)
    const auto cfgParsed = LoadConfigFromString("[Maps]\nMapPanelKey = 0x17\nMapDeathInterceptEnabled = false\nMapBasePackCount = 5\n");
    POE_CHECK(cfgParsed.config.mapPanelKey == 0x17u);
    POE_CHECK(!cfgParsed.config.mapDeathInterceptEnabled);
    POE_CHECK(cfgParsed.config.mapBasePackCount == 5u);
    const auto cfgOutOfRange = LoadConfigFromString("[Maps]\nMapBasePackCount = 7\n");
    POE_CHECK(cfgOutOfRange.config.mapBasePackCount == 3u && !cfgOutOfRange.warnings.empty());

    // 결정 116·170 U-4: 회차의 보상 몬스터 레벨로 XP를 산정한다.
    constexpr std::uint32_t rewardMonsterLevel = 77;
    KillTracker kt;
    kt.RecordFirstHitHostility(0xFF001234, true, true);
    kt.ConfirmDamageContribution(0xFF001234, 50.0);
    const auto mapReward = kt.OnDeathConfirm(0xFF001234, 5, 50, false, false, 60, cfg, false, rewardMonsterLevel);
    POE_CHECK(mapReward.awarded);
    POE_CHECK(mapReward.effectiveMonsterLevel == 77u);
    POE_CHECK(mapReward.xpMultiplier > 1.5);  // Lv60 유배자가 Lv77 지도 적 처치 시 보너스 배율 적용
}

POE_TEST("Stage6 StatEngine: exile level, map penalties, and 4th milestone Spirit 230")
{
    DataPack pack;
    std::string err;
    POE_CHECK(pack.LoadFromFile(DataDir() / "packs" / "gen0" / "core.json", &err));

    Config cfg;
    StatEngine engine;
    Layer1Input l1;
    l1.exileLevel = 50;
    l1.className = "Marauder";
    l1.armour = 2000.0;
    l1.evasion = 1000.0;
    l1.flatEnergyShield = 200.0;
    l1.fireResist = 120.0;
    l1.achievementMask = 0xF;  // 1~4차 모두 완료 -> 정신력 230
    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto openSnap = engine.GetSnapshot();
    POE_CHECK(openSnap != nullptr);
    POE_CHECK(openSnap->maxSpirit == 230);
    POE_CHECK(!openSnap->inActiveMap);
    POE_CHECK(openSnap->exileLevel == 50);
    POE_CHECK_NEAR(openSnap->maxFireResist, 75.0, 1e-6);

    // 활성 회차 셀 진입 + 최대 저항 -10%, 방어도 -40%, 회피 -30%, ES 재충전 -50%
    Layer2Input l2;
    l2.inActiveMap = true;
    l2.activeMapMods.playerMaxResPenalty = 10.0;
    l2.activeMapMods.playerLessArmourPct = 40.0;
    l2.activeMapMods.playerLessEvasionPct = 30.0;
    l2.activeMapMods.playerLessEsRechargePct = 50.0;
    engine.SetLayer2(l2);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto mapSnap = engine.GetSnapshot();
    POE_CHECK(mapSnap != nullptr);
    POE_CHECK(mapSnap->inActiveMap);
    POE_CHECK(mapSnap->exileLevel == 50);
    // 회차 셀 진입 뒤에도 유배자 레벨과 생명력·매지카 풀을 유지한다.
    POE_CHECK_NEAR(mapSnap->poeMaxLife, openSnap->poeMaxLife, 1e-6);
    POE_CHECK_NEAR(mapSnap->poeMaxMana, openSnap->poeMaxMana, 1e-6);
    POE_CHECK_NEAR(mapSnap->maxFireResist, 65.0, 1e-6);
    POE_CHECK_NEAR(mapSnap->fireResist, 65.0, 1e-6);
    POE_CHECK_NEAR(mapSnap->armour, openSnap->armour * 0.60, 1e-3);
    POE_CHECK_NEAR(mapSnap->evasion, openSnap->evasion * 0.70, 1e-3);
    POE_CHECK_NEAR(mapSnap->esRechargeRateMult, openSnap->esRechargeRateMult * 0.50, 1e-6);

    // NaN 방어 검증(6단계 검수 Major 4)
    Layer2Input nanL2 = l2;
    nanL2.activeMapMods.playerMaxResPenalty = std::numeric_limits<double>::quiet_NaN();
    nanL2.activeMapMods.playerLessArmourPct = -50.0;
    engine.SetLayer2(nanL2);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto safeSnap = engine.GetSnapshot();
    POE_CHECK(std::isfinite(safeSnap->maxFireResist) && safeSnap->maxFireResist == 75.0);
    POE_CHECK_NEAR(safeSnap->armour, openSnap->armour, 1e-3);
}

POE_TEST("Stage6 Portal Transactions, Arena4 3-Phase Boss Clamp, and Map/Tribute LootTable")
{
    // 1. 일반 지도 6포털 거래 (PoE1 표준: 개방 1회 + 재입장 5회 = 총 6회 진입/목숨, 이중 차감·0포털 좀비 없음)
    auto run = MapSystem::CreateNormalMapRun(1, 10, {}, 0x000165A8, 0, { 1.0f, 2.0f, 3.0f }, { 0.0f, 0.0f, 0.0f });
    POE_CHECK(run.active && !run.isArena4 && run.portalsRemaining == 5);
    for (int life = 1; life <= 5; ++life) {
        // 사망 귀환: 남은 포털이 남아 있으므로 안전 귀환(활성 유지), 재입장 시 포털 1 소모
        POE_CHECK(MapSystem::ConsumePortalOnDeathEscape(run));
        POE_CHECK(run.active && !run.failed);
        POE_CHECK(MapSystem::TryConsumePortalForReenter(run));
        POE_CHECK(run.portalsRemaining == static_cast<std::uint8_t>(5 - life));
    }
    // 6번째 마지막 포털에서 사망 시 즉시 실패 종료
    POE_CHECK(run.portalsRemaining == 0 && run.active);
    POE_CHECK(!MapSystem::ConsumePortalOnDeathEscape(run));
    POE_CHECK(!run.active && run.failed);
    POE_CHECK(!MapSystem::TryConsumePortalForReenter(run));

    // 2. 4차 결투장 1포털 단판
    auto arena = MapSystem::CreateArena4Run(2, 0x000165A8, 0, { 1.0f, 2.0f, 3.0f }, { 0.0f, 0.0f, 0.0f });
    POE_CHECK(arena.active && arena.isArena4 && arena.portalsRemaining == 0);
    POE_CHECK(!MapSystem::TryConsumePortalForReenter(arena));
    POE_CHECK(!MapSystem::ConsumePortalOnDeathEscape(arena));
    POE_CHECK(!arena.active && arena.failed);

    // 3. 4차 결투장 3페이즈 보스 원샷 방지 클램프 및 무적·NaN 차단(검수 C-5)
    const auto inv = MapSystem::ClampArenaBossDamage(1, true, 1000.0, 1000.0, 5000.0);
    POE_CHECK(inv.blockedByInvuln && inv.clampedDamage == 0.0 && inv.triggeredNextPhase == 0);
    const auto nanRes = MapSystem::ClampArenaBossDamage(1, false, std::numeric_limits<double>::quiet_NaN(), 1000.0, 500.0);
    POE_CHECK(nanRes.clampedDamage == 0.0 && nanRes.triggeredNextPhase == 0);

    const auto p1To2 = MapSystem::ClampArenaBossDamage(1, false, 1000.0, 1000.0, 9999.0);
    POE_CHECK(p1To2.triggeredNextPhase == 2);
    POE_CHECK_NEAR(1000.0 - p1To2.clampedDamage, 665.0, 1e-6);

    const auto p2To3 = MapSystem::ClampArenaBossDamage(2, false, 665.0, 1000.0, 9999.0);
    POE_CHECK(p2To3.triggeredNextPhase == 3);
    POE_CHECK_NEAR(665.0 - p2To3.clampedDamage, 335.0, 1e-6);

    const auto p3Kill = MapSystem::ClampArenaBossDamage(3, false, 335.0, 1000.0, 9999.0);
    POE_CHECK(p3Kill.triggeredNextPhase == 0 && p3Kill.clampedDamage == 9999.0);

    // 4. LootTable: T6 지도 보스 처치 시 지도 확정 드랍 + 첫 T6+ 공물 확정 드랍 + 오픈월드 Lv49 미드랍 / Lv50+ T1 드랍
    ItemData items;
    std::string err;
    POE_CHECK(items.LoadFromFile(DataDir() / "packs" / "gen0" / "items.json", &err));
    Config cfg;
    std::mt19937_64 rng(12345);
    const auto drops = LootTable::RollKillDrops(73, true, false, cfg, items, rng, MonsterRarity::Rare, 6, 40.0, 30.0, true);
    bool gotMap = false;
    bool gotTribute = false;
    for (const auto& d : drops) {
        if (d.kind == DropKind::MapItem && d.tier >= 5 && d.tier <= 7) gotMap = true;
        if (d.kind == DropKind::AscendancyTribute) gotTribute = true;
    }
    POE_CHECK(gotMap);
    POE_CHECK(gotTribute);

    // 오픈월드 Lv49에서는 지도 미드랍, Lv50+ 보스에서는 T1 지도 드랍 발생
    bool lv49Map = false;
    bool lv50Map = false;
    for (int i = 0; i < 40; ++i) {
        for (const auto& d : LootTable::RollKillDrops(49, true, false, cfg, items, rng, MonsterRarity::Normal, 0)) {
            if (d.kind == DropKind::MapItem) lv49Map = true;
        }
        for (const auto& d : LootTable::RollKillDrops(50, true, false, cfg, items, rng, MonsterRarity::Normal, 0)) {
            if (d.kind == DropKind::MapItem && d.tier == 1) lv50Map = true;
        }
    }
    POE_CHECK(!lv49Map);
    POE_CHECK(lv50Map);
}
