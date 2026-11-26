#include "test_harness.h"

#include "core/DamageConversion.h"
#include "core/DataPack.h"

#include <cmath>
#include <filesystem>

using namespace SkyrimPoE::Core;

namespace
{
    DataPack LoadStage2Pack()
    {
        DataPack pack;
        std::string err;
        const std::filesystem::path packPath = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "gen0" / "core.json";
        if (!pack.LoadFromFile(packPath, &err)) {
            throw std::runtime_error("Failed to load core.json: " + err);
        }
        return pack;
    }
}

POE_TEST("DamageConversion: Codex golden check (L=1, P=E(1) -> B=25 for all r)")
{
    const DataPack pack = LoadStage2Pack();
    Config cfg;
    cfg.offenseK = 75.0;
    cfg.offenseBaseHits = 3.0;

    const double e1 = ExpectedHitDamage(1, pack, cfg);
    POE_CHECK(e1 > 0.0);

    for (double r : { 0.0, 0.5, 1.0 }) {
        cfg.offenseRollback = r;
        const double b = ConvertOffenseHit(e1, 1, pack, cfg);
        POE_CHECK_NEAR(b, 25.0, 1e-9);
    }

    // L=50, P=E(50): r=1.0(100% 되돌림) -> 25, r=0.5 -> 25 * sqrt(E(50)/E(1)), r=0.0(0% 되돌림) -> 25 * (E(50)/E(1))
    const double e50 = ExpectedHitDamage(50, pack, cfg);
    cfg.offenseRollback = 1.0;
    POE_CHECK_NEAR(ConvertOffenseHit(e50, 50, pack, cfg), 25.0, 1e-9);
    cfg.offenseRollback = 0.5;
    POE_CHECK_NEAR(ConvertOffenseHit(e50, 50, pack, cfg), 25.0 * std::sqrt(e50 / e1), 1e-6);
    cfg.offenseRollback = 0.0;
    POE_CHECK_NEAR(ConvertOffenseHit(e50, 50, pack, cfg), 25.0 * (e50 / e1), 1e-6);

    // DPS 환산: P_dps = E(1) * 1.6 -> B_dps = 25 * 1.6 = 40
    cfg.offenseRollback = 0.5;
    cfg.baseHitRate = 1.6;
    POE_CHECK_NEAR(ConvertOffenseDps(e1 * 1.6, 1, pack, cfg), 40.0, 1e-9);
}

POE_TEST("DamageConversion: defense conversion preserves exact baseline at L=1 and scales cleanly")
{
    const DataPack pack = LoadStage2Pack();
    Config cfg;
    cfg.defenseRollback = 0.5;
    cfg.defenseDref = 10.7;

    // 기대 방어/저항(L=1)일 때 m == m_e(1) -> 출력 == 입력 D
    const double expPhys1 = ExpectedPhysicalPass(1, pack, cfg);
    const double expElem1 = ExpectedElementalPass(1, pack, cfg);
    const double expChaos1 = ExpectedChaosPass(1, pack, cfg);
    POE_CHECK(expPhys1 > 0.0 && expPhys1 <= 1.0);
    POE_CHECK(expElem1 > 0.0 && expElem1 <= 1.0);
    POE_CHECK_NEAR(expChaos1, 1.0, 1e-9);  // L=1 카오스 기대 저항 0% -> 통과율 1.0

    // 방어력 0, 저항 0 스냅샷
    StatSnapshot naked;
    naked.armour = 0.0;
    naked.fireResist = 0.0;
    naked.chaosResist = 0.0;

    // 방어력이 없으면 기대 방어보다 더 아파야 함 (D / m_e(1) > D)
    const double physNaked = ConvertDefensePhysicalHit(10.7, 1, naked, pack, cfg);
    POE_CHECK_NEAR(physNaked, 10.7 / expPhys1, 1e-6);
    POE_CHECK(physNaked > 10.7);

    // 저항 75% 캡이면 화염 피해가 크게 감소
    StatSnapshot capped = naked;
    capped.fireResist = 75.0;
    const double fireNaked = ConvertDefenseResistDamage(20.0, DamageType::Fire, 1, naked, pack, cfg);
    const double fireCapped = ConvertDefenseResistDamage(20.0, DamageType::Fire, 1, capped, pack, cfg);
    POE_CHECK(fireCapped < fireNaked);
    POE_CHECK_NEAR(fireCapped, 20.0 * 0.25 / expElem1, 1e-6);

    // 음수 피해는 0 바닥
    POE_CHECK_NEAR(ConvertDefensePhysicalHit(-5.0, 1, naked, pack, cfg), 0.0, 1e-9);
    POE_CHECK_NEAR(ConvertDefenseResistDamage(-5.0, DamageType::Cold, 1, naked, pack, cfg), 0.0, 1e-9);
}

POE_TEST("DamageConversion: PoE 3.30 Endurance Charge elem DR, armour+physDR 90% cap, rollback, Act 5/10 chaos, evasion")
{
    const DataPack pack = LoadStage2Pack();
    Config cfg;

    // 1. PoE 3.30 인내 충전 3개: 75% 저항 캡에서도 원소 피해 감소 12%가 곱연산으로 추가 적용되고 카오스는 미적용
    StatSnapshot capped;
    capped.fireResist = 75.0;
    capped.chaosResist = 0.0;
    capped.elementalDamageReduction = 0.0;
    StatSnapshot cappedWithEnd = capped;
    cappedWithEnd.elementalDamageReduction = 12.0;

    const double fireBase = ConvertDefenseResistDamage(20.0, DamageType::Fire, 1, capped, pack, cfg);
    const double fireEnd = ConvertDefenseResistDamage(20.0, DamageType::Fire, 1, cappedWithEnd, pack, cfg);
    POE_CHECK_NEAR(fireEnd, fireBase * 0.88, 1e-9);

    const double chaosBase = ConvertDefenseResistDamage(20.0, DamageType::Chaos, 1, capped, pack, cfg);
    const double chaosEnd = ConvertDefenseResistDamage(20.0, DamageType::Chaos, 1, cappedWithEnd, pack, cfg);
    POE_CHECK_NEAR(chaosEnd, chaosBase, 1e-9);

    // 2. 물리 방어도 + 물리 피해 감소 합산 90% 상한 및 L=50 defenseRollback (0, 0.5, 1.0)
    StatSnapshot tank;
    tank.armour = 1000000.0;
    tank.physDamageReduction = 50.0;
    const double me50 = ExpectedPhysicalPass(50, pack, cfg);
    const double me1 = ExpectedPhysicalPass(1, pack, cfg);
    cfg.defenseRollback = 1.0;
    POE_CHECK_NEAR(ConvertDefensePhysicalHit(10.7, 50, tank, pack, cfg), 10.7 * 0.10 / me50, 1e-6);
    cfg.defenseRollback = 0.0;
    POE_CHECK_NEAR(ConvertDefensePhysicalHit(10.7, 50, tank, pack, cfg), 10.7 * 0.10 / me1, 1e-6);
    cfg.defenseRollback = 0.5;
    POE_CHECK_NEAR(ConvertDefensePhysicalHit(10.7, 50, tank, pack, cfg), 10.7 * 0.10 / std::sqrt(me50 * me1), 1e-6);

    // 3. 5막/10막 카오스 기대 통과율 (0% -> 1.0, -30% -> 1.3, -60% -> 1.6)
    POE_CHECK_NEAR(ExpectedChaosPass(1, pack, cfg), 1.0, 1e-9);
    POE_CHECK_NEAR(ExpectedChaosPass(cfg.act5Level, pack, cfg), 1.3, 1e-9);
    POE_CHECK_NEAR(ExpectedChaosPass(cfg.act10Level, pack, cfg), 1.6, 1e-9);

    // 4. 회피 적중 확률 경계 (회피 0 -> 1.0, 초대형 회피 -> 하한 0.05)
    POE_CHECK_NEAR(EvasionHitChance(0.0, 500.0), 1.0, 1e-9);
    POE_CHECK_NEAR(EvasionHitChance(1e12, 100.0), 0.05, 1e-9);
}

POE_TEST("DamageConversion: Life, ES, and Magicka (including Blood Magic 0/0 guard)")
{
    const DataPack pack = LoadStage2Pack();
    Config cfg;

    const auto* scion = pack.FindClass("Scion");
    POE_CHECK(scion);
    const double life = ExpectedLife(1, scion->baseStr, cfg);
    // Lv1 Scion: 16 + 12 + 20 × 2 = 68.
    POE_CHECK_NEAR(life, 68.0, 1e-9);
    POE_CHECK_NEAR(ComputeSkyrimMaxHealth(100.0, 68.0, life), 100.0, 1e-9);
    POE_CHECK_NEAR(ComputeSkyrimMaxHealth(100.0, 102.0, life), 150.0, 1e-9);
    POE_CHECK_NEAR(ComputeSkyrimMaxEnergyShield(100.0, 34.0, life), 50.0, 1e-9);

    const double mana = ExpectedMana(1, scion->baseInt, cfg);
    POE_CHECK_NEAR(mana, 74.0, 1e-9);
    const auto normalMana = ComputeMagickaScaling(100.0, 74.0, 10.0, mana);
    POE_CHECK(!normalMana.isZeroManaBuild);
    POE_CHECK_NEAR(normalMana.skyrimMaxMagicka, 100.0, 1e-9);
    POE_CHECK_NEAR(normalMana.skyrimSkillCost, 1000.0 / 74.0, 1e-9);

    // 피의 마법(poeMaxMana == 0): 0/0 없이 기본 매지카 유지, 비용 0, NaN/Inf 금지(결정 144)
    const auto bloodMagic = ComputeMagickaScaling(120.0, 0.0, 10.0, mana);
    POE_CHECK(bloodMagic.isZeroManaBuild);
    POE_CHECK(std::isfinite(bloodMagic.skyrimMaxMagicka));
    POE_CHECK(std::isfinite(bloodMagic.skyrimSkillCost));
    POE_CHECK_NEAR(bloodMagic.skyrimMaxMagicka, 120.0, 1e-9);
    POE_CHECK_NEAR(bloodMagic.skyrimSkillCost, 0.0, 1e-9);
}

POE_TEST("DamageConversion: difficulty-adjusted Energy Shield absorb (C2 fix)")
{
    // 전설 난이도(fDiffMultHPToPCL = 3.0): raw 20 -> 체력 단위 유효 피해 60
    // ES 45 보유 시: ES가 45 흡수 -> 남은 유효 피해 15 -> 엔진 잔여 raw = 15 / 3.0 = 5.0
    const auto partial = AbsorbWithEnergyShield(20.0, 3.0, false, 45.0);
    POE_CHECK_NEAR(partial.newEnergyShield, 0.0, 1e-9);
    POE_CHECK_NEAR(partial.absorbedEffectiveDamage, 45.0, 1e-9);
    POE_CHECK_NEAR(partial.remainingRawDamage, 5.0, 1e-9);

    // ES 100 보유 시 전액 흡수 -> 잔여 raw = 0, ES 40 남음
    const auto full = AbsorbWithEnergyShield(20.0, 3.0, false, 100.0);
    POE_CHECK_NEAR(full.newEnergyShield, 40.0, 1e-9);
    POE_CHECK_NEAR(full.absorbedEffectiveDamage, 60.0, 1e-9);
    POE_CHECK_NEAR(full.remainingRawDamage, 0.0, 1e-9);

    // 카오스 유효 피해 60 중 50을 ES 100으로 흡수하고 10이 생명에 남는다.
    const auto chaos = AbsorbWithEnergyShield(20.0, 3.0, true, 100.0);
    POE_CHECK_NEAR(chaos.newEnergyShield, 0.0, 1e-9);
    POE_CHECK_NEAR(chaos.absorbedEffectiveDamage, 50.0, 1e-9);
    POE_CHECK_NEAR(chaos.remainingRawDamage, 10.0 / 3.0, 1e-9);
}
