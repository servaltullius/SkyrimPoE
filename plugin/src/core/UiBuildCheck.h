#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace SkyrimPoE::Core::UiBuildCheck
{
    inline constexpr std::uint64_t kUiProtocolVersion = 1;

    enum class OperationKind { ReadOnly, StateChanging };

    inline constexpr std::pair<std::string_view, OperationKind> kOperations[] = {
        { "close", OperationKind::ReadOnly },
        { "refresh", OperationKind::ReadOnly },
        { "treeSearchFocus", OperationKind::ReadOnly },
        { "switchTab", OperationKind::ReadOnly },
        { "equip", OperationKind::StateChanging },
        { "unequip", OperationKind::StateChanging },
        { "socket", OperationKind::StateChanging },
        { "unsocket", OperationKind::StateChanging },
        { "link", OperationKind::StateChanging },
        { "toggleAura", OperationKind::StateChanging },
        { "treeAllocatePath", OperationKind::StateChanging },
        { "treeRefundNode", OperationKind::StateChanging },
        { "treeRefundAll", OperationKind::StateChanging },
        { "treeSelectClass", OperationKind::StateChanging },
        { "treeSelectAscendancy", OperationKind::StateChanging },
        { "treeClearAscendancy", OperationKind::StateChanging },
        { "treeSelectMastery", OperationKind::StateChanging },
        { "treeSocketJewel", OperationKind::StateChanging },
        { "treeUnsocketJewel", OperationKind::StateChanging },
        { "gearIdentifyToken", OperationKind::StateChanging },
        { "gearIdentifyUnique", OperationKind::StateChanging },
        { "gearApplyOrb", OperationKind::StateChanging },
        { "gearApplyJeweller", OperationKind::StateChanging },
        { "gearEquipRing2", OperationKind::StateChanging },
        { "gearUnequipRing2", OperationKind::StateChanging },
        { "gearEquipBelt", OperationKind::StateChanging },
        { "gearUnequipBelt", OperationKind::StateChanging },
        { "gearEquipCharm", OperationKind::StateChanging },
        { "gearUnequipCharm", OperationKind::StateChanging },
        { "mapOpen", OperationKind::StateChanging },
        { "mapOpenArena4", OperationKind::StateChanging },
        { "mapReenter", OperationKind::StateChanging },
        { "mapReturnPortal", OperationKind::StateChanging },
        { "mapAbandon", OperationKind::StateChanging },
        { "mapSafeEscape", OperationKind::StateChanging },
        { "mapSafeUninstall", OperationKind::StateChanging },
    };

    [[nodiscard]] constexpr OperationKind ClassifyOperation(std::string_view op) noexcept
    {
        for (const auto& [name, kind] : kOperations) {
            if (op == name) {
                return kind;
            }
        }
        return OperationKind::StateChanging;
    }

    struct Result
    {
        bool match;
        std::string_view reason;
    };

    [[nodiscard]] constexpr Result Evaluate(
        std::string_view receivedBuildId,
        std::optional<std::uint64_t> receivedProtocol,
        std::string_view expectedBuildId,
        std::uint64_t expectedProtocol) noexcept
    {
        if (expectedBuildId.empty()) {
            return { false, "DLL build ID is unavailable" };
        }
        if (receivedBuildId.empty()) {
            return { false, "UI build ID is missing or invalid" };
        }
        if (receivedBuildId != expectedBuildId) {
            return { false, "build ID mismatch" };
        }
        if (!receivedProtocol) {
            return { false, "UI protocol is missing or invalid" };
        }
        if (*receivedProtocol != expectedProtocol) {
            return { false, "UI protocol mismatch" };
        }
        return { true, {} };
    }
}
