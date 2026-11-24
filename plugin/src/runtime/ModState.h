#pragma once

#include <memory>
#include <string>

namespace SkyrimPoE::Runtime
{
    enum class ModMode { Normal, CleanupPending, Preserve };

    struct ModStatus
    {
        ModMode mode;
        std::string reason;
    };

    namespace ModState
    {
        [[nodiscard]] std::shared_ptr<const ModStatus> Get() noexcept;
        [[nodiscard]] bool AllowsGameplay() noexcept;
        void Set(ModMode mode, std::string reason);
    }
}
