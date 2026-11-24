#include "test_harness.h"

#include "core/MapSystem.h"

#include <limits>

using namespace SkyrimPoE::Core;

POE_TEST("Map pack size scales total actors, rounds halves up and reserves the boss slot")
{
    POE_CHECK(MapSystem::MapPackActorCount(1, 0.0) == 3);
    POE_CHECK(MapSystem::MapPackActorCount(6, 0.0) == 18);
    POE_CHECK(MapSystem::MapPackActorCount(3, 10.0) == 10);
    POE_CHECK(MapSystem::MapPackActorCount(3, 17.0) == 11);
    POE_CHECK(MapSystem::MapPackActorCount(3, 35.0) == 12);
    POE_CHECK(MapSystem::MapPackActorCount(3, 49.9) == 13);
    POE_CHECK(MapSystem::MapPackActorCount(3, 50.0) == 14);
    POE_CHECK(MapSystem::MapPackActorCount(3, 50.1) == 14);
    POE_CHECK(MapSystem::MapPackActorCount(1, -100.0) == 1);
    POE_CHECK(MapSystem::MapPackActorCount(6, 300.0) == kMaxMapSpawnedActors - 1);
}

POE_TEST("Map spawn deletion requires a disabled non-player actor with the recorded base and cell")
{
    constexpr std::uint32_t baseId = 123;
    constexpr std::uint32_t cellId = 456;
    constexpr MapSpawnDeletionObservation matching{ true, false, true, baseId, cellId };
    POE_CHECK(MapSystem::CanDeleteMapSpawn(baseId, cellId, matching));

    auto observed = matching;
    observed.isActor = false;
    POE_CHECK(!MapSystem::CanDeleteMapSpawn(baseId, cellId, observed));
    observed = matching;
    observed.isPlayer = true;
    POE_CHECK(!MapSystem::CanDeleteMapSpawn(baseId, cellId, observed));
    observed = matching;
    observed.isDisabled = false;
    POE_CHECK(!MapSystem::CanDeleteMapSpawn(baseId, cellId, observed));
    POE_CHECK(!MapSystem::CanDeleteMapSpawn(baseId + 1, cellId, matching));
    POE_CHECK(!MapSystem::CanDeleteMapSpawn(baseId, cellId + 1, matching));

    observed = matching;
    observed.actorBaseId = 0;
    POE_CHECK(!MapSystem::CanDeleteMapSpawn(0, cellId, observed));
    observed = matching;
    observed.cellId = 0;
    POE_CHECK(!MapSystem::CanDeleteMapSpawn(baseId, 0, observed));
}

POE_TEST("A leftover map spawn is an enabled, undeleted, unregistered template actor in the attached swept cell")
{
    constexpr std::uint32_t cellId = 0xC2000800u;
    constexpr std::uint32_t bossBase = 0x0003DF0Cu;
    constexpr std::uint32_t packBase = 0x00039D21u;
    const std::uint32_t templates[] = { bossBase, packBase };
    const std::uint32_t currentRun[] = { 0xFF003800u, 0xFF003801u };
    constexpr MapLeftoverObservation leftover{ true, false, false, false, false, 0xFF000D25u, bossBase, cellId };
    POE_CHECK(MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, leftover));
    POE_CHECK(MapSystem::IsLeftoverMapSpawn(cellId, true, templates, {}, leftover));

    // 셀이 붙어 있지 않으면 건드리지 않는다(결정 178 ⑦).
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, false, templates, currentRun, leftover));
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(0, true, templates, currentRun, leftover));
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId + 1, true, templates, currentRun, leftover));

    // 지금 회차에 등록된 액터는 남은 스폰이 아니다.
    auto observed = leftover;
    observed.formId = 0xFF003801u;
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, observed));

    observed = leftover;
    observed.isActor = false;
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, observed));
    observed = leftover;
    observed.isPlayer = true;
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, observed));
    observed = leftover;
    observed.isFollowerOrCommanded = true;
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, observed));
    observed = leftover;
    observed.isDeleted = true;
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, observed));
    observed = leftover;
    observed.isDisabled = true;
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, observed));
    // 플러그인에 배치된 참조(동적이 아님)와 다른 원형은 남은 스폰이 아니다.
    observed = leftover;
    observed.formId = 0x0005A1F0u;
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, observed));
    observed = leftover;
    observed.actorBaseId = 0x00012345u;
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, observed));
    observed = leftover;
    observed.actorBaseId = 0;
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, observed));
    observed = leftover;
    observed.cellId = 0;
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, templates, currentRun, observed));
    // 원형 목록이 비면(던전을 모름) 아무것도 남은 스폰으로 보지 않는다.
    POE_CHECK(!MapSystem::IsLeftoverMapSpawn(cellId, true, {}, currentRun, leftover));
}

POE_TEST("First tribute is guaranteed only for an eligible T6+ boss in the active run")
{
    POE_CHECK(MapSystem::ShouldGuaranteeFirstTribute(true, true, false, 6, false));
    POE_CHECK(MapSystem::ShouldGuaranteeFirstTribute(true, true, false, 16, false));
    POE_CHECK(!MapSystem::ShouldGuaranteeFirstTribute(true, true, false, 5, false));
    POE_CHECK(!MapSystem::ShouldGuaranteeFirstTribute(true, true, false, 0, false));
    POE_CHECK(!MapSystem::ShouldGuaranteeFirstTribute(false, true, false, 6, false));
    POE_CHECK(!MapSystem::ShouldGuaranteeFirstTribute(true, false, false, 6, false));
    POE_CHECK(!MapSystem::ShouldGuaranteeFirstTribute(true, true, true, 6, false));
    POE_CHECK(!MapSystem::ShouldGuaranteeFirstTribute(true, true, false, 6, true));
}

POE_TEST("Map return defers for unresolved boss death only until the timeout")
{
    POE_CHECK(!MapSystem::ShouldDeferReturnForBossDeath(true, true, false, true, 0.0));
    POE_CHECK(!MapSystem::ShouldDeferReturnForBossDeath(false, false, false, false, 0.0));
    POE_CHECK(!MapSystem::ShouldDeferReturnForBossDeath(false, true, true, false, 0.0));
    POE_CHECK(MapSystem::ShouldDeferReturnForBossDeath(false, false, true, true, 0.0));
    POE_CHECK(MapSystem::ShouldDeferReturnForBossDeath(false, true, false, false, 0.0));

    for (const bool killPending : { false, true }) {
        POE_CHECK(MapSystem::ShouldDeferReturnForBossDeath(false, true, killPending, killPending, 9.999));
        POE_CHECK(!MapSystem::ShouldDeferReturnForBossDeath(false, true, killPending, killPending, 10.0));
        POE_CHECK(!MapSystem::ShouldDeferReturnForBossDeath(false, true, killPending, killPending, 10.001));
    }
}

POE_TEST("Boss kill failure and reward block are judged at the lethal blow when it came first")
{
    // 치명타가 회차 실패보다 먼저였으면, 사망 확정 전에 실패·셀 이탈이 생겨도 그때의 판정을 쓴다.
    auto judged = MapSystem::JudgeBossKill(true, false, true, true);
    POE_CHECK(!judged.runFailed);
    POE_CHECK(!judged.rewardBlocked);
    // 치명타 때 이미 허가 셀 밖이었으면 그 차단은 유지된다.
    judged = MapSystem::JudgeBossKill(true, true, false, false);
    POE_CHECK(!judged.runFailed);
    POE_CHECK(judged.rewardBlocked);
    // 사망 시작 기록이 없으면(실패 뒤 치명타, 기록 누락) 지금 상태를 그대로 쓴다.
    judged = MapSystem::JudgeBossKill(false, false, true, false);
    POE_CHECK(judged.runFailed);
    POE_CHECK(!judged.rewardBlocked);
    judged = MapSystem::JudgeBossKill(false, false, false, true);
    POE_CHECK(!judged.runFailed);
    POE_CHECK(judged.rewardBlocked);
}

POE_TEST("Ending returns hold spawn Disable only while a boss kill is pending and within the cap")
{
    POE_CHECK(MapSystem::ShouldHoldEndingDisableForBossDeath(true, false, 0.0));
    POE_CHECK(MapSystem::ShouldHoldEndingDisableForBossDeath(true, false, 9.999));
    POE_CHECK(!MapSystem::ShouldHoldEndingDisableForBossDeath(true, false, kBossDeathProcessWaitSec));
    POE_CHECK(!MapSystem::ShouldHoldEndingDisableForBossDeath(true, true, 0.0));
    POE_CHECK(!MapSystem::ShouldHoldEndingDisableForBossDeath(false, false, 0.0));
    POE_CHECK(!MapSystem::ShouldHoldEndingDisableForBossDeath(true, false, std::numeric_limits<double>::quiet_NaN()));
}

POE_TEST("An abandoned map ends on return even with portals and a living boss")
{
    auto run = MapSystem::CreateNormalMapRun(1, 1, {}, 0, 0, {}, {});
    POE_CHECK(!run.endRequested);
    POE_CHECK(!MapSystem::ShouldEndOnReturn(run));
    run.endRequested = true;
    POE_CHECK(MapSystem::ShouldEndOnReturn(run));
}

POE_TEST("The exit zone refuses a return that would end the run before the boss dies")
{
    // 결정 182: 결투장은 입장 때 포털 0이라 보스가 살아 있는 동안 출구 구역 귀환을 거절한다.
    const auto arena = MapSystem::CreateArena4Run(1, 0, 0, {}, {});
    POE_CHECK(arena.portalsRemaining == 0);
    POE_CHECK(MapSystem::ShouldEndOnReturn(arena));
    POE_CHECK(MapSystem::ShouldRefuseExitZoneReturn(arena, false));

    // 포털이 남은 일반 지도는 출구 구역 귀환이 회차를 끝내지 않으므로 거절하지 않는다.
    auto map = MapSystem::CreateNormalMapRun(2, 1, {}, 0, 0, {}, {});
    POE_CHECK(map.portalsRemaining > 0);
    POE_CHECK(!MapSystem::ShouldRefuseExitZoneReturn(map, false));
    // 포털을 다 쓴 일반 지도는 결투장과 같다.
    map.portalsRemaining = 0;
    POE_CHECK(MapSystem::ShouldRefuseExitZoneReturn(map, false));

    // 보스가 쓰러졌거나(처치 반영 대기 포함) 종료 요청·실패·스폰을 끈(끝낼 귀환이 실패한) 회차, 비활성 상태는 귀환 쪽 규칙에 맡긴다.
    POE_CHECK(!MapSystem::ShouldRefuseExitZoneReturn(arena, true));
    for (const auto flag : { &ActiveMapState::bossKilled, &ActiveMapState::endRequested, &ActiveMapState::failed,
             &ActiveMapState::spawnsDisabled }) {
        auto changed = arena;
        changed.*flag = true;
        POE_CHECK(!MapSystem::ShouldRefuseExitZoneReturn(changed, false));
    }
    auto inactive = arena;
    inactive.active = false;
    POE_CHECK(!MapSystem::ShouldRefuseExitZoneReturn(inactive, false));
}

POE_TEST("An abandoned map rejects reentry without consuming a portal")
{
    auto run = MapSystem::CreateNormalMapRun(1, 1, {}, 0, 0, {}, {});
    POE_CHECK(MapSystem::TryConsumePortalForReenter(run));
    run.endRequested = true;
    const auto remaining = run.portalsRemaining;
    POE_CHECK(!MapSystem::TryConsumePortalForReenter(run));
    POE_CHECK(run.portalsRemaining == remaining);
}

POE_TEST("Map boss loss needs ten uninterrupted seconds in an authorized, stable cell")
{
    const auto run = MapSystem::CreateNormalMapRun(1, 1, {}, 0, 0, {}, {});
    MapBossLossObservation observation{ .stableRunCell = true, .playerAuthorized = true, .spawned = true, .bossMissing = true };
    auto progress = MapSystem::ObserveBossLoss(run, observation, 0.0, 9.0);
    POE_CHECK(progress.missingSec == 9.0);
    POE_CHECK(!progress.suspected);

    observation.paused = true;
    progress = MapSystem::ObserveBossLoss(run, observation, progress.missingSec, 10.0);
    POE_CHECK(progress.missingSec == 9.0);
    POE_CHECK(!progress.suspected);
    observation.paused = false;
    POE_CHECK(MapSystem::ObserveBossLoss(run, observation, progress.missingSec, 1.0).suspected);

    for (const auto excluded : { &MapBossLossObservation::stableRunCell, &MapBossLossObservation::playerAuthorized,
             &MapBossLossObservation::spawned,
             &MapBossLossObservation::travelPending, &MapBossLossObservation::bossMissing }) {
        auto changed = observation;
        changed.*excluded = !(changed.*excluded);
        progress = MapSystem::ObserveBossLoss(run, changed, 9.0, 2.0);
        POE_CHECK(progress.missingSec == 0.0);
        POE_CHECK(!progress.suspected);
        POE_CHECK(!MapSystem::ObserveBossLoss(run, observation, progress.missingSec, 1.0).suspected);
    }
}

POE_TEST("Unresolved saved bosses in eligible runs can be checked immediately, including the arena")
{
    auto run = MapSystem::CreateArena4Run(1, 0, 0, {}, {});
    run.unresolvedBossRefFormId = 0xFF000001;
    MapBossLossObservation observation{ .stableRunCell = true, .playerAuthorized = true, .spawned = true, .bossMissing = true };
    POE_CHECK(MapSystem::ShouldEndOnReturn(run));
    POE_CHECK(MapSystem::ObserveBossLoss(run, observation, 0.0, 0.0).suspected);

    for (const auto excluded : { &ActiveMapState::active, &ActiveMapState::bossKilled,
             &ActiveMapState::failed, &ActiveMapState::endRequested, &ActiveMapState::spawnsDisabled }) {
        auto changed = run;
        changed.*excluded = !(changed.*excluded);
        const auto progress = MapSystem::ObserveBossLoss(changed, observation, 9.0, 2.0);
        POE_CHECK(progress.missingSec == 0.0);
        POE_CHECK(!progress.suspected);
    }
    observation.playerAuthorized = false;
    POE_CHECK(!MapSystem::ObserveBossLoss(run, observation, 0.0, 0.0).suspected);
    POE_CHECK(!MapSystem::ObserveBossLoss(run, observation, 0.0, 10.0).suspected);
}

POE_TEST("Boss loss resets the timer after death confirmation or a pending kill")
{
    auto run = MapSystem::CreateNormalMapRun(1, 1, {}, 0, 0, {}, {});
    const MapBossLossObservation ready{ .stableRunCell = true, .playerAuthorized = true, .spawned = true, .bossMissing = true };
    for (const auto death : { &MapBossLossObservation::deathConfirmed, &MapBossLossObservation::deathPending }) {
        auto observation = ready;
        observation.*death = true;
        const auto loss = MapSystem::ObserveBossLoss(run, observation, 9.0, 2.0);
        POE_CHECK(loss.missingSec == 0.0 && !loss.suspected);
        run.unresolvedBossRefFormId = 0xFF000001;
        POE_CHECK(!MapSystem::ObserveBossLoss(run, observation, 0.0, 0.0).suspected);
        run.unresolvedBossRefFormId = 0;
    }
}

POE_TEST("A directly resolved map boss must match both its dungeon base and parent cell")
{
    POE_CHECK(MapSystem::MatchesMapBoss(0x101, 0x201, 0x101, 0x201));
    POE_CHECK(!MapSystem::MatchesMapBoss(0x101, 0x201, 0x102, 0x201));
    POE_CHECK(!MapSystem::MatchesMapBoss(0x101, 0x201, 0x101, 0x202));
    POE_CHECK(!MapSystem::MatchesMapBoss(0, 0x201, 0, 0x201));
    POE_CHECK(!MapSystem::MatchesMapBoss(0x101, 0, 0x101, 0));
}

POE_TEST("Boss rebind keeps the spawn list bounded and cleanup includes a separate boss")
{
    ActiveMapState run;
    constexpr std::uint32_t firstSpawn = 0xFF000001;
    constexpr std::uint32_t oldBoss = 0xFF010001;
    constexpr std::uint32_t newBoss = 0xFF010002;
    for (std::uint32_t i = 0; i < kMaxMapSpawnedActors; ++i) {
        run.spawnedRefFormIds.push_back(firstSpawn + i);
    }
    const auto originalSpawns = run.spawnedRefFormIds;
    run.bossRefFormId = oldBoss;
    run.unresolvedBossRefFormId = oldBoss;
    MapSystem::RebindMapBoss(run, newBoss);
    POE_CHECK(run.spawnedRefFormIds == originalSpawns);
    POE_CHECK(run.bossRefFormId == newBoss && run.unresolvedBossRefFormId == 0);
    auto cleanup = MapSystem::MapSpawnCleanupIds(run);
    POE_CHECK(cleanup.size() == kMaxMapSpawnedActors + 1);
    POE_CHECK(cleanup.back() == newBoss);
    POE_CHECK(!MapSystem::IsRunDrop(oldBoss, cleanup));

    run.bossRefFormId = firstSpawn;
    MapSystem::RebindMapBoss(run, newBoss);
    POE_CHECK(run.spawnedRefFormIds.size() == kMaxMapSpawnedActors);
    POE_CHECK(!MapSystem::IsRunDrop(firstSpawn, run.spawnedRefFormIds));
    POE_CHECK(MapSystem::IsRunDrop(newBoss, run.spawnedRefFormIds));
    POE_CHECK(MapSystem::MapSpawnCleanupIds(run) == run.spawnedRefFormIds);
}

POE_TEST("Boss recovery prefers living candidates and only recognizes corpses spawned by the active run")
{
    constexpr std::uint32_t currentBoss = 0xFF000001;
    constexpr std::uint32_t previousBoss = 0xFF000002;
    constexpr std::uint32_t livingBoss = 0xFF000003;
    constexpr std::uint32_t spawned[] = { currentBoss };
    constexpr std::uint32_t staleCorpses[] = { previousBoss };
    constexpr std::uint32_t mixedCorpses[] = { previousBoss, currentBoss };

    POE_CHECK(MapSystem::SelectMapBossCandidate(livingBoss, mixedCorpses, spawned) == livingBoss);
    POE_CHECK(MapSystem::SelectMapBossCandidate(livingBoss, {}, {}) == livingBoss);
    POE_CHECK(MapSystem::SelectMapBossCandidate(0, mixedCorpses, spawned) == currentBoss);
    POE_CHECK(MapSystem::SelectMapBossCandidate(0, staleCorpses, spawned) == 0);
    POE_CHECK(MapSystem::SelectMapBossCandidate(0, mixedCorpses, {}) == 0);
    POE_CHECK(MapSystem::SelectMapBossCandidate(0, {}, {}) == 0);
}

POE_TEST("Only a death escape's owning run can release its flag")
{
    std::atomic<std::uint64_t> owner{ 1 };
    POE_CHECK(MapSystem::ReleaseDeathEscape(owner, 1));
    POE_CHECK(owner.load() == 0);
    owner.store(2);
    POE_CHECK(!MapSystem::ReleaseDeathEscape(owner, 1));
    POE_CHECK(!MapSystem::ReleaseDeathEscape(owner, 0));
    POE_CHECK(owner.load() == 2);
    POE_CHECK(MapSystem::ReleaseDeathEscape(owner, 2));
    POE_CHECK(owner.load() == 0);
}
