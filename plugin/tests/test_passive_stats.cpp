#include "test_harness.h"

#include "core/Config.h"
#include "core/DamageConversion.h"
#include "core/DataPack.h"
#include "core/PassiveStatParser.h"
#include "core/PassiveTreeData.h"
#include "core/StatEngine.h"

#include <filesystem>

namespace
{
    using namespace SkyrimPoE::Core;

    const DataPack& LoadPack()
    {
        static DataPack pack;
        static bool loaded = false;
        if (!loaded) {
            std::string err;
            const auto p = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "core_3_30.json";
            POE_CHECK(pack.LoadFromFile(p, &err));
            loaded = true;
        }
        return pack;
    }

    const PassiveTreeData& LoadTree()
    {
        static PassiveTreeData tree;
        static bool loaded = false;
        if (!loaded) {
            std::string err;
            const auto p = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "tree_3_30.json";
            POE_CHECK(tree.LoadFromFile(p, &err));
            loaded = true;
        }
        return tree;
    }
}

POE_TEST("PassiveStatParser: parses core stat lines and skips conditional/weapon-specific/minion lines")
{
    PassiveStatTotals t;
    POE_CHECK(PassiveStatParser::ParseLine("+10 to Strength", t));
    POE_CHECK(PassiveStatParser::ParseLine("+10 to Strength and Intelligence", t));
    POE_CHECK(PassiveStatParser::ParseLine("+8 to all Attributes", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% increased Strength", t));
    POE_CHECK(PassiveStatParser::ParseLine("+30 to maximum Life", t));
    POE_CHECK(PassiveStatParser::ParseLine("6% increased maximum Life", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% more maximum Life", t));
    POE_CHECK(PassiveStatParser::ParseLine("+20 to maximum Mana", t));
    POE_CHECK(PassiveStatParser::ParseLine("8% increased maximum Mana", t));
    POE_CHECK(PassiveStatParser::ParseLine("12% increased maximum Energy Shield", t));
    POE_CHECK(PassiveStatParser::ParseLine("15% increased Energy Shield Recharge Rate", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% faster start of Energy Shield Recharge", t));
    POE_CHECK(PassiveStatParser::ParseLine("+100 to Armour", t));
    POE_CHECK(PassiveStatParser::ParseLine("12% increased Evasion Rating", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% increased Global Defences", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% increased Armour and Evasion", t));
    POE_CHECK(PassiveStatParser::ParseLine("+12% to Fire Resistance", t));
    POE_CHECK(PassiveStatParser::ParseLine("+10% to all Elemental Resistances", t));
    POE_CHECK(PassiveStatParser::ParseLine("+15% to Fire and Cold Resistances", t));
    POE_CHECK(PassiveStatParser::ParseLine("+1% to maximum Fire Resistance", t));
    POE_CHECK(PassiveStatParser::ParseLine("+1% to all maximum Elemental Resistances", t));
    POE_CHECK(PassiveStatParser::ParseLine("+4% Chance to Block Attack Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("+3% Chance to Block Spell Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("+10% chance to Suppress Spell Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("+3% to amount of Suppressed Spell Damage Prevented", t));
    POE_CHECK(PassiveStatParser::ParseLine("3% additional Physical Damage Reduction", t));
    POE_CHECK(PassiveStatParser::ParseLine("12% increased Physical Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("14% increased Melee Physical Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% increased Melee Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("8% increased Elemental Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% increased Spell Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("12% increased Area Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("4% increased Attack Speed", t));
    POE_CHECK(PassiveStatParser::ParseLine("6% increased Cast Speed", t));
    POE_CHECK(PassiveStatParser::ParseLine("25% increased Critical Strike Chance", t));
    POE_CHECK(PassiveStatParser::ParseLine("+15% to Critical Strike Multiplier", t));
    POE_CHECK(PassiveStatParser::ParseLine("8% increased Area of Effect", t));
    POE_CHECK(PassiveStatParser::ParseLine("5% reduced Mana Cost of Skills", t));
    POE_CHECK(PassiveStatParser::ParseLine("+1 to Maximum Endurance Charges", t));

    // 조건부·무기 한정·소환수 스탯은 미매핑(오적용 방지)
    POE_CHECK(!PassiveStatParser::ParseLine("20% increased Damage while Leeching", t));
    POE_CHECK(!PassiveStatParser::ParseLine("10% increased Attack Speed if you've Killed Recently", t));
    POE_CHECK(!PassiveStatParser::ParseLine("30% increased Damage against Chilled Enemies", t));
    POE_CHECK(!PassiveStatParser::ParseLine("10% increased Damage with Wands", t));
    POE_CHECK(!PassiveStatParser::ParseLine("Minions deal 15% increased Damage", t));

    POE_CHECK_NEAR(t.flatStr, 28.0, 1e-9);
    POE_CHECK_NEAR(t.flatDex, 8.0, 1e-9);
    POE_CHECK_NEAR(t.flatInt, 18.0, 1e-9);
    POE_CHECK_NEAR(t.incStrPct, 10.0, 1e-9);
    POE_CHECK_NEAR(t.flatLife, 30.0, 1e-9);
    POE_CHECK_NEAR(t.incLifePct, 6.0, 1e-9);
    POE_CHECK_NEAR(t.moreLifeMult, 1.1, 1e-9);
    POE_CHECK_NEAR(t.incEnergyShieldPct, 22.0, 1e-9);  // 12 + 10 Global Defences
    POE_CHECK_NEAR(t.incArmourPct, 0.0, 1e-9);
    POE_CHECK_NEAR(t.incEvasionPct, 12.0, 1e-9);
    POE_CHECK_NEAR(t.incArmourAndEvasionPct, 20.0, 1e-9);  // 10 Global + 10 Armour & Evasion
    POE_CHECK_NEAR(t.fireResist, 37.0, 1e-9);          // 12 + 10 + 15
    POE_CHECK_NEAR(t.coldResist, 25.0, 1e-9);          // 10 + 15
    POE_CHECK_NEAR(t.lightningResist, 10.0, 1e-9);
    POE_CHECK_NEAR(t.maxFireResistBonus, 2.0, 1e-9);
    POE_CHECK_NEAR(t.maxColdResistBonus, 1.0, 1e-9);
    POE_CHECK_NEAR(t.attackBlockChance, 4.0, 1e-9);
    POE_CHECK_NEAR(t.spellBlockChance, 3.0, 1e-9);
    POE_CHECK_NEAR(t.spellSuppressionChance, 10.0, 1e-9);
    POE_CHECK_NEAR(t.spellSuppressionPrevent, 3.0, 1e-9);
    POE_CHECK_NEAR(t.physDamageReduction, 3.0, 1e-9);
    POE_CHECK_NEAR(t.incPhysicalDamagePct, 12.0, 1e-9);
    POE_CHECK_NEAR(t.incMeleePhysicalDamagePct, 14.0, 1e-9);
    POE_CHECK_NEAR(t.incMeleeDamagePct, 10.0, 1e-9);
    POE_CHECK_NEAR(t.incElementalDamagePct, 8.0, 1e-9);
    POE_CHECK_NEAR(t.incSpellDamagePct, 10.0, 1e-9);
    POE_CHECK_NEAR(t.incAreaDamagePct, 12.0, 1e-9);
    POE_CHECK_NEAR(t.incAttackSpeedPct, 4.0, 1e-9);
    POE_CHECK_NEAR(t.incCastSpeedPct, 6.0, 1e-9);
    POE_CHECK_NEAR(t.incGlobalCritChancePct, 25.0, 1e-9);
    POE_CHECK_NEAR(t.globalCritMultiplierPct, 15.0, 1e-9);
    POE_CHECK_NEAR(t.incAreaOfEffectPct, 8.0, 1e-9);
    POE_CHECK_NEAR(t.reducedManaCostPct, 5.0, 1e-9);
    POE_CHECK(t.maxEnduranceBonus == 1);
}

POE_TEST("PassiveStatParser: AggregateTree recognizes all 10 Stage-3 keystones from real tree_3_30.json")
{
    const auto& tree = LoadTree();
    PassiveTreeState state;
    state.classSelected = true;
    state.allocatedNodes = {
        57279,  // Blood Magic
        11455,  // Chaos Inoculation
        31961,  // Resolute Technique
        10661,  // Iron Reflexes
        40907,  // Unwavering Stance
        54307,  // Acrobatics
        34098,  // Mind Over Matter
        22088,  // Elemental Overload
        31703,  // Pain Attunement
        50288   // Iron Will
    };

    const auto totals = PassiveStatParser::AggregateTree(tree, state);
    POE_CHECK(totals.bloodMagic);
    POE_CHECK_NEAR(totals.moreLifeMult, 1.1, 1e-9);
    POE_CHECK(totals.chaosInoculation);
    POE_CHECK(totals.resoluteTechnique);
    POE_CHECK(totals.ironReflexes);
    POE_CHECK(totals.unwaveringStance);
    POE_CHECK(totals.cannotEvade);
    POE_CHECK(totals.acrobatics);
    POE_CHECK_NEAR(totals.mindOverMatterRatio, 0.40, 1e-9);
    POE_CHECK(totals.elementalOverload);
    POE_CHECK(totals.painAttunement);
    POE_CHECK(totals.ironWill);

    // 검수 Major #4: Precise Technique(63620) 단독 선택 시 Resolute Technique 패널티 오적용 금지
    PassiveTreeState ptState;
    ptState.classSelected = true;
    ptState.allocatedNodes = { 63620 };
    const auto ptTotals = PassiveStatParser::AggregateTree(tree, ptState);
    POE_CHECK(!ptTotals.resoluteTechnique);

    // 검수 Major #1: 실제 tree_3_30.json의 3.30 영문 문구 5종 직접 검증
    PassiveTreeState real330;
    real330.classSelected = true;
    real330.allocatedNodes = {
        9788,   // Nimbleness: +15% to Critical Strike Multiplier for Spell Damage
        5612,   // Physical Attack Damage: 12% increased Attack Physical Damage
        63251,  // Inveterate: Prevent +3% of Suppressed Spell Damage
        5632,   // Bleed and Critical Strike Chance: 15% increased Critical Strike Chance for Attacks
        3533    // Life and Cost Efficiency: 5% increased Cost Efficiency
    };
    const auto r330 = PassiveStatParser::AggregateTree(tree, real330);
    POE_CHECK_NEAR(r330.spellCritMultiplierPct, 15.0, 1e-9);
    POE_CHECK_NEAR(r330.incPhysicalAttackDamagePct, 12.0, 1e-9);
    POE_CHECK_NEAR(r330.spellSuppressionPrevent, 3.0, 1e-9);
    POE_CHECK_NEAR(r330.incAttackCritChancePct, 15.0, 1e-9);
    POE_CHECK_NEAR(r330.costEfficiencyPct, 5.0, 1e-9);
}

POE_TEST("StatEngine: 2-pass attribute convergence, Iron Reflexes, Blood Magic, and dynamic max resists")
{
    const auto& pack = LoadPack();
    Config cfg;
    StatEngine engine;

    Layer1Input l1;
    l1.exileLevel = 10;
    l1.className = "Marauder";  // 32 Str, 14 Dex, 14 Int
    l1.baseWeaponPhysicalMin = 100.0;
    l1.baseWeaponPhysicalMax = 100.0;
    l1.slots[0].slotIndex = 0;
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 1;
    l1.slots[0].gem.gemId = "Cyclone";
    l1.slots[0].gem.level = 1;

    // 1패스 능력치 증가(20% inc Str, +68 flat Str -> 100 * 1.2 = 120 Str -> +60 Life, +24% melee phys)
    l1.passives.flatStr = 68.0;
    l1.passives.incStrPct = 20.0;
    l1.passives.flatDex = 86.0;  // 14 + 86 = 100 Dex
    l1.passives.flatArmour = 100.0;
    l1.passives.flatEvasion = 200.0;
    l1.passives.incArmourPct = 50.0;
    l1.passives.incEvasionPct = 50.0;
    l1.passives.ironReflexes = true;  // Dex 회피 보너스 억제 + 회피 -> 방어도 전환
    l1.passives.fireResist = 85.0;
    l1.passives.maxFireResistBonus = 3.0;  // 상한 78%
    l1.passives.coldResist = 95.0;
    l1.passives.maxColdResistBonus = 20.0;  // PoE 하드캡 90%

    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snap = engine.GetSnapshot();

    POE_CHECK(snap->str == 120);
    POE_CHECK(snap->dex == 100);
    POE_CHECK_NEAR(snap->evasion, 0.0, 1e-9);
    // Iron Reflexes: Dex evasion bonus suppressed (0%).
    // flatArmour(100) * (1 + 0.5) = 150, flatEvasion(200) * (1 + 0.5 + 0.5) = 400 -> total 550!
    POE_CHECK_NEAR(snap->armour, 550.0, 1e-9);
    POE_CHECK_NEAR(snap->maxFireResist, 78.0, 1e-9);
    POE_CHECK_NEAR(snap->maxColdResist, 90.0, 1e-9);

    // 동적 최대 저항 반영 검증: 85% 화염 저항 + 78% 상한 -> 22% 통과 (Lv10 기대 저항 100% 기준 비율 검증)
    const double expElem10 = ExpectedElementalPass(10, pack, cfg);
    const double expElem1 = ExpectedElementalPass(1, pack, cfg);
    const double denomElem = std::pow(expElem10, cfg.defenseRollback) * std::pow(expElem1, 1.0 - cfg.defenseRollback);
    const double fireThrough = ConvertDefenseResistDamage(100.0, DamageType::Fire, 10, *snap, pack, cfg);
    POE_CHECK_NEAR(fireThrough, 100.0 * 0.22 / denomElem, 1e-6);
    // 95% 냉기 저항 + 90% 하드캡 -> 10% 통과
    const double coldThrough = ConvertDefenseResistDamage(100.0, DamageType::Cold, 10, *snap, pack, cfg);
    POE_CHECK_NEAR(coldThrough, 100.0 * 0.10 / denomElem, 1e-6);
}

POE_TEST("StatEngine & DamageConversion: Chaos Inoculation, Pain Attunement, Iron Will, RT, EO, Acrobatics, MoM, Blood Magic")
{
    const auto& pack = LoadPack();
    Config cfg;
    StatEngine engine;

    // 1. Chaos Inoculation + Pain Attunement (CI에서는 Low Life여도 Pain Attunement 비활성)
    Layer1Input l1;
    l1.exileLevel = 20;
    l1.className = "Witch";
    l1.slots[0].slotIndex = 0;
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 10;
    l1.slots[0].gem.gemId = "IceNova";
    l1.slots[0].gem.level = 1;
    l1.passives.chaosInoculation = true;
    l1.passives.painAttunement = true;
    l1.passives.spellSuppressionChance = 100.0;
    l1.passives.acrobatics = true;  // 100% * 0.5 = 50% spell dodge
    engine.SetLayer1(l1);

    Layer2Input l2;
    l2.lowLife = true;
    engine.SetLayer2(l2);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapCi = engine.GetSnapshot();

    POE_CHECK_NEAR(snapCi->poeMaxLife, 1.0, 1e-9);
    POE_CHECK(snapCi->chaosImmune);
    POE_CHECK_NEAR(ConvertDefenseResistDamage(500.0, DamageType::Chaos, 20, *snapCi, pack, cfg), 0.0, 1e-9);
    POE_CHECK_NEAR(ComputeSkyrimMaxHealth(200.0, 1.0, 20, pack, cfg, true), 25.0, 1e-9);
    POE_CHECK_NEAR(ComputeSkyrimMaxHealth(100.0, 1.0, 20, pack, cfg, true), 15.0, 1e-9);
    POE_CHECK_NEAR(snapCi->spellSuppressionChance, 0.0, 1e-9);
    POE_CHECK_NEAR(snapCi->spellDodgeChance, 50.0, 1e-9);

    // CI 해제 시 Low Life + Pain Attunement -> 주문 피해 1.3배
    const double iceDmgUnderCi = snapCi->slotStats[0].baseHitDamage.TotalAverage();
    l1.passives.chaosInoculation = false;
    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapPa = engine.GetSnapshot();
    POE_CHECK_NEAR(snapPa->slotStats[0].baseHitDamage.TotalAverage(), iceDmgUnderCi * 1.3, 1e-6);

    // 2. Iron Will + Elemental Overload + Resolute Technique + Unwavering Stance
    Layer1Input l1b;
    l1b.exileLevel = 20;
    l1b.className = "Marauder";  // 32 Str -> +6.4%
    l1b.passives.flatStr = 68.0; // total 100 Str -> +20% melee phys (& spell with Iron Will)
    l1b.baseWeaponPhysicalMin = 50.0;
    l1b.baseWeaponPhysicalMax = 50.0;
    l1b.slots[0].slotIndex = 0;
    l1b.slots[0].occupied = true;
    l1b.slots[0].gem.instanceId = 20;
    l1b.slots[0].gem.gemId = "Cyclone";
    l1b.slots[0].gem.level = 1;
    l1b.slots[1].slotIndex = 1;
    l1b.slots[1].occupied = true;
    l1b.slots[1].gem.instanceId = 21;
    l1b.slots[1].gem.gemId = "IceNova";
    l1b.slots[1].gem.level = 1;
    engine.SetLayer1(l1b);
    engine.SetLayer2(Layer2Input{});
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapBase = engine.GetSnapshot();

    l1b.passives.ironWill = true;
    l1b.passives.resoluteTechnique = true;
    l1b.passives.elementalOverload = true;
    l1b.passives.unwaveringStance = true;
    l1b.passives.cannotEvade = true;
    l1b.passives.flatEvasion = 500.0;
    l1b.passives.incCastSpeedPct = 20.0;
    l1b.passives.costEfficiencyPct = 25.0;  // cost / 1.25
    engine.SetLayer1(l1b);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapKs = engine.GetSnapshot();

    // Iron Will -> IceNova 피해 +20%, 시전 속도 1.20x, 비용 효율 /1.25
    POE_CHECK_NEAR(
        snapKs->slotStats[1].baseHitDamage.TotalAverage(),
        snapBase->slotStats[1].baseHitDamage.TotalAverage() * 1.20,
        1e-6);
    POE_CHECK_NEAR(snapKs->slotStats[1].castSpeedMult, 1.20, 1e-9);
    POE_CHECK_NEAR(snapKs->slotStats[1].poeManaCost, snapBase->slotStats[1].poeManaCost / 1.25, 1e-6);
    // Resolute Technique -> PoE 3.30 원작 규칙: 공격(Cyclone)·주문(IceNova) 모두 치명타 0%
    POE_CHECK_NEAR(snapKs->slotStats[0].baseCritChancePct, 0.0, 1e-9);
    POE_CHECK_NEAR(snapKs->slotStats[1].baseCritChancePct, 0.0, 1e-9);
    // Elemental Overload -> 치명타 배율 100%, 최근 치명타 활성 시 원소 피해 1.40배
    POE_CHECK_NEAR(snapKs->slotStats[1].critMultiplierPct, 100.0, 1e-9);
    Layer2Input l2Eo;
    l2Eo.elementalOverloadActive = true;
    engine.SetLayer2(l2Eo);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapEo = engine.GetSnapshot();
    POE_CHECK_NEAR(
        snapEo->slotStats[1].baseHitDamage.TotalAverage(),
        snapKs->slotStats[1].baseHitDamage.TotalAverage() * 1.40,
        1e-6);
    engine.SetLayer2(Layer2Input{});
    // Unwavering Stance -> 기절 면역 + 회피 0
    POE_CHECK_NEAR(snapKs->evasion, 0.0, 1e-9);
    POE_CHECK(snapKs->unwaveringStance);

    // Juggernaut "Cannot be Stunned"만 있을 때(cannotEvade == false)는 회피가 유지돼야 함
    l1b.passives.cannotEvade = false;
    engine.SetLayer1(l1b);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapJug = engine.GetSnapshot();
    POE_CHECK(snapJug->unwaveringStance);
    POE_CHECK(snapJug->evasion > 500.0);

    // 3. Mind Over Matter 난이도 배율 정렬 및 음수 마나 클램프
    const auto momFull = AbsorbWithMindOverMatter(100.0, 1.0, 0.40, 200.0);
    POE_CHECK_NEAR(momFull.absorbedMagicka, 40.0, 1e-9);
    POE_CHECK_NEAR(momFull.remainingRawDamage, 60.0, 1e-9);

    // 전설 난이도(3.0배): incoming=100 -> 실제 체력 피해 300 -> 40% 흡수 목표 120 매지카 -> 매지카 50뿐이면 50 흡수 -> 체력 피해 250 -> 원시 피해 250/3
    const auto momLegendary = AbsorbWithMindOverMatter(100.0, 3.0, 0.40, 50.0);
    POE_CHECK_NEAR(momLegendary.absorbedMagicka, 50.0, 1e-9);
    POE_CHECK_NEAR(momLegendary.remainingRawDamage, 250.0 / 3.0, 1e-6);

    // 음수 매지카 입력 시 0 흡수
    const auto momNeg = AbsorbWithMindOverMatter(100.0, 1.0, 0.40, -10.0);
    POE_CHECK_NEAR(momNeg.absorbedMagicka, 0.0, 1e-9);
    POE_CHECK_NEAR(momNeg.remainingRawDamage, 100.0, 1e-9);

    // 4. Blood Magic 생명력 비용 환산 (skyrimMaxHealth=300, poeMaxLife=200, poeSkillCost=20 -> 30 HP)
    POE_CHECK_NEAR(ComputeBloodMagicLifeCost(300.0, 200.0, 20.0), 30.0, 1e-9);
}

POE_TEST("Config: parses [PassiveTree] section from SkyrimPoE.ini")
{
    const auto iniPath = std::filesystem::path(POE_TEST_PLUGIN_DIR) / "dist" / "SKSE" / "Plugins" / "SkyrimPoE.ini";
    const auto loaded = LoadConfigFromFile(iniPath);
    POE_CHECK(loaded.fileFound);
    const auto& cfg = loaded.config;
    POE_CHECK(cfg.treePanelKey == 0x19);
    POE_CHECK(cfg.freePassiveRefund);
    POE_CHECK(cfg.simpleImeMode == "auto");
    POE_CHECK(cfg.bonusPassivePoints == 0);
    POE_CHECK(cfg.bonusAscendancyPoints == 0);
}
