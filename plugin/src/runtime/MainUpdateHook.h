#pragma once

#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <cstdint>
#include <functional>

namespace SkyrimPoE::Runtime
{
    class MainUpdateHook
    {
    public:
        using FrameCallback = std::function<void(RE::Main*, double deltaSec)>;

        // AE 1.6.1170의 ID 36544+0x160에 체인 훅을 설치한다(결정 124).
        static bool Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            const Core::DataPack* dataPack,
            const Core::Config* config);

        static void AddFrameCallback(FrameCallback cb);
        [[nodiscard]] static bool IsInstalled() noexcept;
        [[nodiscard]] static std::uint64_t TickCount() noexcept;

    private:
        static void Thunk(RE::Main* a_main);
    };
}
