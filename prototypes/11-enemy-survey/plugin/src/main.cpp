#include "PCH.h"
#include "Survey.h"

#include "RE/A/AIProcess.h"
#include "RE/B/BGSSaveLoadManager.h"
#include "RE/C/ConcreteFormFactory.h"
#include "RE/F/FormTraits.h"
#include "RE/H/HitData.h"
#include "RE/I/INIPrefSettingCollection.h"
#include "RE/I/InventoryEntryData.h"
#include "RE/L/LoadingMenu.h"
#include "RE/M/Main.h"
#include "RE/M/MenuEventHandler.h"
#include "RE/M/MainMenu.h"
#include "RE/M/MenuOpenCloseEvent.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/P/ProcessLists.h"
#include "RE/S/Script.h"
#include "RE/S/SendHUDMessage.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESFile.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESRace.h"
#include "RE/U/UI.h"
#include "SKSE/SKSE.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <sys/stat.h>

namespace
{
    using Clock = std::chrono::steady_clock;
    namespace fs = std::filesystem;
    constexpr auto kTick = std::chrono::milliseconds(25);
    constexpr auto kPurgeWait = std::chrono::seconds(3);  // pcb 뒤 셀이 내려가기를 기다리는 시간
    constexpr auto kMainMenu = RE::MainMenu::MENU_NAME;
    constexpr auto kLoadingMenu = RE::LoadingMenu::MENU_NAME;
    constexpr auto kConfigPath = "Data/SKSE/Plugins/PoEProtoSurvey.ini";
    constexpr std::array kAutosaveNames = {
        "fAutosaveEveryXMins:SaveGame"sv, "bSaveOnTravel:SaveGame"sv, "bSaveOnWait:SaveGame"sv,
        "bSaveOnRest:SaveGame"sv, "bSaveOnCharacterMenu:SaveGame"sv
    };

    enum class Phase { Idle, MenuDelay, AwaitLoad, StartDelay, Level, VisitStart, VisitWait, Finished };
    enum class LoadState { None, Requested, Started, Success, Failed };

    struct SavedSetting
    {
        RE::Setting* setting;
        std::string_view name;
        std::string_view collection;
        std::variant<bool, float> original;

        void Disable() const
        {
            if (std::holds_alternative<bool>(original)) {
                setting->SetBool(false);
            } else {
                setting->SetFloat(0);
            }
        }

        void Restore() const
        {
            if (const auto* flag = std::get_if<bool>(&original)) {
                setting->SetBool(*flag);
            } else {
                setting->SetFloat(std::get<float>(original));
            }
        }
    };

    class Record
    {
    public:
        ~Record() { Close(); }

        void Open(const fs::path& directory)
        {
            fs::create_directories(directory);
            auto time = std::time(nullptr);
            std::tm local{};
            if (localtime_s(&local, &time) != 0) {
                throw std::runtime_error("기록 파일 시각을 만들지 못함");
            }
            std::array<char, 16> stamp{};
            if (std::strftime(stamp.data(), stamp.size(), "%Y%m%d-%H%M%S", &local) == 0) {
                throw std::runtime_error("기록 파일 시각 서식 실패");
            }
            const auto path = Survey::UniqueLogPath(directory, stamp.data(), [&](const fs::path& candidate) {
                const auto error = _wsopen_s(&fd_, candidate.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
                    _SH_DENYWR, _S_IREAD | _S_IWRITE);
                if (error == EEXIST) {
                    return true;
                }
                if (error != 0) {
                    throw std::system_error(error, std::generic_category(), "기록 파일 열기 실패");
                }
                return false;
            });
            Write(Survey::kHeader);
            SKSE::log::info("기록 파일 {}", path.filename().string());
        }

        void Write(std::string_view row) const
        {
            if (fd_ == -1) {
                return;
            }
            const auto written = _write(fd_, row.data(), static_cast<unsigned>(row.size()));
            if (written < 0 || static_cast<std::size_t>(written) != row.size() || _commit(fd_) != 0) {
                throw std::runtime_error("기록 파일 쓰기 실패");
            }
        }

        void Close() noexcept
        {
            if (fd_ != -1) {
                _close(std::exchange(fd_, -1));
            }
        }

    private:
        int fd_ = -1;
    };

    struct Session
    {
        Record record;
        Phase phase = Phase::Idle;
        std::string save;
        Clock::time_point start{};
        Clock::time_point deadline{};
        Clock::time_point visitStart{};
        std::optional<Clock::time_point> readySince;
        std::optional<Clock::time_point> purgedAt;
        std::vector<RE::FormID> cellIds;
        std::map<RE::FormID, std::pair<RE::TESObjectCELL*, std::string_view>> cells;
        std::deque<RE::FormID> visits;
        std::size_t nextLevel = 0;
        int level = 0;
        bool loadRequested = false;
        std::size_t reloadedFor = 0;  // 세이브를 다시 불러온 단계 번호. 첫 단계는 첫 불러오기로 친다
        std::vector<SavedSetting> savedSettings;
        bool enabledGodMode = false;
        bool disabledDetection = false;
    };

    Survey::Config g_config;
    Session g_run;
    fs::path g_logDirectory;
    REL::Version g_runtime;

    // 이 표지만 콜백과 틱 스레드에서 공유한다.
    std::atomic<bool> g_firstMenu = false;
    std::atomic<bool> g_menuPending = false;
    std::atomic<bool> g_active = false;
    std::atomic<bool> g_tickerDone = false;
    std::atomic<bool> g_tickQueued = false;
    std::atomic<bool> g_unexpectedLoad = false;
    std::atomic<bool> g_menuReturned = false;
    std::atomic<LoadState> g_load = LoadState::None;
    std::atomic<bool> g_loadedOnce = false;  // 세이브를 한 번이라도 불러왔으면 그 뒤 주 메뉴가 열리는 것은 중단 사유

    template <class Action>
    bool TryRun(std::string_view label, Action action) noexcept
    {
        try {
            action();
            return true;
        } catch (const std::exception& error) {
            try {
                SKSE::log::error("{} 처리 중 예외: {}", label, error.what());
            } catch (...) {
            }
        } catch (...) {
            try {
                SKSE::log::error("{} 처리 중 예외", label);
            } catch (...) {
            }
        }
        return false;
    }

    auto Millis(Clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    }

    template <class... Args>
    void State(std::format_string<Args...> format, Args&&... args)
    {
        g_run.record.Write(Survey::Row('S', Millis(g_run.start), std::format(format, std::forward<Args>(args)...)));
    }

    void Notify(std::string_view message)
    {
        if (g_config.notify) {
            const auto text = std::format("[PoE Proto 11] {}", message);
            RE::SendHUDMessage::ShowHUDMessage(text.c_str());
        }
    }

    void LogSetting(const SavedSetting& saved, bool restore)
    {
        const auto original = std::visit([](auto value) { return std::format("{}", value); }, saved.original);
        const auto action = restore ? "restored" : "disabled";
        const auto value = restore ? original : (std::holds_alternative<bool>(saved.original) ? "false" : "0");
        State("setting\t{}\t{}\t{}\t{}\t{}", saved.name, saved.collection, original, action, value);
        SKSE::log::info("자동 저장 {} {} {} → {} ({})", saved.name, saved.collection, original, value, action);
    }

    void DisableAutosaves()
    {
        const std::array collections = {
            std::pair<RE::INISettingCollection*, std::string_view>{ RE::INIPrefSettingCollection::GetSingleton(), "INIPrefs"sv },
            std::pair{ RE::INISettingCollection::GetSingleton(), "INI"sv }
        };
        g_run.savedSettings.reserve(collections.size() * kAutosaveNames.size());
        for (const auto& name : kAutosaveNames) {
            for (const auto& [collection, label] : collections) {
                auto* setting = collection ? collection->GetSetting(name) : nullptr;
                if (!setting) {
                    State("setting\t{}\t{}\t-\tmissing\t-", name, label);
                    SKSE::log::info("자동 저장 설정 없음: {} {}", name, label);
                    continue;
                }
                if (std::ranges::find(g_run.savedSettings, setting, &SavedSetting::setting) != g_run.savedSettings.end()) {
                    SKSE::log::info("자동 저장 설정이 다른 컬렉션과 같음: {} {}", name, label);
                    continue;
                }
                auto& saved = g_run.savedSettings.emplace_back(setting, name, label,
                    name.starts_with('b') ? std::variant<bool, float>(setting->GetBool()) : std::variant<bool, float>(setting->GetFloat()));
                LogSetting(saved, false);
                saved.Disable();
            }
        }
    }

    void Finish(std::string_view reason = {}) noexcept
    {
        if (g_run.phase == Phase::Idle || g_run.phase == Phase::Finished) {
            g_tickerDone = true;
            return;
        }
        g_run.phase = Phase::Finished;
        const bool aborted = !reason.empty();
        for (const auto& saved : g_run.savedSettings) {
            if (TryRun("자동 저장 설정 복원", [&] { saved.Restore(); })) {
                TryRun("자동 저장 복원 기록", [&] { LogSetting(saved, true); });
            }
        }
        if (g_run.enabledGodMode) {
            TryRun("신 모드 복원", [] {
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    player->SetGodMode(false);
                }
            });
        }
        if (g_run.disabledDetection) {
            TryRun("AI 감지 복원", [] {
                if (auto* processes = RE::ProcessLists::GetSingleton()) {
                    processes->runDetection = true;
                }
            });
        }
        TryRun("종료 기록", [&] {
            if (aborted) {
                State("ABORT\t{}", Survey::Clean(reason));
                SKSE::log::warn("ABORT {}", reason);
            } else {
                State("DONE");
                SKSE::log::info("DONE");
            }
        });
        g_run.record.Close();
        TryRun("종료 알림", [&] { Notify(aborted ? std::format("조사 중단 ({})", reason) : "조사 끝"); });
        if (g_run.loadRequested && (aborted || g_config.quitWhenDone)) {
            TryRun("게임 종료", [] {
                if (auto* main = RE::Main::GetSingleton()) {
                    main->GetRuntimeData().quitGame = true;
                } else {
                    SKSE::log::error("게임 종료 플래그를 찾지 못함");
                }
            });
        }
        g_active = false;
        g_tickerDone = true;
    }

    void RunConsole(RE::PlayerCharacter& player, const std::string& command)
    {
        const std::unique_ptr<RE::Script> script(RE::IFormFactory::Create<RE::Script>());
        if (!script) {
            throw std::runtime_error("콘솔 스크립트 팩토리를 찾지 못함");
        }
        SKSE::log::info("콘솔 {}", command);
        script->SetCommand(command);
        script->CompileAndRun(&player);
    }

    bool PreventCombat(RE::PlayerCharacter& player)
    {
        const bool godBefore = player.IsGodMode();
        if (!godBefore) {
            g_run.enabledGodMode = true;
            RunConsole(player, "tgm");
        }
        const bool godAfter = player.IsGodMode();
        SKSE::log::info("신 모드 {} → {}", godBefore, godAfter);

        auto* processes = RE::ProcessLists::GetSingleton();
        if (!processes) {
            // 상태를 읽을 수 없으면 토글하지 않는다. 다시 불러올 때마다 토글하면 감지가 도로 켜진다.
            State("detection-still-on");
            SKSE::log::warn("ProcessLists를 찾지 못해 AI 감지를 끄지 않음. 신 모드로 계속함");
            return godAfter;
        }
        const bool detectionBefore = processes->runDetection;
        if (detectionBefore) {
            g_run.disabledDetection = true;
            RunConsole(player, "tdetect");
        }
        SKSE::log::info("AI 감지 {} → {}", detectionBefore, processes->runDetection);
        if (processes->runDetection) {
            // 감지가 남아도 신 모드면 측정은 할 수 있다. 동료나 적이 싸울 수 있다는 것만 기록한다.
            State("detection-still-on");
            SKSE::log::warn("tdetect 뒤에도 AI 감지가 켜져 있음. 신 모드로 계속함");
        }
        return godAfter;
    }

    void ResolveCells()
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data) {
            throw std::runtime_error("데이터 핸들러를 찾지 못함");
        }
        for (const auto& spec : g_config.cells) {
            auto* cell = data->LookupForm<RE::TESObjectCELL>(spec.localId, spec.plugin);
            if (!cell || !cell->IsInteriorCell()) {
                SKSE::log::warn("셀 제외 {} {:06X} {}: {}", spec.editorId, spec.localId, spec.plugin,
                    cell ? "실내 셀이 아님" : "셀을 찾지 못함");
                continue;
            }
            const auto* editor = cell->GetFormEditorID();
            if (editor && *editor && _stricmp(editor, spec.editorId.c_str()) != 0) {
                SKSE::log::warn("셀 제외 {} {:06X} {}: 실제 EditorID는 {}", spec.editorId, spec.localId, spec.plugin, editor);
                continue;
            }
            const auto id = cell->GetFormID();
            if (!g_run.cells.try_emplace(id, cell, spec.editorId).second) {
                SKSE::log::warn("셀 제외 {} {:08X}: 이미 목록에 있음", spec.editorId, id);
                continue;
            }
            g_run.cellIds.push_back(id);
        }
    }

    void ReadAutoMarker()
    {
        const auto marker = g_logDirectory / "PoEProtoSurvey.autotest";
        if (!fs::exists(marker)) {
            g_tickerDone = true;
            return;
        }
        std::array<char, 1024> buffer{};
        std::ifstream input(marker, std::ios::binary);
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto size = static_cast<std::size_t>(input.gcount());
        const bool read = input.eof() && !input.bad();
        input.close();
        if (!fs::remove(marker)) {
            SKSE::log::error("조사 표지 파일을 지우지 못함");
            g_tickerDone = true;
            return;
        }
        auto save = read ? Survey::ParseMarker({ buffer.data(), size }) : std::nullopt;
        if (!save) {
            SKSE::log::error("조사 표지 파일 읽기 실패 또는 세이브 이름 형식 오류");
            g_tickerDone = true;
            return;
        }
        g_run.phase = Phase::MenuDelay;
        g_run.start = Clock::now();
        g_run.save = std::move(*save);
        g_active = true;
        g_run.record.Open(g_logDirectory / "PoEProtoSurvey");
        State("start\t{}\t{}\t{}", SKSE::GetPluginVersion().string("."), g_runtime.string("."), Survey::Clean(g_run.save));
        ResolveCells();
        std::string levels;
        for (int level : g_config.levels) {
            if (!levels.empty()) {
                levels += ',';
            }
            levels += std::to_string(level);
        }
        State("config\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", Survey::Clean(levels), g_run.cellIds.size(),
            g_config.mainMenuDelay.count(), g_config.startDelay.count(), g_config.settle.count(), g_config.cellTimeout.count(),
            g_config.quitWhenDone, g_config.notify);
        if (!g_config.CanRun() || g_run.cellIds.size() < 2) {
            Finish("invalid-config");
            return;
        }
        g_run.deadline = Clock::now() + g_config.mainMenuDelay;
        SKSE::log::info("조사 준비: 세이브 {}, 레벨 {}, 셀 {}개", g_run.save, levels, g_run.cellIds.size());
    }

    // HitData를 만들지 못하면 physical에 -1을 적어 "엔진이 0을 채움"과 구별한다.
    struct HitProbe
    {
        float physical = -1;
        float total = 0;
        float blocked = 0;
        std::uint32_t flags = 0;
    };

    // 공격자가 오른손 장비 항목으로 대상을 쳤다고 할 때 엔진이 채우는 HitData를 만들어 읽고 버린다.
    // Populate는 값만 채우고 피해는 주지 않는다(피해는 38586이 적용). total ÷ physical이 대상의 방어구·막기 비율이다.
    HitProbe ProbeHit(RE::Actor& aggressor, RE::Actor& target, RE::InventoryEntryData* weapon)
    {
        auto* hit = RE::HitData::Create(&aggressor, &target, weapon, false);
        if (!hit) {
            return {};
        }
        const HitProbe probe{ hit->physicalDamage, hit->totalDamage, hit->percentBlocked, hit->flags.underlying() };
        hit->attackData.reset();
        RE::free(hit);
        return probe;
    }

    // 적이 플레이어를 쳤다고 할 때의 HitData. 조사 중에는 신 모드(tgm)를 켜 두는데, 신 모드에서는 이 값이 모두 0이었다
    // (2026-11-19 5차 첫 실행, 적 115명). 그래서 시험 타격 동안만 신 모드 플래그(PlayerCharacter::IsGodMode가 읽는 전역)를 끄고
    // 곧바로 되돌린다. 같은 주 스레드 작업 안이라 그 사이에 프레임이 지나지 않고, Populate는 피해를 적용하지 않는다.
    HitProbe ProbeEnemyHit(RE::Actor& enemy, RE::PlayerCharacter& player, RE::InventoryEntryData* weapon)
    {
        // 예외로 빠져나가도 원래 값으로 돌아가게 소멸자에서 되돌린다.
        struct GodModeOff
        {
            bool& flag;
            const bool saved;
            explicit GodModeOff(bool& a_flag) : flag(a_flag), saved(a_flag) { flag = false; }
            ~GodModeOff() { flag = saved; }
            GodModeOff(const GodModeOff&) = delete;
            GodModeOff& operator=(const GodModeOff&) = delete;
        };
        static REL::Relocation<bool*> godMode{ RELOCATION_ID(517711, 404238) };
        const GodModeOff off{ *godMode.get() };
        return ProbeHit(enemy, player, weapon);
    }

    // 액터의 오른손 장비 항목과 무기 종류. 중간·높은 처리 단계가 없으면 장비를 알 수 없어 종류 -2,
    // 항목이 없거나(맨손·생물의 기본 공격) 무기가 아니면 -1.
    struct Wielded
    {
        RE::InventoryEntryData* entry = nullptr;
        std::uint32_t form = 0;
        int type = -1;
    };

    Wielded RightHand(const RE::Actor& actor)
    {
        const auto* process = actor.GetActorRuntimeData().currentProcess;
        if (!process || !process->middleHigh) {
            return { nullptr, 0, -2 };
        }
        auto* entry = actor.GetEquippedEntryData(false);
        const auto* object = entry ? entry->GetObject() : nullptr;
        const auto* weapon = object ? object->As<RE::TESObjectWEAP>() : nullptr;
        return { entry, object ? object->GetFormID() : 0, weapon ? static_cast<int>(weapon->GetWeaponType()) : -1 };
    }

    std::size_t ReadActors(RE::TESObjectCELL& cell, std::string_view editor, RE::PlayerCharacter& player)
    {
        std::vector<RE::NiPointer<RE::Actor>> actors;
        // 셀 잠금 안에서는 참조를 보관하기만 한다.
        cell.ForEachReference([&](RE::TESObjectREFR* ref) {
            if (ref != &player && ref->IsActor()) {
                actors.emplace_back(static_cast<RE::Actor*>(ref));
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        for (const auto& actor : actors) {
            const auto* base = actor->GetActorBase();
            const auto* root = base ? base->GetRootFaceNPC() : nullptr;
            const auto* file = root ? root->GetFile(0) : nullptr;
            const auto* race = actor->GetRace();
            const auto* values = actor->AsActorValueOwner();
            std::string flags;
            if (actor->IsHostileToActor(&player)) flags += 'H';
            if (actor->IsPlayerTeammate()) flags += 'T';
            if (actor->IsSummoned()) flags += 'S';
            if (actor->IsEssential()) flags += 'E';
            if (actor->IsProtected()) flags += 'P';
            if (base && base->IsUnique()) flags += 'U';
            if (actor->IsDead()) flags += 'D';
            if (actor->IsDisabled()) flags += 'X';
            const auto* name = base ? base->GetName() : nullptr;
            const auto probe = ProbeHit(player, *actor, player.GetEquippedEntryData(false));
            const auto wielded = RightHand(*actor);
            const auto out = ProbeEnemyHit(*actor, player, wielded.entry);
            g_run.record.Write(Survey::ActorRow(Millis(g_run.start), g_run.level, editor, {
                actor->GetFormID(), base ? base->GetFormID() : 0, root ? root->GetFormID() : 0,
                file ? file->GetFilename() : ""sv, name ? name : ""sv, race ? race->GetFormID() : 0,
                actor->GetLevel(), actor->GetActorValueMax(RE::ActorValue::kHealth),
                values->GetBaseActorValue(RE::ActorValue::kHealth), values->GetActorValue(RE::ActorValue::kHealth),
                values->GetActorValue(RE::ActorValue::kAggression), flags,
                values->GetActorValue(RE::ActorValue::kDamageResist), probe.physical, probe.total, probe.blocked,
                wielded.form, wielded.type, out.physical, out.total, out.flags
            }));
        }
        return actors.size();
    }

    void EndVisit(std::string_view outcome, std::int64_t entryMs, std::size_t actors)
    {
        const auto id = g_run.visits.front();
        const auto editor = g_run.cells.at(id).second;
        g_run.record.Write(Survey::VisitRow(Millis(g_run.start), g_run.level, editor, id, outcome, entryMs, actors));
        SKSE::log::info("레벨 {} 셀 {} {} (입장 {}ms, 액터 {}명)", g_run.level, editor, outcome, entryMs, actors);
        g_run.visits.pop_front();
        g_run.deadline = {};
        g_run.readySince.reset();
        g_run.phase = g_run.visits.empty() ? Phase::Level : Phase::VisitStart;
    }

    void StartVisit(RE::PlayerCharacter& player, bool loading, Clock::time_point now)
    {
        const auto* current = player.GetParentCell();
        const auto first = g_run.visits.front();
        if (!Survey::DeferCurrentCell(g_run.visits, current ? current->GetFormID() : 0)) {
            EndVisit("skipped", -1, 0);
            return;
        }
        const auto id = g_run.visits.front();
        if (id != first) {
            SKSE::log::info("레벨 {} 현재 셀 {}을 맨 뒤로 미룸", g_run.level, g_run.cells.at(first).second);
            g_run.deadline = {};
        }
        const auto& [cell, editor] = g_run.cells.at(id);
        if (g_run.deadline == Clock::time_point{}) {
            g_run.deadline = now + g_config.cellTimeout;
            g_run.purgedAt.reset();
        }
        if (loading) {
            if (now >= g_run.deadline) {
                EndVisit("timeout", g_config.cellTimeout.count() * 1000, 0);
            }
            return;
        }
        if (const auto state = cell->cellState.get(); state != RE::TESObjectCELL::CellState::kUnloaded) {
            // 앞서 들른 셀이 셀 버퍼에 남아 있으면 한 번 비우고, 그래도 남으면 상태를 적고 진행한다.
            if (!g_run.purgedAt) {
                g_run.purgedAt = now;
                RunConsole(player, "pcb");
                return;
            }
            if (now - *g_run.purgedAt < kPurgeWait) {
                return;
            }
            State("cell-not-unloaded\t{}\t{}\t{}", g_run.level, editor, std::to_underlying(state));
            SKSE::log::warn("레벨 {} 셀 {}이 버퍼를 비운 뒤에도 상태 {}. 그대로 초기화하고 들어감", g_run.level, editor,
                std::to_underlying(state));
        }
        RunConsole(player, std::format("resetinterior {}", editor));
        g_run.visitStart = Clock::now();
        g_run.deadline = g_run.visitStart + g_config.cellTimeout;
        g_run.phase = Phase::VisitWait;
        RunConsole(player, std::format("coc {}", editor));
    }

    void WaitForVisit(RE::PlayerCharacter& player, bool loading, Clock::time_point now)
    {
        const auto& [cell, editor] = g_run.cells.at(g_run.visits.front());
        if (!loading && player.GetParentCell() == cell && cell->IsAttached()) {
            if (!g_run.readySince && now <= g_run.deadline) {
                g_run.readySince = now;
            }
            if (g_run.readySince && now - *g_run.readySince >= g_config.settle) {
                const auto entry = std::chrono::duration_cast<std::chrono::milliseconds>(*g_run.readySince - g_run.visitStart).count();
                EndVisit("ok", entry, ReadActors(*cell, editor, player));
                return;
            }
        } else {
            g_run.readySince.reset();
        }
        if (!g_run.readySince && now >= g_run.deadline) {
            EndVisit("timeout", Millis(g_run.visitStart), 0);
        }
    }

    void ChangeLevel(RE::PlayerCharacter& player)
    {
        g_run.deadline = {};
        if (g_run.nextLevel == g_config.levels.size()) {
            Finish();
            return;
        }
        const auto level = g_config.levels[g_run.nextLevel++];
        RunConsole(player, std::format("player.setlevel {}", level));
        const int actual = player.GetLevel();
        g_run.record.Write(Survey::LevelRow(Millis(g_run.start), level, actual, player.GetActorValueMax(RE::ActorValue::kHealth)));
        SKSE::log::info("플레이어 레벨 {} 요청, 실제 {}", level, actual);
        if (actual != level) {
            SKSE::log::warn("플레이어 레벨 불일치로 {}단계 건너뜀", level);
            for (const auto id : g_run.cellIds) {
                g_run.record.Write(Survey::VisitRow(Millis(g_run.start), level, g_run.cells.at(id).second, id, "skipped", -1, 0));
            }
            return;
        }
        g_run.level = level;
        // 시험 타격(ProbeHit)이 넘기는 것과 같은 항목을 적는다. 오른손이 비었거나 주문이면 항목이 없어 0이다.
        const auto* entry = player.GetEquippedEntryData(false);
        const auto* weapon = entry ? entry->GetObject() : nullptr;
        const auto* weaponName = weapon ? weapon->GetName() : nullptr;
        State("hit-weapon\t{}\t{:08X}\t{}", level, weapon ? weapon->GetFormID() : 0,
            Survey::Clean(weaponName ? weaponName : ""));
        Notify(std::format("플레이어 레벨 {}", level));
        g_run.visits.assign(g_run.cellIds.begin(), g_run.cellIds.end());
        g_run.phase = Phase::VisitStart;
    }

    void RequestLoad(RE::BGSSaveLoadManager& manager)
    {
        g_run.phase = Phase::AwaitLoad;
        g_run.deadline = Clock::now() + g_config.cellTimeout;
        g_run.loadRequested = true;
        g_load = LoadState::Requested;
        SKSE::log::info("세이브 불러오기 요청 {}", g_run.save);
        manager.Load(g_run.save.c_str());
    }

    void Tick()
    {
        if (g_menuPending.exchange(false)) {
            ReadAutoMarker();
        }
        if (!g_active) {
            return;
        }
        if (g_unexpectedLoad) {
            Finish("unexpected-game-load");
            return;
        }
        if (g_menuReturned) {
            Finish("main-menu-returned");
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            Finish("ui-missing");
            return;
        }
        const bool mainMenu = ui->IsMenuOpen(kMainMenu);
        const bool loading = ui->IsMenuOpen(kLoadingMenu);
        const auto now = Clock::now();
        if (g_run.phase == Phase::MenuDelay) {
            if (!mainMenu) {
                Finish("main-menu-closed");
            } else if (now >= g_run.deadline) {
                auto* manager = RE::BGSSaveLoadManager::GetSingleton();
                if (!manager) {
                    Finish("save-load-manager-missing");
                    return;
                }
                DisableAutosaves();
                RequestLoad(*manager);
            }
            return;
        }
        if (g_run.phase == Phase::AwaitLoad) {
            if (g_load == LoadState::Success && !mainMenu && !loading) {
                g_run.phase = Phase::StartDelay;
                g_run.deadline = now + g_config.startDelay;
                for (const auto& saved : g_run.savedSettings) {
                    saved.Disable();
                }
                SKSE::log::info("세이브 불러오기 완료");
            } else if (g_load == LoadState::Failed || now >= g_run.deadline) {
                Finish(g_load == LoadState::Failed ? "load-failed" : "load-timeout");
            }
            return;
        }
        if (mainMenu) {
            Finish("main-menu-returned");
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || player->IsDead()) {
            Finish(player ? "player-dead" : "player-missing");
            return;
        }
        switch (g_run.phase) {
        case Phase::StartDelay:
            if (!loading && now >= g_run.deadline) {
                if (!PreventCombat(*player)) {
                    Finish("combat-toggle-failed");
                    return;
                }
                Notify(g_run.nextLevel == 0 ? "조사 시작" : "다시 불러옴");
                g_run.deadline = {};
                g_run.phase = Phase::Level;
            } else if (loading && now >= g_run.deadline + g_config.cellTimeout) {
                Finish("loading-stuck");
            }
            break;
        case Phase::Level:
            if (!loading && g_run.nextLevel > 0 && g_run.nextLevel < g_config.levels.size() &&
                g_run.reloadedFor != g_run.nextLevel) {
                // 조우 구역의 레벨은 처음 들어갈 때 정해져 resetinterior로 풀리지 않는다(첫 실행, 2026-11-18).
                // 단계마다 세이브를 다시 불러와 모든 셀을 처음 들어가는 상태로 만든다.
                auto* manager = RE::BGSSaveLoadManager::GetSingleton();
                if (!manager) {
                    Finish("save-load-manager-missing");
                    return;
                }
                g_run.reloadedFor = g_run.nextLevel;
                State("reload\t{}", g_config.levels[g_run.nextLevel]);
                RequestLoad(*manager);
            } else if (!loading) {
                ChangeLevel(*player);
            } else {
                if (g_run.deadline == Clock::time_point{}) {
                    g_run.deadline = now + g_config.cellTimeout;
                }
                if (now >= g_run.deadline) {
                    Finish("loading-stuck");
                }
            }
            break;
        case Phase::VisitStart:
            StartVisit(*player, loading, now);
            break;
        case Phase::VisitWait:
            WaitForVisit(*player, loading, now);
            break;
        default:
            break;
        }
    }

    void MenuOpened()
    {
        if (!g_firstMenu.exchange(true)) {
            g_menuPending = true;
        } else if (g_active && g_loadedOnce) {
            g_menuReturned = true;
        }
    }

    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) noexcept override
        {
            TryRun("메뉴 이벤트", [&] {
                if (event && event->opening && event->menuName == kMainMenu) {
                    MenuOpened();
                }
            });
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    MenuSink g_menuSink;

    // SKSE 작업 안에서는 다음 작업을 맡기지 않는다.
    void Ticker() noexcept
    {
        while (!g_tickerDone) {
            std::this_thread::sleep_for(kTick);
            TryRun("틱 예약", [] {
                if (g_tickerDone || (!g_menuPending && !g_active) || g_tickQueued.exchange(true)) {
                    return;
                }
                try {
                    SKSE::GetTaskInterface()->AddTask([]() noexcept {
                        if (!TryRun("조사 틱", Tick)) {
                            Finish("exception");
                        }
                        g_tickQueued = false;
                    });
                } catch (...) {
                    g_tickQueued = false;
                    throw;
                }
            });
        }
    }

    void OnMessage(SKSE::MessagingInterface::Message* message) noexcept
    {
        if (!message) {
            return;
        }
        TryRun("SKSE 메시지", [&] {
            switch (message->type) {
            case SKSE::MessagingInterface::kDataLoaded:
                if (auto* ui = RE::UI::GetSingleton()) {
                    ui->AddEventSink(&g_menuSink);
                    if (ui->IsMenuOpen(kMainMenu)) {
                        MenuOpened();
                    }
                    std::thread(Ticker).detach();
                } else {
                    SKSE::log::error("UI를 찾지 못함. 조사 표지를 읽을 수 없음");
                }
                break;
            case SKSE::MessagingInterface::kPreLoadGame:
                if (g_active) {
                    auto expected = LoadState::Requested;
                    if (!g_load.compare_exchange_strong(expected, LoadState::Started)) {
                        g_unexpectedLoad = true;
                    }
                }
                break;
            case SKSE::MessagingInterface::kPostLoadGame:
                if (g_active) {
                    auto expected = LoadState::Started;
                    if (!g_load.compare_exchange_strong(expected, message->data ? LoadState::Success : LoadState::Failed)) {
                        g_unexpectedLoad = true;
                    } else if (message->data) {
                        g_loadedOnce = true;
                    }
                }
                break;
            case SKSE::MessagingInterface::kNewGame:
                if (g_active) {
                    g_unexpectedLoad = true;
                }
                break;
            default:
                break;
            }
        });
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    bool ready = false;
    TryRun("플러그인 시작", [&] {
        const auto directory = SKSE::log::log_directory();
        if (!directory) {
            return;
        }
        g_logDirectory = *directory;
        fs::create_directories(g_logDirectory);
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((g_logDirectory / "PoEProtoSurvey.log").string(), true);
        auto logger = std::make_shared<spdlog::logger>("poe_proto_survey", std::move(sink));
        spdlog::set_default_logger(std::move(logger));
        spdlog::flush_on(spdlog::level::info);
        SKSE::Init(skse, { .log = false });
        g_runtime = skse->RuntimeVersion();
        std::ifstream config(kConfigPath, std::ios::binary);
        if (config) {
            g_config = Survey::ReadConfig(config, [](std::string_view warning) { SKSE::log::warn("[Survey] {}", warning); });
        } else {
            SKSE::log::error("설정 파일을 열지 못함: {}", kConfigPath);
        }
        const auto* messaging = SKSE::GetMessagingInterface();
        if (!SKSE::GetTaskInterface() || !messaging || !messaging->RegisterListener(OnMessage)) {
            SKSE::log::error("SKSE 작업 또는 메시지 인터페이스를 사용할 수 없음");
            return;
        }
        SKSE::log::info("PoEProtoSurvey {} 준비 완료. 런타임 {}", SKSE::GetPluginVersion().string("."), g_runtime.string("."));
        ready = true;
    });
    return ready;
}
