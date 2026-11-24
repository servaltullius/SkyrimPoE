#include "test_harness.h"

#include "core/Config.h"
#include "core/MapSystem.h"
#include "core/MapTravel.h"

#include <limits>

using namespace SkyrimPoE::Core;

namespace
{
    constexpr MapTravelTarget kEntrance{ 10, 0, { 100.0f, 200.0f, 300.0f } };
    constexpr MapTravelTarget kInterior{ 20, 0, { 400.0f, 500.0f, 600.0f } };
    constexpr MapTravelTarget kExterior{ 30, 40, { 700.0f, 800.0f, 900.0f } };
    constexpr MapTravelTarget kSafeMarker{ 50, 40, { 1000.0f, 1100.0f, 1200.0f } };
    constexpr MapTravelObservation kAtEntrance{ 10, 0, kEntrance.position, true, false, true };

    MapTravelRequest Entry(MapTravelKind kind = MapTravelKind::OpenMap)
    {
        return { kind, kEntrance, 64.0, false, std::nullopt, 1, 5.0 };
    }

    MapTravelRequest Return(MapTravelTarget target)
    {
        return { MapTravelKind::Return, target, 64.0, true, kSafeMarker, 2, 5.0 };
    }
}

POE_TEST("Map travel rejects overlapping entry and return requests")
{
    MapTravel travel;
    POE_CHECK(travel.TryStart(Entry()));
    POE_CHECK(!travel.TryStart(Entry(MapTravelKind::Reenter)));
    POE_CHECK(!travel.TryStart(Return(kInterior)));
    POE_CHECK(travel.Current()->kind == MapTravelKind::OpenMap);
    POE_CHECK(travel.Tick(kAtEntrance, 0.0, false) == MapTravelOutcome::Arrived);
    POE_CHECK(!travel.Current());
    POE_CHECK(travel.TryStart(Return(kInterior)));
    POE_CHECK(!travel.TryStart(Entry()));
}

POE_TEST("Map travel suspends timeout while paused or loading")
{
    MapTravel travel;
    POE_CHECK(travel.TryStart(Entry()));
    auto player = kAtEntrance;
    player.loading = true;
    POE_CHECK(travel.Tick(player, 100.0, false) == MapTravelOutcome::Waiting);
    POE_CHECK(travel.Current()->elapsedSec == 0.0);
    player.loading = false;
    player.loaded3D = false;
    POE_CHECK(travel.Tick(player, 2.0, false) == MapTravelOutcome::Waiting);
    POE_CHECK(travel.Tick(player, 100.0, true) == MapTravelOutcome::Waiting);
    POE_CHECK(travel.Current()->elapsedSec == 2.0);
    POE_CHECK(travel.Tick(player, 3.0, false) == MapTravelOutcome::Failed);
    POE_CHECK(!travel.Current());
}

POE_TEST("Map entry arrival needs the target cell, radius, 3D and no loading screen")
{
    for (const auto kind : { MapTravelKind::OpenMap, MapTravelKind::OpenArena, MapTravelKind::Reenter }) {
        const auto request = Entry(kind);
        auto player = kAtEntrance;
        player.position[2] += 64.0f;
        POE_CHECK(HasArrived(request, player));
        player.position[2] += 1.0f;
        POE_CHECK(!HasArrived(request, player));
        player = kAtEntrance;
        player.cell = kInterior.cell;
        POE_CHECK(!HasArrived(request, player));
        player = kAtEntrance;
        player.loaded3D = false;
        POE_CHECK(!HasArrived(request, player));
        player = kAtEntrance;
        player.loading = true;
        POE_CHECK(!HasArrived(request, player));
    }
}

POE_TEST("Map return arrival matches the indoor cell or the outdoor world and radius")
{
    auto request = Return(kInterior);
    MapTravelObservation player{ kInterior.cell, 0, kExterior.position, true, false, false };
    POE_CHECK(HasArrived(request, player));
    player.cell = kExterior.cell;
    POE_CHECK(!HasArrived(request, player));
    player.cell = kInterior.cell;
    player.inPackCell = true;
    POE_CHECK(!HasArrived(request, player));

    request = Return(kExterior);
    player = { kInterior.cell, kExterior.worldspace, kExterior.position, true, false, false };
    POE_CHECK(HasArrived(request, player));
    player.position[0] += 65.0f;
    POE_CHECK(!HasArrived(request, player));
    player.position = kExterior.position;
    ++player.worldspace;
    POE_CHECK(!HasArrived(request, player));
    player.worldspace = kExterior.worldspace;
    const auto knownCell = player.cell;
    player.cell = 0;
    POE_CHECK(!HasArrived(request, player));
    player.cell = knownCell;
    player.inPackCell = true;
    POE_CHECK(!HasArrived(request, player));
    player.inPackCell = false;
    player.loaded3D = false;
    POE_CHECK(!HasArrived(request, player));
    player.loaded3D = true;
    player.loading = true;
    POE_CHECK(!HasArrived(request, player));
}

POE_TEST("Map return timeout retries the saved target then tries the fixed marker once")
{
    MapTravel travel;
    POE_CHECK(travel.TryStart(Return(kInterior)));
    POE_CHECK(travel.Current()->attempts == 1);
    POE_CHECK(travel.Tick(kAtEntrance, 5.0, false) == MapTravelOutcome::Retry);
    POE_CHECK(travel.Current()->attempts == 2 && travel.Current()->target.cell == kInterior.cell);
    POE_CHECK(travel.Current()->elapsedSec == 0.0);
    POE_CHECK(travel.Tick(kAtEntrance, 5.0, false) == MapTravelOutcome::Fallback);
    POE_CHECK(travel.Current()->attempts == 3 && travel.Current()->target.cell == kSafeMarker.cell);
    POE_CHECK(!travel.Current()->fallback && travel.Current()->elapsedSec == 0.0);
    POE_CHECK(travel.Tick(kAtEntrance, 5.0, false) == MapTravelOutcome::Failed);
    POE_CHECK(!travel.Current());

    auto request = Return(kInterior);
    request.maxAttempts = 1;
    POE_CHECK(travel.TryStart(request));
    POE_CHECK(travel.Tick(kAtEntrance, 5.0, false) == MapTravelOutcome::Fallback);
    const MapTravelObservation atMarker{ kSafeMarker.cell, kSafeMarker.worldspace, kSafeMarker.position, true };
    POE_CHECK(travel.Tick(atMarker, 0.0, false) == MapTravelOutcome::Arrived);
    POE_CHECK(!travel.Current());
    request.fallback.reset();
    POE_CHECK(travel.TryStart(request));
    POE_CHECK(travel.Tick(kAtEntrance, 5.0, false) == MapTravelOutcome::Failed);
    POE_CHECK(!travel.Current());
}

POE_TEST("Map entry timeout cancels the request and late arrival grants no access")
{
    MapTravel travel;
    POE_CHECK(travel.TryStart(Entry(MapTravelKind::Reenter)));
    POE_CHECK(travel.Tick({}, 5.0, false) == MapTravelOutcome::Failed);
    POE_CHECK(!travel.Current());
    POE_CHECK(travel.Tick(kAtEntrance, 0.0, false) == MapTravelOutcome::Waiting);
    const auto late = CheckMapEntryAccess(kEntrance.cell, 0, kAtEntrance);
    POE_CHECK(!late.inActiveMap && late.unauthorized);
}

POE_TEST("Map entry authorization expires after a non-loading cell change")
{
    const auto permitted = CheckMapEntryAccess(kEntrance.cell, kEntrance.cell, kAtEntrance);
    POE_CHECK(permitted.inActiveMap && !permitted.unauthorized);
    const auto inactive = CheckMapEntryAccess(0, kEntrance.cell, kAtEntrance);
    POE_CHECK(inactive.permittedCell == 0 && !inactive.inActiveMap && inactive.unauthorized);
    MapTravelObservation outside{ kInterior.cell, 0, kInterior.position, true, true, false };
    const auto loading = CheckMapEntryAccess(kEntrance.cell, permitted.permittedCell, outside);
    POE_CHECK(loading.permittedCell == kEntrance.cell && !loading.inActiveMap && !loading.unauthorized);
    outside.loading = false;
    const auto left = CheckMapEntryAccess(kEntrance.cell, loading.permittedCell, outside);
    POE_CHECK(left.permittedCell == 0 && !left.inActiveMap && !left.unauthorized);
    const auto returned = CheckMapEntryAccess(kEntrance.cell, left.permittedCell, kAtEntrance);
    POE_CHECK(!returned.inActiveMap && returned.unauthorized);
}

POE_TEST("Map entry authorization survives a cell transition with no cell yet")
{
    MapTravelObservation between{ 0, 0, {}, false, false, false };
    const auto kept = CheckMapEntryAccess(kEntrance.cell, kEntrance.cell, between);
    POE_CHECK(kept.permittedCell == kEntrance.cell && !kept.inActiveMap && !kept.unauthorized);
    const auto back = CheckMapEntryAccess(kEntrance.cell, kept.permittedCell, kAtEntrance);
    POE_CHECK(back.inActiveMap && !back.unauthorized);
}

POE_TEST("Map run ends on return after a boss kill or with no portals")
{
    auto run = MapSystem::CreateNormalMapRun(1, 1, {}, 0, 0, {}, {});
    for (const bool bossKilled : { false, true }) {
        run.bossKilled = bossKilled;
        run.portalsRemaining = 1;
        POE_CHECK(MapSystem::ShouldEndOnReturn(run) == bossKilled);
        run.portalsRemaining = 0;
        POE_CHECK(MapSystem::ShouldEndOnReturn(run));
    }
}

POE_TEST("Map travel INI parses its bounds and keeps defaults for invalid values")
{
    const auto minimum = LoadConfigFromString(
        "[Maps]\nMapTravelTimeoutSec=5\nMapTravelArrivalRadius=64\nMapTravelMaxAttempts=1\n");
    POE_CHECK(minimum.warnings.empty());
    POE_CHECK(minimum.mapTravel.timeoutSec == 5.0 && minimum.mapTravel.arrivalRadius == 64.0 && minimum.mapTravel.maxAttempts == 1);
    const auto maximum = LoadConfigFromString(
        "[Maps]\nMapTravelTimeoutSec=120\nMapTravelArrivalRadius=4096\nMapTravelMaxAttempts=5\n");
    POE_CHECK(maximum.warnings.empty());
    POE_CHECK(maximum.mapTravel.timeoutSec == 120.0 && maximum.mapTravel.arrivalRadius == 4096.0 && maximum.mapTravel.maxAttempts == 5);
    const auto invalid = LoadConfigFromString(
        "[Maps]\nMapTravelTimeoutSec=4\nMapTravelArrivalRadius=4097\nMapTravelMaxAttempts=6\n");
    POE_CHECK(invalid.warnings.size() == 3);
    POE_CHECK(invalid.mapTravel.timeoutSec == 20.0 && invalid.mapTravel.arrivalRadius == 512.0 && invalid.mapTravel.maxAttempts == 2);
}

POE_TEST("Map fall requires a pack cell and a height strictly below the threshold")
{
    constexpr double minimumZ = -600.0;
    constexpr double margin = 512.0;
    auto player = kAtEntrance;
    player.position[2] = -1111.99f;
    POE_CHECK(!HasFallenBelowMap(player, minimumZ, margin));
    player.position[2] = -1112.0f;
    POE_CHECK(!HasFallenBelowMap(player, minimumZ, margin));
    player.position[2] = -1112.01f;
    POE_CHECK(HasFallenBelowMap(player, minimumZ, margin));
    player.inPackCell = false;
    POE_CHECK(!HasFallenBelowMap(player, minimumZ, margin));
    player.inPackCell = true;
    player.position[2] = std::numeric_limits<float>::quiet_NaN();
    POE_CHECK(!HasFallenBelowMap(player, minimumZ, margin));
}

// 결정 177 ③ 재보완: 출구 구역은 자동 출구 자리 중심의 3차원 구이고, 경계에 닿은 것(거리 == 반경)은 아직 밖이다.
POE_TEST("Map exit zone uses a 3D sphere around each automatic exit and picks the nearest")
{
    const std::array<MapAutoExit, 2> exits{ {
        { 0x81A, { 1000.0f, 0.0f, 0.0f }, 0.0f },
        { 0x81B, { 0.0f, 1000.0f, 300.0f }, 0.0f },
    } };
    auto player = kAtEntrance;
    player.position = { 1000.0f, 0.0f, 0.0f };
    auto hit = FindExitZoneHit(player, exits, 350.0);
    POE_CHECK(hit && hit->index == 0 && hit->distance == 0.0);
    player.position = { 1000.0f - 349.0f, 0.0f, 0.0f };
    POE_CHECK(FindExitZoneHit(player, exits, 350.0).has_value());
    player.position = { 1000.0f - 350.0f, 0.0f, 0.0f };
    POE_CHECK(!FindExitZoneHit(player, exits, 350.0));
    // 높이 차도 거리에 들어간다(pinemoon 걷기 실측: 수평 329, 높이 108 → 약 346).
    player.position = { 1000.0f - 329.0f, 0.0f, 108.0f };
    hit = FindExitZoneHit(player, exits, 350.0);
    POE_CHECK(hit && hit->distance > 340.0 && hit->distance < 350.0);
    player.position = { 1000.0f - 200.0f, 0.0f, 300.0f };
    POE_CHECK(!FindExitZoneHit(player, exits, 350.0));
    // 두 구에 모두 들면 가까운 출구
    player.position = { 550.0f, 650.0f, 150.0f };
    const auto both = FindExitZoneHit(player, exits, 1000.0);
    POE_CHECK(both && both->index == 1);
    // 팩 밖, 반경 없음, 좌표가 유한하지 않으면 판정하지 않는다.
    player.position = { 1000.0f, 0.0f, 0.0f };
    player.inPackCell = false;
    POE_CHECK(!FindExitZoneHit(player, exits, 350.0));
    player.inPackCell = true;
    POE_CHECK(!FindExitZoneHit(player, exits, 0.0));
    POE_CHECK(!FindExitZoneHit(player, exits, std::numeric_limits<double>::quiet_NaN()));
    POE_CHECK(!FindExitZoneHit(player, std::span<const MapAutoExit>{}, 350.0));
    player.position[0] = std::numeric_limits<float>::infinity();
    POE_CHECK(!FindExitZoneHit(player, exits, 350.0));
}

POE_TEST("Map exit zone latch holds within the same pack cell until the cell changes")
{
    auto player = kAtEntrance;
    player.cell = 0xC2008800;
    POE_CHECK(NextExitZoneLatch(0, player, false) == 0);
    POE_CHECK(NextExitZoneLatch(0, player, true) == 0xC2008800);
    // 거절 대기 중 구를 지나쳐 나가도 같은 셀이면 유지
    POE_CHECK(NextExitZoneLatch(0xC2008800, player, false) == 0xC2008800);
    // 다른 지도 팩 셀이면 잊고, 그 셀의 구역에 들면 그 셀로
    player.cell = 0xC2002800;
    POE_CHECK(NextExitZoneLatch(0xC2008800, player, false) == 0);
    POE_CHECK(NextExitZoneLatch(0xC2008800, player, true) == 0xC2002800);
    // 팩 밖이나 셀을 모르면 잊는다
    player.inPackCell = false;
    POE_CHECK(NextExitZoneLatch(0xC2002800, player, true) == 0);
    player.inPackCell = true;
    player.cell = 0;
    POE_CHECK(NextExitZoneLatch(0xC2002800, player, true) == 0);
}

POE_TEST("Map exit zone latch releases outside the zone while the boss fight holds the exit")
{
    // 결정 182 상태(포털 0, 보스 미처치)에서는 구역 안에서만 다시 시도하고, 벗어나면 같은 셀이어도 잊는다.
    auto player = kAtEntrance;
    player.cell = 0xC2008800;
    POE_CHECK(NextExitZoneLatch(0xC2008800, player, true, true) == 0xC2008800);
    POE_CHECK(NextExitZoneLatch(0xC2008800, player, false, true) == 0);
    POE_CHECK(NextExitZoneLatch(0, player, true, true) == 0xC2008800);
    player.inPackCell = false;
    POE_CHECK(NextExitZoneLatch(0xC2008800, player, true, true) == 0);
}

POE_TEST("Map fall margin INI accepts its bounds and defaults on invalid values")
{
    POE_CHECK(LoadConfigFromString("[Maps]\n").mapTravel.fallMargin == 512.0);
    const auto minimum = LoadConfigFromString("[Maps]\nMapFallMargin=128\n");
    const auto maximum = LoadConfigFromString("[Maps]\nMapFallMargin=8192\n");
    POE_CHECK(minimum.warnings.empty() && minimum.mapTravel.fallMargin == 128.0);
    POE_CHECK(maximum.warnings.empty() && maximum.mapTravel.fallMargin == 8192.0);
    for (const auto* value : { "127.999", "8192.001", "nan", "inf" }) {
        const auto invalid = LoadConfigFromString(std::string("[Maps]\nMapFallMargin=") + value);
        POE_CHECK(invalid.warnings.size() == 1 && invalid.mapTravel.fallMargin == 512.0);
    }
}

POE_TEST("Map drop cleanup selects only corpses from the closed run")
{
    constexpr std::array<std::uint32_t, 3> spawned{ 0xFF000010, 0xFF000011, 0 };
    POE_CHECK(MapSystem::IsRunDrop(0xFF000010, spawned));
    POE_CHECK(MapSystem::IsRunDrop(0xFF000011, spawned));
    POE_CHECK(!MapSystem::IsRunDrop(0xFF000012, spawned));
    POE_CHECK(!MapSystem::IsRunDrop(0, spawned));
    POE_CHECK(!MapSystem::IsRunDrop(0xFF000010, {}));
}
