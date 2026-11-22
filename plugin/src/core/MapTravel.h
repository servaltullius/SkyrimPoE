#pragma once

#include <array>
#include <cstdint>
#include <optional>

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

    [[nodiscard]] bool HasArrived(const MapTravelRequest& request, const MapTravelObservation& player) noexcept;
    [[nodiscard]] bool HasFallenBelowMap(const MapTravelObservation& player, double navmeshMinZ, double margin) noexcept;

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
