// 프로토타입 14: 지도 팩 실험(6단계 재작업 실험 단계 E). README 참고.
// 표지 파일이 있을 때만 동작한다. 세이브를 불러와 G-3 → G-1 → G-2 → G-4·G-7 → G-6 순서로 시험하고 기록한 뒤 게임을 끈다.
// 모든 엔진 작업은 SKSE 작업 큐(주 스레드)에서 한다. 같은 작업 안에서 Disable()과 SetDelete(true)를 잇달아 부르지 않는다.
#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;

    constexpr auto kTick = 100ms;
    constexpr auto kConfigPath = "Data/SKSE/Plugins/PoEProtoMapExperiment.ini";
    constexpr auto kManifestPath = "Data/SkyrimPoE_Maps.json";
    constexpr auto kMarker = "PoEProtoMapExperiment.autotest";
    constexpr std::string_view kPlugin = "SkyrimPoE_Maps.esp";

    // Skyrim.esm 번호(실험 고정값, README)
    constexpr RE::FormID kSpawnTemplate = 0x03DECB;    // LvlBanditMelee1H
    constexpr RE::FormID kEmbershardOutside = 0x0B6CCE; // Embershard 바깥 입구 문(영구)
    constexpr RE::FormID kEmbershardResident = 0x0B6FF7;
    constexpr std::array<std::pair<RE::FormID, std::string_view>, 2> kQuests{ { { 0x0B6FEF, "dunEmbershardQST" }, { 0x06CD54, "dunIronbindQST" } } };

    constexpr float kArriveMarkerDistance = 128.0f;
    constexpr float kReturnDistance = 256.0f;
    constexpr float kMovedDistance = 64.0f;
    constexpr float kReachedDistance = 400.0f;
    constexpr float kFallDrop = 300.0f;
    constexpr std::size_t kSpawnPerCycle = 6;
    constexpr std::size_t kPursuitCap = 20;

    struct Config
    {
        bool quitWhenDone = true;
        int cycles = 5;
        std::string cleanupDungeon = "embershard";
        std::string resetDungeon = "pinemoon";
        std::string tourDungeon;
        std::chrono::seconds mainMenuDelay = 2s;
        std::chrono::seconds loadTimeout = 60s;
        std::chrono::seconds arriveTimeout = 40s;
        std::chrono::seconds detachTimeout = 20s;
        std::chrono::seconds pursuit = 25s;
    };

    struct AutoExit
    {
        RE::FormID local = 0;
        RE::NiPoint3 pos;
        float rotZ = 0.0f;  // 라디안(목록 파일의 rotation[2])
    };

    struct Dungeon
    {
        std::string key;
        std::string role;
        RE::FormID cellLocal = 0;
        RE::FormID entryLocal = 0;
        RE::FormID bossLocal = 0;
        std::vector<RE::FormID> packLocals;
        std::vector<std::string> doorLocals;  // 목록 파일의 귀환·막힌 문·자동 출구(G-2 문 시험에서 뺌)
        std::vector<AutoExit> autoExits;
        RE::TESObjectCELL* cell = nullptr;
        RE::TESObjectREFR* entry = nullptr;
        std::vector<RE::TESObjectREFR*> anchors;  // [0] = 보스
        std::vector<RE::FormID> manifestDoors;
    };

    // 플레이어 자리(셀·월드스페이스·좌표)
    struct Spot
    {
        RE::FormID cell = 0;
        RE::FormID worldspace = 0;
        bool interior = false;
        RE::NiPoint3 pos;
        RE::NiPoint3 rot;
    };

    struct Spawned
    {
        RE::ObjectRefHandle handle;
        RE::TESObjectREFR* raw = nullptr;
        RE::FormID id = 0;
        RE::NiPoint3 start;
        float minDistance = std::numeric_limits<float>::max();
        float movedAt8s = -1.0f;
        std::size_t anchorIndex = 0;
    };

    struct Step
    {
        std::string name;
        std::function<void()> enter;
        std::function<bool()> tick;  // true면 끝
        Clock::duration timeout = 0s;
        std::function<void()> onTimeout;
        // 앞의 입장이 실패해 블록을 건너뛰는 중에도 실행할 단계(귀환·정리·기록)
        bool essential = false;
    };

    enum class LoadState { None, Requested, Started, Succeeded, Failed, Unexpected };

    Config g_config;
    std::string g_save;
    std::ofstream g_log;
    std::ofstream g_tsv;
    bool g_start = false;
    std::atomic<LoadState> g_load{ LoadState::None };
    std::atomic_bool g_tickQueued{ false };
    std::atomic_bool g_done{ false };
    std::deque<Step> g_steps;
    bool g_stepEntered = false;
    Clock::time_point g_stepDeadline;
    std::vector<Dungeon> g_dungeons;
    Spot g_origin;
    Spot g_exteriorOrigin;
    int g_pass = 0;
    int g_fail = 0;
    std::set<RE::FormID> g_everSpawned;
    std::set<RE::FormID> g_deletedIds;
    std::map<std::string, std::string> g_baselineG6;
    // 입장에 실패하면 다음 BlockStart까지 essential이 아닌 단계를 건너뛴다(로드되지 않은 셀에 스폰·전투를 걸지 않게).
    bool g_skipBlock = false;
    bool g_godModeBefore = false;
    bool g_abort = false;

    // ---------------- 기록 ----------------

    std::string Stamp()
    {
        const auto now = std::chrono::system_clock::now();
        return std::format("{:%H:%M:%S}", std::chrono::floor<std::chrono::seconds>(now));
    }

    void Log(std::string_view text)
    {
        if (g_log) {
            g_log << Stamp() << ' ' << text << '\n' << std::flush;
        } else {
            const auto line = std::format("PoEProtoMapExperiment: {}\n", text);
            OutputDebugStringA(line.c_str());
        }
    }

    // verdict: PASS / FAIL / INFO
    void Result(std::string_view gate, std::string_view item, std::string_view verdict, std::string_view detail)
    {
        if (verdict == "PASS") {
            ++g_pass;
        } else if (verdict == "FAIL") {
            ++g_fail;
        }
        g_tsv << gate << '\t' << item << '\t' << verdict << '\t' << detail << '\n' << std::flush;
        Log(std::format("[{}] {} {}: {}", gate, verdict, item, detail));
    }

    std::string_view Trim(std::string_view text)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == text.npos) {
            return {};
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    std::string Hex(RE::FormID id) { return std::format("{:08X}", id); }

    std::string Vec(const RE::NiPoint3& p) { return std::format("({:.0f},{:.0f},{:.0f})", p.x, p.y, p.z); }

    // ---------------- 설정과 표지 ----------------

    void ReadConfig()
    {
        std::ifstream input(kConfigPath, std::ios::binary);
        if (!input) {
            Log("INI를 읽지 못해 기본값을 씁니다.");
            return;
        }
        std::string line;
        bool section = false;
        while (std::getline(input, line)) {
            auto text = Trim(line);
            if (text.starts_with("\xEF\xBB\xBF")) {
                text = Trim(text.substr(3));
            }
            if (text.starts_with('[')) {
                section = text == "[Experiment]";
                continue;
            }
            text = Trim(text.substr(0, text.find_first_of(";#")));
            if (!section || text.empty()) {
                continue;
            }
            const auto eq = text.find('=');
            if (eq == text.npos) {
                Log(std::format("INI 줄 형식이 틀렸습니다: {}", text));
                continue;
            }
            const auto key = Trim(text.substr(0, eq));
            const auto value = Trim(text.substr(eq + 1));
            auto seconds = [&](std::chrono::seconds& out, int lo, int hi) {
                int v = 0;
                const auto [end, err] = std::from_chars(value.data(), value.data() + value.size(), v);
                if (err != std::errc{} || end != value.data() + value.size() || v < lo || v > hi) {
                    Log(std::format("INI 값이 범위 밖입니다: {}", key));
                    return;
                }
                out = std::chrono::seconds(v);
            };
            if (key == "QuitWhenDone") {
                if (value == "true" || value == "false") {
                    g_config.quitWhenDone = value == "true";
                } else {
                    Log("INI QuitWhenDone은 true/false입니다.");
                }
            } else if (key == "Cycles") {
                std::chrono::seconds c{ g_config.cycles };
                seconds(c, 1, 20);
                g_config.cycles = static_cast<int>(c.count());
            } else if (key == "CleanupDungeon") {
                g_config.cleanupDungeon = std::string(value);
            } else if (key == "ResetDungeon") {
                g_config.resetDungeon = std::string(value);
            } else if (key == "TourDungeon") {
                g_config.tourDungeon = std::string(value);
            } else if (key == "MainMenuDelaySeconds") {
                seconds(g_config.mainMenuDelay, 0, 60);
            } else if (key == "LoadTimeoutSeconds") {
                seconds(g_config.loadTimeout, 5, 600);
            } else if (key == "ArriveTimeoutSeconds") {
                seconds(g_config.arriveTimeout, 5, 300);
            } else if (key == "DetachTimeoutSeconds") {
                seconds(g_config.detachTimeout, 1, 300);
            } else if (key == "PursuitSeconds") {
                seconds(g_config.pursuit, 9, 120);
            } else {
                Log(std::format("모르는 INI 키: {}", key));
            }
        }
    }

    bool ReadMarker(const fs::path& marker)
    {
        std::ifstream input(marker, std::ios::binary);
        std::string line;
        std::getline(input, line);
        input.close();
        std::error_code error;
        if (!fs::remove(marker, error)) {
            Log("표지 파일을 지우지 못했습니다(다음 실행에서 다시 돌지 않게 지워야 함).");
            return false;
        }
        auto name = Trim(line);
        if (name.starts_with("\xEF\xBB\xBF")) {
            name = Trim(name.substr(3));
        }
        if (name.empty() || name.find_first_of("/\\:*?\"<>|") != name.npos) {
            Log("표지 파일 첫 줄에 세이브 이름이 있어야 합니다.");
            return false;
        }
        g_save = std::string(name);
        return true;
    }

    // ---------------- 목록 파일과 폼 찾기 ----------------

    RE::FormID ParseLocal(const std::string& s) { return static_cast<RE::FormID>(std::stoul(s, nullptr, 16)); }

    bool LoadManifest()
    {
        std::ifstream input(kManifestPath, std::ios::binary);
        if (!input) {
            Result("준비", "목록 파일", "FAIL", std::format("{}을 열지 못했습니다(지도 팩 모드가 꺼짐?)", kManifestPath));
            return false;
        }
        try {
            const auto j = nlohmann::json::parse(input);
            if (j.at("plugin").get<std::string>() != kPlugin) {
                Result("준비", "목록 파일", "FAIL", "plugin이 SkyrimPoE_Maps.esp가 아닙니다.");
                return false;
            }
            for (const auto& d : j.at("dungeons")) {
                Dungeon dg;
                dg.key = d.at("key").get<std::string>();
                dg.role = d.at("role").get<std::string>();
                dg.cellLocal = ParseLocal(d.at("cell").get<std::string>());
                dg.entryLocal = ParseLocal(d.at("entryMarker").get<std::string>());
                dg.bossLocal = ParseLocal(d.at("bossAnchor").get<std::string>());
                for (const auto& p : d.at("packAnchors")) {
                    dg.packLocals.push_back(ParseLocal(p.get<std::string>()));
                }
                for (const auto& p : d.at("returnDoors")) {
                    dg.manifestDoors.push_back(ParseLocal(p.get<std::string>()));
                }
                for (const auto& p : d.at("sealedDoors")) {
                    dg.manifestDoors.push_back(ParseLocal(p.get<std::string>()));
                }
                for (const auto& x : d.at("autoExits")) {
                    const auto& pos = x.at("position");
                    AutoExit ax{ ParseLocal(x.at("id").get<std::string>()), RE::NiPoint3(pos.at(0).get<float>(), pos.at(1).get<float>(), pos.at(2).get<float>()),
                        x.at("rotation").at(2).get<float>() };
                    dg.manifestDoors.push_back(ax.local);
                    dg.autoExits.push_back(ax);
                }
                g_dungeons.push_back(std::move(dg));
            }
            Result("준비", "목록 파일", "PASS", std::format("던전 {}개, packVersion {}", g_dungeons.size(), j.at("packVersion").get<int>()));
            return true;
        } catch (const std::exception& e) {
            Result("준비", "목록 파일", "FAIL", std::format("형식 오류: {}", e.what()));
            return false;
        }
    }

    bool ResolveForms()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !dh->LookupModByName(kPlugin)) {
            Result("준비", "지도 팩 플러그인", "FAIL", "SkyrimPoE_Maps.esp가 로드되지 않았습니다(플러그인이 꺼짐?)");
            return false;
        }
        bool ok = true;
        for (auto& d : g_dungeons) {
            d.cell = dh->LookupForm<RE::TESObjectCELL>(d.cellLocal, kPlugin);
            d.entry = dh->LookupForm<RE::TESObjectREFR>(d.entryLocal, kPlugin);
            auto* boss = dh->LookupForm<RE::TESObjectREFR>(d.bossLocal, kPlugin);
            d.anchors.clear();
            d.anchors.push_back(boss);
            for (const auto local : d.packLocals) {
                d.anchors.push_back(dh->LookupForm<RE::TESObjectREFR>(local, kPlugin));
            }
            const bool anchorsOk = std::ranges::all_of(d.anchors, [](auto* a) { return a != nullptr; });
            const bool cellOk = d.cell && d.cell->IsInteriorCell();
            const bool entryOk = d.entry && d.entry->GetParentCell() == d.cell;
            const bool anchorCellOk = anchorsOk && std::ranges::all_of(d.anchors, [&](auto* a) { return a->GetParentCell() == d.cell; });
            const bool pass = cellOk && entryOk && anchorsOk && anchorCellOk;
            ok = ok && pass;
            Result("준비", std::format("{} 폼 조회", d.key), pass ? "PASS" : "FAIL",
                std::format("셀 {} 입구 {} 앵커 {}/{} (부모 셀 일치 {})", d.cell ? Hex(d.cell->GetFormID()) : "없음",
                    d.entry ? Hex(d.entry->GetFormID()) : "없음", std::ranges::count_if(d.anchors, [](auto* a) { return a != nullptr; }),
                    d.anchors.size(), anchorCellOk));
        }
        return ok;
    }

    Dungeon* Find(std::string_view key)
    {
        const auto it = std::ranges::find_if(g_dungeons, [&](const Dungeon& d) { return d.key == key; });
        return it == g_dungeons.end() ? nullptr : &*it;
    }

    // ---------------- 엔진 도우미(주 스레드 전용) ----------------

    RE::PlayerCharacter* Player() { return RE::PlayerCharacter::GetSingleton(); }

    bool Loading()
    {
        auto* ui = RE::UI::GetSingleton();
        return !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::FaderMenu::MENU_NAME);
    }

    bool PlayerReady()
    {
        auto* p = Player();
        return p && p->Is3DLoaded() && p->GetParentCell() && !Loading();
    }

    Spot Capture()
    {
        Spot s;
        auto* p = Player();
        auto* cell = p->GetParentCell();
        auto* ws = p->GetWorldspace();
        s.cell = cell ? cell->GetFormID() : 0;
        s.worldspace = ws ? ws->GetFormID() : 0;
        s.interior = cell && cell->IsInteriorCell();
        s.pos = p->GetPosition();
        s.rot = p->GetAngle();
        return s;
    }

    // TESObjectREFR::MoveTo_Impl(CommonLib에서는 private). 6단계 브랜치가 쓰는 것과 같은 주소 ID(SE 56227 / AE 56626).
    void MoveToImpl(RE::TESObjectREFR* self, const RE::ObjectRefHandle& target, RE::TESObjectCELL* cell, RE::TESWorldSpace* ws,
        const RE::NiPoint3& pos, const RE::NiPoint3& rot)
    {
        using Fn = void (*)(RE::TESObjectREFR*, const RE::ObjectRefHandle&, RE::TESObjectCELL*, RE::TESWorldSpace*, const RE::NiPoint3&,
            const RE::NiPoint3&);
        static REL::Relocation<Fn> fn{ RELOCATION_ID(56227, 56626) };
        fn(self, target, cell, ws, pos, rot);
    }

    void MoveToSpot(const Spot& s)
    {
        auto* p = Player();
        // 야외도 저장한 셀이 조회되면 넘긴다(셀 없이 부르는 경로는 검증된 적이 없음).
        auto* cell = s.cell ? RE::TESForm::LookupByID<RE::TESObjectCELL>(s.cell) : nullptr;
        auto* ws = s.worldspace ? RE::TESForm::LookupByID<RE::TESWorldSpace>(s.worldspace) : nullptr;
        MoveToImpl(p, p->CreateRefHandle(), cell, ws, s.pos, s.rot);
    }

    bool AtSpot(const Spot& s)
    {
        if (!PlayerReady()) {
            return false;
        }
        auto* p = Player();
        auto* cell = p->GetParentCell();
        auto* ws = p->GetWorldspace();
        const bool sameSpace = s.interior ? cell->GetFormID() == s.cell : (ws && ws->GetFormID() == s.worldspace);
        return sameSpace && p->GetPosition().GetDistance(s.pos) <= kReturnDistance;
    }

    // 소유 확인: 핸들이 가리키는 것이 우리가 만든 그 참조일 때만 돌려준다(다시 쓰인 번호의 다른 참조를 건드리지 않게).
    RE::TESObjectREFR* Owned(const Spawned& s)
    {
        auto ptr = s.handle.get();
        if (!ptr || ptr.get() != s.raw || ptr->GetFormID() != s.id) {
            return nullptr;
        }
        return ptr.get();
    }

    std::optional<Spawned> SpawnAt(RE::TESObjectREFR* anchor, std::size_t anchorIndex)
    {
        auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(kSpawnTemplate);
        if (!anchor || !base) {
            return std::nullopt;
        }
        auto placed = anchor->PlaceObjectAtMe(base, false);
        if (!placed) {
            return std::nullopt;
        }
        Spawned s;
        s.raw = placed.get();
        s.id = placed->GetFormID();
        s.handle = placed->CreateRefHandle();
        s.start = placed->GetPosition();
        s.anchorIndex = anchorIndex;
        return s;
    }

    struct CellCount
    {
        int visibleOurs = 0;
        int deadOurs = 0;
        int disabledOurs = 0;
        int otherDynamicActors = 0;
        int totalRefs = 0;
    };

    CellCount CountCell(RE::TESObjectCELL* cell)
    {
        CellCount c;
        if (!cell) {
            return c;
        }
        cell->ForEachReference([&](RE::TESObjectREFR* r) {
            ++c.totalRefs;
            if (!r) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto id = r->GetFormID();
            const bool ours = g_everSpawned.contains(id);
            auto* actor = r->As<RE::Actor>();
            if (ours) {
                if (r->IsDisabled() || r->IsDeleted()) {
                    ++c.disabledOurs;
                } else {
                    ++c.visibleOurs;
                    if (actor && actor->IsDead()) {
                        ++c.deadOurs;
                    }
                }
            } else if (actor && (id >> 24) == 0xFF && !r->IsDisabled() && !r->IsDeleted()) {
                ++c.otherDynamicActors;
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        return c;
    }

    std::string Describe(const CellCount& c)
    {
        return std::format("우리 스폰 보임 {}(시체 {}), 비활성·삭제 표시 {}, 다른 동적 액터 {}, 셀 참조 {}", c.visibleOurs, c.deadOurs,
            c.disabledOurs, c.otherDynamicActors, c.totalRefs);
    }

    bool CellResetViaPapyrus(RE::TESObjectCELL* cell)
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm || !cell) {
            return false;
        }
        auto* policy = vm->GetObjectHandlePolicy();
        if (!policy) {
            return false;
        }
        const auto handle = policy->GetHandleForObject(RE::FormType::Cell, cell);
        if (handle == policy->EmptyHandle()) {
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        auto* args = RE::MakeFunctionArguments();
        return vm->DispatchMethodCall2(handle, "Cell", "Reset", args, callback);
    }

    RE::TESObjectREFR* FindTestDoor(const Dungeon& d)
    {
        RE::TESObjectREFR* found = nullptr;
        auto* dh = RE::TESDataHandler::GetSingleton();
        std::set<RE::FormID> skip;
        for (const auto local : d.manifestDoors) {
            if (const auto full = dh->LookupFormID(local, kPlugin)) {
                skip.insert(full);
            }
        }
        d.cell->ForEachReference([&](RE::TESObjectREFR* r) {
            auto* base = r ? r->GetBaseObject() : nullptr;
            if (base && base->Is(RE::FormType::Door) && !skip.contains(r->GetFormID()) && !r->IsDisabled() &&
                RE::BGSOpenCloseForm::GetOpenState(r) == RE::BGSOpenCloseForm::OPEN_STATE::kClosed) {
                found = r;
                return RE::BSContainer::ForEachResult::kStop;
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        return found;
    }

    std::map<std::string, std::string> SnapshotG6()
    {
        std::map<std::string, std::string> m;
        for (const auto& [id, name] : kQuests) {
            auto* q = RE::TESForm::LookupByID<RE::TESQuest>(id);
            m[std::string(name)] = q ? std::format("stage={} running={} completed={}", q->GetCurrentStageID(), q->IsRunning(), q->IsCompleted())
                                     : "없음";
        }
        auto* resident = RE::TESForm::LookupByID<RE::Actor>(kEmbershardResident);
        if (resident) {
            auto* cell = resident->GetSaveParentCell();
            m["주민 0B6FF7"] = std::format("dead={} cell={} disabled={}", resident->IsDead(), cell ? Hex(cell->GetFormID()) : "없음", resident->IsDisabled());
        } else {
            m["주민 0B6FF7"] = "없음";
        }
        return m;
    }

    // ---------------- 단계 만들기 ----------------

    void Push(Step s) { g_steps.push_back(std::move(s)); }

    void Do(std::string name, std::function<void()> fn, bool essential = false)
    {
        Push({ std::move(name), std::move(fn), [] { return true; }, 1s, {}, essential });
    }

    void BlockStart(std::string name)
    {
        Push({ std::move(name), [] { g_skipBlock = false; }, [] { return true; }, 1s, {}, true });
    }

    void Wait(std::string name, Clock::duration d)
    {
        auto until = std::make_shared<Clock::time_point>();
        Push({ std::move(name), [until, d] { *until = Clock::now() + d; }, [until] { return Clock::now() >= *until; }, d + 5s, {} });
    }

    // 입구 마커로 이동해 도착을 확인한다. 결과를 gate/item으로 기록한다.
    void Enter(Dungeon* d, std::string gate, std::string item, bool record)
    {
        auto started = std::make_shared<Clock::time_point>();
        Push({ std::format("{} 입장", d->key),
            [d, started] {
                *started = Clock::now();
                Player()->MoveTo(d->entry);
            },
            [d, started, gate, item, record] {
                if (!PlayerReady() || Player()->GetParentCell() != d->cell) {
                    return false;
                }
                const auto dist = Player()->GetPosition().GetDistance(d->entry->GetPosition());
                if (record) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *started).count();
                    Result(gate, item, dist <= kArriveMarkerDistance ? "PASS" : "FAIL",
                        std::format("셀 {} 3D 로드, 마커까지 {:.0f}, {}ms", Hex(d->cell->GetFormID()), dist, ms));
                }
                return true;
            },
            g_config.arriveTimeout,
            [d, gate, item] {
                auto* cell = Player()->GetParentCell();
                Result(gate, item, "FAIL", std::format("제한 시간 안에 도착하지 않음(현재 셀 {}, 로딩 {}). 이 블록의 나머지는 건너뜀", cell ? Hex(cell->GetFormID()) : "없음", Loading()));
                g_skipBlock = true;
            } });
    }

    void Return(const Spot* spot, std::string gate, std::string item, bool record)
    {
        auto started = std::make_shared<Clock::time_point>();
        Push({ "출발지로 귀환",
            [spot, started] {
                *started = Clock::now();
                MoveToSpot(*spot);
            },
            [spot, started, gate, item, record] {
                if (!AtSpot(*spot)) {
                    return false;
                }
                if (record) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *started).count();
                    Result(gate, item, "PASS", std::format("{} {} 도착, 거리 {:.0f}, {}ms", spot->interior ? "셀" : "월드스페이스",
                                                       Hex(spot->interior ? spot->cell : spot->worldspace), Player()->GetPosition().GetDistance(spot->pos), ms));
                }
                return true;
            },
            g_config.arriveTimeout,
            [spot, gate, item] {
                auto* cell = Player()->GetParentCell();
                Result(gate, item, "FAIL", std::format("제한 시간 안에 돌아오지 않음(목표 {}, 현재 셀 {}, 거리 {:.0f})", Vec(spot->pos),
                                               cell ? Hex(cell->GetFormID()) : "없음", Player()->GetPosition().GetDistance(spot->pos)));
            },
            true });
    }

    void WaitDetached(Dungeon* d, std::string gate, std::string item, std::shared_ptr<bool> detached = std::make_shared<bool>(false))
    {
        auto started = std::make_shared<Clock::time_point>();
        Push({ std::format("{} 셀 떨어짐 대기", d->key), [started, detached] { *started = Clock::now(); *detached = false; },
            [d, started, gate, item, detached] {
                if (d->cell->IsAttached()) {
                    return false;
                }
                *detached = true;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *started).count();
                Log(std::format("{}: 셀이 떨어짐({}ms)", d->key, ms));
                return true;
            },
            g_config.detachTimeout, [gate, item] { Result(gate, item, "INFO", "제한 시간 안에 셀이 떨어지지 않음(이후 단계는 붙은 채로 진행)"); }, true });
    }

    using SpawnList = std::shared_ptr<std::vector<Spawned>>;

    void SpawnStep(Dungeon* d, SpawnList list, std::size_t count, std::string gate)
    {
        Do(std::format("{} 스폰 {}마리", d->key, count), [d, list, count, gate] {
            int reused = 0;
            for (std::size_t i = 0; i < count && !d->anchors.empty(); ++i) {
                // 무리 앵커(1번부터)를 돌아가며 쓴다. 앵커가 보스 하나뿐이면 보스 자리를 쓴다.
                const auto idx = d->anchors.size() < 2 ? 0 : 1 + (i % (d->anchors.size() - 1));
                if (auto s = SpawnAt(d->anchors[idx], idx)) {
                    reused += g_deletedIds.contains(s->id) ? 1 : 0;
                    g_everSpawned.insert(s->id);
                    list->push_back(*s);
                }
            }
            Log(std::format("{}: 스폰 {}/{}, 지운 번호 재사용 {}", d->key, list->size(), count, reused));
            if (reused > 0) {
                Result(gate, "번호 재사용", "INFO", std::format("지운 FormID를 새 스폰이 {}번 다시 받음", reused));
            }
        });
    }

    void KillHalf(SpawnList list)
    {
        Do("절반 처치", [list] {
            for (std::size_t i = 0; i < list->size(); i += 2) {
                if (auto* r = Owned((*list)[i])) {
                    if (auto* a = r->As<RE::Actor>()) {
                        a->KillImmediate();
                    }
                }
            }
        });
    }

    void DisableAll(SpawnList list, std::string name)
    {
        Do(std::move(name), [list] {
            int n = 0;
            for (const auto& s : *list) {
                if (auto* r = Owned(s)) {
                    r->Disable();
                    ++n;
                }
            }
            Log(std::format("비활성화 {}/{}", n, list->size()));
        }, true);
    }

    void DeleteAll(SpawnList list, std::string name)
    {
        Do(std::move(name), [list] {
            int n = 0;
            for (const auto& s : *list) {
                if (auto* r = Owned(s)) {
                    r->SetDelete(true);
                    g_deletedIds.insert(s.id);
                    ++n;
                }
            }
            Log(std::format("삭제 표시 {}/{}", n, list->size()));
        }, true);
    }

    void Tally(SpawnList list, std::string gate, std::string item, std::shared_ptr<bool> detachedBeforeDelete)
    {
        Do("회차 집계", [list, gate, item, detachedBeforeDelete] {
            int gone = 0, deleted = 0, disabled = 0, alive = 0;
            for (const auto& s : *list) {
                auto* r = Owned(s);
                if (!r) {
                    ++gone;
                } else if (r->IsDeleted()) {
                    ++deleted;
                } else if (r->IsDisabled()) {
                    ++disabled;
                } else {
                    ++alive;
                }
            }
            Result(gate, item, alive == 0 ? "PASS" : "FAIL",
                std::format("{}마리: 사라짐 {}, 삭제 표시 {}, 비활성 {}, 보이는 채 남음 {}{}", list->size(), gone, deleted, disabled, alive,
                    *detachedBeforeDelete ? "" : " (셀이 떨어지기 전에 정리함)"));
        }, true);
    }

    void CountStep(Dungeon* d, std::string gate, std::string item, std::function<std::string(const CellCount&)> judge)
    {
        Do(std::format("{} 셀 세기", d->key), [d, gate, item, judge] {
            const auto c = CountCell(d->cell);
            const auto verdict = judge(c);
            Result(gate, item, verdict, Describe(c));
        });
    }

    // ---------------- 시나리오 ----------------

    void BuildScenario()
    {
        // 준비: 무적(처음 상태를 기억해 끝에 되돌림), 출발지(실내여야 함)
        Do("무적 켜기", [] {
            g_godModeBefore = RE::PlayerCharacter::IsGodMode();
            Player()->SetGodMode(true);
            Result("준비", "무적", RE::PlayerCharacter::IsGodMode() ? "INFO" : "FAIL",
                std::format("시작 상태 {} → 켬 {}", g_godModeBefore, RE::PlayerCharacter::IsGodMode()));
        }, true);
        Do("출발지 기록", [] {
            g_origin = Capture();
            Result("준비", "출발지", g_origin.interior ? "INFO" : "FAIL", std::format("{} {} {}", g_origin.interior ? "실내 셀" : "월드스페이스",
                                                                                 Hex(g_origin.interior ? g_origin.cell : g_origin.worldspace), Vec(g_origin.pos)));
            if (!g_origin.interior) {
                g_abort = true;  // 실내 세이브가 전제라 멈춘다(단계 실행기가 현재 단계를 마친 뒤 비움)
                Log("세이브 위치가 실내가 아니라 시험을 멈춥니다.");
            }
        }, true);
        Do("G-6 처음 값(기록만)", [] {
            for (const auto& [k, v] : SnapshotG6()) {
                Result("G-6", std::format("{} 처음", k), "INFO", v);
            }
        });

        // G-3: 실내에서 들어가고 돌아오기
        for (auto& d : g_dungeons) {
            BlockStart("G-3 실내 블록");
            Enter(&d, "G-3", std::format("실내→{} 입장", d.key), true);
            Wait("머무름", 1s);
            Return(&g_origin, "G-3", std::format("{}→실내 귀환", d.key), true);
        }
        // G-3: 야외(Embershard 바깥 문)에서 들어가고 돌아오기
        BlockStart("G-3 야외 블록");
        auto outside = std::make_shared<Clock::time_point>();
        Push({ "야외로 이동",
            [outside] {
                *outside = Clock::now();
                if (auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(kEmbershardOutside)) {
                    Player()->MoveTo(door);
                }
            },
            [] { return PlayerReady() && Player()->GetWorldspace() && !Player()->GetParentCell()->IsInteriorCell(); },
            g_config.arriveTimeout,
            [] {
                Result("G-3", "야외 출발지로 이동", "FAIL", "Embershard 바깥 문으로 가지 못함. 야외 블록은 건너뜀");
                g_skipBlock = true;
            } });
        Wait("야외 안정", 3s);
        Do("야외 출발지 기록", [] {
            g_exteriorOrigin = Capture();
            Result("G-3", "야외 출발지", "INFO", std::format("월드스페이스 {} {}", Hex(g_exteriorOrigin.worldspace), Vec(g_exteriorOrigin.pos)));
        });
        for (auto& d : g_dungeons) {
            Enter(&d, "G-3", std::format("야외→{} 입장", d.key), true);
            Wait("머무름", 1s);
            Return(&g_exteriorOrigin, "G-3", std::format("{}→야외 귀환", d.key), true);
        }
        Return(&g_origin, "준비", "실내 출발지로 복귀", false);
        // G-6 기준값: 바닐라 바깥(Embershard 입구)에 다녀온 뒤에 잡는다. 이후로는 복제 셀과 출발지만 오간다.
        Do("G-6 기준값", [] {
            g_baselineG6 = SnapshotG6();
            for (const auto& [k, v] : g_baselineG6) {
                Result("G-6", std::format("{} 기준", k), "INFO", v);
            }
        }, true);

        // G-1: 정리 방법 비교
        if (auto* d = Find(g_config.cleanupDungeon)) {
            for (const char method : { 'a', 'b', 'c' }) {
                for (int i = 1; i <= g_config.cycles; ++i) {
                    auto list = std::make_shared<std::vector<Spawned>>();
                    auto detached = std::make_shared<bool>(false);
                    const auto item = std::format("방법 {} 회차 {}", method, i);
                    BlockStart("G-1 회차 블록");
                    Enter(d, "G-1", item + " 입장", false);
                    Wait("AI 준비", 1500ms);
                    SpawnStep(d, list, kSpawnPerCycle, "G-1");
                    Wait("전투", 2s);
                    KillHalf(list);
                    Wait("시체 안정", 1500ms);
                    DisableAll(list, "정리: 비활성화");
                    if (method == 'b') {
                        Wait("셀 안에서 3초", 3s);
                        DeleteAll(list, "정리(b): 셀 안에서 삭제 표시");
                    }
                    Return(&g_origin, "G-1", item + " 귀환", false);
                    WaitDetached(d, "G-1", item + " 셀 떨어짐", detached);
                    if (method == 'a') {
                        DeleteAll(list, "정리(a): 셀이 떨어진 뒤 삭제 표시");
                    }
                    Wait("엔진 정리 대기", 1s);
                    // (a)만 "셀이 떨어진 뒤"가 조건이다. (b)·(c)는 떨어짐과 무관하게 집계한다.
                    Tally(list, "G-1", item, method == 'a' ? detached : std::make_shared<bool>(true));
                }
                const auto item = std::format("방법 {} 뒤 재입장", method);
                BlockStart("G-1 재입장 블록");
                Enter(d, "G-1", item, false);
                Wait("셀 안정", 2s);
                CountStep(d, "G-1", item, [](const CellCount& c) { return c.visibleOurs == 0 ? "PASS" : "FAIL"; });
                Return(&g_origin, "G-1", item + " 귀환", false);
                WaitDetached(d, "G-1", item + " 셀 떨어짐");
            }
        } else {
            Result("G-1", "던전", "FAIL", std::format("목록에 {}가 없음", g_config.cleanupDungeon));
        }

        // G-2: 재입장 보존과 새 회차
        if (auto* d = Find(g_config.resetDungeon)) {
            auto list = std::make_shared<std::vector<Spawned>>();
            auto door = std::make_shared<RE::FormID>(0);
            auto doorState = [door]() -> std::string {
                auto* r = *door ? RE::TESForm::LookupByID<RE::TESObjectREFR>(*door) : nullptr;
                if (!r) {
                    return "시험 문 없음";
                }
                const auto st = RE::BGSOpenCloseForm::GetOpenState(r);
                return std::format("시험 문 {} 상태 {}", Hex(*door), static_cast<int>(st));
            };
            BlockStart("G-2 1회차 블록");
            Enter(d, "G-2", "1회차 입장", false);
            Wait("AI 준비", 1500ms);
            SpawnStep(d, list, kSpawnPerCycle, "G-2");
            Wait("전투", 2s);
            KillHalf(list);
            Do("문 열기", [d, door] {
                if (auto* r = FindTestDoor(*d)) {
                    *door = r->GetFormID();
                    RE::BGSOpenCloseForm::SetOpenState(r, true, true);
                }
                Log(std::format("G-2 시험 문 {}", *door ? Hex(*door) : "없음"));
            });
            Wait("안정", 1500ms);
            Return(&g_origin, "G-2", "1회차 귀환", false);
            WaitDetached(d, "G-2", "1회차 셀 떨어짐");
            BlockStart("G-2 재입장 블록");
            Enter(d, "G-2", "재입장", false);
            Wait("셀 안정", 2s);
            Do("재입장 세기", [d, doorState] {
                const auto c = CountCell(d->cell);
                // 6마리 가운데 3마리를 죽였으므로 산 적 3, 시체 3이 그대로 있어야 한다.
                const bool kept = c.deadOurs == 3 && c.visibleOurs - c.deadOurs == 3;
                Result("G-2", "재입장 보존", kept ? "PASS" : "FAIL", Describe(c) + ", " + doorState());
            });
            DisableAll(list, "정리: 비활성화");
            Return(&g_origin, "G-2", "재입장 귀환", false);
            WaitDetached(d, "G-2", "재입장 셀 떨어짐");
            DeleteAll(list, "정리(a): 삭제 표시");
            Wait("엔진 정리 대기", 1s);
            BlockStart("G-2 새 회차(i) 블록");
            Enter(d, "G-2", "새 회차(우리 정리만)", false);
            Wait("셀 안정", 2s);
            Do("새 회차 세기(i)", [d, doorState] {
                const auto c = CountCell(d->cell);
                Result("G-2", "새 회차(i) 우리 정리만", c.visibleOurs == 0 ? "PASS" : "FAIL", Describe(c) + ", " + doorState());
            });
            // (ii) 우리 정리 없이 Cell.Reset()만: 무엇이 되돌아가는지 본다.
            auto list2 = std::make_shared<std::vector<Spawned>>();
            SpawnStep(d, list2, kSpawnPerCycle, "G-2");
            Wait("전투", 2s);
            KillHalf(list2);
            Do("문 다시 열기", [door] {
                if (auto* r = *door ? RE::TESForm::LookupByID<RE::TESObjectREFR>(*door) : nullptr) {
                    RE::BGSOpenCloseForm::SetOpenState(r, true, true);
                }
            });
            Wait("안정", 1500ms);
            Return(&g_origin, "G-2", "(ii) 귀환", false);
            WaitDetached(d, "G-2", "(ii) 셀 떨어짐");
            Do("Cell.Reset 호출", [d] {
                const bool ok = CellResetViaPapyrus(d->cell);
                Result("G-2", "Cell.Reset 호출", ok ? "INFO" : "FAIL", ok ? "Papyrus 호출 보냄" : "호출 실패");
            });
            Wait("Papyrus 처리", 2s);
            BlockStart("G-2 새 회차(ii) 블록");
            Enter(d, "G-2", "새 회차(Cell.Reset만)", false);
            Wait("셀 안정", 2s);
            Do("새 회차 세기(ii)", [d, doorState] {
                const auto c = CountCell(d->cell);
                Result("G-2", "새 회차(ii) Cell.Reset만", "INFO", Describe(c) + ", " + doorState());
            });
            DisableAll(list2, "(ii) 뒷정리: 비활성화");
            Return(&g_origin, "G-2", "(ii) 뒷정리 귀환", false);
            WaitDetached(d, "G-2", "(ii) 뒷정리 셀 떨어짐");
            DeleteAll(list2, "(ii) 뒷정리: 삭제 표시");
        } else {
            Result("G-2", "던전", "FAIL", std::format("목록에 {}가 없음", g_config.resetDungeon));
        }

        // G-4·G-7: 앵커마다 추격, 자동 출구
        for (auto& dref : g_dungeons) {
            auto* d = &dref;
            auto list = std::make_shared<std::vector<Spawned>>();
            BlockStart(std::format("G-4 {} 블록", d->key));
            Enter(d, "G-4", std::format("{} 입장", d->key), false);
            Wait("AI 준비", 1500ms);
            Do(std::format("{} 앵커마다 스폰", d->key), [d, list] {
                for (std::size_t i = 0; i < d->anchors.size() && i < kPursuitCap; ++i) {
                    if (auto s = SpawnAt(d->anchors[i], i)) {
                        g_everSpawned.insert(s->id);
                        list->push_back(*s);
                    }
                }
                Log(std::format("{}: 앵커 스폰 {}/{}", d->key, list->size(), std::min(d->anchors.size(), kPursuitCap)));
            });
            Wait("스폰 안정", 500ms);
            Do("전투 걸기", [list] {
                for (const auto& s : *list) {
                    if (auto* r = Owned(s)) {
                        if (auto* a = r->As<RE::Actor>()) {
                            a->StartCombat(Player());
                        }
                    }
                }
            });
            auto pursuitStart = std::make_shared<Clock::time_point>();
            Push({ std::format("{} 추격 관찰", d->key), [pursuitStart] { *pursuitStart = Clock::now(); },
                [list, pursuitStart] {
                    const auto elapsed = Clock::now() - *pursuitStart;
                    const auto playerPos = Player()->GetPosition();
                    for (auto& s : *list) {
                        auto* r = Owned(s);
                        if (!r || !r->Is3DLoaded()) {
                            continue;
                        }
                        const auto pos = r->GetPosition();
                        s.minDistance = std::min(s.minDistance, pos.GetDistance(playerPos));
                        if (s.movedAt8s < 0 && elapsed >= 8s) {
                            s.movedAt8s = pos.GetDistance(s.start);
                        }
                    }
                    return elapsed >= g_config.pursuit;
                },
                g_config.pursuit + 10s, {} });
            Do(std::format("{} 추격 판정", d->key), [d, list] {
                int moved = 0, reached = 0;
                for (const auto& s : *list) {
                    const bool m = s.movedAt8s > kMovedDistance;
                    const bool reach = s.minDistance <= kReachedDistance;
                    moved += m ? 1 : 0;
                    reached += reach ? 1 : 0;
                    const auto label = s.anchorIndex == 0 ? std::string("보스") : std::format("무리{:02}", s.anchorIndex);
                    Result("G-4", std::format("{} {} 앵커", d->key, label), m ? "PASS" : "FAIL",
                        std::format("시작 {} 8초 이동 {:.0f}, 플레이어에게 최소 {:.0f}{}", Vec(s.start), s.movedAt8s, s.minDistance,
                            reach ? " (입구까지 옴)" : ""));
                }
                Result("G-4", std::format("{} 요약", d->key), "INFO", std::format("앵커 {}: 움직임 {}, 입구까지 옴 {}", list->size(), moved, reached));
            });
            // G-7: 자동 출구의 문 원점과, 문이 향한 방향 앞뒤 150유닛. 문 너머가 빈 공간이면 그쪽 자리에서 떨어진다.
            for (const auto& ax : d->autoExits) {
                for (const float offset : { 0.0f, 150.0f, -150.0f }) {
                    const RE::NiPoint3 at(ax.pos.x + std::sin(ax.rotZ) * offset, ax.pos.y + std::cos(ax.rotZ) * offset, ax.pos.z);
                    const auto item = std::format("{} 자동 출구 {} ({:+.0f})", d->key, Hex(ax.local), offset);
                    auto minZ = std::make_shared<float>(at.z);
                    auto t0 = std::make_shared<Clock::time_point>();
                    BlockStart("G-7 자리 블록");
                    Push({ item,
                        [d, at, minZ, t0] {
                            Spot s{ d->cell->GetFormID(), 0, true, at, Player()->GetAngle() };
                            MoveToSpot(s);
                            *t0 = Clock::now();
                            *minZ = at.z;
                        },
                        [d, at, minZ, t0, item] {
                            if (Clock::now() - *t0 < 500ms || !PlayerReady()) {
                                return false;
                            }
                            *minZ = std::min(*minZ, Player()->GetPosition().z);
                            const bool leftCell = Player()->GetParentCell() != d->cell;
                            if (leftCell || Clock::now() - *t0 >= 3500ms) {
                                const bool fell = at.z - *minZ > kFallDrop;
                                Result("G-7", item, (fell || leftCell) ? "FAIL" : "PASS",
                                    std::format("놓은 자리 {}, 최저 z {:.0f}, 셀 이탈 {}", Vec(at), *minZ, leftCell));
                                return true;
                            }
                            return false;
                        },
                        10s, [item] { Result("G-7", item, "FAIL", "이동하지 못함"); } });
                    Enter(d, "G-7", "입구로 복귀", false);
                }
            }
            DisableAll(list, "정리: 비활성화");
            Return(&g_origin, "G-4", std::format("{} 귀환", d->key), false);
            WaitDetached(d, "G-4", std::format("{} 셀 떨어짐", d->key));
            DeleteAll(list, "정리(a): 삭제 표시");
        }

        // 끝: 무적 되돌리기, G-6 비교, 둘러보기
        BlockStart("마무리 블록");
        Do("무적 되돌리기", [] {
            Player()->SetGodMode(g_godModeBefore);
            Log(std::format("무적을 처음 상태({})로 되돌림", g_godModeBefore));
        }, true);
        Do("G-6 비교", [] {
            const auto now = SnapshotG6();
            for (const auto& [k, v] : now) {
                const auto it = g_baselineG6.find(k);
                const bool same = it != g_baselineG6.end() && it->second == v;
                Result("G-6", k, same ? "PASS" : "FAIL", std::format("기준 [{}] 끝 [{}]", it == g_baselineG6.end() ? "없음" : it->second, v));
            }
        }, true);
        if (auto* tour = Find(g_config.tourDungeon)) {
            Enter(tour, "G-5", std::format("{} 둘러보기 위치", tour->key), true);
        }
        Do("요약", [] { Result("요약", "합계", g_fail == 0 ? "PASS" : "FAIL", std::format("PASS {}, FAIL {}", g_pass, g_fail)); }, true);
    }

    // ---------------- 실행 ----------------

    enum class Stage { WaitMainMenu, WaitLoad, Settle, Running, Done };
    Stage g_stage = Stage::WaitMainMenu;
    std::optional<Clock::time_point> g_mainMenuSince;
    Clock::time_point g_deadline;

    void Finish(std::string_view why)
    {
        Log(std::format("끝: {}", why));
        g_stage = Stage::Done;
        g_done.store(true);
    }

    void RunSteps()
    {
        while (!g_steps.empty()) {
            auto& s = g_steps.front();
            if (!g_stepEntered && g_skipBlock && !s.essential) {
                Log(std::format("건너뜀: {}", s.name));
                g_steps.pop_front();
                continue;
            }
            if (!g_stepEntered) {
                g_stepEntered = true;
                g_stepDeadline = Clock::now() + s.timeout;
                try {
                    if (s.enter) {
                        s.enter();
                    }
                    if (g_abort) {
                        g_steps.clear();
                        g_stepEntered = false;
                        Finish("중단 요청");
                        return;
                    }
                } catch (const std::exception& e) {
                    Result("실행", s.name, "FAIL", std::format("시작 중 예외: {}", e.what()));
                    g_steps.pop_front();
                    g_stepEntered = false;
                    continue;
                }
            }
            bool done = false;
            try {
                done = s.tick();
            } catch (const std::exception& e) {
                Result("실행", s.name, "FAIL", std::format("진행 중 예외: {}", e.what()));
                done = true;
            }
            if (!done && Clock::now() >= g_stepDeadline) {
                if (s.onTimeout) {
                    s.onTimeout();
                } else {
                    Result("실행", s.name, "FAIL", "제한 시간 초과");
                }
                done = true;
            }
            if (!done) {
                return;  // 다음 틱에 이어서
            }
            g_steps.pop_front();
            g_stepEntered = false;
            if (!g_steps.empty() && !g_steps.front().enter) {
                continue;
            }
            return;  // 한 틱에 한 단계씩 시작(엔진이 상태를 반영할 틈을 둠)
        }
        Finish("모든 단계 완료");
    }

    void TickGame()
    {
        auto* ui = RE::UI::GetSingleton();
        const auto now = Clock::now();
        const auto load = g_load.load();
        if (load == LoadState::Failed || load == LoadState::Unexpected) {
            Finish(load == LoadState::Failed ? "세이브 불러오기 실패" : "예상하지 못한 불러오기");
            return;
        }
        switch (g_stage) {
        case Stage::WaitMainMenu:
            if (ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
                if (!g_mainMenuSince) {
                    g_mainMenuSince = now;
                } else if (now >= *g_mainMenuSince + g_config.mainMenuDelay) {
                    if (auto* m = RE::BGSSaveLoadManager::GetSingleton()) {
                        g_load.store(LoadState::Requested);
                        Log(std::format("세이브 불러오기 요청: {}", g_save));
                        m->Load(g_save.c_str());
                        g_stage = Stage::WaitLoad;
                        g_deadline = now + g_config.loadTimeout;
                    } else {
                        Finish("세이브 관리자를 쓸 수 없음");
                    }
                }
            }
            break;
        case Stage::WaitLoad:
            if (load == LoadState::Succeeded && PlayerReady() && !ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
                g_stage = Stage::Settle;
                g_deadline = now + 3s;
            } else if (now >= g_deadline) {
                Finish("세이브 불러오기 시간 초과");
            }
            break;
        case Stage::Settle:
            if (now >= g_deadline) {
                if (!LoadManifest() || !ResolveForms()) {
                    Finish("준비 실패");
                    return;
                }
                BuildScenario();
                g_stage = Stage::Running;
            }
            break;
        case Stage::Running:
            RunSteps();
            break;
        case Stage::Done:
            break;
        }
    }

    void Ticker() noexcept
    {
        const auto* tasks = SKSE::GetTaskInterface();
        while (!g_done.load()) {
            if (!g_tickQueued.exchange(true)) {
                tasks->AddTask([]() noexcept {
                    try {
                        TickGame();
                    } catch (const std::exception& e) {
                        Log(std::format("틱 예외: {}", e.what()));
                        Finish("예외");
                    } catch (...) {
                        Finish("알 수 없는 예외");
                    }
                    g_tickQueued.store(false);
                });
            }
            std::this_thread::sleep_for(kTick);
        }
        const bool stay = !g_config.tourDungeon.empty();
        Log(std::format("결과: PASS {}, FAIL {}{}", g_pass, g_fail, stay ? " (둘러보기로 게임을 켜 둠)" : ""));
        if (g_config.quitWhenDone && !stay) {
            tasks->AddTask([]() noexcept {
                if (auto* main = RE::Main::GetSingleton()) {
                    main->GetRuntimeData().quitGame = true;
                }
            });
        }
    }

    void OnMessage(SKSE::MessagingInterface::Message* message) noexcept
    {
        if (!message || !g_start) {
            return;
        }
        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            std::thread(Ticker).detach();
            break;
        case SKSE::MessagingInterface::kPreLoadGame: {
            auto expected = LoadState::Requested;
            if (!g_load.compare_exchange_strong(expected, LoadState::Started)) {
                g_load.store(LoadState::Unexpected);
            }
            break;
        }
        case SKSE::MessagingInterface::kPostLoadGame: {
            auto expected = LoadState::Started;
            if (!g_load.compare_exchange_strong(expected, message->data ? LoadState::Succeeded : LoadState::Failed)) {
                g_load.store(LoadState::Unexpected);
            }
            break;
        }
        case SKSE::MessagingInterface::kNewGame:
            g_load.store(LoadState::Unexpected);
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    try {
        const auto directory = SKSE::log::log_directory();
        if (!directory) {
            return true;
        }
        const auto marker = *directory / kMarker;
        if (!fs::exists(marker)) {
            return true;
        }
        g_log.open(*directory / "PoEProtoMapExperiment.log", std::ios::out | std::ios::trunc);
        g_tsv.open(*directory / "PoEProtoMapExperiment.tsv", std::ios::out | std::ios::trunc);
        if (!g_log || !g_tsv) {
            return true;
        }
        g_tsv << "관문\t항목\t판정\t내용\n";
        if (!ReadMarker(marker)) {
            return true;
        }
        ReadConfig();
        SKSE::Init(skse, { .log = false });
        const auto version = skse->RuntimeVersion();
        Log(std::format("PoEProtoMapExperiment {} 런타임 {}", SKSE::GetPluginVersion().string("."), version.string(".")));
        if (version != SKSE::RUNTIME_SSE_1_6_1170) {
            Log("지원하지 않는 런타임이라 아무것도 하지 않습니다.");
            return true;
        }
        const auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging || !SKSE::GetTaskInterface() || !messaging->RegisterListener(OnMessage)) {
            Log("SKSE 메시지·작업 인터페이스를 쓸 수 없습니다.");
            return true;
        }
        g_start = true;
    } catch (const std::exception& e) {
        Log(std::format("시작 실패: {}", e.what()));
    }
    return true;
}
