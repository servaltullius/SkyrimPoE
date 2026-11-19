#include "Measurement.h"

#include "RE/B/BGSSaveLoadManager.h"
#include "RE/L/LoadingMenu.h"
#include "RE/M/Main.h"
#include "RE/M/MenuOpenCloseEvent.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/S/State.h"
#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"
#include "REL/Offset2ID.h"
#include "REX/W32/KERNEL32.h"
#include "SKSE/SKSE.h"

#include <Windows.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;
    using namespace Probe;

    constexpr auto kTick = 100ms;
    constexpr auto kConfig = "Data/SKSE/Plugins/PoEProtoMainUpdate.ini";
    constexpr auto kMarker = "PoEProtoMainUpdate.autotest";
    constexpr auto kRuntime = SKSE::RUNTIME_SSE_1_6_1170;

    struct Config
    {
        bool installWithoutAutotest = false;
        bool quitWhenDone = true;
        std::chrono::seconds interval = 1s;
        std::chrono::seconds mainMenuDelay = 2s;
        std::chrono::seconds loadTimeout = 60s;
        std::chrono::seconds menuTimeout = 3s;
        std::array<std::chrono::seconds, kPhases> duration{ 5s, 5s, 3s, 5s, 3s, 0s, 5s };
    };

    enum class LoadState { None, Requested, Started, Succeeded, Failed, Unexpected };
    enum class WriteStep
    {
        Idle, Observing, DrainMagicka, RestoreMagicka, HitHealth, RestoreHealth, InProgress, Finished, Unavailable
    };

    struct ThreadMismatch
    {
        std::uint32_t expected = 0;
        std::uint32_t actual = 0;
    };

    struct ValueTrial
    {
        float before = 0.0f;
        float after = 0.0f;
        float restored = 0.0f;
        float damage = 0.0f;
        bool restoredOnHook = false;
    };

    struct Run
    {
        std::string save;
        std::optional<Phase> phase;
        Clock::time_point deadline;
        Clock::time_point menuCloseDeadline;
        Clock::time_point writeSelectAt;
        std::optional<Clock::time_point> mainMenuSince;
        std::string_view abortReason;
        bool requestedInitialLoad = false;
        bool openedMenu = false;
        bool requestedClose = false;
    };

    Config g_config;
    Run g_run;
    Measurement g_measurement;
    std::ofstream g_log;
    std::ofstream g_tsv;
    std::array<bool, kCandidates> g_installed{};
    Candidate g_writer = Candidate::Count;
    const std::uint32_t* g_updateCounter = nullptr;
    std::atomic<LoadState> g_load{ LoadState::None };
    std::atomic_bool g_tickQueued{ false };
    std::atomic<Tick> g_lastTick{ 0 };
    std::atomic<WriteStep> g_writeStep{ WriteStep::Idle };
    std::atomic_uint g_observedWriteCandidates{ 0 };
    std::array<std::atomic<ThreadMismatch>, kCandidates> g_lastWriteThreadMismatch{};
    ValueTrial g_magicka;
    ValueTrial g_health;
    bool g_damageReturned = false;
    bool g_startAutotest = false;

    void StartupLog(std::string_view text)
    {
        if (g_log) {
            g_log << text << '\n' << std::flush;
        } else {
            const auto line = std::format("PoEProtoMainUpdate: {}\n", text);
            REX::W32::OutputDebugStringA(line.c_str());
        }
    }

    std::string_view Trim(std::string_view text)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == text.npos) {
            return {};
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    void ReadConfig()
    {
        std::ifstream input(kConfig, std::ios::binary);
        if (!input) {
            if (fs::exists(kConfig)) {
                StartupLog("Could not read the INI. Using defaults.");
            }
            return;
        }
        const std::array durationKeys{ "BaselineSeconds", "JournalSeconds", "AfterJournalSeconds", "InventorySeconds",
            "EngineWriteSeconds", "ReloadSeconds", "AfterReloadSeconds" };
        std::string line;
        bool first = true;
        bool settings = true;
        while (std::getline(input, line)) {
            auto text = Trim(line);
            if (std::exchange(first, false) && text.starts_with("\xEF\xBB\xBF")) {
                text = Trim(text.substr(3));
            }
            if (text.starts_with('[')) {
                settings = text == "[Autotest]";
                continue;
            }
            if (!settings) {
                continue;
            }
            text = Trim(text.substr(0, text.find_first_of(";#")));
            if (text.empty()) {
                continue;
            }
            const auto equals = text.find('=');
            const auto key = Trim(text.substr(0, equals));
            const auto value = equals == text.npos ? std::string_view{} : Trim(text.substr(equals + 1));
            if (key == "InstallWithoutAutotest" || key == "QuitWhenDone") {
                if (value == "true" || value == "false") {
                    auto& flag = key == "InstallWithoutAutotest" ? g_config.installWithoutAutotest : g_config.quitWhenDone;
                    flag = value == "true";
                } else {
                    StartupLog(std::format("Invalid INI boolean: {}", key));
                }
                continue;
            }
            auto* duration = [&]() -> std::chrono::seconds* {
                if (key == "IntervalSeconds") {
                    return &g_config.interval;
                }
                if (key == "MainMenuDelaySeconds") {
                    return &g_config.mainMenuDelay;
                }
                if (key == "LoadTimeoutSeconds") {
                    return &g_config.loadTimeout;
                }
                if (key == "MenuTimeoutSeconds") {
                    return &g_config.menuTimeout;
                }
                const auto match = std::find(durationKeys.begin(), durationKeys.end(), key);
                return match == durationKeys.end() ? nullptr : &g_config.duration[match - durationKeys.begin()];
            }();
            if (value.empty() || !duration) {
                StartupLog(std::format("Invalid INI setting: {}", key));
                continue;
            }
            int seconds = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
            const bool mayBeZero = key == "MainMenuDelaySeconds" || key == "ReloadSeconds";
            const int maximum = key == "IntervalSeconds" ? 60 : 600;
            if (error != std::errc{} || end != value.data() + value.size() ||
                seconds < (mayBeZero ? 0 : 1) || seconds > maximum) {
                StartupLog(std::format("Out-of-range INI seconds: {}", key));
                continue;
            }
            *duration = std::chrono::seconds(seconds);
        }
        if (input.bad()) {
            StartupLog("An error interrupted the INI read.");
        }
    }

    bool ReadMarker(const fs::path& marker)
    {
        std::array<char, 1024> buffer{};
        std::ifstream input(marker, std::ios::binary);
        input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const bool read = !input.fail();
        const auto length = static_cast<std::size_t>(input.gcount()) - (read && !input.eof() ? 1 : 0);
        input.close();
        std::error_code error;
        if (!fs::remove(marker, error)) {
            StartupLog("Could not remove the autotest marker.");
            return false;
        }
        auto name = Trim(std::string_view(buffer.data(), length));
        if (name.starts_with("\xEF\xBB\xBF")) {
            name = Trim(name.substr(3));
        }
        if (!read || name.empty() || name == "." || name == ".." ||
            name.find_first_of("/\\:*?\"<>|") != name.npos ||
            std::ranges::any_of(name, [](unsigned char ch) { return ch < 32 || ch == 127; })) {
            StartupLog("The autotest marker requires a save name on its first line.");
            return false;
        }
        g_run.save = name;
        return true;
    }

    bool Contains(REL::Segment segment, std::uintptr_t address, std::size_t size)
    {
        return address >= segment.address() && size <= segment.size() &&
            address - segment.address() <= segment.size() - size;
    }

    std::uintptr_t FindAddress(const REL::Offset2ID& database, std::uint64_t id)
    {
        const auto entry = std::ranges::find(database, id, &REL::Offset2ID::value_type::id);
        return entry == database.end() ? 0 : REL::Module::get().base() + entry->offset;
    }

    std::uintptr_t RelativeTarget(std::uintptr_t address, std::size_t opcodeLength)
    {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, reinterpret_cast<const void*>(address + opcodeLength), sizeof(displacement));
        return address + opcodeLength + sizeof(displacement) + displacement;
    }

    std::string ModuleFileName(std::uintptr_t address)
    {
        HMODULE module = nullptr;
        constexpr auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!::GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(address), &module)) {
            return "module unavailable";
        }
        std::array<wchar_t, 32768> path;
        const auto length = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            return "module filename unavailable";
        }
        const auto name = fs::path(std::wstring_view(path.data(), length)).filename().wstring();
        return SKSE::stl::utf16_to_utf8(name).value_or("module filename unavailable");
    }

    void Capture(Candidate candidate) noexcept
    {
        if (g_measurement.Active()) {
            g_measurement.Record(candidate, REX::W32::GetCurrentThreadId());
        }
    }

    void ApplyDamage(ValueTrial& trial, RE::PlayerCharacter& player, RE::ActorValue value)
    {
        trial.before = player.AsActorValueOwner()->GetActorValue(value);
        const auto beforeModifier = player.GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, value);
        if (value == RE::ActorValue::kMagicka) {
            player.AsActorValueOwner()->DamageActorValue(value, 1.0f);
        } else {
            g_damageReturned = player.DoDamage(1.0f, nullptr, false);
        }
        trial.after = player.AsActorValueOwner()->GetActorValue(value);
        trial.damage = beforeModifier - player.GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, value);
    }

    void RestoreDamage(ValueTrial& trial, RE::PlayerCharacter& player, RE::ActorValue value)
    {
        if (std::isfinite(trial.damage) && trial.damage > 0.0f) {
            player.AsActorValueOwner()->RestoreActorValue(value, trial.damage);
        }
        trial.restored = player.AsActorValueOwner()->GetActorValue(value);
    }

    void TryEngineWrite(Candidate candidate, RE::Main* main) noexcept
    {
        auto step = g_writeStep.load(std::memory_order_acquire);
        if (!main || step == WriteStep::Idle || step == WriteStep::InProgress ||
            step == WriteStep::Finished || step == WriteStep::Unavailable) {
            return;
        }
        if (step != WriteStep::Observing && candidate != g_writer) {
            return;
        }
        const ThreadMismatch threads{ main->threadID, REX::W32::GetCurrentThreadId() };
        if (threads.actual != threads.expected) {
            g_lastWriteThreadMismatch[static_cast<std::size_t>(candidate)].store(threads, std::memory_order_relaxed);
            return;
        }
        if (step == WriteStep::Observing) {
            g_observedWriteCandidates.fetch_or(CandidateBit(candidate), std::memory_order_relaxed);
            return;
        }
        if (!g_writeStep.compare_exchange_strong(step, WriteStep::InProgress)) {
            return;
        }
        const auto load = g_load.load();
        if (load != LoadState::Succeeded) {
            g_writeStep.store(WriteStep::Unavailable);
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || player->IsDead()) {
            g_writeStep.store(WriteStep::Unavailable);
            return;
        }
        if (step == WriteStep::DrainMagicka || step == WriteStep::HitHealth) {
            auto* ui = RE::UI::GetSingleton();
            if (!g_measurement.Active() || !ui || ui->GameIsPaused() || main->GetRuntimeData().freezeTime) {
                g_writeStep.store(WriteStep::Unavailable);
                return;
            }
        }
        switch (step) {
        case WriteStep::DrainMagicka:
            if (player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka) > 1.0f &&
                player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) > 10.0f && !player->IsGodMode()) {
                ApplyDamage(g_magicka, *player, RE::ActorValue::kMagicka);
                g_writeStep.store(WriteStep::RestoreMagicka);
            } else {
                g_writeStep.store(WriteStep::Unavailable);
            }
            break;
        case WriteStep::RestoreMagicka:
            RestoreDamage(g_magicka, *player, RE::ActorValue::kMagicka);
            g_magicka.restoredOnHook = true;
            g_writeStep.store(WriteStep::HitHealth);
            break;
        case WriteStep::HitHealth:
            if (player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) > 10.0f && !player->IsGodMode()) {
                ApplyDamage(g_health, *player, RE::ActorValue::kHealth);
                g_writeStep.store(WriteStep::RestoreHealth);
            } else {
                g_writeStep.store(WriteStep::Unavailable);
            }
            break;
        case WriteStep::RestoreHealth:
            RestoreDamage(g_health, *player, RE::ActorValue::kHealth);
            g_health.restoredOnHook = true;
            g_writeStep.store(WriteStep::Finished);
            break;
        default:
            g_writeStep.store(WriteStep::Unavailable);
            break;
        }
    }

    template <Candidate Which>
    struct MainUpdateHook
    {
        static void Thunk(RE::Main* a_main, float a_arg2, std::uintptr_t a_arg3, std::uintptr_t a_arg4)
        {
            Capture(Which);
            if (g_writeStep.load(std::memory_order_relaxed) != WriteStep::Idle) {
                TryEngineWrite(Which, a_main);
            }
            // Forward RCX, XMM1, R8 and R9 because the probe can clobber these volatile argument registers.
            original(a_main, a_arg2, a_arg3, a_arg4);
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    struct PlayerUpdateHook
    {
        static void Thunk(RE::PlayerCharacter* player, float delta)
        {
            Capture(Candidate::C);
            original(player, delta);
            if (g_writeStep.load(std::memory_order_relaxed) != WriteStep::Idle) {
                TryEngineWrite(Candidate::C, RE::Main::GetSingleton());
            }
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    template <Candidate Which>
    bool InstallMainCall(std::uintptr_t site, std::uintptr_t target)
    {
        const auto label = kCandidateNames[static_cast<std::size_t>(Which)];
        const auto text = REL::Module::get().segment(REL::Segment::textx);
        if (!Contains(text, site, 5)) {
            StartupLog(std::format("{}: skipped (call site outside the executable code section).", label));
            return false;
        }
        const auto opcode = *reinterpret_cast<const std::uint8_t*>(site);
        if (opcode != 0xE8) {
            StartupLog(std::format("{}: skipped (opcode 0x{:02X}, expected an E8 relative call).", label, opcode));
            return false;
        }
        auto& trampoline = SKSE::GetTrampoline();
        if (trampoline.free_size() < 14) {
            StartupLog(std::format("{}: skipped (SKSE branch pool unavailable).", label));
            return false;
        }
        const auto previous = trampoline.write_call<5>(site, MainUpdateHook<Which>::Thunk);
        MainUpdateHook<Which>::original = previous;
        StartupLog(std::format("{}: installed ({}), previous target 0x{:X} ({}).", label,
            previous == target ? "direct" : "chained", previous, ModuleFileName(previous)));
        return true;
    }

    void InstallHooks(REL::Version version)
    {
        const REL::Offset2ID database;
        if (version == kRuntime) {
            const auto target = FindAddress(database, 36564);
            // On 1.6.1170 the update count lives outside BSGraphics::State. Main::Update
            // increments ID 380737 via "inc dword ptr [rip+disp32]" at +0x47B.
            const auto increment = target + 0x47B;
            const auto counter = FindAddress(database, 380737);
            if (Contains(REL::Module::get().segment(REL::Segment::textx), increment, 6) &&
                Contains(REL::Module::get().segment(REL::Segment::data), counter, sizeof(std::uint32_t)) &&
                std::memcmp(reinterpret_cast<const void*>(increment), "\xFF\x05", 2) == 0 &&
                RelativeTarget(increment, 2) == counter) {
                g_updateCounter = reinterpret_cast<const std::uint32_t*>(counter);
            } else {
                StartupLog("Update counter: could not verify the AE 1.6.1170 increment. Recording -1.");
            }

            constexpr std::size_t budget = 64;
            if (const auto* trampolineInterface = SKSE::GetTrampolineInterface()) {
                if (auto* memory = trampolineInterface->AllocateFromBranchPool(budget)) {
                    SKSE::GetTrampoline().set_trampoline(memory, budget);
                }
            }
            g_installed[0] = InstallMainCall<Candidate::A>(FindAddress(database, 36544) + 0x160, target);
            g_installed[1] = InstallMainCall<Candidate::B>(FindAddress(database, 36550) + 0x11F, target);
        } else {
            StartupLog("A and B: skipped (call sites are verified on AE 1.6.1170 only).");
        }

        constexpr auto slot = 0xAD;
        const auto id = version == kRuntime ? 208040 : 261916;  // VTABLE_PlayerCharacter[0]
        const auto table = FindAddress(database, id);
        const auto segment = REL::Module::get().segment(REL::Segment::rdata);
        if (Contains(segment, table, (slot + 1) * sizeof(void*))) {
            REL::Relocation<std::uintptr_t> vtable{ table };
            const auto original = reinterpret_cast<const std::uintptr_t*>(table)[slot];
            if (original != 0) {
                PlayerUpdateHook::original = original;
                // Keep any existing PlayerCharacter::Update hook in the call chain.
                vtable.write_vfunc(slot, PlayerUpdateHook::Thunk);
                g_installed[2] = true;
                StartupLog("C: installed.");
            }
        }
        if (!g_installed[2]) {
            StartupLog("C: skipped (PlayerCharacter::Update vtable entry unavailable).");
        }
        g_installed[3] = true;
    }

    Context ReadContext(RE::UI& ui, RE::Main* main)
    {
        Context context;
        context.paused = ui.GameIsPaused();
        context.freeze = main ? static_cast<int>(main->GetRuntimeData().freezeTime) : -1;
        context.loadActive = g_load.load() == LoadState::Started;
        for (std::size_t i = 0; i < kMenus.size(); ++i) {
            if (ui.IsMenuOpen(kMenus[i])) {
                context.menus |= 1u << i;
            }
        }
        return context;
    }

    void Note(std::string message)
    {
        g_measurement.Note(std::move(message));
    }

    void ReportWriteResult(WriteStep step)
    {
        for (const auto candidate : kWritePriority) {
            const auto index = static_cast<std::size_t>(candidate);
            const auto mismatch = g_lastWriteThreadMismatch[index].load(std::memory_order_relaxed);
            if (mismatch.actual != 0) {
                Note(std::format("EngineWrite {}: skipped calls outside Main::threadID (last thread={}, Main::threadID={}).",
                    kCandidateNames[index], mismatch.actual, mismatch.expected));
            }
        }
        if (g_writer == Candidate::Count) {
            Note("EngineWrite not run: no installed candidate was observed on Main::threadID before selection ended.");
            return;
        }
        const auto reflected = [](const ValueTrial& trial) {
            return trial.restoredOnHook && trial.after < trial.before && trial.damage > 0.0f &&
                std::isfinite(trial.restored) && trial.restored + 0.1f >= trial.before;
        };
        const bool passed = step == WriteStep::Finished && reflected(g_magicka) && reflected(g_health) &&
            std::abs((g_magicka.before - g_magicka.after) - 1.0f) <= 0.01f;
        const bool attempted = g_magicka.before != 0.0f || g_health.before != 0.0f;
        g_measurement.SetWriteResult(passed ? WriteResult::Passed : attempted ? WriteResult::Failed : WriteResult::NotRun,
            g_writer);
        const auto* detail = step == WriteStep::Unavailable ? " (playable, living player state could not be confirmed)" :
            step != WriteStep::Finished ? " (eligible candidate calls did not finish in time)" : "";
        Note(std::format("EngineWrite {} {}{}: magicka before={} after={} restored={} hook_restore={}; "
                         "health before={} after={} restored={} hook_restore={}; DoDamage returned={}.",
            kCandidateNames[static_cast<std::size_t>(g_writer)], passed ? "passed" : attempted ? "failed" : "not run", detail,
            g_magicka.before, g_magicka.after, g_magicka.restored, g_magicka.restoredOnHook,
            g_health.before, g_health.after, g_health.restored, g_health.restoredOnHook, g_damageReturned));
    }

    bool StopWrite()
    {
        auto step = g_writeStep.load();
        do {
            if (step == WriteStep::Idle) {
                return true;
            }
            // A later control task can collect the result once the hook publishes it.
            if (step == WriteStep::InProgress) {
                return false;
            }
        } while (!g_writeStep.compare_exchange_weak(step, WriteStep::Idle));

        if ((step == WriteStep::RestoreMagicka || step == WriteStep::RestoreHealth) && g_load == LoadState::Succeeded) {
            auto* main = RE::Main::GetSingleton();
            const auto actualThread = REX::W32::GetCurrentThreadId();
            if (!main || main->threadID != actualThread) {
                Note(std::format("EngineWrite: partial rollback skipped on control thread {} (Main::threadID={}).",
                    actualThread, main ? main->threadID : 0));
            } else if (auto* player = RE::PlayerCharacter::GetSingleton(); player && !player->IsDead()) {
                // Undo partial damage if the next update did not arrive before the deadline.
                if (step == WriteStep::RestoreMagicka) {
                    RestoreDamage(g_magicka, *player, RE::ActorValue::kMagicka);
                } else {
                    RestoreDamage(g_health, *player, RE::ActorValue::kHealth);
                }
                Note("EngineWrite: restored partial damage from the control task.");
            }
        }
        ReportWriteResult(step);
        return true;
    }

    bool MenuMessage(std::string_view menu, RE::UI_MESSAGE_TYPE type)
    {
        if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
            queue->AddMessage(RE::BSFixedString(menu), type, nullptr);
            return true;
        }
        return false;
    }

    Menu StageMenu(Phase phase)
    {
        return phase == Phase::Journal ? Menu::Journal : Menu::Inventory;
    }

    std::string_view StageMenuName(Phase phase)
    {
        return kMenus[phase == Phase::Journal ? 0 : 1];
    }

    void Abort(std::string_view reason)
    {
        if (g_run.phase == Phase::EngineWrite && !StopWrite()) {
            g_run.abortReason = reason;
            return;
        }
        if (g_run.phase == Phase::Journal || g_run.phase == Phase::Inventory) {
            if (g_run.openedMenu && !g_run.requestedClose) {
                MenuMessage(StageMenuName(*g_run.phase), RE::UI_MESSAGE_TYPE::kHide);
            }
        }
        Note(std::format("Aborted: {}.", reason));
        g_measurement.Finish(RunResult::Aborted);
    }

    void RequestLoad()
    {
        auto* manager = RE::BGSSaveLoadManager::GetSingleton();
        if (!manager) {
            Abort("save/load manager unavailable");
            return;
        }
        g_load.store(LoadState::Requested);
        Note("Requested save load.");
        manager->Load(g_run.save.c_str());
    }

    void Enter(Phase phase, Context context, Clock::time_point now, PhaseResult previous = PhaseResult::Complete)
    {
        g_run.phase = phase;
        g_run.deadline = now + g_config.duration[static_cast<std::size_t>(phase)];
        if (phase == Phase::Baseline) {
            g_measurement.Start(context);
        } else {
            g_measurement.Next(phase, context, previous);
        }
        Note(std::format("Entered {}.", kPhaseNames[static_cast<std::size_t>(phase)]));

        if (phase == Phase::Journal || phase == Phase::Inventory) {
            g_run.openedMenu = g_run.requestedClose = false;
            g_run.deadline = now + g_config.menuTimeout;
            if (!MenuMessage(StageMenuName(phase), RE::UI_MESSAGE_TYPE::kShow)) {
                Note("UI message queue unavailable.");
                g_run.deadline = now;
            }
        } else if (phase == Phase::EngineWrite && SelectWriteCandidate(g_installed) != Candidate::Count &&
                   context.paused == 0 && context.freeze == 0) {
            g_run.writeSelectAt = now + kTick;
            g_writeStep.store(WriteStep::Observing);
        } else if (phase == Phase::Reload) {
            g_run.menuCloseDeadline = now + g_config.loadTimeout;
            RequestLoad();
        }
    }

    void TickMenu(Phase phase, Context context, Clock::time_point now)
    {
        const auto next = phase == Phase::Journal ? Phase::AfterJournal : Phase::EngineWrite;
        const bool open = (context.menus & StageMenu(phase)) != 0;
        if (g_run.requestedClose) {
            if (!open) {
                Note(std::format("{}: confirmed closed.", StageMenuName(phase)));
                Enter(next, context, now);
            } else if (now >= g_run.menuCloseDeadline) {
                Abort("test menu did not close");
            }
        } else if (!g_run.openedMenu) {
            if (open) {
                g_run.openedMenu = true;
                g_run.deadline = now + g_config.duration[static_cast<std::size_t>(phase)];
                Note(std::format("{}: confirmed open (paused={}).", StageMenuName(phase), context.paused));
            } else if (now >= g_run.deadline) {
                Note(std::format("{}: did not open.", StageMenuName(phase)));
                // Cancel any delayed show before proceeding.
                MenuMessage(StageMenuName(phase), RE::UI_MESSAGE_TYPE::kHide);
                Enter(next, context, now, PhaseResult::MenuNotOpened);
            }
        } else if (!open) {
            Abort("test menu closed before its hold time");
        } else if (now >= g_run.deadline) {
            g_run.requestedClose = true;
            g_run.menuCloseDeadline = now + g_config.menuTimeout;
            MenuMessage(StageMenuName(phase), RE::UI_MESSAGE_TYPE::kHide);
        }
    }

    void TickGame()
    {
        g_lastTick.store(Now());
        if (g_measurement.Result() != RunResult::Running) {
            if (g_run.phase == Phase::EngineWrite) {
                StopWrite();
            }
            return;
        }
        if (!g_run.abortReason.empty()) {
            Abort(g_run.abortReason);
            return;
        }
        auto* main = RE::Main::GetSingleton();
        g_measurement.SetMainThread(main ? main->threadID : 0);
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            Abort("UI unavailable");
            return;
        }
        const auto now = Clock::now();
        const auto context = ReadContext(*ui, main);
        g_measurement.SetContext(context);
        const bool mainMenu = (context.menus & Menu::MainMenu) != 0;
        const bool loading = (context.menus & Menu::Loading) != 0;
        const auto load = g_load.load();
        if (load == LoadState::Unexpected || load == LoadState::Failed) {
            Abort(load == LoadState::Failed ? "save load failed" : "unexpected game load");
            return;
        }

        if (!g_run.phase) {
            if (!g_run.requestedInitialLoad) {
                if (mainMenu) {
                    if (!g_run.mainMenuSince) {
                        g_run.mainMenuSince = now;
                    }
                    if (now >= *g_run.mainMenuSince + g_config.mainMenuDelay) {
                        g_run.requestedInitialLoad = true;
                        g_run.deadline = now + g_config.loadTimeout;
                        RequestLoad();
                    }
                } else if (now >= g_run.deadline) {
                    Abort("main menu did not open");
                }
                return;
            }
            if (load == LoadState::Succeeded && !mainMenu && !loading && context.paused == 0 && context.freeze == 0) {
                Enter(Phase::Baseline, context, now);
            } else if (now >= g_run.deadline) {
                Abort("initial save load timed out");
            }
            return;
        }

        const auto phase = *g_run.phase;
        if (phase != Phase::Reload && mainMenu) {
            Abort("returned to the main menu");
            return;
        }
        switch (phase) {
        case Phase::Baseline:
            if (now >= g_run.deadline) {
                Enter(Phase::Journal, context, now);
            }
            break;
        case Phase::Journal:
        case Phase::Inventory:
            TickMenu(phase, context, now);
            break;
        case Phase::AfterJournal:
            if (now >= g_run.deadline) {
                Enter(Phase::Inventory, context, now);
            }
            break;
        case Phase::EngineWrite:
            if (SelectWriteCandidate(g_installed) == Candidate::Count) {
                Note("EngineWrite skipped: none of A, C or B was installed.");
                Enter(Phase::Reload, context, now, PhaseResult::Skipped);
            } else if (now >= g_run.deadline) {
                if (g_writeStep == WriteStep::Idle) {
                    Note("EngineWrite: no writes armed in the paused or frozen context.");
                    g_measurement.SetWriteResult(WriteResult::NotRun);
                } else if (!StopWrite()) {
                    break;
                }
                Enter(Phase::Reload, context, now);
            } else if (g_writeStep == WriteStep::Observing && now >= g_run.writeSelectAt) {
                const auto observed = g_observedWriteCandidates.load(std::memory_order_relaxed);
                const auto candidate = SelectWriteCandidate(g_installed, observed);
                if (candidate != Candidate::Count) {
                    g_writer = candidate;
                    Note(std::format("EngineWrite selected {}.", kCandidateNames[static_cast<std::size_t>(candidate)]));
                    g_writeStep.store(WriteStep::DrainMagicka);
                }
            }
            break;
        case Phase::Reload:
            if (load == LoadState::Succeeded && !loading && !mainMenu && context.paused == 0 &&
                context.freeze == 0 && now >= g_run.deadline) {
                Enter(Phase::AfterReload, context, now);
            } else if (now >= g_run.menuCloseDeadline && (load != LoadState::Succeeded || loading || mainMenu ||
                           context.paused != 0 || context.freeze != 0)) {
                Abort("reload timed out");
            }
            break;
        case Phase::AfterReload:
            if (now >= g_run.deadline) {
                g_measurement.Finish(RunResult::Done);
            }
            break;
        default:
            break;
        }
    }

    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) noexcept override
        {
            if (event && g_measurement.Active()) {
                try {
                    const auto name = std::string_view(event->menuName.c_str());
                    const auto match = std::ranges::find(kMenus, name);
                    if (match != kMenus.end()) {
                        if (auto* ui = RE::UI::GetSingleton()) {
                            auto context = ReadContext(*ui, RE::Main::GetSingleton());
                            if (name == RE::LoadingMenu::MENU_NAME) {
                                // The event also covers a load screen between two AddTask executions.
                                const auto flag = 1u << (match - kMenus.begin());
                                context.menus = event->opening ? context.menus | flag : context.menus & ~flag;
                            }
                            g_measurement.SetContext(context);
                        }
                    }
                } catch (...) {
                    REX::W32::OutputDebugStringA("PoEProtoMainUpdate: menu observation failed.\n");
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    MenuSink g_menuSink;

    void Ticker() noexcept
    {
        try {
            const auto* tasks = SKSE::GetTaskInterface();
            auto nextSample = Clock::now() + g_config.interval;
            while (g_measurement.Result() == RunResult::Running) {
                const auto now = Clock::now();
                if (g_measurement.Active()) {
                    tasks->AddTask([]() noexcept { Capture(Candidate::D); });
                }
                if (!g_tickQueued.exchange(true)) {
                    tasks->AddTask([]() noexcept {
                        try {
                            TickGame();
                        } catch (...) {
                            try {
                                Abort("control task exception");
                            } catch (...) {
                                REX::W32::OutputDebugStringA("PoEProtoMainUpdate: control task failed.\n");
                            }
                        }
                        g_tickQueued.store(false);
                    });
                }
                if (now >= nextSample) {
                    g_measurement.Flush(g_log, g_tsv, true);
                    nextSample = now + g_config.interval;
                }
                if (Clock::duration(Now() - g_lastTick.load()) > std::max(g_config.loadTimeout, 10s)) {
                    Note("Aborted: SKSE control tasks stopped running.");
                    g_measurement.Finish(RunResult::Aborted);
                }
                std::this_thread::sleep_until(now + kTick);
            }
        } catch (const std::exception& error) {
            g_log << "Ticker failed: " << error.what() << '\n';
            try {
                g_measurement.Finish(RunResult::Aborted);
            } catch (...) {
                REX::W32::OutputDebugStringA("PoEProtoMainUpdate: could not finalize after ticker failure.\n");
            }
        }

        try {
            g_measurement.Flush(g_log, g_tsv, false);
            g_measurement.Summarize(g_log);
            if (!g_log || !g_tsv) {
                REX::W32::OutputDebugStringA("PoEProtoMainUpdate: could not finish writing the test records.\n");
                return;
            }
            if (g_config.quitWhenDone && g_measurement.Result() == RunResult::Done) {
                SKSE::GetTaskInterface()->AddTask([]() noexcept {
                    if (auto* main = RE::Main::GetSingleton()) {
                        main->GetRuntimeData().quitGame = true;
                    } else {
                        REX::W32::OutputDebugStringA("PoEProtoMainUpdate: Main singleton unavailable for game exit.\n");
                    }
                });
            }
        } catch (...) {
            REX::W32::OutputDebugStringA("PoEProtoMainUpdate: could not write the summary or queue game exit.\n");
        }
    }

    void LoadMessage(LoadState expected, LoadState next)
    {
        if (!g_load.compare_exchange_strong(expected, next)) {
            g_load.store(LoadState::Unexpected);
        }
        if (g_measurement.Active()) {
            if (auto* ui = RE::UI::GetSingleton()) {
                g_measurement.SetContext(ReadContext(*ui, RE::Main::GetSingleton()));
            }
        }
    }

    void OnMessage(SKSE::MessagingInterface::Message* message) noexcept
    {
        if (!message || !g_startAutotest) {
            return;
        }
        try {
            switch (message->type) {
            case SKSE::MessagingInterface::kDataLoaded: {
                auto* ui = RE::UI::GetSingleton();
                if (!ui) {
                    StartupLog("Aborted: UI unavailable at kDataLoaded.");
                    return;
                }
                auto* graphics = RE::BSGraphics::State::GetSingleton();
                g_measurement.Configure(graphics ? &graphics->GetFrameCount() : nullptr, g_updateCounter,
                    g_installed);
                ui->AddEventSink(&g_menuSink);
                g_run.deadline = Clock::now() + g_config.loadTimeout;
                g_lastTick.store(Now());
                std::thread(Ticker).detach();
                break;
            }
            case SKSE::MessagingInterface::kPreLoadGame:
                LoadMessage(LoadState::Requested, LoadState::Started);
                break;
            case SKSE::MessagingInterface::kPostLoadGame:
                LoadMessage(LoadState::Started, message->data ? LoadState::Succeeded : LoadState::Failed);
                break;
            case SKSE::MessagingInterface::kNewGame:
                g_load.store(LoadState::Unexpected);
                break;
            default:
                break;
            }
        } catch (...) {
            REX::W32::OutputDebugStringA("PoEProtoMainUpdate: SKSE message handling failed.\n");
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    try {
        const auto directory = SKSE::log::log_directory();
        if (!directory) {
            StartupLog("SKSE log directory unavailable.");
            return true;
        }
        const auto marker = *directory / kMarker;
        const bool markerExists = fs::exists(marker);
        if (markerExists) {
            g_log.open(*directory / "PoEProtoMainUpdate.log", std::ios::out | std::ios::trunc);
            g_tsv.open(*directory / "PoEProtoMainUpdate.tsv", std::ios::out | std::ios::trunc);
            if (!g_log || !g_tsv) {
                StartupLog("Aborted: could not open the log and TSV.");
                return true;
            }
            Measurement::WriteHeader(g_tsv);
            g_tsv.flush();
            g_startAutotest = ReadMarker(marker);
            if (!g_startAutotest) {
                return true;
            }
        }
        ReadConfig();
        if (!g_startAutotest && !g_config.installWithoutAutotest) {
            return true;
        }

        SKSE::Init(skse, { .log = false });
        const auto version = skse->RuntimeVersion();
        StartupLog(std::format("PoEProtoMainUpdate {} on runtime {}.", SKSE::GetPluginVersion().string("."),
            version.string(".")));
        if (version != kRuntime && version != SKSE::RUNTIME_SSE_1_5_97) {
            StartupLog("Unsupported runtime. No hooks installed.");
            return true;
        }
        const auto addressLibrary = fs::path("Data/SKSE/Plugins") /
            std::format("{}-{}.bin", version == kRuntime ? "versionlib" : "version", version.string());
        if (!fs::is_regular_file(addressLibrary)) {
            StartupLog("Address Library file unavailable. No hooks installed.");
            return true;
        }
        if (g_startAutotest) {
            const auto* messaging = SKSE::GetMessagingInterface();
            if (!messaging || !SKSE::GetTaskInterface() || !messaging->RegisterListener(OnMessage)) {
                StartupLog("Aborted: SKSE messaging or task interface unavailable.");
                return true;
            }
        }
        try {
            InstallHooks(version);
        } catch (const std::exception& error) {
            StartupLog(std::format("Hook installation interrupted: {}.", error.what()));
            g_installed[3] = true;
        }
    } catch (const std::exception& error) {
        StartupLog(std::format("Startup failed: {}.", error.what()));
    }
    return true;
}
