#include "core/EnemyEsHud.h"

#include <algorithm>
#include <cmath>

namespace SkyrimPoE::Core
{
    bool IsEnemyEsTargetable(const EnemyEsCandidate& candidate) noexcept
    {
        return candidate.formId != 0 && candidate.alive && candidate.enemy;
    }

    bool IsEnemyEsDisplayable(const EnemyEsCandidate& candidate) noexcept
    {
        return IsEnemyEsTargetable(candidate) && std::isfinite(candidate.maxEs) && candidate.maxEs > 0.0;
    }

    bool IsEnemyEsEngagementRecent(double engagedAgeSec) noexcept
    {
        return std::isfinite(engagedAgeSec) && engagedAgeSec >= 0.0 && engagedAgeSec <= kEnemyEsEngagedHoldSec;
    }

    std::uint32_t SelectEnemyEsTarget(
        const EnemyEsCandidate& crosshair, const EnemyEsCandidate& engaged, double engagedAgeSec) noexcept
    {
        if (IsEnemyEsTargetable(crosshair)) {
            return IsEnemyEsDisplayable(crosshair) ? crosshair.formId : 0u;
        }
        return IsEnemyEsEngagementRecent(engagedAgeSec) && IsEnemyEsDisplayable(engaged) ? engaged.formId : 0u;
    }

    EnemyEsBarValues MakeEnemyEsBarValues(double currentEs, double maxEs) noexcept
    {
        if (!std::isfinite(maxEs) || maxEs <= 0.0) {
            return {};
        }
        const double cur = std::isfinite(currentEs) ? std::clamp(currentEs, 0.0, maxEs) : 0.0;
        constexpr double kCap = 1.0e9;
        const double scale = maxEs > kCap ? kCap / maxEs : 1.0;
        return { static_cast<float>(cur * scale), static_cast<float>(maxEs * scale) };
    }

    EnemyEsView MakeEnemyEsView(std::uint32_t formId, MonsterRarity rarity, double currentEs, double maxEs) noexcept
    {
        EnemyEsView view;
        if (formId == 0 || !std::isfinite(maxEs) || maxEs <= 0.0) {
            return view;
        }
        const double cur = std::isfinite(currentEs) ? std::clamp(currentEs, 0.0, maxEs) : 0.0;
        view.visible = true;
        view.formId = formId;
        view.rarity = rarity;
        view.fillPermille = std::clamp(static_cast<int>(std::llround(cur / maxEs * 1000.0)), 0, 1000);
        // 화면 수치는 int이므로 반올림 전에 상한을 둔다(llround 넘침 방지). 막대 폭은 위에서 실제 비율로 정했다.
        constexpr double kDisplayCap = 1.0e9;
        view.max = std::max(1, static_cast<int>(std::llround(std::min(maxEs, kDisplayCap))));
        view.current = std::clamp(static_cast<int>(std::llround(std::min(cur, kDisplayCap))), 0, view.max);
        return view;
    }
}
