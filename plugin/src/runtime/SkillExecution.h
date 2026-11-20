#pragma once

#include "core/KillTracker.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <cstdint>
#include <vector>

namespace SkyrimPoE::Runtime
{
    // 결정 52, 53, 55, 109~112, 133, 139, 148:
    // 표적 선택(아군 제외·적대 판정), 난이도 배율 조회, DLL 직접 피해(DoDamage) + 무해 EXPL 연출, 재귀 가드.
    class SkillExecution
    {
    public:
        static constexpr float kVerticalHeightLimit = 150.0f;

        static void Initialize(Core::KillTracker* killTracker, Core::StatEngine* statEngine = nullptr);

        // 현재 스레드가 우리 DLL의 DoDamage 직접 피해 호출 안에 있는지 여부(재귀 방지)
        [[nodiscard]] static bool IsInPluginDirectDamage() noexcept;

        // 결정 52: 플레이어 본인, 동료, 플레이어 소환수/지배 대상은 아군으로 제외
        [[nodiscard]] static bool IsAllyOfPlayer(const RE::PlayerCharacter* player, const RE::Actor* actor) noexcept;

        // U3 확정(2026-11-20): 전 모듈 단일 보스 판정 기준(ActorTypeDragon 키워드 또는 LocTypeBoss 참조 타입)
        [[nodiscard]] static bool IsDragonActor(const RE::Actor* actor) noexcept;
        [[nodiscard]] static bool HasLocationBossRefType(const RE::Actor* actor) noexcept;
        [[nodiscard]] static bool IsBossOrDragon(const RE::Actor* actor) noexcept;

        // 결정 109, 133, 139, 148: 번지는 피해(isDirectHit=false)는 현재 적대(IsHostileToActor) 필수,
        // 직격(isDirectHit=true)은 비적대 대상도 허용(아군은 항상 제외).
        [[nodiscard]] static bool CanDamageTarget(
            RE::PlayerCharacter* player,
            RE::Actor* target,
            bool isDirectHit);

        // origin 반경 안의 유효한 적대 대상 수집(결정 52, 109, 148)
        [[nodiscard]] static std::vector<RE::NiPointer<RE::Actor>> CollectHostileTargets(
            RE::PlayerCharacter* player,
            const RE::NiPoint3& origin,
            float radius);

        // 엔진 게임 설정에서 받는(toPlayer=true) 또는 주는(toPlayer=false) 난이도 배율을 읽는다(실패 시 1.0).
        [[nodiscard]] static double GetDifficultyMultiplier(bool toPlayer) noexcept;

        // 주 갱신 스레드 전용: 고정된 좌표(origin)에 무해 EXPL 연출을 직접 생성하고 반경 안 적대 대상에게 DoDamage 적용.
        // 번지는 피해 전용으로, 실행 직전 대상 유효성과 적대 여부(IsHostileToActor)를 재검사한다(결정 109, 133, 139, 137(나)②).
        static std::size_t ExecuteAoESpell(
            const RE::NiPoint3& origin,
            float radius,
            double skyrimDamage,
            RE::FormID vfxExplosionLocalId,
            Core::DamageType dominantType = Core::DamageType::Cold);

        // 5단계: 고정 좌표에 무해 EXPL 연출만 스폰(주 갱신 스레드 전용)
        static void SpawnHarmlessVfx(const RE::NiPoint3& origin, RE::FormID vfxExplosionLocalId);

        // 5단계: 단일 대상 DLL 직접 피해(감전 증폭 + 몬스터 저항/적 ES + KillTracker 100% ES 흡수 기여 보장 + DoDamage)
        static bool DealDirectSkillDamage(
            RE::Actor* target,
            double rawSkyrimDamage,
            Core::DamageType dominantType,
            bool isDirectHit,
            bool bypassesMonsterEs = false);
    };
}
