#pragma once

#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <cstdint>

namespace SkyrimPoE::Runtime
{
    // 5단계(결정 55, 111, 163, 프로토타입 6 검증):
    // - RE::VTABLE_MissileProjectile[0] vtable 0xBD (AddImpact) 체인 훅
    // - -a_velocity 부호 반전, 원자적 단회 적중 가드, CommandQueue 주 갱신 이연 실행
    // - 관통·분기·연쇄·아군 통과(결정 111) 전 경로 shooter=hitActor + clearance + lift 재발사
    // - Arc 즉발 연쇄, SpellEcho 0.18초 반복 시전, HeraldOfIce/HeraldOfAsh 처치 폭발
    class ProjectileRuntime
    {
    public:
        static bool Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            Core::SkillSystem* skillSystem,
            const Core::DataPack* dataPack,
            const Core::Config* config);

        [[nodiscard]] static bool HasSpellDelivery(std::uint32_t generation, std::string_view gemId) noexcept;

        // Main thread: launch the projectile, nova or chain delivery for a ready spell.
        static bool LaunchSpellDelivery(
            const Core::TriggeredSpellCast& cast,
            const RE::NiPoint3& aimOrigin,
            const RE::NiPoint3& aimDir);

        // 주 갱신 스레드 전용: 플레이어 시선/손 기준 발사 원점·방향 산출 헬퍼
        static void ComputePlayerAim(
            RE::PlayerCharacter* player,
            RE::NiPoint3& outOrigin,
            RE::NiPoint3& outDir) noexcept;

        // 주 갱신 스레드 틱: SpellEcho 예약 반복 시전 + 8초 초과 만료 투사체 정리
        static void Tick(double deltaSec, bool gamePaused);

        // DeathListener 호출: 얼음의 전령·재의 전령 처치 폭발
        static void TriggerHeraldOfIceBurst(const RE::NiPoint3& origin, const Core::StatSnapshot& snap);
        static void TriggerHeraldOfAshBurst(const RE::NiPoint3& origin, double overkillSkyrimDmg, const Core::StatSnapshot& snap);

        // 진단·AutoTest F5 카운터
        [[nodiscard]] static std::uint64_t TotalLaunchedCount() noexcept;
        [[nodiscard]] static std::uint64_t TotalImpactCount() noexcept;
        [[nodiscard]] static std::uint64_t TotalRelaunchCount() noexcept;

        // Main-thread diagnostics for the generation-one Spark game test.
        struct TestHitResult
        {
            unsigned launched = 0;
            unsigned hits = 0;
            double firstHpLost = 0.0;
            bool crit = false;
            // 진단: 피해 적용 전 충돌 결과(표적·다른 액터·지형)와 표적 피해 적용 실패 수, 마지막 표적 피해 계산값.
            unsigned targetImpacts = 0;
            unsigned otherActorImpacts = 0;
            unsigned nonActorImpacts = 0;
            unsigned targetDealFailures = 0;
            double lastTargetSkyrimDamage = 0.0;
        };
        static void SetHitTargetForTest(RE::FormID actorId);
        [[nodiscard]] static TestHitResult GetHitForTest();
        static void ResetOnRevert();
    };
}
