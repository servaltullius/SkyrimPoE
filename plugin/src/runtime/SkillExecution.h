#pragma once

#include "core/KillTracker.h"

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

        static void Initialize(Core::KillTracker* killTracker);

        // 현재 스레드가 우리 DLL의 DoDamage 직접 피해 호출 안에 있는지 여부(재귀 방지)
        [[nodiscard]] static bool IsInPluginDirectDamage() noexcept;

        // 결정 52: 플레이어 본인, 동료, 플레이어 소환수/지배 대상은 아군으로 제외
        [[nodiscard]] static bool IsAllyOfPlayer(const RE::PlayerCharacter* player, const RE::Actor* actor) noexcept;

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
            RE::FormID vfxExplosionLocalId);
    };
}
