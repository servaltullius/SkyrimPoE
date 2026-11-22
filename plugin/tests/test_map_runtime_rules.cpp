#include "test_harness.h"

#include "core/MapSystem.h"

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

POE_TEST("An abandoned map ends on return even with portals and a living boss")
{
    auto run = MapSystem::CreateNormalMapRun(1, 1, {}, 0, 0, {}, {});
    POE_CHECK(!run.endRequested);
    POE_CHECK(!MapSystem::ShouldEndOnReturn(run));
    run.endRequested = true;
    POE_CHECK(MapSystem::ShouldEndOnReturn(run));
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
