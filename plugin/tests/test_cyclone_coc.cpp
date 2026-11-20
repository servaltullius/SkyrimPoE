#include "test_harness.h"

#include "core/CycloneState.h"
#include "core/DataPack.h"
#include "core/EnergyShield.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"

#include <filesystem>

using namespace SkyrimPoE::Core;

namespace
{
    DataPack LoadStage2Pack()
    {
        DataPack pack;
        std::string err;
        const std::filesystem::path packPath = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "core_3_30.json";
        if (!pack.LoadFromFile(packPath, &err)) {
            throw std::runtime_error("Failed to load core_3_30.json: " + err);
        }
        return pack;
    }
}

POE_TEST("CycloneState: start magicka check, pending start shortfall, single-cycle cost, outro no-cost/no-hit, stagger")
{
    CycloneStateMachine sm;

    // 1. 시작 이벤트에서 매지카 부족 -> false(원래 강공격 통과, 결정 97, 122)
    POE_CHECK(!sm.TryBeginFromPowerAttack(true, false, true, 5.0, 10.0));
    POE_CHECK(!sm.IsActive());

    // 프렌지 양보 중 -> false(결정 63)
    POE_CHECK(!sm.TryBeginFromPowerAttack(true, true, true, 50.0, 10.0));
    POE_CHECK(!sm.IsActive());

    // 2. 시작 승인 뒤 첫 주 갱신 시점에 매지카 부족 -> OAR 켜지 않고 종료, 유료 타격 0회(결정 128)
    POE_CHECK(sm.TryBeginFromPowerAttack(true, false, true, 20.0, 10.0));
    POE_CHECK(sm.Phase() == CyclonePhase::PendingStart);
    CycloneTickInput in;
    in.deltaSec = 0.016;
    in.buttonHeld = true;
    in.playerAllowed = true;
    in.weaponSpeed = 1.0;
    in.poeAttackSpeedMult = 3.0;  // 3 hits/s -> period = 0.3333s
    in.currentMagicka = 5.0;      // 부족!
    in.cycleMagickaCost = 10.0;
    const auto failStart = sm.Tick(in);
    POE_CHECK(!failStart.startOar);
    POE_CHECK(!failStart.consumeMagicka);
    POE_CHECK(!failStart.performAreaHit);
    POE_CHECK(failStart.ended);
    POE_CHECK(!sm.IsActive());

    // 3. 정확히 1회분 매지카 -> 첫 주 갱신에 1회 차감 + OAR 시작, 반 주기(0.167s) 뒤 첫 타격(결정 128)
    POE_CHECK(sm.TryBeginFromPowerAttack(true, false, true, 10.0, 10.0));
    in.currentMagicka = 10.0;
    const auto startOut = sm.Tick(in);
    POE_CHECK(startOut.startOar);
    POE_CHECK(startOut.consumeMagicka);
    POE_CHECK_NEAR(startOut.magickaToConsume, 10.0, 1e-9);
    POE_CHECK(!startOut.performAreaHit);
    POE_CHECK(sm.Phase() == CyclonePhase::Channeling);

    // 반 주기 경과 -> 첫 타격 발생, 아직 두 번째 비용 시점 전
    in.currentMagicka = 0.0;  // 1회분 소비 완료
    in.deltaSec = 0.10;
    const auto mid1 = sm.Tick(in);
    POE_CHECK(!mid1.performAreaHit);
    const auto mid2 = sm.Tick(in);  // 누적 0.20s > 0.1667s -> 첫 타격!
    POE_CHECK(mid2.performAreaHit);
    POE_CHECK(!mid2.consumeMagicka);

    // 4. 약한 경직(<= 0.5)은 삼키고, 큰 경직(> 0.5)은 즉시 종료 후 통과(결정 56, 150 ③)
    const auto lightStagger = sm.OnStaggerStart(0.5, 0.5);
    POE_CHECK(lightStagger.swallowEvent && !lightStagger.abortedChannel);
    POE_CHECK(sm.IsActive());

    const auto heavyStagger = sm.OnStaggerStart(0.75, 0.5);
    POE_CHECK(!heavyStagger.swallowEvent && heavyStagger.abortedChannel);
    POE_CHECK(!sm.IsActive());

    // 5. 버튼 뗌 + 최소 시간 경과 -> Outro 진입, Outro 동안 무비용·무타격(결정 150 ②)
    POE_CHECK(sm.TryBeginFromPowerAttack(true, false, true, 100.0, 10.0));
    in.currentMagicka = 100.0;
    in.minChannelSec = 0.15;
    in.outroSec = 0.12;
    in.buttonHeld = true;
    (void)sm.Tick(in);  // PendingStart -> Channeling
    in.deltaSec = 0.08;
    (void)sm.Tick(in);  // elapsed 0.08
    (void)sm.Tick(in);  // elapsed 0.16 >= minChannelSec
    in.buttonHeld = false;
    in.deltaSec = 0.05;
    const auto toOutro = sm.Tick(in);
    POE_CHECK(sm.Phase() == CyclonePhase::Outro);
    POE_CHECK(!toOutro.consumeMagicka && !toOutro.performAreaHit);

    const auto inOutro1 = sm.Tick(in);
    POE_CHECK(!inOutro1.consumeMagicka && !inOutro1.performAreaHit);
    in.deltaSec = 0.10;
    const auto endOutro = sm.Tick(in);
    POE_CHECK(!endOutro.consumeMagicka && !endOutro.performAreaHit);
    POE_CHECK(endOutro.stopOar && endOutro.ended && !sm.IsActive());

    // 이벤트 이름 판별
    POE_CHECK(CycloneStateMachine::IsAttackStartEvent("attackStart"));
    POE_CHECK(CycloneStateMachine::IsAttackStartEvent("attackPowerStartForward"));
    POE_CHECK(!CycloneStateMachine::IsAttackStartEvent("attackStop"));
    POE_CHECK(CycloneStateMachine::IsPowerAttackStartEvent("attackPowerStartForward"));
    POE_CHECK(!CycloneStateMachine::IsPowerAttackStartEvent("attackStart"));
    POE_CHECK(!CycloneStateMachine::IsPowerAttackStartEvent("blockStart"));
    POE_CHECK(CycloneStateMachine::IsStaggerStartEvent("staggerStart"));
    POE_CHECK(CycloneStateMachine::IsStaggerStartEvent("StaggerStart"));
}

POE_TEST("CycloneState: high attack speed phase stability, out_of_magicka during minChannel, gamePaused/disallowed stop")
{
    CycloneStateMachine sm;
    CycloneTickInput in;
    in.buttonHeld = true;
    in.playerAllowed = true;
    in.weaponSpeed = 1.0;
    in.poeAttackSpeedMult = 20.0;  // period = 0.05s (최대 공속)
    in.currentMagicka = 100.0;
    in.cycleMagickaCost = 10.0;
    in.minChannelSec = 0.6;

    POE_CHECK(sm.TryBeginFromPowerAttack(true, false, true, 100.0, 10.0));
    in.deltaSec = 0.01;
    POE_CHECK(sm.Tick(in).startOar);

    // 고공속 + 프레임 지연(dt = 0.08 >= period 0.05) 반복 시에도 매 틱 비용과 타격이 1:1 위상을 유지해야 한다
    int hits = 0;
    int costs = 1;  // 시작 1회
    in.deltaSec = 0.08;
    for (int i = 0; i < 4; ++i) {
        const auto out = sm.Tick(in);
        if (out.performAreaHit) ++hits;
        if (out.consumeMagicka) ++costs;
    }
    POE_CHECK(hits == 4);
    POE_CHECK(costs == 5);

    // minChannelSec 미달 중 마나 고갈 -> 즉시 종료(결정 97)
    in.currentMagicka = 2.0;
    const auto oom = sm.Tick(in);
    POE_CHECK(oom.ended && oom.stopOar && oom.endReason == "out_of_magicka");
    POE_CHECK(!sm.IsActive());

    // gamePaused 즉시 종료(결정 150 ④)
    POE_CHECK(sm.TryBeginFromPowerAttack(true, false, true, 100.0, 10.0));
    in.currentMagicka = 100.0;
    in.deltaSec = 0.01;
    (void)sm.Tick(in);
    in.gamePaused = true;
    const auto pausedOut = sm.Tick(in);
    POE_CHECK(pausedOut.ended && pausedOut.stopOar && !sm.IsActive());
}

POE_TEST("SkillSystem (CoC): round-robin 2 spells, insufficient magicka keeps cursor/cd, depth-1 guard, own cd max")
{
    const DataPack pack = LoadStage2Pack();
    Config cfg;
    cfg.enforceGemRequirements = false;  // Lv10 Scion 기본 스탯으로 CoC 순환·쿨다운 규칙 독립 검증
    StatEngine engine;

    Layer1Input l1;
    l1.exileLevel = 10;
    l1.className = "Scion";
    l1.slots[0].slotIndex = 0;
    l1.slots[0].occupied = true;
    l1.slots[0].gem = GemInstance{ 100, "Cyclone", 1, 0, 0, 3, {} };

    l1.slots[1].slotIndex = 1;
    l1.slots[1].occupied = true;
    l1.slots[1].linkedAttackSlot = 0;
    l1.slots[1].gem = GemInstance{
        200, "SupportCastOnCriticalStrike", 1, 0, 0, 3,
        {
            GemInstance{ 301, "IceNova", 1, 0, 0, 3, {} },
            GemInstance{ 302, "ShockNova", 1, 0, 0, 3, {} },
        }
    };

    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snap = engine.GetSnapshot();

    SkillSystem sys;
    // 1. 적 2명 동시 치명타 -> 주문 2개(IceNova, ShockNova) 각각 1회 순환 발동
    const auto cast1 = sys.TryTriggerOnCrit(0, *snap, l1.slots, 200.0, 100.0, pack, cfg);
    POE_CHECK(cast1.has_value());
    POE_CHECK(cast1->gemId == "IceNova");
    POE_CHECK(cast1->skyrimHitDamage > 0.0);
    POE_CHECK(cast1->skyrimMagickaCost > 0.0);
    POE_CHECK_NEAR(sys.RemainingCooldown(301), 0.15, 1e-6);
    POE_CHECK(sys.RoundRobinCursor(200) == 1);

    const auto cast2 = sys.TryTriggerOnCrit(0, *snap, l1.slots, 200.0, 100.0, pack, cfg);
    POE_CHECK(cast2.has_value());
    POE_CHECK(cast2->gemId == "ShockNova");
    POE_CHECK_NEAR(sys.RemainingCooldown(302), 0.15, 1e-6);
    POE_CHECK(sys.RoundRobinCursor(200) == 0);

    // 둘 다 쿨다운 중이면 3번째 치명타는 발동 안 함
    POE_CHECK(!sys.TryTriggerOnCrit(0, *snap, l1.slots, 200.0, 100.0, pack, cfg).has_value());

    // 2. 쿨다운 회복 후 매지카 부족 -> 다음 싼 주문으로 건너뛰지 않고 폐기, 순환 차례(0) 유지, 쿨다운 미소모(결정 126, 127)
    sys.TickCooldowns(0.20, false);
    POE_CHECK_NEAR(sys.RemainingCooldown(301), 0.0, 1e-9);
    StatSnapshot expensiveFirst = *snap;
    expensiveFirst.triggeredSpellStats[301].poeManaCost = 40.0;  // 비싼 주문
    expensiveFirst.triggeredSpellStats[302].poeManaCost = 1.0;   // 싼 주문
    const auto noMana = sys.TryTriggerOnCrit(0, expensiveFirst, l1.slots, 20.0, 100.0, pack, cfg);
    POE_CHECK(!noMana.has_value());
    POE_CHECK(sys.RoundRobinCursor(200) == 0);
    POE_CHECK_NEAR(sys.RemainingCooldown(301), 0.0, 1e-9);
    POE_CHECK_NEAR(sys.RemainingCooldown(302), 0.0, 1e-9);

    // 3. 발동 주문 min~max 피해 롤 및 주문 치명타 배율 적용 검증
    bool isCrit = false;
    const double nonCritDmg = SkillSystem::RollTriggeredSpellSkyrimDamage(*cast1, pack, cfg, 0.5, 0.99, isCrit);
    POE_CHECK(!isCrit && nonCritDmg > 0.0);
    const double critDmg = SkillSystem::RollTriggeredSpellSkyrimDamage(*cast1, pack, cfg, 0.5, 0.001, isCrit);
    POE_CHECK(isCrit);
    POE_CHECK_NEAR(critDmg, nonCritDmg * 1.5, 1e-6);

    // 4. 깊이 1 가드(결정 103): 트리거 실행 중 추가 발동 차단
    sys.EnterTriggerDepth();
    POE_CHECK(!sys.TryTriggerOnCrit(0, *snap, l1.slots, 200.0, 100.0, pack, cfg).has_value());
    sys.ExitTriggerDepth();

    // 5. 자체 쿨다운 2.5초 주문 -> max(0.15, 2.5) = 2.5 적용
    StatSnapshot customSnap = *snap;
    customSnap.triggeredSpellStats[301].cooldownSec = 2.5;
    const auto longCdCast = sys.TryTriggerOnCrit(0, customSnap, l1.slots, 200.0, 100.0, pack, cfg);
    POE_CHECK(longCdCast.has_value());
    POE_CHECK_NEAR(longCdCast->cooldownAppliedSec, 2.5, 1e-6);
    POE_CHECK_NEAR(sys.RemainingCooldown(301), 2.5, 1e-6);
}

POE_TEST("EnergyShieldPool: difficulty absorb, 2s recharge delay, chaos interrupts recharge, pause freezes tick")
{
    EnergyShieldPool pool;
    pool.Configure(2.0, 0.33);
    pool.ResetFull(100.0);

    // 전설(diff=3.0) 원시 20 -> 유효 60 흡수 -> ES 40 남음, 잔여 0
    const auto r1 = pool.AbsorbDamage(20.0, 3.0, false);
    POE_CHECK_NEAR(r1.absorbedEffectiveDamage, 60.0, 1e-9);
    POE_CHECK_NEAR(r1.remainingRawDamage, 0.0, 1e-9);
    POE_CHECK_NEAR(pool.Snapshot().current, 40.0, 1e-9);

    // 일시정지 중에는 재충전 지연이 흐르지 않음
    pool.Tick(5.0, true);
    POE_CHECK_NEAR(pool.Snapshot().current, 40.0, 1e-9);

    // 1.5초 경과 -> 아직 지연(2.0s) 미만이라 미재충전
    pool.Tick(1.5, false);
    POE_CHECK_NEAR(pool.Snapshot().current, 40.0, 1e-9);

    // 카오스 피해는 ES를 우회하지만 재충전 지연을 0으로 리셋
    const auto chaos = pool.AbsorbDamage(10.0, 3.0, true);
    POE_CHECK_NEAR(chaos.remainingRawDamage, 10.0, 1e-9);
    POE_CHECK_NEAR(pool.Snapshot().current, 40.0, 1e-9);
    pool.Tick(1.5, false);
    POE_CHECK_NEAR(pool.Snapshot().current, 40.0, 1e-9);

    // 추가 0.5초(합 2.0초) + 1.0초 -> 초당 33 회복 (40 + 33 = 73)
    pool.Tick(0.5, false);
    pool.Tick(1.0, false);
    POE_CHECK_NEAR(pool.Snapshot().current, 73.0, 1e-6);
}
