#include "test_harness.h"

#include "core/KillTracker.h"

#include <cmath>

using namespace SkyrimPoE::Core;

POE_TEST("KillTracker: hostility-on-first-hit, partial block contribution, zero-damage exclusion, single reward, summon/reanimate")
{
    Config cfg;
    KillTracker kt;

    // 1. 처음 맞을 때 비적대(주민 등) -> 적대화 후 처치해도 보상 0 (결정 134, 140)
    kt.RecordFirstHitHostility(0x1001, false);
    kt.RecordFirstHitHostility(0x1001, true);  // 이미 첫 적대 기록됨 -> 덮어쓰지 않음
    kt.ConfirmDamageContribution(0x1001, 25.0);
    kt.OnDeathStart(0x1001, false, false, false);
    const auto citizen = kt.OnDeathConfirm(0x1001, 10, 10, false, false, 10, cfg);
    POE_CHECK(!citizen.awarded && citizen.exileXpGained == 0);

    // 2. 부분 막기 감소(> 0) -> 기여 인정; 0 감소 -> 제외(결정 140)
    kt.RecordFirstHitHostility(0x1002, true);
    kt.ConfirmDamageContribution(0x1002, 0.0);
    kt.OnDeathStart(0x1002, false, false, false);
    POE_CHECK(!kt.OnDeathConfirm(0x1002, 10, 10, false, false, 10, cfg).awarded);

    kt.RecordFirstHitHostility(0x1003, true);
    kt.ConfirmDamageContribution(0x1003, 1.5);  // 부분 막기 후 1.5 감소
    kt.OnDeathStart(0x1003, false, false, false);
    const auto partialOk = kt.OnDeathConfirm(0x1003, 10, 10, false, false, 10, cfg);
    POE_CHECK(partialOk.awarded);
    POE_CHECK(partialOk.exileXpGained == 100);
    POE_CHECK(partialOk.gemXpGained == 10);

    // 중복 사망 확정 이벤트 -> 두 번째는 보상 0 (결정 114, 137(가)③)
    const auto dupDeath = kt.OnDeathConfirm(0x1003, 10, 10, false, false, 10, cfg);
    POE_CHECK(!dupDeath.awarded);

    // 3. 소환·되살림 대상 -> 사망 시작에 고정되어 보상 0 (결정 115)
    kt.RecordFirstHitHostility(0x1004, true);
    kt.ConfirmDamageContribution(0x1004, 50.0);
    kt.OnDeathStart(0x1004, false, true, false);  // 되살림
    POE_CHECK(!kt.OnDeathConfirm(0x1004, 10, 10, false, false, 10, cfg).awarded);

    // 3b. OnDeathStart 누락 직사 경로에서도 fallbackExcluded=true면 소환/되살림 제외
    kt.RecordFirstHitHostility(0x1005, true);
    kt.ConfirmDamageContribution(0x1005, 50.0);
    POE_CHECK(!kt.OnDeathConfirm(0x1005, 10, 10, false, false, 10, cfg, true).awarded);

    // 3c. 동료가 단독 처치해 OnDeathStart가 발생한 뒤 쓰러지는 시체를 쳐도 기여·적대 미기록(악용 차단)
    kt.OnDeathStart(0x1006, false, false, false);
    kt.RecordFirstHitHostility(0x1006, true);
    kt.ConfirmDamageContribution(0x1006, 50.0);
    POE_CHECK(!kt.OnDeathConfirm(0x1006, 10, 10, false, false, 10, cfg).awarded);

    // 4. TESResetEvent(새 삶) 이후에는 다시 정상 보상 가능(결정 113)
    kt.OnActorReset(0x1003);
    kt.RecordFirstHitHostility(0x1003, true);
    kt.ConfirmDamageContribution(0x1003, 50.0);
    kt.OnDeathStart(0x1003, false, false, false);
    POE_CHECK(kt.OnDeathConfirm(0x1003, 10, 10, false, false, 10, cfg).awarded);

    // 5. C-12: 부활/리스폰된 생존 액터 재타격 시 새 세대(lifeGeneration: 1 -> 2)로 2회차 보상 허용
    POE_CHECK(kt.GetActorRecordSnapshot(0x1003).lifeGeneration == 1);
    kt.RecordFirstHitHostility(0x1003, true, true);
    POE_CHECK(kt.GetActorRecordSnapshot(0x1003).lifeGeneration == 2);
    kt.ConfirmDamageContribution(0x1003, 50.0);
    kt.OnDeathStart(0x1003, false, false, false);
    POE_CHECK(kt.OnDeathConfirm(0x1003, 10, 10, false, false, 10, cfg).awarded);

    // 6. Q-2: 원샷 처치로 OnDeathStart가 ConfirmDamageContribution보다 먼저 와도 보상 1회 허용
    kt.RecordFirstHitHostility(0x1004, true, true);
    kt.OnDeathStart(0x1004, false, false, false);
    kt.ConfirmDamageContribution(0x1004, 120.0);
    POE_CHECK(kt.OnDeathConfirm(0x1004, 10, 10, false, false, 10, cfg).awarded);

    // 7. kRevert 시 메모리 기록 전체 초기화(결정 138)
    POE_CHECK(kt.TrackedActorCount() > 0);
    kt.ClearOnRevert();
    POE_CHECK(kt.TrackedActorCount() == 0);
}

POE_TEST("KillTracker: effective monster level, PoB 2.68.1 lower-enemy penalty, higher bonus, exile & gem progression")
{
    Config cfg;

    // 1. 오픈월드 유효 몬스터 레벨(결정 146)
    // diff clamp [-10, +10], boss +3, dragon +3 -> 합산 최대 +5
    POE_CHECK(KillTracker::ComputeEffectiveMonsterLevel(20, 50, 10, false, false, cfg) == 30);
    POE_CHECK(KillTracker::ComputeEffectiveMonsterLevel(20, 1, 30, false, false, cfg) == 10);
    POE_CHECK(KillTracker::ComputeEffectiveMonsterLevel(20, 20, 20, true, false, cfg) == 23);
    POE_CHECK(KillTracker::ComputeEffectiveMonsterLevel(20, 20, 20, true, true, cfg) == 25);  // max +5
    POE_CHECK(KillTracker::ComputeEffectiveMonsterLevel(99, 99, 99, true, true, cfg) == 100);
    POE_CHECK(KillTracker::ComputeEffectiveMonsterLevel(2, 1, 20, false, false, cfg) == 1);

    // 2. 레벨 차 경험치 배율(결정 136, 146)
    // 동렙 -> 1.0
    POE_CHECK_NEAR(KillTracker::ComputeLevelDiffXpMultiplier(20, 20, cfg), 1.0, 1e-9);
    // 낮은 적 안전 폭 내(L=20 -> safeZone = 3 + floor(20/16) = 4 -> M=16이면 d=0 -> 1.0, NaN 없음!)
    const double safeMult = KillTracker::ComputeLevelDiffXpMultiplier(20, 16, cfg);
    POE_CHECK(std::isfinite(safeMult));
    POE_CHECK_NEAR(safeMult, 1.0, 1e-9);
    // 안전 폭 밖(L=20, M=10 -> d=6) -> PoB 2.68.1 식: (25 / (25 + 6^2.5))^1.5 ~= 0.103786 (T-7)
    const double lowMult = KillTracker::ComputeLevelDiffXpMultiplier(20, 10, cfg);
    POE_CHECK_NEAR(lowMult, 0.103786, 1e-4);
    // 극단적 저렙(L=80, M=1) -> 하한 1%
    POE_CHECK_NEAR(KillTracker::ComputeLevelDiffXpMultiplier(80, 1, cfg), 0.01, 1e-9);
    // 높은 적: 레벨당 +10%, 상한 2.0
    POE_CHECK_NEAR(KillTracker::ComputeLevelDiffXpMultiplier(20, 25, cfg), 1.5, 1e-9);
    POE_CHECK_NEAR(KillTracker::ComputeLevelDiffXpMultiplier(20, 40, cfg), 2.0, 1e-9);

    // 3. 선형 임시 곡선 kills(L) = 10 + 2*(L-1), 기본 100 (결정 147)
    POE_CHECK(KillTracker::RequiredXpForNextExileLevel(1, cfg) == 1000);
    POE_CHECK(KillTracker::RequiredXpForNextExileLevel(2, cfg) == 1200);
    POE_CHECK(KillTracker::RequiredXpForNextExileLevel(20, cfg) == 4800);

    std::uint32_t lv = 1;
    std::uint64_t xp = 0;
    std::uint32_t pts = 0;
    // 2500 XP -> lv1(1000) + lv2(1200) 돌파 -> lv3, 잔여 300, 패시브 포인트 +2
    const auto up = KillTracker::ApplyExileXp(lv, xp, pts, 2500, cfg);
    POE_CHECK(up.levelsGained == 2 && lv == 3 && xp == 300 && pts == 2);

    // 4. 장착 젬 10% 성장(스킬·보조·발동·발동 안 주문/보조 모두 성장, 결정 108)
    std::array<SkillSlot, kSkillSlotCount> slots{};
    slots[0].occupied = true;
    slots[0].gem = GemInstance{
        1, "Cyclone", 1, 0, 0, 3,
        { GemInstance{ 2, "SupportMeleePhysicalDamage", 1, 0, 0, 0, {} } }
    };
    slots[1].occupied = true;
    slots[1].gem = GemInstance{
        3, "SupportCastOnCriticalStrike", 1, 0, 0, 3,
        { GemInstance{ 4, "IceNova", 1, 0, 0, 3, { GemInstance{ 5, "SupportConcentratedEffect", 1, 0, 0, 0, {} } } } }
    };

    const auto gemSum = KillTracker::ApplyGemXpToSlots(slots, 150);  // lv1 필요 XP 100 -> 5개 젬 모두 lv2, 잔여 50
    POE_CHECK(gemSum.gemsGainedXp == 5);
    POE_CHECK(gemSum.gemLevelsGained == 5);
    POE_CHECK(slots[0].gem.level == 2 && slots[0].gem.xp == 50);
    POE_CHECK(slots[0].gem.socketedGems[0].level == 2);
    POE_CHECK(slots[1].gem.level == 2);
    POE_CHECK(slots[1].gem.socketedGems[0].level == 2);
    POE_CHECK(slots[1].gem.socketedGems[0].socketedGems[0].level == 2);

    // 5. 상한 경계: 유배자 L100, 젬 L20에서는 추가 성장 및 gemsGainedXp 증가 없음
    std::uint32_t maxLv = kMaxExileLevel;
    std::uint64_t maxXp = 0;
    std::uint32_t maxPts = 0;
    const auto capRes = KillTracker::ApplyExileXp(maxLv, maxXp, maxPts, 50000, cfg);
    POE_CHECK(capRes.levelsGained == 0 && maxLv == kMaxExileLevel && maxXp == 0 && maxPts == 0);

    std::array<SkillSlot, kSkillSlotCount> maxSlots{};
    maxSlots[0].occupied = true;
    maxSlots[0].gem = GemInstance{ 99, "Cyclone", static_cast<std::uint8_t>(kMaxGemLevel), 0, 0, 3, {} };
    const auto maxGemSum = KillTracker::ApplyGemXpToSlots(maxSlots, 500);
    POE_CHECK(maxGemSum.gemsGainedXp == 0 && maxGemSum.gemLevelsGained == 0 && maxSlots[0].gem.level == kMaxGemLevel);
}
