#pragma once

#include <optional>

// 결정 237: 우리 스킬에 맞았을 때의 작은 경직(움찔) 판정. 엔진과 무관한 규칙만 둔다.
namespace SkyrimPoE::Core
{
    struct FlinchRule
    {
        double minLifeRatio = 0.10;  // 이번 타격(스카이림 체력 단위, 에너지 실드 흡수 포함) ÷ 대상 최대 체력
        double cooldownSec = 1.0;    // 같은 대상은 이 시간 안에 다시 움찔하지 않음
    };

    // 이번 타격으로 작은 경직을 걸지. 어떤 값이든 유한하지 않거나, 최대 체력이 0 이하이거나, 비율·쉬는 시간이 음수이면 걸지 않는다.
    // lastFlinchSec은 같은 대상이 마지막으로 움찔한 시각(없으면 처음).
    [[nodiscard]] bool ShouldFlinch(double hitSkyrim, double maxHealth, double nowSec,
        std::optional<double> lastFlinchSec, const FlinchRule& rule) noexcept;
}
