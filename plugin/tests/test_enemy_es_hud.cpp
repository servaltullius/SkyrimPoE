#include "test_harness.h"

#include "core/EnemyEsHud.h"

#include <limits>

using namespace SkyrimPoE::Core;

namespace
{
    // 결정 185의 유지 시간. 제품 상수를 그대로 따르지 않게 기대값을 따로 둔다.
    constexpr double kExpectedHoldSec = 5.0;

    EnemyEsCandidate Enemy(std::uint32_t id, double maxEs) { return { id, true, true, maxEs }; }
}

// 결정 185의 유지 시간이 바뀌면 빌드에서 바로 드러나게 한다.
static_assert(kEnemyEsEngagedHoldSec == kExpectedHoldSec);

POE_TEST("Enemy ES target follows a living crosshair enemy, hiding it without ES")
{
    const auto engaged = Enemy(0xFF000020, 300.0);
    POE_CHECK(SelectEnemyEsTarget(Enemy(0xFF000010, 120.0), engaged, 1.0) == 0xFF000010);
    // 조준점이 ES 없는 살아 있는 적이면 그 적이 대상이고 숨긴다(다른 적의 막대를 보이지 않음)
    POE_CHECK(SelectEnemyEsTarget(Enemy(0xFF000011, 0.0), engaged, 1.0) == 0u);
}

POE_TEST("Enemy ES target falls back to the recent engaged enemy")
{
    const EnemyEsCandidate none{};
    const EnemyEsCandidate dead{ 0xFF000012, false, true, 50.0 };
    const EnemyEsCandidate ally{ 0xFF000013, true, false, 50.0 };
    const auto engaged = Enemy(0xFF000020, 300.0);
    // 조준점이 비었거나, 죽었거나, 적이 아니면 마지막 교전 적
    POE_CHECK(SelectEnemyEsTarget(none, engaged, 0.0) == 0xFF000020);
    POE_CHECK(SelectEnemyEsTarget(dead, engaged, 2.0) == 0xFF000020);
    POE_CHECK(SelectEnemyEsTarget(ally, engaged, kExpectedHoldSec) == 0xFF000020);
    // 유지 시간이 지났거나 기록이 없으면 표시 안 함
    POE_CHECK(SelectEnemyEsTarget(none, engaged, kExpectedHoldSec + 0.01) == 0u);
    POE_CHECK(SelectEnemyEsTarget(none, engaged, -1.0) == 0u);
    POE_CHECK(SelectEnemyEsTarget(none, engaged, std::numeric_limits<double>::quiet_NaN()) == 0u);
    POE_CHECK(SelectEnemyEsTarget(none, engaged, std::numeric_limits<double>::infinity()) == 0u);
    // 교전 적도 ES가 없거나 죽었거나 아군이면 표시 안 함
    POE_CHECK(SelectEnemyEsTarget(none, Enemy(0xFF000021, 0.0), 0.0) == 0u);
    POE_CHECK(SelectEnemyEsTarget(none, dead, 0.0) == 0u);
    POE_CHECK(SelectEnemyEsTarget(none, ally, 0.0) == 0u);
}

POE_TEST("Enemy ES bar values for TrueHUD keep the fill ratio and stay finite")
{
    const auto normal = MakeEnemyEsBarValues(120.5, 300.0);
    POE_CHECK(normal.current == 120.5f && normal.max == 300.0f);
    // 최대가 없거나 잘못되면 0/0
    const auto none = MakeEnemyEsBarValues(10.0, 0.0);
    POE_CHECK(none.current == 0.0f && none.max == 0.0f);
    const auto nanMax = MakeEnemyEsBarValues(10.0, std::numeric_limits<double>::quiet_NaN());
    POE_CHECK(nanMax.current == 0.0f && nanMax.max == 0.0f);
    // 현재는 0..최대
    POE_CHECK(MakeEnemyEsBarValues(-3.0, 50.0).current == 0.0f);
    POE_CHECK(MakeEnemyEsBarValues(500.0, 50.0).current == 50.0f);
    POE_CHECK(MakeEnemyEsBarValues(std::numeric_limits<double>::infinity(), 50.0).current == 0.0f);
    // 아주 큰 값은 같은 비율로 줄여 반쯤 찬 바가 가득 차 보이지 않게
    const auto huge = MakeEnemyEsBarValues(1.0e12, 2.0e12);
    POE_CHECK(huge.max == 1.0e9f && huge.current == 5.0e8f);
}

POE_TEST("Enemy ES view rounds, clamps, keeps the true fill and hides enemies without ES")
{
    POE_CHECK(!MakeEnemyEsView(0, MonsterRarity::Rare, 10.0, 100.0).visible);
    POE_CHECK(!MakeEnemyEsView(0xFF000010, MonsterRarity::Rare, 0.0, 0.0).visible);
    POE_CHECK(!MakeEnemyEsView(0xFF000010, MonsterRarity::Rare, 1.0, std::numeric_limits<double>::quiet_NaN()).visible);

    const auto view = MakeEnemyEsView(0xFF000010, MonsterRarity::Magic, 119.6, 300.4);
    POE_CHECK(view.visible);
    POE_CHECK(view.formId == 0xFF000010);
    POE_CHECK(view.rarity == MonsterRarity::Magic);
    POE_CHECK(view.current == 120);
    POE_CHECK(view.max == 300);
    POE_CHECK(view.fillPermille == 398);

    // 작은 값: 수치는 0 / 1로 보이지만 막대는 실제 비율(가득)
    const auto tiny = MakeEnemyEsView(0xFF000010, MonsterRarity::Normal, 0.3, 0.3);
    POE_CHECK(tiny.visible && tiny.max == 1 && tiny.current == 0 && tiny.fillPermille == 1000);
    // 큰 값: 수치는 상한으로 잘려도 막대는 실제 비율
    POE_CHECK(MakeEnemyEsView(0xFF000010, MonsterRarity::Normal, 3.0e9, 4.0e9).fillPermille == 750);
    POE_CHECK(MakeEnemyEsView(0xFF000010, MonsterRarity::Normal, 500.0, 100.0).current == 100);
    POE_CHECK(MakeEnemyEsView(0xFF000010, MonsterRarity::Normal, -5.0, 100.0).current == 0);
    const auto inf = MakeEnemyEsView(0xFF000010, MonsterRarity::Normal, std::numeric_limits<double>::infinity(), 100.0);
    POE_CHECK(inf.current == 0 && inf.fillPermille == 0);

    // 바닥난 ES(현재 0)와 ES 없음(최대 0)은 구분된다
    const auto empty = MakeEnemyEsView(0xFF000010, MonsterRarity::Rare, 0.0, 80.0);
    POE_CHECK(empty.visible && empty.fillPermille == 0);
    // 같은 수치라도 다른 액터면 다른 화면 값
    POE_CHECK(!(MakeEnemyEsView(0xFF000010, MonsterRarity::Rare, 50.0, 80.0) ==
                MakeEnemyEsView(0xFF000011, MonsterRarity::Rare, 50.0, 80.0)));
}
