#pragma once

#include "core/Types.h"

#include <cstdint>

namespace SkyrimPoE::Core
{
    // 결정 185: 적 ES 자체 표시(결정 23의 기본)의 대상 고르기와 화면 값.
    // 조준점이 살아 있는 적을 가리키면 그 적을 보여 주고(ES가 없으면 숨김), 그렇지 않으면 플레이어가 마지막으로
    // 맞혔거나 플레이어를 마지막으로 맞힌 적을 잠시 보여 준다. 동료·소환수·지배한 액터처럼 적이 아닌 액터는 쓰지 않는다.

    // 마지막 교전 적을 보여 주는 시간(초)
    inline constexpr double kEnemyEsEngagedHoldSec = 5.0;

    struct EnemyEsCandidate
    {
        std::uint32_t formId = 0;
        bool alive = false;     // 살아 있고 꺼지지 않았으며 3D가 불린 액터
        bool enemy = false;     // 플레이어의 적(적대이고 아군이 아님)
        double maxEs = 0.0;     // 몬스터 프로필의 최대 ES(스카이림 단위). 프로필이 없으면 0
    };

    // 살아 있는 적인지(표시 대상이 될 수 있는지). ES 유무와 무관하다.
    [[nodiscard]] bool IsEnemyEsTargetable(const EnemyEsCandidate& candidate) noexcept;

    // ES를 표시할 수 있는지. 살아 있는 적이고 최대 ES가 0보다 커야 한다.
    [[nodiscard]] bool IsEnemyEsDisplayable(const EnemyEsCandidate& candidate) noexcept;

    // 보여 줄 액터의 FormID(없으면 0).
    // 조준점이 살아 있는 적이면 그 적이 대상이며, ES가 없으면 0(마지막 교전 적으로 넘어가지 않음).
    // 조준점이 비었거나 적이 아니면 마지막 교전 적을 쓴다.
    // engagedAgeSec: 마지막 교전 뒤 지난 시간. 음수·NaN·무한대는 교전 기록이 없는 것으로 본다.
    [[nodiscard]] std::uint32_t SelectEnemyEsTarget(
        const EnemyEsCandidate& crosshair, const EnemyEsCandidate& engaged, double engagedAgeSec) noexcept;

    // 마지막 교전 적을 아직 보여 줄 시간인지
    [[nodiscard]] bool IsEnemyEsEngagementRecent(double engagedAgeSec) noexcept;

    struct EnemyEsView
    {
        bool visible = false;
        std::uint32_t formId = 0;
        MonsterRarity rarity = MonsterRarity::Normal;
        int current = 0;        // 표시 수치(반올림)
        int max = 0;
        int fillPermille = 0;   // 막대 폭(0..1000). 반올림 전 실제 비율로 계산한다

        [[nodiscard]] bool operator==(const EnemyEsView&) const noexcept = default;
    };

    // 화면 값. 수치는 정수로 반올림하고, 최대 ES가 0보다 크면 표시 최대치는 1 이상이다. 현재치는 0..최대로 자른다.
    // 막대 폭은 반올림 전 값의 비율이라 0.3 / 0.3 같은 작은 값도 가득 찬 막대가 된다.
    [[nodiscard]] EnemyEsView MakeEnemyEsView(
        std::uint32_t formId, MonsterRarity rarity, double currentEs, double maxEs) noexcept;
}
