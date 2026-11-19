#pragma once

#include "core/DamageConversion.h"
#include "core/DataPack.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <array>
#include <cstdint>
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
        double skyrimHitDamage = 0.0;   // 평균 기대값(참고·비롤 폴백용)
        double areaMult = 1.0;
        double cooldownAppliedSec = 0.15;
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
            const Config& config);

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
        std::unordered_map<std::uint64_t, double> cooldowns_;
        // 결정 130: 순환 커서는 슬롯 번호가 아니라 발동 젬 인스턴스 ID에 귀속한다.
        std::unordered_map<std::uint64_t, std::size_t> cursors_;
        std::uint32_t triggerDepth_ = 0;
    };
}
