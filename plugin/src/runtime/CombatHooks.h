#pragma once

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/EnergyShield.h"
#include "core/KillTracker.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <cstdint>

namespace SkyrimPoE::Runtime
{
    // 사이클론 타격 시 DealDamage(38627) → ApplyHit(38627+0x4A8)로 PoE 피해 값을 전달하는 스레드 로컬 문맥.
    struct CycloneHitContext
    {
        bool active = false;
        double convertedSkyrimDamage = 0.0;
        bool isCrit = false;
        RE::FormID expectedTargetId = 0;
        bool* outAbsorbedByMonsterEs = nullptr;  // 검수 M3: 적 ES 100% 흡수 시에도 CoC/상태이상 발동 보장
    };

    class ScopedCycloneHitContext
    {
    public:
        ScopedCycloneHitContext(
            double convertedSkyrimDamage,
            bool isCrit,
            RE::FormID targetId,
            bool* outAbsorbedByMonsterEs = nullptr) noexcept;
        ~ScopedCycloneHitContext() noexcept;
        ScopedCycloneHitContext(const ScopedCycloneHitContext&) = delete;
        ScopedCycloneHitContext& operator=(const ScopedCycloneHitContext&) = delete;

    private:
        CycloneHitContext previous_;
    };

    // 결정 51, 53, 65~73, 75~87, 93, 94, 113, 134, 140, 142, 144:
    // - 주는/받는 타격 훅(38586 호출 7+1자리, 폭발 비율 44004+0xC1)
    // - 받는 마법 훅(AddTarget::SetEffectiveness 34547+0x656, 35086 vtable 15 + 직접 호출 8)
    // - 실제 체력 감소 기여 확정(Character/PlayerCharacter HandleHealthDamage 0x104)
    // - 플레이어 최대 체력·매지카·에너지 실드 풀 동기화 및 ES 재충전
    class CombatHooks
    {
    public:
        static bool Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            Core::EnergyShieldPool* esPool,
            Core::KillTracker* killTracker,
            const Core::DataPack* dataPack,
            const Core::Config* config,
            Core::SkillSystem* skillSystem = nullptr,
            Core::CharmSystem* charmSystem = nullptr);

        // 주 갱신 스레드 프레임 콜백에서 호출: 최대 체력·매지카·ES 동기화, MoM 매지카 차감, Low Life/EO 감시 및 ES 재충전.
        static void TickPlayerPools(double deltaSec, bool gamePaused);

        // 3단계 Elemental Overload: 원소/스킬 치명타 발생 시 8초 원소 피해 증폭 타이머 갱신
        static void NotifyPlayerElementalCrit() noexcept;

        // 현재 플레이어의 바닐라(모드 보정 제외) 최대 매지카 추정치(비용 환산용)
        [[nodiscard]] static double GetBaseSkyrimMaxMagicka(RE::PlayerCharacter* player) noexcept;

        // 진단·자동 시험용: 플레이어가 아군(동료)에게 실제 체력 피해를 준 누적 횟수
        [[nodiscard]] static std::uint64_t GetPlayerToTeammateHitCount() noexcept;
    };
}
