#include "core/MapTravel.h"

#include <cmath>

namespace SkyrimPoE::Core
{
    bool IsMapEntry(MapTravelKind kind) noexcept
    {
        return kind == MapTravelKind::OpenMap || kind == MapTravelKind::OpenArena || kind == MapTravelKind::Reenter;
    }

    bool HasArrived(const MapTravelRequest& request, const MapTravelObservation& player) noexcept
    {
        if (player.loading || !player.loaded3D || (request.requireOutsidePack && (player.cell == 0 || player.inPackCell))) {
            return false;
        }
        const auto& target = request.target;
        if (IsMapEntry(request.kind)) {
            if (target.cell == 0 || player.cell != target.cell) {
                return false;
            }
        } else if (target.worldspace == 0) {
            return target.cell != 0 && player.cell == target.cell;
        } else if (player.worldspace != target.worldspace) {
            return false;
        }

        double distanceSquared = 0.0;
        for (std::size_t axis = 0; axis < target.position.size(); ++axis) {
            const double offset = static_cast<double>(player.position[axis]) - target.position[axis];
            distanceSquared += offset * offset;
        }
        return distanceSquared <= request.arrivalRadius * request.arrivalRadius;
    }

    bool MapTravel::TryStart(MapTravelRequest request) noexcept
    {
        if (current_) {
            return false;
        }
        current_ = request;
        return true;
    }

    MapTravelOutcome MapTravel::Tick(const MapTravelObservation& player, double deltaSec, bool paused) noexcept
    {
        if (!current_ || player.loading) {
            return MapTravelOutcome::Waiting;
        }
        auto& request = *current_;
        if (HasArrived(request, player)) {
            Clear();
            return MapTravelOutcome::Arrived;
        }
        if (!paused && std::isfinite(deltaSec) && deltaSec > 0.0) {
            request.elapsedSec += deltaSec;
        }
        if (request.elapsedSec < request.timeoutSec) {
            return MapTravelOutcome::Waiting;
        }
        if (!IsMapEntry(request.kind)) {
            if (request.attempts < request.maxAttempts) {
                ++request.attempts;
                request.elapsedSec = 0.0;
                return MapTravelOutcome::Retry;
            }
            if (request.fallback) {
                request.target = *request.fallback;
                request.fallback.reset();
                ++request.attempts;
                request.elapsedSec = 0.0;
                return MapTravelOutcome::Fallback;
            }
        }
        Clear();
        return MapTravelOutcome::Failed;
    }

    MapEntryAccess CheckMapEntryAccess(
        std::uint32_t activeCell, std::uint32_t permittedCell, const MapTravelObservation& player) noexcept
    {
        if (permittedCell != activeCell ||
            (!player.loading && player.cell != 0 && player.cell != permittedCell)) {
            permittedCell = 0;
        }
        const bool inActiveMap = permittedCell != 0 && player.cell == permittedCell;
        return { permittedCell, inActiveMap, player.inPackCell && !inActiveMap };
    }
}
