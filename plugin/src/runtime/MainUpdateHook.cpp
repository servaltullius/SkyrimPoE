#include "PCH.h"
#include "runtime/MainUpdateHook.h"
#include "runtime/Addresses.h"
#include "runtime/AutoTest.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/ModState.h"
#include "runtime/SaveLoad.h"
#include "runtime/TrueHudBridge.h"
#include "runtime/UiBridge.h"

#include <REL/Offset2ID.h>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::Config* g_config = nullptr;

        std::mutex g_callbackMutex;
        std::vector<MainUpdateHook::FrameCallback> g_callbacks;

        std::atomic<std::uintptr_t> g_originalMainUpdateAddr{ 0 };
        std::atomic_bool g_installed{ false };
        std::atomic<std::uint64_t> g_tickCount{ 0 };
        std::chrono::steady_clock::time_point g_lastTick{};
        bool g_hasLastTick = false;
        bool g_hudSuspended = false;

        bool ContainsAddress(const REL::Segment& seg, std::uintptr_t addr, std::size_t size) noexcept
        {
            const auto begin = seg.address();
            const auto end = begin + seg.size();
            return addr >= begin && addr <= end && size <= (end - addr);
        }

        void CallOriginalMainUpdate(RE::Main* main)
        {
            const auto address = g_originalMainUpdateAddr.load(std::memory_order_acquire);
            if (address != 0) {
                using MainUpdateFn = void (*)(RE::Main*);
                reinterpret_cast<MainUpdateFn>(address)(main);
            }
        }

        bool SkipPluginFrame(RE::Main* main)
        {
            if (ModState::AllowsGameplay()) {
                g_hudSuspended = false;
                return false;
            }
            g_hasLastTick = false;
            if (!g_hudSuspended) {
                try {
                    TrueHudBridge::SuspendSpecialBarOnMainThread();
                    UiBridge::SuspendHudOnMainThread();
                    g_hudSuspended = true;
                } catch (...) {
                    SKSE::log::warn("MainUpdateHook: could not suspend the HUD; retrying on the next update.");
                }
            }
            try {
                AutoTest::TickSaveOnlyWhileSuspended(main);
            } catch (...) {
                SKSE::log::warn("MainUpdateHook: suspended AutoTest frame failed.");
            }
            if (ModState::Get()->mode == ModMode::CleanupPending) {
                try {
                    SaveLoad::TickMigration();
                } catch (const std::exception& e) {
                    SKSE::log::error("MainUpdateHook: legacy cleanup tick failed: {}", e.what());
                } catch (...) {
                    SKSE::log::error("MainUpdateHook: legacy cleanup tick failed with an unknown exception.");
                }
            }
            CallOriginalMainUpdate(main);
            return true;
        }
    }

    void MainUpdateHook::Thunk(RE::Main* a_main)
    {
        if (SkipPluginFrame(a_main)) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        double dt = 0.0;
        if (g_hasLastTick) {
            dt = std::clamp(std::chrono::duration<double>(now - g_lastTick).count(), 0.0, 0.25);
        }
        g_lastTick = now;
        g_hasLastTick = true;

        g_tickCount.fetch_add(1, std::memory_order_relaxed);

        // 1. 프레임 시작 스냅샷의 예약 큐만 FIFO로 처리(결정 119, 137(나)①)
        if (g_queue) {
            const double budget = g_config ? g_config->mainUpdateBudgetMs : 2.0;
            try {
                (void)g_queue->DrainFrame(budget);
            } catch (const std::exception& e) {
                SKSE::log::error("MainUpdateHook: CommandQueue exception: {}", e.what());
            } catch (...) {
                SKSE::log::error("MainUpdateHook: CommandQueue unknown exception.");
            }
        }
        if (SkipPluginFrame(a_main)) {
            return;
        }

        // 2. 층 ①·②가 dirty면 주 갱신 스레드에서만 단일 불변 StatSnapshot 재계산(결정 117, 118, 137(다))
        if (g_statEngine && g_dataPack && g_config && g_dataPack->IsLoaded()) {
            try {
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            } catch (const std::exception& e) {
                SKSE::log::error("MainUpdateHook: StatEngine exception: {}", e.what());
            } catch (...) {
                SKSE::log::error("MainUpdateHook: StatEngine unknown exception.");
            }
        }
        if (SkipPluginFrame(a_main)) {
            return;
        }

        // 3. 등록된 런타임 프레임 콜백 실행(ES 재충전, 사이클론 틱 등 후속 마일스톤 확장점)
        std::vector<FrameCallback> localCallbacks;
        {
            std::scoped_lock lock(g_callbackMutex);
            localCallbacks = g_callbacks;
        }
        for (const auto& cb : localCallbacks) {
            if (!cb) continue;
            try {
                cb(a_main, dt);
            } catch (const std::exception& e) {
                SKSE::log::error("MainUpdateHook: callback exception: {}", e.what());
            } catch (...) {
                SKSE::log::error("MainUpdateHook: callback unknown exception.");
            }
        }

        CallOriginalMainUpdate(a_main);
    }

    bool MainUpdateHook::Install(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        const Core::DataPack* dataPack,
        const Core::Config* config)
    {
        if (g_installed.load(std::memory_order_acquire)) {
            return true;
        }
        g_queue = queue;
        g_statEngine = statEngine;
        g_dataPack = dataPack;
        g_config = config;
        MapDeviceRuntime::BindDataPack(dataPack);

        const auto ver = REL::Module::get().version();
        if (!Addresses::IsVerifiedRuntime(ver)) {
            SKSE::log::warn("MainUpdateHook: skipped on unverified runtime {} (AE 1.6.1170 verified only, decision 141).",
                ver.string("."));
            return false;
        }

        const auto callerAddr = REL::ID(Addresses::kAeMainUpdateCallerId).address();
        const auto targetAddr = REL::ID(Addresses::kAeMainUpdateTargetId).address();
        const auto site = callerAddr + Addresses::kAeMainUpdateCallOffset;

        const auto textSeg = REL::Module::get().segment(REL::Segment::textx);
        if (!ContainsAddress(textSeg, site, 5)) {
            SKSE::log::error("MainUpdateHook: call site 0x{:X} is outside executable code section.", site);
            return false;
        }
        const auto opcode = *reinterpret_cast<const std::uint8_t*>(site);
        if (opcode != 0xE8) {
            SKSE::log::error("MainUpdateHook: unexpected opcode 0x{:02X} at 0x{:X} (expected 0xE8).", opcode, site);
            return false;
        }

        auto& trampoline = SKSE::GetTrampoline();
        if (trampoline.free_size() < 14) {
            SKSE::log::error("MainUpdateHook: insufficient trampoline space ({} bytes free).", trampoline.free_size());
            return false;
        }

        // 패치 직후 다른 경로가 Thunk에 진입해도 원본 호출이 누락되지 않도록, E8 rel32의 기존 대상을 먼저 원자적으로 기록한다.
        std::int32_t rel32 = 0;
        std::memcpy(&rel32, reinterpret_cast<const void*>(site + 1), sizeof(rel32));
        const auto preReadTarget = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(site + 5) + static_cast<std::intptr_t>(rel32));
        g_originalMainUpdateAddr.store(preReadTarget, std::memory_order_release);

        // 기존 훅이 먼저 걸려 있더라도 이전 호출 대상을 이어 받아 체인으로 연결(결정 124, 프로토타입 12)
        const auto previous = trampoline.write_call<5>(site, Thunk);
        g_originalMainUpdateAddr.store(previous, std::memory_order_release);
        g_installed.store(true, std::memory_order_release);

        SKSE::log::info("MainUpdateHook: installed at AE 36544+0x160 (0x{:X}, {}), previous target 0x{:X}.",
            site,
            previous == targetAddr ? "direct" : "chained",
            previous);
        return true;
    }

    void MainUpdateHook::AddFrameCallback(FrameCallback cb)
    {
        if (!cb) return;
        std::scoped_lock lock(g_callbackMutex);
        g_callbacks.push_back(std::move(cb));
    }

    bool MainUpdateHook::IsInstalled() noexcept
    {
        return g_installed.load(std::memory_order_acquire);
    }

    std::uint64_t MainUpdateHook::TickCount() noexcept
    {
        return g_tickCount.load(std::memory_order_relaxed);
    }
}
