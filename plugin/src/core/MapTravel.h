#pragma once

#include "core/MapPack.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace SkyrimPoE::Core
{
    enum class MapTravelKind
    {
        OpenMap, OpenArena, Reenter, Return, Abandon, DeathEscape, CancelledRun, SafeEscape
    };

    [[nodiscard]] bool IsMapEntry(MapTravelKind kind) noexcept;

    struct MapTravelTarget
    {
        std::uint32_t cell = 0;
        std::uint32_t worldspace = 0;
        std::array<float, 3> position{};
        std::array<float, 3> angle{};
        std::uint32_t marker = 0;
    };

    struct MapTravelObservation
    {
        std::uint32_t cell = 0;
        std::uint32_t worldspace = 0;
        std::array<float, 3> position{};
        bool loaded3D = false;
        bool loading = false;
        bool inPackCell = false;
    };

    struct MapTravelRequest
    {
        MapTravelKind kind;
        MapTravelTarget target;
        double arrivalRadius;
        bool requireOutsidePack;
        std::optional<MapTravelTarget> fallback;
        std::uint32_t maxAttempts;
        double timeoutSec;
        std::uint32_t attempts = 1;
        double elapsedSec = 0.0;
    };

    enum class MapTravelOutcome { Waiting, Arrived, Retry, Fallback, Failed };

    constexpr double kMapSpawnDisableWaitTimeoutSec = 3.0;
    enum class MapSpawnDisableWaitOutcome { Waiting, Ready, TimedOut };

    struct MapSpawnDisableWaitResult
    {
        double elapsedSec;
        MapSpawnDisableWaitOutcome outcome;
    };

    [[nodiscard]] MapSpawnDisableWaitResult AdvanceMapSpawnDisableWait(
        double elapsedSec, double deltaSec, std::size_t remaining) noexcept;
    [[nodiscard]] bool HasArrived(const MapTravelRequest& request, const MapTravelObservation& player) noexcept;
    [[nodiscard]] bool HasFallenBelowMap(const MapTravelObservation& player, double navmeshMinZ, double margin) noexcept;

    // 결정 177 ③ 재보완: 지도 팩 셀 안에서 자동 출구 자리로부터 3차원 거리가 반경보다 작으면 출구 구역에 든 것이다.
    // 엔진 자동 문의 발동 범위(fAutoDoorActivateDistance)와 같은 구를 쓰되, 경계를 넘는 순간이 아니라 안에 있는 동안 계속 판정한다.
    struct MapExitZoneHit
    {
        std::size_t index;
        double distance;
    };
    [[nodiscard]] std::optional<MapExitZoneHit> FindExitZoneHit(
        const MapTravelObservation& player, std::span<const MapAutoExit> exits, double radius) noexcept;
    // 출구 구역에 든 셀을 이동이 시작될 때까지 기억한다(귀환이 거절된 동안 구를 지나쳐 나가도 다시 시도하게).
    // 구역 안이면 그 셀, 같은 지도 팩 셀에 머무는 동안은 그대로, 셀이 바뀌거나 팩 밖이면 잊는다(0).
    [[nodiscard]] std::uint32_t NextExitZoneLatch(std::uint32_t latchedCell, const MapTravelObservation& player, bool inZone) noexcept;

    class MapTravel
    {
    public:
        [[nodiscard]] bool TryStart(MapTravelRequest request) noexcept;
        [[nodiscard]] const std::optional<MapTravelRequest>& Current() const noexcept { return current_; }
        [[nodiscard]] MapTravelOutcome Tick(const MapTravelObservation& player, double deltaSec, bool paused) noexcept;
        void Clear() noexcept { current_.reset(); }

    private:
        std::optional<MapTravelRequest> current_;
    };

    struct MapEntryAccess
    {
        std::uint32_t permittedCell = 0;
        bool inActiveMap = false;
        bool unauthorized = false;
    };

    [[nodiscard]] MapEntryAccess CheckMapEntryAccess(
        std::uint32_t activeCell, std::uint32_t permittedCell, const MapTravelObservation& player) noexcept;
}
