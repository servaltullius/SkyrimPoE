#pragma once

#include "core/DamageConversion.h"
#include "core/DataPack.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace SkyrimPoE::Core
{
    struct TriggeredSpellCast
    {
        std::uint64_t spellInstanceId = 0;
        std::string gemId;
        std::uint32_t exileLevel = 1;
        DamagePacket poeBaseHitDamage;
        double effectiveCritChancePct = 0.0;
        double critMultiplierPct = 150.0;
        double skyrimMagickaCost = 0.0;
        double skyrimHealthCost = 0.0;  // Blood Magic 시 생명력 비용(1 HP 자살 방지 가드 통과 완료)
        bool usesHealthCost = false;
        double skyrimHitDamage = 0.0;   // 평균 기대값(참고·비롤 폴백용)
        double areaMult = 1.0;
        double cooldownAppliedSec = 0.15;

        // 5단계: 투사체·연쇄·분기·관통·반복·상태이상·손 시전 메타
        bool isHandCast = false;
        std::uint8_t projectileCount = 1;
        std::uint8_t chainCount = 0;
        bool forkEnabled = false;
        std::uint8_t pierceCount = 0;
        bool alwaysPierce = false;
        std::uint8_t spellRepeatCount = 0;
        double igniteChancePct = 0.0;
        double arcRemainingChainMorePct = 0.0;
        bool starforgeShock = false;
    };

    // 5단계(결정 163): Molten Shell 가드 버프 상태 스냅샷
    struct GuardShieldSnapshot
    {
        bool active = false;
        double currentShieldSkyrim = 0.0;
        double maxShieldSkyrim = 0.0;
        double remainingDurationSec = 0.0;
        double cooldownRemainingSec = 0.0;
        double bonusFlatArmour = 0.0;
        double totalAbsorbedSkyrim = 0.0;
    };

    struct GuardAbsorbResult
    {
        double absorbedEffective = 0.0;
        double remainingRawDamage = 0.0;
        bool brokeNow = false;
        double reflectedFireSkyrim = 0.0;
    };

    struct GuardTickResult
    {
        bool stateChanged = false;     // 만료 등으로 StatEngine 재계산 필요
        bool expiredNow = false;
        double reflectedFireSkyrim = 0.0;
    };

    struct GuardActivationResult
    {
        bool ok = false;
        double skyrimMagickaCost = 0.0;
        double skyrimHealthCost = 0.0;
        bool usesHealthCost = false;
        double bonusFlatArmour = 0.0;
        double maxShieldSkyrim = 0.0;
        std::string failReason;
    };

    // 결정 100~103, 126, 127, 129~132, 145:
    // 치명타 시 시전(CoC) 발동 젬의 순환 시전·개별 쿨다운·매지카 즉시 판정·깊이 1 제한.
    class SkillSystem
    {
    public:
        static constexpr double kMinTriggerCooldownSec = 0.15;

        void TickCooldowns(double deltaSec, bool gamePaused);
        void ResetOnRevert();

        // 깊이 1 가드(발동 주문이 다시 발동을 부르지 않도록 차단, 결정 103)
        void EnterTriggerDepth() noexcept { ++triggerDepth_; }
        void ExitTriggerDepth() noexcept { if (triggerDepth_ > 0) --triggerDepth_; }
        [[nodiscard]] std::uint32_t TriggerDepth() const noexcept { return triggerDepth_; }

        // 주 갱신 스레드에서 치명타 적중(체력 감소 > 0, 치명타 처치 포함) 1건마다 호출.
        // - 준비된 다음 주문을 순환 탐색하고, 매지카가 부족하면 발동 폐기·차례 유지·쿨다운 미소모(결정 126, 127).
        // - 통과 시 즉시 쿨다운을 시작하고 순환 차례를 전진한 뒤 실행 정보를 반환한다.
        [[nodiscard]] std::optional<TriggeredSpellCast> TryTriggerOnCrit(
            std::uint8_t attackSlotIndex,
            const StatSnapshot& snapshot,
            const std::array<SkillSlot, kSkillSlotCount>& slots,
            double currentSkyrimMagicka,
            double baseSkyrimMaxMagicka,
            const DataPack& pack,
            const Config& config,
            double currentSkyrimHealth = 100.0,
            double skyrimMaxHealth = 100.0);

        // 5단계(결정 163): 대리 주문 손 시전 빌드(쿨다운·요구치·비용 판정 및 쿨다운 시작)
        [[nodiscard]] std::optional<TriggeredSpellCast> TryBuildHandCastSpell(
            std::uint8_t slotIndex,
            const StatSnapshot& snapshot,
            const std::array<SkillSlot, kSkillSlotCount>& slots,
            double currentSkyrimMagicka,
            double baseSkyrimMaxMagicka,
            const DataPack& pack,
            const Config& config,
            double currentSkyrimHealth = 100.0,
            double skyrimMaxHealth = 100.0);

        // 5단계(결정 163): Molten Shell 즉발 가드 파워 발동·흡수·틱 (CombatHooks 워커 스레드 안전)
        [[nodiscard]] GuardActivationResult TryActivateMoltenShell(
            std::uint8_t slotIndex,
            const StatSnapshot& snapshot,
            const std::array<SkillSlot, kSkillSlotCount>& slots,
            double currentSkyrimMagicka,
            double baseSkyrimMaxMagicka,
            double baseSkyrimMaxHealth,
            const DataPack& pack,
            const Config& config,
            double currentSkyrimHealth = 100.0,
            double skyrimMaxHealth = 100.0);

        [[nodiscard]] GuardAbsorbResult AbsorbWithGuardShield(
            double rawPostDefenseDamage,
            double difficultyToPcMult,
            bool isDoT);

        [[nodiscard]] GuardTickResult TickGuardShield(double deltaSec, bool gamePaused);
        [[nodiscard]] GuardShieldSnapshot GuardSnapshot() const;

        // 발동 주문의 min~max 피해 롤 + 주문 치명타 판정/배율 적용 후 스카이림 피해로 환산한다.
        [[nodiscard]] static double RollTriggeredSpellSkyrimDamage(
            const TriggeredSpellCast& cast,
            const DataPack& pack,
            const Config& config,
            double damageRoll01,
            double critRoll01,
            bool& outIsCrit) noexcept;

        [[nodiscard]] double RemainingCooldown(std::uint64_t spellInstanceId) const;
        [[nodiscard]] std::size_t RoundRobinCursor(std::uint64_t triggerGemInstanceId) const;

    private:
        [[nodiscard]] double RemainingCooldownLocked(std::uint64_t spellInstanceId) const noexcept;

        mutable std::mutex cooldownMutex_;
        std::unordered_map<std::uint64_t, double> cooldowns_;
        // 결정 130: 순환 커서는 슬롯 번호가 아니라 발동 젬 인스턴스 ID에 귀속한다.
        std::unordered_map<std::uint64_t, std::size_t> cursors_;
        std::uint32_t triggerDepth_ = 0;

        mutable std::mutex guardMutex_;
        GuardShieldSnapshot guard_{};
        double guardAbsorbRatio_ = 0.75;
        double guardCooldownOnEndSec_ = 4.0;
        double guardReflectPerAbsorbedSkyrim_ = 1.0;  // 검수 M3: ConvertOffenseHit 기반 공격 환산 반사 계수
    };
}
