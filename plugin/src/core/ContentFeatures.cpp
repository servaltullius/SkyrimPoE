#include "core/ContentFeatures.h"

#include <atomic>

namespace SkyrimPoE::Core
{
    namespace
    {
        std::atomic<std::uint32_t> g_packageGeneration{ 0 };
    }

    void SetActiveContentGeneration(std::uint32_t packageGeneration) noexcept
    {
        g_packageGeneration.store(packageGeneration, std::memory_order_release);
    }

    std::uint32_t ActiveContentGeneration() noexcept
    {
        return g_packageGeneration.load(std::memory_order_acquire);
    }

    ContentFeatures ActiveContentFeatures() noexcept
    {
        return ContentFeaturesFor(ActiveContentGeneration());
    }
}
