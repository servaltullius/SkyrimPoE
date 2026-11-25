#pragma once

#include "core/Types.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace SkyrimPoE::Core
{
    struct KillRewardResult
    {
        bool awarded = false;
        bool playerContributed = false;
        std::uint32_t effectiveMonsterLevel = 1;
        double xpMultiplier = 1.0;
        std::uint64_t exileXpGained = 0;
        std::uint64_t gemXpGained = 0;
    };

    struct ExileLevelUpResult
    {
        std::uint32_t levelsGained = 0;
        std::uint32_t newLevel = 1;
    };

    struct GemLevelUpSummary
    {
        std::size_t gemsGainedXp = 0;
        std::size_t gemLevelsGained = 0;
    };

    struct ActorKillRecordSnapshot
    {
        bool found = false;
        bool firstHostilityRecorded = false;
        bool wasHostileOnFirstHit = false;
        bool playerContributed = false;
        bool deathStarted = false;
        bool deathExcluded = false;
        bool rewardClaimed = false;
        bool directHitActive = false;
        std::uint32_t lifeGeneration = 0;
    };

    // 결정 108, 113~116, 134~140, 146~149:
    // 적의 첫 피격 적대 기록, 실제 체력 감소 기여, 사망 시작/확정 보상 1회 지급,
    // 오픈월드 유효 몬스터 레벨 및 PoB 2.68.1 레벨 차 경험치 곡선, 장착 젬 10% 성장.
    class KillTracker
    {
    public:
        // DoDamage 중 동기로 사망 확정이 도착할 수 있다. 실제 감소의 사후 기록은 별도로 유지한다.
        class DirectHitScope
        {
        public:
            DirectHitScope(KillTracker* tracker, std::uint32_t actorFormId);
            ~DirectHitScope();
            DirectHitScope(const DirectHitScope&) = delete;
            DirectHitScope& operator=(const DirectHitScope&) = delete;

        private:
            KillTracker* tracker_;
            std::uint32_t actorFormId_;
            std::uint64_t scopeId_ = 0;
        };

        // 피해 적용 직전: 이번 삶에서 첫 피격이면 적대 여부를 기록한다(결정 134, 140, 148).
        // C-12: actorAliveForNewLife=true이고 직전 삶이 이미 보상 종료(rewardClaimed)된 상태면
        // 리셋 없이 부활했거나 FF 참조 FormID가 재사용된 새 삶으로 보고 lifeGeneration을 전진한다.
        void RecordFirstHitHostility(std::uint32_t actorFormId, bool wasHostileToPlayer, bool actorAliveForNewLife = false);

        // 실제 체력/에너지 실드 감소 확정 시 호출(결정 113, 140: 부분 막기 감소 > 0 포함, 0 감소 제외).
        void ConfirmDamageContribution(std::uint32_t actorFormId, double actualLoss);

        // TESDeathEvent(dead=false): 소환/되살림/지배 상태를 사망 시작 시점에 고정(결정 114, 115).
        void OnDeathStart(std::uint32_t actorFormId, bool isSummoned, bool isReanimated, bool isCommanded);

        // TESDeathEvent(dead=true): 사망 확정 시점에만 보상 1회 지급 후 삶 갱신(결정 114, 137(가)).
        // fallbackExcluded: dead=false가 누락된 직사 경로에서도 소환·되살림·지배를 제외하기 위한 폴백.
        [[nodiscard]] KillRewardResult OnDeathConfirm(
            std::uint32_t actorFormId,
            std::uint32_t enemySkyrimLevel,
            std::uint32_t playerSkyrimLevel,
            bool isBoss,
            bool isDragon,
            std::uint32_t currentExileLevel,
            const Config& config,
            bool fallbackExcluded = false,
            std::uint32_t overrideEffectiveMonsterLevel = 0,
            std::uint32_t gemGeneration = 0);

        // TESResetEvent: 부활/리스폰된 액터의 새 삶 시작(결정 113).
        void OnActorReset(std::uint32_t actorFormId);

        // kRevert / 새 게임 시 메모리 기여·적대 기록 전체 초기화(결정 138).
        void ClearOnRevert();

        [[nodiscard]] std::size_t TrackedActorCount() const;
        [[nodiscard]] ActorKillRecordSnapshot GetActorRecordSnapshot(std::uint32_t actorFormId) const;

        // 순수 계산 함수(단위 테스트 및 UI 표기 공용)
        [[nodiscard]] static std::uint32_t ComputeEffectiveMonsterLevel(
            std::uint32_t exileLevel,
            std::uint32_t enemySkyrimLevel,
            std::uint32_t playerSkyrimLevel,
            bool isBoss,
            bool isDragon,
            const Config& config) noexcept;

        [[nodiscard]] static double ComputeLevelDiffXpMultiplier(
            std::uint32_t exileLevel,
            std::uint32_t effectiveMonsterLevel,
            const Config& config) noexcept;

        [[nodiscard]] static std::uint64_t RequiredXpForNextExileLevel(
            std::uint32_t exileLevel,
            const Config& config) noexcept;

        [[nodiscard]] static ExileLevelUpResult ApplyExileXp(
            std::uint32_t& exileLevel,
            std::uint64_t& exileXp,
            std::uint32_t& unallocatedPassivePoints,
            std::uint64_t gainedXp,
            const Config& config) noexcept;

        [[nodiscard]] static std::uint64_t RequiredXpForNextGemLevel(std::uint8_t gemLevel) noexcept;

        [[nodiscard]] static GemLevelUpSummary ApplyGemXpToSlots(
            std::array<SkillSlot, kSkillSlotCount>& slots,
            std::uint64_t gemXp, std::uint32_t gemGeneration = 0) noexcept;

    private:
        struct ActorLifeRecord
        {
            std::uint32_t lifeGeneration = 1;
            std::uint64_t lastTouchedSeq = 0;
            std::uint64_t directHitScopeId = 0;
            std::size_t directHitDepth = 0;
            bool firstHostilityRecorded = false;
            bool wasHostileOnFirstHit = false;
            bool playerContributed = false;
            bool deathStarted = false;
            bool deathExcluded = false;  // 소환·되살림·지배
            bool rewardClaimed = false;
        };

        mutable std::mutex mutex_;
        std::uint64_t seqCounter_ = 0;
        std::unordered_map<std::uint32_t, ActorLifeRecord> records_;
    };
}
