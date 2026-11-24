#include "runtime/ModState.h"

#include <atomic>
#include <utility>

namespace SkyrimPoE::Runtime::ModState
{
    namespace
    {
        std::atomic<std::shared_ptr<const ModStatus>> g_status{
            std::make_shared<const ModStatus>(ModMode::Preserve, "콘텐츠를 검사하는 중입니다.")
        };
    }

    std::shared_ptr<const ModStatus> Get() noexcept
    {
        return g_status.load(std::memory_order_acquire);
    }

    bool AllowsGameplay() noexcept
    {
        return Get()->mode == ModMode::Normal;
    }

    void Set(ModMode mode, std::string reason)
    {
        g_status.store(std::make_shared<const ModStatus>(mode, std::move(reason)), std::memory_order_release);
    }
}
