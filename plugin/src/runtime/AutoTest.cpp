#include "PCH.h"
#include "runtime/AutoTest.h"
#include "runtime/CombatHooks.h"
#include "runtime/CraftingRuntime.h"
#include "runtime/CycloneRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/ItemTracker.h"
#include "runtime/SaveLoad.h"
#include "runtime/SimpleImeBridge.h"
#include "runtime/SkillExecution.h"
#include "runtime/UiBridge.h"
#include "runtime/WorldDropTracker.h"

#include "core/DamageConversion.h"
#include "core/ItemCrafting.h"
#include "core/LootTable.h"
#include "core/PassiveTree.h"
#include "core/SkillSystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        namespace fs = std::filesystem;
        using Clock = std::chrono::steady_clock;
        using json = nlohmann::json;

        constexpr auto kMarkerFilename = "SkyrimPoE.autotest";
        constexpr auto kReportFilename = "SkyrimPoE.autotest.log";
        constexpr auto kPrevReportFilename = "SkyrimPoE.autotest.previous.log";

        // houseCARL로 대조 검증한 바닐라 Skyrim.esm 폼 ID
        constexpr RE::FormID kIronSwordId = 0x00012EB7;      // IronSword
        constexpr RE::FormID kIronDaggerId = 0x0001397E;     // IronDagger (E3 오브 왕복용)
        constexpr RE::FormID kIronWarAxeId = 0x00013790;     // IronWarAxe (E6 중복 베이스 격리용)
        constexpr RE::FormID kIronHelmetId = 0x00012E4D;     // IronHelmet (E4 장비 스탯 반영용)
        constexpr RE::FormID kEncSkeeverId = 0x00023AB7;     // EncSkeever (Skyrim.esm 순정, Aggressive, Lv1)
        constexpr RE::FormID kNeutralNpcId = 0x0010D13E;     // AADeleteWhenDoneTestJeremyRegular (순정, 무팩션·Unaggressive·NoCrime)
        constexpr auto kFrenzyPlugin = "BerserkerFrenzy.esp"sv;
        constexpr RE::FormID kFrenzyEffectLocalId = 0x800;

        using DealDamageFn = void*(RE::Actor* attacker, RE::Actor* target, RE::Projectile* projectile, bool leftHand);
        // DllMain 로더 락 안에서 Address Library를 열지 않도록 함수 내부 static으로 지연 해석한다.
        void CallDealDamage(RE::Actor* attacker, RE::Actor* target, RE::Projectile* projectile, bool leftHand)
        {
            static const REL::Relocation<DealDamageFn> fn{ REL::RelocationID(37673, 38627) };
            (void)fn(attacker, target, projectile, leftHand);
        }

        struct AutoTestConfig
        {
            std::string baselineSave;
            std::string tempSave = "SkyrimPoE_autotest";
            bool quitWhenDone = true;
            bool consumeMarker = true;
            double globalTimeoutSec = 300.0;
        };

        enum class Phase : std::uint8_t
        {
            Inactive = 0,
            AwaitMainMenu,
            MenuDelay,
            AwaitBaselineLoad,
            BaselineSettle,
            ScenarioA_Setup,
            ScenarioA_PanelOpenWait,
            ScenarioA_PanelCloseWait,
            ScenarioA_SavePump,
            ScenarioA_SaveWaitBeforeLoad,
            ScenarioA_AwaitReload,
            ScenarioA_ReloadSettle,
            ScenarioB_PrepareWeapon,
            ScenarioB_SpawnWait,
            ScenarioB_Channeling,
            ScenarioB_AwaitStop,
            ScenarioB_KillWait,
            ScenarioC_EsTrigger,
            ScenarioC_EsDelay1s,
            ScenarioC_EsRecharge3s,
            ScenarioC_PrepareStagger,
            ScenarioC_StaggerWarmup,
            ScenarioC_StaggerCleanupWait,
            ScenarioC_NonHostileSpawnWait,
            ScenarioC_NonHostileFinishWait,
            ScenarioC_NonHostileXpWait,
            ScenarioD_BaselineWait,
            ScenarioD_AllocateAndPoolWait,
            ScenarioD_KeystoneCiVerify,
            ScenarioD_SearchFocusWait,
            ScenarioD_TabCloseWait,
            ScenarioD_SavePump,
            ScenarioD_SaveWaitBeforeLoad,
            ScenarioD_AwaitReload,
            ScenarioD_ReloadSettle,
            ScenarioE_Start,
            ScenarioE_EquipPoolWait,
            ScenarioE_DropWait,
            ScenarioE_PickupWait,
            ScenarioE_CorpseWait,
            ScenarioE_SavePump,
            ScenarioE_SaveWaitBeforeLoad,
            ScenarioE_AwaitReload,
            ScenarioE_ReloadSettle,
            ScenarioE_GearTabCloseWait,
            QuitDelay,
            Done
        };

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        Core::EnergyShieldPool* g_esPool = nullptr;
        Core::KillTracker* g_killTracker = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        const Core::Config* g_config = nullptr;

        std::atomic_bool g_enabled{ false };
        AutoTestConfig g_cfg;
        fs::path g_reportPath;
        std::ofstream g_report;

        Phase g_phase = Phase::Inactive;
        Clock::time_point g_startedAt{};
        Clock::time_point g_phaseEnteredAt{};
        double g_unpausedAccumSec = 0.0;
        bool g_equipRequested = false;
        bool g_drawRequested = false;

        int g_passCount = 0;
        int g_failCount = 0;
        int g_warnCount = 0;

        // SKSE 메시징 스레드 <-> 주 갱신 스레드 공유 상태
        std::mutex g_msgMutex;
        bool g_postLoadSignaled = false;
        bool g_postLoadSucceeded = false;
        std::string g_expectedSaveStem;
        bool g_saveMatched = false;

        // 세이브 재시도 상태(프로토타입 5 검증: NPC 대화 등으로 Save()가 무시되면 2초마다 재시도)
        Clock::time_point g_nextSaveRetryAt{};
        int g_saveAttempts = 0;

        // 시나리오 B 추적 상태
        std::vector<RE::ActorHandle> g_spawnedSkeevers;
        int g_extraSkeeverWavesSpawned = 0;
        std::uint64_t g_teammateHitsBeforeB = 0;
        bool g_observedOarOne = false;
        bool g_observedMagickaDrop = false;
        double g_magickaBeforeChannel = 0.0;
        std::uint32_t g_exileLevelBeforeB = 1;
        std::uint64_t g_exileXpBeforeB = 0;
        std::uint8_t g_cycGemLvlBeforeB = 1;
        std::uint64_t g_cycGemXpBeforeB = 0;
        CycloneTelemetry g_telBeforeB{};

        // 시나리오 C 추적 상태
        bool g_tempEsInjected = false;
        double g_esMaxForC = 0.0;
        double g_esAfterHit = 0.0;
        CycloneTelemetry g_telBeforeStagger{};
        RE::ActorHandle g_foxHandle{};
        RE::FormID g_foxFormId = 0;
        std::uint32_t g_exileLevelBeforeFox = 1;
        std::uint64_t g_exileXpBeforeFox = 0;

        std::string Trim(std::string_view sv)
        {
            std::size_t b = 0;
            while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) {
                ++b;
            }
            std::size_t e = sv.size();
            while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) {
                --e;
            }
            return std::string(sv.substr(b, e - b));
        }

        std::string ToLowerAscii(std::string_view sv)
        {
            std::string out(sv);
            std::transform(out.begin(), out.end(), out.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return out;
        }

        // 세이브 이름 안의 점(.)을 보존하면서 경로 접두사와 끝의 ".ess"만 대소문자 무관하게 떼어낸다.
        std::string StripSaveStem(std::string_view raw)
        {
            while (!raw.empty() && raw.back() == '\0') {
                raw.remove_suffix(1);
            }
            std::string s = Trim(raw);
            if (const auto slash = s.find_last_of("\\/"); slash != std::string::npos) {
                s = s.substr(slash + 1);
            }
            if (s.size() >= 4 && ToLowerAscii(std::string_view(s).substr(s.size() - 4)) == ".ess") {
                s.resize(s.size() - 4);
            }
            return s;
        }

        bool ParseBool(std::string_view sv, bool fallback)
        {
            const auto low = ToLowerAscii(Trim(sv));
            if (low == "true" || low == "1" || low == "yes" || low == "on") return true;
            if (low == "false" || low == "0" || low == "no" || low == "off") return false;
            return fallback;
        }

        void WriteLine(std::string_view tag, const std::string& msg)
        {
            const auto line = std::format("[{}] {}", tag, msg);
            SKSE::log::info("AutoTest {}", line);
            if (g_report.is_open()) {
                g_report << line << '\n';
                g_report.flush();
            }
        }

        void LogInfo(const std::string& msg) { WriteLine("INFO", msg); }
        void LogWarn(const std::string& msg)
        {
            ++g_warnCount;
            WriteLine("WARN", msg);
        }
        void Check(std::string_view name, bool ok, const std::string& detail)
        {
            if (ok) {
                ++g_passCount;
                WriteLine("PASS", std::format("{}: {}", name, detail));
            } else {
                ++g_failCount;
                WriteLine("FAIL", std::format("{}: {}", name, detail));
            }
        }

        void EnterPhase(Phase next)
        {
            g_phase = next;
            g_phaseEnteredAt = Clock::now();
            g_unpausedAccumSec = 0.0;
            g_equipRequested = false;
            g_drawRequested = false;
        }

        double PhaseElapsedSec() noexcept
        {
            return std::chrono::duration<double>(Clock::now() - g_phaseEnteredAt).count();
        }

        void RevertTempEnergyShield()
        {
            if (!g_tempEsInjected) {
                return;
            }
            g_tempEsInjected = false;
            SaveLoad::SyncStateToStatEngine();
            if (g_statEngine && g_dataPack && g_config) {
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            }
            if (g_esPool) {
                g_esPool->Clear();
            }
        }

        void FinishAndScheduleQuit(const std::string& reason)
        {
            if (g_phase == Phase::QuitDelay || g_phase == Phase::Done) {
                return;
            }
            CycloneRuntime::SetAutoTestHeld(false);
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                CycloneRuntime::StopNowOnMainThread(player);
            }
            UiBridge::ClosePanelOnMainThread();
            RevertTempEnergyShield();
            // 검수 Major #4: 비정상 종료/타임아웃 시에도 임시 키스톤(CI/BM 등) 주입이 남지 않도록 원상태로 복원
            SaveLoad::SyncStateToStatEngine();
            if (g_statEngine && g_dataPack && g_config && g_dataPack->IsLoaded()) {
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            }
            {
                std::scoped_lock lock(g_msgMutex);
                g_expectedSaveStem.clear();
                g_saveMatched = false;
            }

            if (!reason.empty()) {
                LogInfo(reason);
            }
            const bool overallOk = (g_failCount == 0 && g_passCount > 0);
            const auto summary = std::format(
                "=== SUMMARY: PASS={} FAIL={} WARN={} RESULT={} ===",
                g_passCount, g_failCount, g_warnCount, overallOk ? "PASS" : "FAIL");
            SKSE::log::info("AutoTest {}", summary);
            if (g_report.is_open()) {
                g_report << summary << '\n';
                g_report.flush();
                g_report.close();
            }

            if (g_cfg.quitWhenDone) {
                EnterPhase(Phase::QuitDelay);
            } else {
                EnterPhase(Phase::Done);
                g_enabled.store(false, std::memory_order_release);
            }
        }

        std::string SaveStemFromMessage(const SKSE::MessagingInterface::Message& msg)
        {
            if (!msg.data || msg.dataLen == 0) {
                return {};
            }
            const std::string_view raw(static_cast<const char*>(msg.data), msg.dataLen);
            return StripSaveStem(raw);
        }

        bool ParseMarkerFile(const fs::path& markerPath, AutoTestConfig& out)
        {
            std::ifstream ifs(markerPath);
            if (!ifs) {
                return false;
            }
            std::string line;
            bool firstLine = true;
            while (std::getline(ifs, line)) {
                if (firstLine) {
                    firstLine = false;
                    // UTF-8 BOM 제거
                    if (line.size() >= 3 &&
                        static_cast<unsigned char>(line[0]) == 0xEF &&
                        static_cast<unsigned char>(line[1]) == 0xBB &&
                        static_cast<unsigned char>(line[2]) == 0xBF) {
                        line.erase(0, 3);
                    }
                }
                const auto trimmed = Trim(line);
                if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
                    continue;
                }
                const auto eq = trimmed.find('=');
                if (eq == std::string::npos) {
                    continue;
                }
                const auto key = ToLowerAscii(Trim(std::string_view(trimmed).substr(0, eq)));
                const auto val = Trim(std::string_view(trimmed).substr(eq + 1));
                if (key == "save") {
                    out.baselineSave = StripSaveStem(val);
                } else if (key == "temp_save") {
                    const auto stripped = StripSaveStem(val);
                    if (!stripped.empty()) {
                        out.tempSave = stripped;
                    }
                } else if (key == "quit") {
                    out.quitWhenDone = ParseBool(val, out.quitWhenDone);
                } else if (key == "consume_marker") {
                    out.consumeMarker = ParseBool(val, out.consumeMarker);
                } else if (key == "timeout_sec") {
                    try {
                        const double sec = std::stod(val);
                        if (std::isfinite(sec) && sec >= 30.0 && sec <= 1800.0) {
                            out.globalTimeoutSec = sec;
                        }
                    } catch (...) {
                    }
                }
            }
            return true;
        }

        bool IsPlayerReadyInWorld()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* ui = RE::UI::GetSingleton();
            if (!player || !player->Is3DLoaded() || !ui) {
                return false;
            }
            if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::FaderMenu::MENU_NAME)) {
                return false;
            }
            return true;
        }

        // 인벤토리에서 gemId에 해당하는 순정(0:0) 또는 빈 추적 젬 키를 우선 반환해 중복/가득 찬 소켓 오염을 피한다.
        Core::InventoryItemKey FindCleanGemKeyInPlayerInventory(RE::PlayerCharacter* player, std::string_view gemId)
        {
            const auto inv = GemInventory::ListPlayerGems(player);
            for (const auto& p : inv.plainStacks) {
                if (p.gemId == gemId && p.count > 0) {
                    return Core::InventoryItemKey{};
                }
            }
            for (const auto& t : inv.trackedGems) {
                if (t.gem.gemId == gemId && t.gem.socketedGems.empty()) {
                    return t.key;
                }
            }
            for (const auto& t : inv.trackedGems) {
                if (t.gem.gemId == gemId) {
                    return t.key;
                }
            }
            return Core::InventoryItemKey{};
        }

        void CollectGemIdsRecursive(const Core::GemInstance& gem, std::unordered_set<std::string>& out, std::uint32_t depth = 0)
        {
            if (depth > 4 || gem.gemId.empty()) {
                return;
            }
            out.insert(gem.gemId);
            for (const auto& child : gem.socketedGems) {
                CollectGemIdsRecursive(child, out, depth + 1);
            }
        }

        void SendUiCmd(const json& obj)
        {
            UiBridge::ExecuteCommandForTestOnMainThread(obj.dump());
        }

        // 장착 중인 모든 슬롯의 중첩 소켓 젬까지 역순으로 완전히 분리한 뒤 탈착한다.
        void EnsureCleanTestSlots()
        {
            const auto st = SaveLoad::GetStateCopy();
            for (std::size_t i = 0; i < st.slots.size(); ++i) {
                if (!st.slots[i].occupied) {
                    continue;
                }
                const auto& root = st.slots[i].gem;
                for (std::size_t c = root.socketedGems.size(); c > 0; --c) {
                    const std::size_t childIdx = c - 1;
                    const auto& child = root.socketedGems[childIdx];
                    for (std::size_t gc = child.socketedGems.size(); gc > 0; --gc) {
                        SendUiCmd(json{
                            { "op", "unsocket" },
                            { "slot", i },
                            { "spellIdx", static_cast<std::int32_t>(childIdx) },
                            { "socketIdx", gc - 1 }
                        });
                    }
                    SendUiCmd(json{
                        { "op", "unsocket" },
                        { "slot", i },
                        { "spellIdx", -1 },
                        { "socketIdx", childIdx }
                    });
                }
                SendUiCmd(json{ { "op", "unequip" }, { "slot", i } });
            }
        }

        bool VerifyScenarioASlotLayout(const PersistentState& st, const Core::StatSnapshot* snap, std::string& detailOut)
        {
            const auto& s0 = st.slots[0];
            const auto& s1 = st.slots[1];
            const bool s0Ok = s0.occupied && s0.gem.gemId == "Cyclone" && s0.gem.socketedGems.size() == 2;
            const bool s1Ok = s1.occupied &&
                s1.gem.gemId == "SupportCastOnCriticalStrike" &&
                s1.linkedAttackSlot == 0 &&
                s1.gem.socketedGems.size() == 2 &&
                s1.gem.socketedGems[0].gemId == "IceNova" &&
                s1.gem.socketedGems[0].socketedGems.size() == 1 &&
                s1.gem.socketedGems[0].socketedGems[0].gemId == "SupportConcentratedEffect" &&
                s1.gem.socketedGems[1].gemId == "ShockNova";

            bool snapOk = false;
            if (snap && s0Ok && s1Ok) {
                const bool cycSnap = (snap->slotStats[0].gemId == "Cyclone" &&
                    snap->slotStats[0].baseHitDamage.TotalAverage() > 0.0);
                const bool iceSnap = snap->triggeredSpellStats.contains(s1.gem.socketedGems[0].instanceId);
                const bool shockSnap = snap->triggeredSpellStats.contains(s1.gem.socketedGems[1].instanceId);
                snapOk = cycSnap && iceSnap && shockSnap;
            }

            detailOut = std::format("slot0={}({} sup), slot1={}({} spells, link={}), snapOk={}",
                s0.occupied ? s0.gem.gemId : "empty",
                s0.occupied ? s0.gem.socketedGems.size() : 0,
                s1.occupied ? s1.gem.gemId : "empty",
                s1.occupied ? s1.gem.socketedGems.size() : 0,
                s1.linkedAttackSlot,
                snapOk);
            return s0Ok && s1Ok && snapOk;
        }

        void RestorePlayerVitals(RE::PlayerCharacter* player, bool fullMagicka, bool fullHealth)
        {
            if (!player) {
                return;
            }
            auto* av = player->AsActorValueOwner();
            if (!av) {
                return;
            }
            if (fullHealth) {
                const float dmg = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth);
                if (dmg < -0.5f) {
                    av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -dmg);
                }
            }
            if (fullMagicka) {
                const float dmg = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka);
                if (dmg < -0.5f) {
                    av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka, -dmg);
                }
            }
        }

        bool HasMeleeWeaponEquipped(RE::PlayerCharacter* player)
        {
            if (!player) {
                return false;
            }
            auto* eq = player->GetEquippedObject(false);
            auto* weap = eq ? eq->As<RE::TESObjectWEAP>() : nullptr;
            return weap && weap->IsMelee() && !weap->IsHandToHandMelee();
        }

        // 근접 무기 장착 및 발도 완료 시 true 반환(장착/발도 요청은 단계당 1회만 수행)
        bool PumpMeleeWeaponDrawn(RE::PlayerCharacter* player)
        {
            if (!player) {
                return false;
            }
            if (!HasMeleeWeaponEquipped(player)) {
                if (!g_equipRequested) {
                    g_equipRequested = true;
                    auto* sword = RE::TESForm::LookupByID<RE::TESObjectWEAP>(kIronSwordId);
                    auto* equipMgr = RE::ActorEquipManager::GetSingleton();
                    if (!sword || !equipMgr) {
                        LogWarn("Could not look up IronSword (0x00012EB7) or ActorEquipManager.");
                        return false;
                    }
                    if (player->GetItemCount(sword) <= 0) {
                        player->AddObjectToContainer(sword, nullptr, 1, nullptr);
                    }
                    equipMgr->EquipObject(player, sword, nullptr, 1, nullptr, false, true, false, true);
                    LogInfo("Equipped IronSword (0x00012EB7) for Cyclone melee check.");
                }
                if (!HasMeleeWeaponEquipped(player)) {
                    return false;
                }
            }

            const auto* actorState = player->AsActorState();
            const bool drawn = actorState && actorState->IsWeaponDrawn();
            if (!drawn) {
                if (!g_drawRequested) {
                    g_drawRequested = true;
                    player->DrawWeaponMagicHands(true);
                }
                return false;
            }
            return true;
        }

        RE::ActorHandle SpawnNpcInFrontOfPlayer(
            RE::PlayerCharacter* player,
            RE::FormID npcFormId,
            float forwardDist,
            float sideOffset,
            float extraPermHealth,
            bool startCombat)
        {
            if (!player) {
                return {};
            }
            auto* npcBase = RE::TESForm::LookupByID<RE::TESNPC>(npcFormId);
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!npcBase || !dataHandler) {
                return {};
            }

            const float yaw = player->GetAngleZ();
            const float fx = std::sin(yaw);
            const float fy = std::cos(yaw);
            const float rx = fy;
            const float ry = -fx;

            RE::NiPoint3 pos = player->GetPosition();
            pos.x += fx * forwardDist + rx * sideOffset;
            pos.y += fy * forwardDist + ry * sideOffset;
            pos.z += 8.0f;

            const RE::NiPoint3 rot{ 0.0f, 0.0f, yaw + 3.14159265f };
            auto refHandle = dataHandler->CreateReferenceAtLocation(
                npcBase,
                pos,
                rot,
                player->GetParentCell(),
                player->GetWorldspace(),
                nullptr,
                nullptr,
                RE::ObjectRefHandle{},
                false,
                true);

            auto ref = refHandle.get();
            auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
            if (!actor) {
                return {};
            }
            if (extraPermHealth > 0.0f) {
                if (auto* av = actor->AsActorValueOwner()) {
                    av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kHealth, extraPermHealth);
                }
            }
            if (startCombat) {
                actor->StartCombat(player);
            }
            return actor->GetHandle();
        }

        void FinishRemainingSpawnedSkeevers(RE::PlayerCharacter* player)
        {
            if (!player) {
                return;
            }
            for (const auto& h : g_spawnedSkeevers) {
                auto actor = h.get();
                if (actor && !actor->IsDead()) {
                    if (!actor->IsHostileToActor(player)) {
                        actor->StartCombat(player);
                    }
                    if (g_killTracker) {
                        g_killTracker->RecordFirstHitHostility(actor->GetFormID(), true);
                    }
                    (void)actor->DoDamage(1200.0f, player, true);
                }
            }
        }

        bool RequestLoadSave(const std::string& saveStem)
        {
            auto* mgr = RE::BGSSaveLoadManager::GetSingleton();
            if (!mgr) {
                return false;
            }
            {
                std::scoped_lock lock(g_msgMutex);
                g_postLoadSignaled = false;
                g_postLoadSucceeded = false;
            }
            // 누락 모드 경고 모달로 멈추지 않도록 a_checkForMods=false로 로드한다.
            mgr->Load(saveStem.c_str(), false);
            return true;
        }

        void BeginScenarioC_Es()
        {
            const auto st = SaveLoad::GetStateCopy();
            g_esMaxForC = 0.0;
            g_esAfterHit = 0.0;
            if (g_statEngine && g_dataPack && g_config) {
                Core::Layer1Input l1;
                l1.exileLevel = st.exileLevel;
                l1.className = st.className.empty() ? "Scion" : st.className;
                l1.slots = st.slots;
                l1.flatEnergyShield = 100.0;
                g_statEngine->SetLayer1(l1);
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                g_tempEsInjected = true;
            }
            EnterPhase(Phase::ScenarioC_EsTrigger);
        }

        void BeginScenarioC_Stagger()
        {
            RevertTempEnergyShield();
            EnterPhase(Phase::ScenarioC_PrepareStagger);
        }

        void DispelFrenzyIfActive(RE::PlayerCharacter* player)
        {
            if (!player) {
                return;
            }
            CycloneRuntime::ForceRefreshYieldCheckOnMainThread(player);
            if (!CycloneRuntime::IsYieldingToFrenzy()) {
                return;
            }
            auto* handler = RE::TESDataHandler::GetSingleton();
            auto* frenzyEffect = handler ? handler->LookupForm<RE::EffectSetting>(kFrenzyEffectLocalId, kFrenzyPlugin) : nullptr;
            auto* mt = player->AsMagicTarget();
            int dispelled = 0;
            if (mt && frenzyEffect) {
                if (auto* list = mt->GetActiveEffectList()) {
                    for (auto* ae : *list) {
                        if (ae && ae->GetBaseObject() == frenzyEffect && !ae->flags.any(RE::ActiveEffect::Flag::kInactive)) {
                            ae->Dispel(true);
                            ++dispelled;
                        }
                    }
                }
            }
            CycloneRuntime::ForceRefreshYieldCheckOnMainThread(player);
            LogInfo(std::format(
                "Baseline save had BerserkerFrenzy active (Decision 63 YieldToFrenzy active); dispelled={} effect(s), yieldingNow={}.",
                dispelled, CycloneRuntime::IsYieldingToFrenzy()));
        }

        std::string DescribeCycloneBlockers(RE::PlayerCharacter* player)
        {
            if (!player) {
                return "player=null";
            }
            const auto* st = player->AsActorState();
            const auto* av = player->AsActorValueOwner();
            return std::format("yielding={}, drawn={}, melee={}, blocking={}, staggered={}, magicka={:.1f}",
                CycloneRuntime::IsYieldingToFrenzy(),
                st && st->IsWeaponDrawn(),
                HasMeleeWeaponEquipped(player),
                player->IsBlocking(),
                st && st->IsStaggered(),
                av ? av->GetActorValue(RE::ActorValue::kMagicka) : -1.0f);
        }

        void BeginNonHostileTest(RE::PlayerCharacter* player)
        {
            const auto stBeforeFox = SaveLoad::GetStateCopy();
            g_exileLevelBeforeFox = stBeforeFox.exileLevel;
            g_exileXpBeforeFox = stBeforeFox.exileXp;
            g_foxHandle = SpawnNpcInFrontOfPlayer(player, kNeutralNpcId, 70.0f, 0.0f, 0.0f, false);
            auto npc = g_foxHandle.get();
            g_foxFormId = npc ? npc->GetFormID() : 0;
            EnterPhase(Phase::ScenarioC_NonHostileSpawnWait);
        }

        // 시나리오 D(3단계 패시브 트리) 관측 상태
        std::uint32_t g_treeStrBefore = 0;
        double g_treePoeLifeBefore = 0.0;
        double g_treePermHealthBefore = 0.0;
        bool g_d6OpenedOnTree = false;
        bool g_d6SurvivedSearchFocus = false;
        bool g_d6ImeVerified = false;
        bool g_d6SwitchedToSkills = false;
        std::vector<std::uint32_t> g_d7ExpectedNodes;
        std::unordered_map<std::uint32_t, std::uint32_t> g_d7ExpectedMasteries;

        // 시나리오 E(4단계 아이템·제작·장비 동기화·바닥 드랍·UI) 관측 상태
        std::uint32_t g_eStrBefore = 0;
        double g_ePoeLifeBefore = 0.0;
        double g_eFireResBefore = 0.0;
        double g_ePermHpBefore = 0.0;
        Core::InventoryItemKey g_eHelmetKey{};
        Core::InventoryItemKey g_eIdentifiedKey{};
        Core::InventoryItemKey g_eCraftedDaggerKey{};
        Core::InventoryItemKey g_eDupOrigKey{};
        Core::PoEItemRecord g_eDupExpectedRec{};
        RE::ObjectRefHandle g_eDroppedRefHandle{};
        bool g_eDroppedWorldStamped = false;
        RE::ActorHandle g_eCorpseHandle{};
        std::vector<TrackedItemEntry> g_eExpectedTrackedItems;
        bool g_e8CosaveOk = false;
        bool g_e8PayloadOk = false;
        bool g_e8OpenedGearTab = false;
        std::string g_e8Detail;

        void BeginScenarioE()
        {
            EnterPhase(Phase::ScenarioE_Start);
        }

        void BeginScenarioD_AscendancyAndSave()
        {
            // D8: 1차 보스 이정표(전직 +2점) 부여 -> Juggernaut 선택 + 전직 노드 2개(32115, 44297) 할당·스탯 반영 확인
            const auto snapBeforeAsc = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
            const double armourBeforeAsc = snapBeforeAsc ? snapBeforeAsc->armour : 0.0;
            const double atkSpdBeforeAsc = snapBeforeAsc ? snapBeforeAsc->slotStats[0].attackSpeedMult : 0.0;

            SaveLoad::UpdateState([](PersistentState& st) {
                st.tree.dragonKillCount = std::max<std::uint32_t>(st.tree.dragonKillCount, 1u);
                st.tree.ascendancyMilestonesBitmask |= 0x1u;
            });
            SendUiCmd(json{ { "op", "treeSelectAscendancy" }, { "ascendancyId", "Juggernaut" } });
            SendUiCmd(json{ { "op", "treeAllocatePath" }, { "nodes", json::array({ 32115, 44297 }) } });

            const auto stAsc = SaveLoad::GetStateCopy();
            const auto snapAfterAsc = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
            const auto spentAsc = g_treeData ? Core::PassiveTree::ComputeSpentAscendancyPoints(*g_treeData, stAsc.tree) : 0;
            const auto earnedAsc = g_config ? Core::PassiveTree::ComputeEarnedAscendancyPoints(stAsc.tree, *g_config) : 2;
            const auto availAsc = (g_treeData && g_config)
                ? Core::PassiveTree::ComputeAvailableAscendancyPoints(*g_treeData, stAsc.tree, *g_config)
                : -1;
            const bool ascStatUp = snapAfterAsc &&
                (snapAfterAsc->armour > armourBeforeAsc + 0.5) &&
                (snapAfterAsc->slotStats[0].attackSpeedMult > atkSpdBeforeAsc + 0.01);
            const bool d8Ok = (stAsc.tree.ascendancyId == "Juggernaut") &&
                (spentAsc == 2) && (availAsc == (earnedAsc - 2)) && ascStatUp;
            Check("D8_AscendancyMilestoneAndAssign", d8Ok,
                std::format("asc='{}', spentAsc={}, availAsc={}/{}, armour {:.1f}->{:.1f}, atkSpd {:.3f}->{:.3f}",
                    stAsc.tree.ascendancyId, spentAsc, availAsc, earnedAsc, armourBeforeAsc,
                    snapAfterAsc ? snapAfterAsc->armour : 0.0,
                    atkSpdBeforeAsc,
                    snapAfterAsc ? snapAfterAsc->slotStats[0].attackSpeedMult : 0.0));

            // D7 준비: 현재 트리 상태를 기록한 뒤 임시 세이브 저장 -> 재로드 왕복 검증
            g_d7ExpectedNodes = stAsc.tree.allocatedNodes;
            g_d7ExpectedMasteries = stAsc.tree.masterySelections;
            g_saveAttempts = 0;
            g_nextSaveRetryAt = Clock::now();
            {
                std::scoped_lock lock(g_msgMutex);
                g_expectedSaveStem = g_cfg.tempSave;
                g_saveMatched = false;
            }
            EnterPhase(Phase::ScenarioD_SavePump);
        }

        void BeginScenarioD()
        {
            // D1: 트리 데이터 로드 검증
            std::size_t ascTotal = 0;
            if (g_treeData && g_treeData->IsLoaded()) {
                for (const auto& c : g_treeData->Classes()) {
                    ascTotal += c.ascendancies.size();
                }
            }
            const bool d1Ok = g_treeData && g_treeData->IsLoaded() &&
                g_treeData->Classes().size() == 7 &&
                ascTotal == 19 &&
                g_treeData->NodeCount() > 2300 &&
                g_treeData->MasteryEffectCount() == 353;
            Check("D1_TreeDataLoaded", d1Ok,
                std::format("loaded={}, classes={}, ascendancies={}, nodes={}, masteries={}",
                    g_treeData && g_treeData->IsLoaded(),
                    g_treeData ? g_treeData->Classes().size() : 0,
                    ascTotal,
                    g_treeData ? g_treeData->NodeCount() : 0,
                    g_treeData ? g_treeData->MasteryEffectCount() : 0));

            if (!d1Ok || !g_statEngine || !g_dataPack || !g_config) {
                FinishAndScheduleQuit("Scenario D aborted because PassiveTreeData was not loaded.");
                return;
            }

            // 시나리오 D 전용: 유배자 레벨 50(49 포인트)으로 설정하고 트리 초기화 후 기준 풀 동기화를 기다린다.
            SaveLoad::UpdateState([](PersistentState& st) {
                st.exileLevel = 50;
                st.tree = Core::PassiveTreeState{};
                st.className = "Scion";
                st.unallocatedPassivePoints = 49;
            });
            SaveLoad::SyncStateToStatEngine();
            (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                RestorePlayerVitals(player, true, true);
            }
            EnterPhase(Phase::ScenarioD_BaselineWait);
        }
    }

    void AutoTest::Install(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        Core::EnergyShieldPool* esPool,
        Core::KillTracker* killTracker,
        const Core::DataPack* dataPack,
        const Core::PassiveTreeData* treeData,
        const Core::ItemData* itemData,
        const Core::Config* config)
    {
        g_queue = queue;
        g_statEngine = statEngine;
        g_esPool = esPool;
        g_killTracker = killTracker;
        g_dataPack = dataPack;
        g_treeData = treeData;
        g_itemData = itemData;
        g_config = config;

        const auto logDir = SKSE::log::log_directory();
        if (!logDir) {
            return;
        }
        const auto markerPath = *logDir / kMarkerFilename;
        std::error_code ec;
        if (!fs::is_regular_file(markerPath, ec)) {
            return;
        }

        AutoTestConfig parsed;
        if (!ParseMarkerFile(markerPath, parsed)) {
            SKSE::log::warn("AutoTest: marker '{}' found but unreadable; ignoring.", markerPath.string());
            return;
        }

        if (parsed.consumeMarker) {
            fs::remove(markerPath, ec);
        }

        g_cfg = std::move(parsed);
        g_reportPath = *logDir / kReportFilename;
        const auto prevPath = *logDir / kPrevReportFilename;
        if (fs::exists(g_reportPath, ec)) {
            fs::rename(g_reportPath, prevPath, ec);
        }
        g_report.open(g_reportPath, std::ios::out | std::ios::trunc);

        g_passCount = 0;
        g_failCount = 0;
        g_warnCount = 0;
        g_startedAt = Clock::now();
        g_enabled.store(true, std::memory_order_release);

        LogInfo(std::format("SkyrimPoE Stage 4 AutoTest armed (save='{}', temp_save='{}', quit={}, timeout={:.0f}s).",
            g_cfg.baselineSave, g_cfg.tempSave, g_cfg.quitWhenDone, g_cfg.globalTimeoutSec));

        if (g_cfg.baselineSave.empty()) {
            Check("A0_MarkerConfig", false, "Marker file missing required 'save=<stem>' entry.");
            FinishAndScheduleQuit("Aborting due to missing baseline save.");
            return;
        }

        EnterPhase(Phase::AwaitMainMenu);
    }

    bool AutoTest::IsEnabled() noexcept
    {
        return g_enabled.load(std::memory_order_acquire);
    }

    void AutoTest::OnPreLoadGame()
    {
        if (!IsEnabled()) {
            return;
        }
        std::scoped_lock lock(g_msgMutex);
        g_postLoadSignaled = false;
        g_postLoadSucceeded = false;
    }

    void AutoTest::OnPostLoadGame(bool succeeded)
    {
        if (!IsEnabled()) {
            return;
        }
        std::scoped_lock lock(g_msgMutex);
        g_postLoadSignaled = true;
        g_postLoadSucceeded = succeeded;
    }

    void AutoTest::OnSaveGame(const SKSE::MessagingInterface::Message& msg)
    {
        if (!IsEnabled()) {
            return;
        }
        const auto stem = SaveStemFromMessage(msg);
        if (stem.empty()) {
            return;
        }
        std::scoped_lock lock(g_msgMutex);
        if (!g_expectedSaveStem.empty() && ToLowerAscii(stem) == ToLowerAscii(g_expectedSaveStem)) {
            g_saveMatched = true;
        }
    }

    void AutoTest::Tick(RE::Main* main, double deltaSec)
    {
        if (!IsEnabled() || g_phase == Phase::Inactive || g_phase == Phase::Done) {
            return;
        }

        if (g_phase == Phase::QuitDelay) {
            if (PhaseElapsedSec() >= 2.0) {
                EnterPhase(Phase::Done);
                g_enabled.store(false, std::memory_order_release);
                if (auto* m = main ? main : RE::Main::GetSingleton()) {
                    m->GetRuntimeData().quitGame = true;
                }
            }
            return;
        }

        const double totalSec = std::chrono::duration<double>(Clock::now() - g_startedAt).count();
        if (totalSec > g_cfg.globalTimeoutSec) {
            Check("GlobalTimeout", false, std::format("Exceeded {:.0f}s in phase {}",
                g_cfg.globalTimeoutSec, static_cast<int>(g_phase)));
            FinishAndScheduleQuit("Global timeout reached.");
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        const bool paused = ui && ui->GameIsPaused();
        // MainUpdateHook의 0.25s 상한과 동일한 deltaSec를 그대로 누적해 EnergyShieldPool::Tick과 시계 편차를 없앤다.
        if (!paused && std::isfinite(deltaSec) && deltaSec > 0.0) {
            g_unpausedAccumSec += deltaSec;
        }

        switch (g_phase) {
        case Phase::AwaitMainMenu: {
            if (ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
                LogInfo("MainMenu detected; waiting 2.0s before loading baseline save.");
                EnterPhase(Phase::MenuDelay);
            } else if (IsPlayerReadyInWorld()) {
                LogInfo("Player already in world; proceeding to load baseline save.");
                EnterPhase(Phase::MenuDelay);
            }
            break;
        }

        case Phase::MenuDelay: {
            if (PhaseElapsedSec() < 2.0) {
                break;
            }
            LogInfo(std::format("Loading baseline save '{}'...", g_cfg.baselineSave));
            if (!RequestLoadSave(g_cfg.baselineSave)) {
                Check("A0_LoadBaseline", false, "BGSSaveLoadManager singleton unavailable.");
                FinishAndScheduleQuit("Failed to request baseline save load.");
                return;
            }
            EnterPhase(Phase::AwaitBaselineLoad);
            break;
        }

        case Phase::AwaitBaselineLoad: {
            bool signaled = false;
            bool ok = false;
            {
                std::scoped_lock lock(g_msgMutex);
                signaled = g_postLoadSignaled;
                ok = g_postLoadSucceeded;
            }
            if (signaled && !ok) {
                Check("A0_LoadBaseline", false, std::format("kPostLoadGame reported failure for '{}'.", g_cfg.baselineSave));
                FinishAndScheduleQuit("Baseline save failed to load.");
                return;
            }
            if (signaled && ok && IsPlayerReadyInWorld()) {
                LogInfo("Baseline save loaded; settling unpaused world + Prisma DOM.");
                EnterPhase(Phase::BaselineSettle);
            } else if (PhaseElapsedSec() > 90.0) {
                Check("A0_LoadBaseline", false, "Timed out waiting for baseline save load.");
                FinishAndScheduleQuit("Baseline load timeout.");
            }
            break;
        }

        case Phase::BaselineSettle: {
            const bool minSettled = (g_unpausedAccumSec >= 2.5);
            const bool domReady = UiBridge::IsDomReady();
            if (!minSettled && PhaseElapsedSec() < 8.0) {
                break;
            }
            if (!domReady && PhaseElapsedSec() < 8.0) {
                break;
            }
            EnterPhase(Phase::ScenarioA_Setup);
            break;
        }

        // ================= SCENARIO A =================
        case Phase::ScenarioA_Setup: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !g_statEngine || !g_dataPack || !g_config) {
                Check("A0_RuntimeReady", false, "Core singletons unavailable.");
                FinishAndScheduleQuit("Runtime not ready.");
                return;
            }

            // A1. ESP 폼 검증
            std::size_t espFound = 0;
            if (FormIds::Lookup<RE::EffectSetting>(FormIds::kProxySpellEffect)) ++espFound;
            if (FormIds::Lookup<RE::EffectSetting>(FormIds::kProxyPowerEffect)) ++espFound;
            for (const auto id : FormIds::kProxySpells) {
                if (FormIds::Lookup<RE::SpellItem>(id)) ++espFound;
            }
            for (const auto id : FormIds::kProxyPowers) {
                if (FormIds::Lookup<RE::SpellItem>(id)) ++espFound;
            }
            for (const auto& e : FormIds::kStage2GemForms) {
                if (FormIds::Lookup<RE::TESObjectMISC>(e.localFormId)) ++espFound;
            }
            if (FormIds::Lookup<RE::BGSExplosion>(FormIds::kVfxIceNova)) ++espFound;
            if (FormIds::Lookup<RE::BGSExplosion>(FormIds::kVfxShockNova)) ++espFound;
            const std::size_t espExpected = 2 + FormIds::kProxySpells.size() + FormIds::kProxyPowers.size() +
                FormIds::kStage2GemForms.size() + 2;
            Check("A1_EspForms", espFound == espExpected, std::format("{}/{} forms found", espFound, espExpected));

            // A2. 로드 직후 Variable09 == 0
            const float var09 = player->AsActorValueOwner()
                ? player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kVariable09)
                : -1.0f;
            Check("A2_Variable09ZeroAfterLoad", std::abs(var09) < 1e-3f, std::format("Variable09={:.3f}", var09));

            // A3. 시작 젬 9종 지급 확인(중첩 소켓된 추적 젬도 재귀적으로 포함해 카운트)
            EnsureCleanTestSlots();
            const auto stInit = SaveLoad::GetStateCopy();
            const auto invInit = GemInventory::ListPlayerGems(player);
            std::unordered_set<std::string> ownedGemIds;
            for (const auto& t : invInit.trackedGems) {
                CollectGemIdsRecursive(t.gem, ownedGemIds);
            }
            for (const auto& p : invInit.plainStacks) {
                if (p.count > 0) {
                    ownedGemIds.insert(p.gemId);
                }
            }
            std::size_t distinctStarterGems = 0;
            for (const auto& entry : FormIds::kStage2GemForms) {
                if (ownedGemIds.contains(std::string(entry.gemId))) {
                    ++distinctStarterGems;
                }
            }
            Check("A3_StarterGemsGiven", stInit.starterGemsGiven && distinctStarterGems == FormIds::kStage2GemForms.size(),
                std::format("starterGemsGiven={}, distinct={}/{} (tracked={}, plain={})",
                    stInit.starterGemsGiven, distinctStarterGems, FormIds::kStage2GemForms.size(),
                    invInit.trackedGems.size(), invInit.plainStacks.size()));

            // A4. Prisma DOM & HUD 확인
            const bool domReady = UiBridge::IsDomReady();
            const bool gameActive = !main || main->GetRuntimeData().gameActive;
            if (gameActive) {
                Check("A4_PrismaDomAndHud", domReady && UiBridge::IsHudShown(),
                    std::format("domReady={}, hudShown={}, gameActive={}", domReady, UiBridge::IsHudShown(), gameActive));
            } else {
                Check("A4_PrismaDomReady", domReady, std::format("domReady={}, gameActive=false (window unfocused)", domReady));
                LogWarn("Window unfocused (gameActive=false); HUD visibility depends on active focus.");
            }

            // A5. 실제 UiBridge JSON 경로로 Cyclone + 보조 2개, CoC + IceNova[ConcEff] + ShockNova 장착·연결
            auto equipGem = [&](std::uint8_t slot, std::string_view gemId) {
                const auto k = FindCleanGemKeyInPlayerInventory(player, gemId);
                SendUiCmd(json{
                    { "op", "equip" }, { "slot", slot },
                    { "ownerFormId", k.ownerFormId }, { "uniqueId", k.uniqueId },
                    { "gemId", gemId }
                });
            };
            auto socketGem = [&](std::uint8_t slot, std::int32_t spellIdx, std::string_view gemId) {
                const auto k = FindCleanGemKeyInPlayerInventory(player, gemId);
                SendUiCmd(json{
                    { "op", "socket" }, { "slot", slot }, { "spellIdx", spellIdx },
                    { "ownerFormId", k.ownerFormId }, { "uniqueId", k.uniqueId },
                    { "gemId", gemId }
                });
            };

            equipGem(0, "Cyclone");
            socketGem(0, -1, "SupportMeleePhysicalDamage");
            socketGem(0, -1, "SupportIncreasedCriticalStrikes");

            equipGem(1, "SupportCastOnCriticalStrike");
            SendUiCmd(json{ { "op", "link" }, { "slot", 1 }, { "attackSlot", 0 } });
            socketGem(1, -1, "IceNova");
            socketGem(1, -1, "ShockNova");
            socketGem(1, 0, "SupportConcentratedEffect");

            const auto stAfterEquip = SaveLoad::GetStateCopy();
            const auto snapAfterEquip = g_statEngine->GetSnapshot();
            std::string layoutDetail;
            const bool layoutOk = VerifyScenarioASlotLayout(stAfterEquip, snapAfterEquip.get(), layoutDetail);
            Check("A5_EquipSocketLink", layoutOk, layoutDetail);

            // A6. CoC 탈착 -> 정확히 방금 뺀 instanceId의 인벤토리 데이터 젬 보존 확인 -> 재장착·재연결 복원 확인
            const std::uint64_t expectedCocInstanceId = stAfterEquip.slots[1].occupied
                ? stAfterEquip.slots[1].gem.instanceId
                : 0;
            SendUiCmd(json{ { "op", "unequip" }, { "slot", 1 } });
            const auto invAfterUnequip = GemInventory::ListPlayerGems(player);
            Core::InventoryItemKey trackedCocKey{};
            bool trackedCocHasChildren = false;
            for (const auto& t : invAfterUnequip.trackedGems) {
                if (expectedCocInstanceId != 0 && t.gem.instanceId == expectedCocInstanceId) {
                    trackedCocKey = t.key;
                    trackedCocHasChildren = (t.gem.socketedGems.size() == 2 &&
                        t.gem.socketedGems[0].gemId == "IceNova" &&
                        t.gem.socketedGems[0].socketedGems.size() == 1);
                    break;
                }
            }
            if (trackedCocKey.IsValid()) {
                SendUiCmd(json{
                    { "op", "equip" }, { "slot", 1 },
                    { "ownerFormId", trackedCocKey.ownerFormId },
                    { "uniqueId", trackedCocKey.uniqueId },
                    { "gemId", "SupportCastOnCriticalStrike" }
                });
                SendUiCmd(json{ { "op", "link" }, { "slot", 1 }, { "attackSlot", 0 } });
            }
            const auto stAfterRoundTrip = SaveLoad::GetStateCopy();
            const auto snapAfterRoundTrip = g_statEngine->GetSnapshot();
            std::string rtDetail;
            const bool rtOk = trackedCocKey.IsValid() && trackedCocHasChildren &&
                VerifyScenarioASlotLayout(stAfterRoundTrip, snapAfterRoundTrip.get(), rtDetail);
            Check("A6_UnequipReequipTrackedData", rtOk,
                std::format("instId={}, trackedKey={:08X}:{}, childrenPreserved={}, {}",
                    expectedCocInstanceId, trackedCocKey.ownerFormId, trackedCocKey.uniqueId,
                    trackedCocHasChildren, rtDetail));

            // A7~A8. Prisma UI 패널 열기/닫기 및 HUD 유지 확인
            if (gameActive && UiBridge::IsDomReady()) {
                (void)UiBridge::TryOpenPanelOnMainThread();
                EnterPhase(Phase::ScenarioA_PanelOpenWait);
            } else {
                LogWarn("A7/A8 panel open/close focus check skipped (gameActive or domReady false).");
                g_saveAttempts = 0;
                g_nextSaveRetryAt = Clock::now();
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem = g_cfg.tempSave;
                    g_saveMatched = false;
                }
                EnterPhase(Phase::ScenarioA_SavePump);
            }
            break;
        }

        case Phase::ScenarioA_PanelOpenWait: {
            if (PhaseElapsedSec() < 0.4) {
                break;
            }
            const bool panelOpen = UiBridge::IsPanelOpen();
            const bool gamePaused = ui && ui->GameIsPaused();
            Check("A7_PanelOpenAndPause", panelOpen && gamePaused,
                std::format("panelOpen={}, gamePaused={}", panelOpen, gamePaused));

            UiBridge::ClosePanelOnMainThread();
            EnterPhase(Phase::ScenarioA_PanelCloseWait);
            break;
        }

        case Phase::ScenarioA_PanelCloseWait: {
            if (PhaseElapsedSec() < 0.4) {
                break;
            }
            const bool panelClosed = !UiBridge::IsPanelOpen();
            const bool hudStillShown = UiBridge::IsHudShown();
            Check("A8_PanelCloseKeepsHud", panelClosed && hudStillShown,
                std::format("panelClosed={}, hudShown={}", panelClosed, hudStillShown));

            g_saveAttempts = 0;
            g_nextSaveRetryAt = Clock::now();
            {
                std::scoped_lock lock(g_msgMutex);
                g_expectedSaveStem = g_cfg.tempSave;
                g_saveMatched = false;
            }
            EnterPhase(Phase::ScenarioA_SavePump);
            break;
        }

        case Phase::ScenarioA_SavePump: {
            bool matched = false;
            {
                std::scoped_lock lock(g_msgMutex);
                matched = g_saveMatched;
            }
            if (matched) {
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                LogInfo(std::format("Temp save '{}' confirmed by kSaveGame (attempts={}).", g_cfg.tempSave, g_saveAttempts));
                EnterPhase(Phase::ScenarioA_SaveWaitBeforeLoad);
                break;
            }
            if (PhaseElapsedSec() > 25.0) {
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                Check("A9_SaveLoadRoundTrip", false,
                    std::format("Timed out waiting for kSaveGame('{}') after {} attempts.", g_cfg.tempSave, g_saveAttempts));
                EnterPhase(Phase::ScenarioB_PrepareWeapon);
                break;
            }
            const auto now = Clock::now();
            if (now >= g_nextSaveRetryAt) {
                if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
                    ++g_saveAttempts;
                    mgr->Save(g_cfg.tempSave.c_str());
                }
                g_nextSaveRetryAt = now + std::chrono::seconds(2);
            }
            break;
        }

        case Phase::ScenarioA_SaveWaitBeforeLoad: {
            if (PhaseElapsedSec() < 1.0) {
                break;
            }
            LogInfo(std::format("Reloading temp save '{}' to verify SPOE co-save round-trip...", g_cfg.tempSave));
            if (!RequestLoadSave(g_cfg.tempSave)) {
                Check("A9_SaveLoadRoundTrip", false, "BGSSaveLoadManager singleton unavailable.");
                EnterPhase(Phase::ScenarioB_PrepareWeapon);
                break;
            }
            EnterPhase(Phase::ScenarioA_AwaitReload);
            break;
        }

        case Phase::ScenarioA_AwaitReload: {
            bool signaled = false;
            bool ok = false;
            {
                std::scoped_lock lock(g_msgMutex);
                signaled = g_postLoadSignaled;
                ok = g_postLoadSucceeded;
            }
            if (signaled && !ok) {
                Check("A9_SaveLoadRoundTrip", false, "kPostLoadGame reported failure on temp save reload.");
                FinishAndScheduleQuit("Reload failed.");
                return;
            }
            if (signaled && ok && IsPlayerReadyInWorld()) {
                EnterPhase(Phase::ScenarioA_ReloadSettle);
            } else if (PhaseElapsedSec() > 90.0) {
                Check("A9_SaveLoadRoundTrip", false, "Timed out waiting for temp save reload.");
                FinishAndScheduleQuit("Reload timeout.");
            }
            break;
        }

        case Phase::ScenarioA_ReloadSettle: {
            if (g_unpausedAccumSec < 2.5 && PhaseElapsedSec() < 8.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto st = SaveLoad::GetStateCopy();
            const auto snap = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
            const float var09 = (player && player->AsActorValueOwner())
                ? player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kVariable09)
                : -1.0f;
            std::string detail;
            const bool layoutOk = VerifyScenarioASlotLayout(st, snap.get(), detail);
            Check("A9_SaveLoadRoundTrip", layoutOk && std::abs(var09) < 1e-3f,
                std::format("var09={:.3f}, {}", var09, detail));

            EnterPhase(Phase::ScenarioB_PrepareWeapon);
            break;
        }

        // ================= SCENARIO B =================
        case Phase::ScenarioB_PrepareWeapon: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                Check("B0_PlayerReady", false, "PlayerCharacter unavailable.");
                FinishAndScheduleQuit("Player unavailable.");
                return;
            }
            RestorePlayerVitals(player, true, true);
            DispelFrenzyIfActive(player);
            const bool weaponReady = PumpMeleeWeaponDrawn(player);
            if (!weaponReady) {
                if (PhaseElapsedSec() < 3.0) {
                    break;
                }
                LogWarn("Melee weapon equip/draw wait reached 3.0s; proceeding to spawn test.");
            }

            g_teammateHitsBeforeB = CombatHooks::GetPlayerToTeammateHitCount();

            // 적대 스키버 6마리 소환(Skyrim.esm 순정 EncSkeever, 반경 40~55u 근접 배치 +500 HP로 안정적 치명타·CoC 표본 확보)
            g_spawnedSkeevers.clear();
            g_extraSkeeverWavesSpawned = 0;
            constexpr std::array<std::pair<float, float>, 6> kOffsets{{
                {  45.0f, -20.0f },
                {  45.0f,  20.0f },
                {  55.0f,   0.0f },
                {  35.0f, -35.0f },
                {  35.0f,  35.0f },
                { -45.0f,   0.0f }
            }};
            for (const auto& [fwd, side] : kOffsets) {
                auto h = SpawnNpcInFrontOfPlayer(player, kEncSkeeverId, fwd, side, 500.0f, true);
                if (h) {
                    g_spawnedSkeevers.push_back(h);
                }
            }
            LogInfo(std::format("Scenario B: spawned {}/6 hostile EncSkeever.", g_spawnedSkeevers.size()));
            EnterPhase(Phase::ScenarioB_SpawnWait);
            break;
        }

        case Phase::ScenarioB_SpawnWait: {
            if (g_unpausedAccumSec < 0.35) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                FinishAndScheduleQuit("Player unavailable.");
                return;
            }
            for (const auto& h : g_spawnedSkeevers) {
                if (auto a = h.get(); a && !a->IsDead() && !a->IsHostileToActor(player)) {
                    a->StartCombat(player);
                }
            }

            RestorePlayerVitals(player, true, true);
            const auto stBefore = SaveLoad::GetStateCopy();
            g_exileLevelBeforeB = stBefore.exileLevel;
            g_exileXpBeforeB = stBefore.exileXp;
            g_cycGemLvlBeforeB = stBefore.slots[0].occupied ? stBefore.slots[0].gem.level : 1;
            g_cycGemXpBeforeB = stBefore.slots[0].occupied ? stBefore.slots[0].gem.xp : 0;
            g_magickaBeforeChannel = player->AsActorValueOwner()
                ? static_cast<double>(player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka))
                : 0.0;
            g_telBeforeB = CycloneRuntime::GetTelemetry();
            g_observedOarOne = false;
            g_observedMagickaDrop = false;

            DispelFrenzyIfActive(player);
            const bool started = CycloneRuntime::TryStartForAutoTest(player);
            if (!started) {
                Check("B1_CycloneStartAndOar", false,
                    std::format("CycloneRuntime::TryStartForAutoTest returned false ({}).", DescribeCycloneBlockers(player)));
                FinishRemainingSpawnedSkeevers(player);
                BeginScenarioC_Es();
                break;
            }
            EnterPhase(Phase::ScenarioB_Channeling);
            break;
        }

        case Phase::ScenarioB_Channeling: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                FinishAndScheduleQuit("Player unavailable.");
                return;
            }
            auto* av = player->AsActorValueOwner();
            if (av) {
                const float var09 = av->GetActorValue(RE::ActorValue::kVariable09);
                if (var09 >= 0.99f) {
                    g_observedOarOne = true;
                }
                const double curMag = static_cast<double>(av->GetActorValue(RE::ActorValue::kMagicka));
                if (curMag < g_magickaBeforeChannel - 0.1) {
                    g_observedMagickaDrop = true;
                }
                const float maxHp = av->GetPermanentActorValue(RE::ActorValue::kHealth);
                if (av->GetActorValue(RE::ActorValue::kHealth) < maxHp * 0.5f) {
                    RestorePlayerVitals(player, false, true);
                }
                if (g_observedMagickaDrop && curMag < 25.0) {
                    RestorePlayerVitals(player, true, false);
                }
            }

            const auto tel = CycloneRuntime::GetTelemetry();
            const bool haveCoc = (tel.cocExecutions > g_telBeforeB.cocExecutions);
            const bool haveTicks = (tel.areaHitTicks >= g_telBeforeB.areaHitTicks + 3);

            // CoC 발동 전에 반경 내 생존 스키버가 2마리 미만이면 최대 6회까지 추가 소환
            if (!haveCoc && g_extraSkeeverWavesSpawned < 6 && g_unpausedAccumSec >= 1.0) {
                const auto pPos = player->GetPosition();
                std::size_t aliveNear = 0;
                for (const auto& h : g_spawnedSkeevers) {
                    if (auto a = h.get(); a && !a->IsDead() && a->GetPosition().GetDistance(pPos) <= 130.0f) {
                        ++aliveNear;
                    }
                }
                if (aliveNear < 2) {
                    ++g_extraSkeeverWavesSpawned;
                    for (float side : { -30.0f, 0.0f, 30.0f }) {
                        if (auto h = SpawnNpcInFrontOfPlayer(player, kEncSkeeverId, 48.0f, side, 500.0f, true)) {
                            g_spawnedSkeevers.push_back(h);
                        }
                    }
                    LogInfo(std::format("Scenario B: spawned extra EncSkeever wave #{} for CoC crit sampling.",
                        g_extraSkeeverWavesSpawned));
                }
            }

            if ((haveCoc && haveTicks && g_unpausedAccumSec >= 1.5) || g_unpausedAccumSec >= 12.0 || !CycloneRuntime::IsActive()) {
                CycloneRuntime::SetAutoTestHeld(false);
                EnterPhase(Phase::ScenarioB_AwaitStop);
            }
            break;
        }

        case Phase::ScenarioB_AwaitStop: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const bool active = CycloneRuntime::IsActive();
            const float var09 = (player && player->AsActorValueOwner())
                ? player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kVariable09)
                : -1.0f;

            if ((active || std::abs(var09) >= 1e-3f) && g_unpausedAccumSec < 2.0) {
                break;
            }

            const auto telAfter = CycloneRuntime::GetTelemetry();
            const auto dTicks = telAfter.areaHitTicks - g_telBeforeB.areaHitTicks;
            const auto dHits = telAfter.dealDamageHits - g_telBeforeB.dealDamageHits;
            const auto dCrits = telAfter.critHits - g_telBeforeB.critHits;
            const auto dCocTrig = telAfter.cocTriggers - g_telBeforeB.cocTriggers;
            const auto dCocExec = telAfter.cocExecutions - g_telBeforeB.cocExecutions;
            const auto dCocHits = telAfter.cocTargetsHit - g_telBeforeB.cocTargetsHit;
            const auto teammateHitsDelta = CombatHooks::GetPlayerToTeammateHitCount() - g_teammateHitsBeforeB;

            Check("B1_CycloneStartAndOar", g_observedOarOne && g_observedMagickaDrop,
                std::format("observedVar09_1={}, magickaDropped={}", g_observedOarOne, g_observedMagickaDrop));
            Check("B2_CycloneAreaHitAndDamage", dTicks > 0 && dHits > 0,
                std::format("areaTicks={}, dealDamageHits={}, crits={}", dTicks, dHits, dCrits));
            Check("B3_CoCTriggeredAndExecuted", dCocTrig > 0 && dCocExec > 0,
                std::format("cocTriggers={}, cocExecutions={}, cocTargetsHit={}", dCocTrig, dCocExec, dCocHits));
            Check("B4_CycloneCleanStop", !active && std::abs(var09) < 1e-3f,
                std::format("active={}, var09={:.3f}", active, var09));
            Check("B5_TeammateZeroDamage", teammateHitsDelta == 0,
                std::format("playerToTeammateHits={}", teammateHitsDelta));

            FinishRemainingSpawnedSkeevers(player);
            EnterPhase(Phase::ScenarioB_KillWait);
            break;
        }

        case Phase::ScenarioB_KillWait: {
            const auto stAfter = SaveLoad::GetStateCopy();
            const bool exileGained = (stAfter.exileLevel > g_exileLevelBeforeB) || (stAfter.exileXp > g_exileXpBeforeB);
            const auto cycLvlNow = stAfter.slots[0].occupied ? stAfter.slots[0].gem.level : 0;
            const auto cycXpNow = stAfter.slots[0].occupied ? stAfter.slots[0].gem.xp : 0;
            const bool gemGained = (cycLvlNow > g_cycGemLvlBeforeB) || (cycXpNow > g_cycGemXpBeforeB);

            // 스카이림 TESDeathEvent(dead=true)는 사망 모션(~2.5초) 뒤에 발송되므로 최대 5초까지 대기한다.
            if (!(exileGained && gemGained) && g_unpausedAccumSec < 5.0 && PhaseElapsedSec() < 8.0) {
                break;
            }

            Check("B6_KillTrackerExileAndGemXp", exileGained && gemGained,
                std::format("wait={:.2f}s, exile Lv{} XP {} -> Lv{} XP {}, Cyclone Lv{} XP {} -> Lv{} XP {}",
                    g_unpausedAccumSec,
                    g_exileLevelBeforeB, g_exileXpBeforeB, stAfter.exileLevel, stAfter.exileXp,
                    g_cycGemLvlBeforeB, g_cycGemXpBeforeB, cycLvlNow, cycXpNow));

            BeginScenarioC_Es();
            break;
        }

        // ================= SCENARIO C =================
        case Phase::ScenarioC_EsTrigger: {
            if (!g_esPool) {
                Check("C1_EsAbsorbDifficultyAligned", false, "EnergyShieldPool unavailable.");
                BeginScenarioC_Stagger();
                break;
            }
            const auto esSnap = g_esPool->Snapshot();
            if (esSnap.max <= 0.0 && g_unpausedAccumSec < 1.0 && PhaseElapsedSec() < 3.0) {
                break;
            }
            g_esMaxForC = esSnap.max;
            if (g_esMaxForC <= 0.0) {
                Check("C1_EsAbsorbDifficultyAligned", false, "esMax remained 0.0 after temporary ES injection.");
                Check("C2_EsRechargeDelayHoldAt1s", false, "Skipped because esMax == 0.");
                Check("C3_EsRechargeActiveAfter2s", false, "Skipped because esMax == 0.");
                BeginScenarioC_Stagger();
                break;
            }

            g_esPool->ResetFull(g_esMaxForC);
            const double diffMult = SkillExecution::GetDifficultyMultiplier(true);
            const auto res = g_esPool->AbsorbDamage(10.0, diffMult, false);
            g_esAfterHit = g_esPool->Snapshot().current;

            const double expectedAbsorb = std::min(g_esMaxForC, 10.0 * diffMult);
            const bool c1Ok = (std::abs(res.absorbedEffectiveDamage - expectedAbsorb) < 0.1) &&
                (g_esAfterHit < g_esMaxForC - 0.1);
            Check("C1_EsAbsorbDifficultyAligned", c1Ok,
                std::format("esMax={:.2f}, diffMult={:.2f}, absorbed={:.2f} (exp {:.2f}), esAfter={:.2f}",
                    g_esMaxForC, diffMult, res.absorbedEffectiveDamage, expectedAbsorb, g_esAfterHit));

            EnterPhase(Phase::ScenarioC_EsDelay1s);
            break;
        }

        case Phase::ScenarioC_EsDelay1s: {
            if (!g_esPool) {
                BeginScenarioC_Stagger();
                break;
            }
            const auto snap = g_esPool->Snapshot();
            // EnergyShieldPool 자체 시계(timeSinceLastHit)를 직접 관측해 프레임 스파이크 편차를 원천 차단한다.
            if (snap.timeSinceLastHit < 1.0 && PhaseElapsedSec() < 5.0) {
                break;
            }
            const bool holdOk = (g_esMaxForC > 0.0) &&
                (snap.timeSinceLastHit >= 1.0) &&
                (snap.timeSinceLastHit < snap.rechargeDelay) &&
                (std::abs(snap.current - g_esAfterHit) < 0.05);
            Check("C2_EsRechargeDelayHoldAt1s", holdOk,
                std::format("t={:.2f}s (< {:.2f}s), es={:.2f} (expected {:.2f})",
                    snap.timeSinceLastHit, snap.rechargeDelay, snap.current, g_esAfterHit));
            EnterPhase(Phase::ScenarioC_EsRecharge3s);
            break;
        }

        case Phase::ScenarioC_EsRecharge3s: {
            if (!g_esPool) {
                BeginScenarioC_Stagger();
                break;
            }
            const auto snap = g_esPool->Snapshot();
            if (snap.timeSinceLastHit < (snap.rechargeDelay + 0.8) && PhaseElapsedSec() < 8.0) {
                break;
            }
            Check("C3_EsRechargeActiveAfter2s", snap.current > g_esAfterHit + 0.5,
                std::format("t={:.2f}s, es={:.2f} (afterHit={:.2f})", snap.timeSinceLastHit, snap.current, g_esAfterHit));

            BeginScenarioC_Stagger();
            break;
        }

        case Phase::ScenarioC_PrepareStagger: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                FinishAndScheduleQuit("Player unavailable.");
                return;
            }
            RestorePlayerVitals(player, true, true);
            DispelFrenzyIfActive(player);
            // 자동 납도 모드 등으로 무기가 들어갔을 수 있으므로 경직 시험 전 근접 발도를 다시 보장한다.
            if (!PumpMeleeWeaponDrawn(player) && PhaseElapsedSec() < 2.5) {
                break;
            }
            const bool started = CycloneRuntime::TryStartForAutoTest(player);
            if (!started) {
                Check("C4_LowStaggerSwallowed", false,
                    std::format("Could not start Cyclone for stagger test ({}).", DescribeCycloneBlockers(player)));
                Check("C5_HighStaggerAbortsCyclone", false, "Skipped because Cyclone failed to start.");
                BeginNonHostileTest(player);
                break;
            }
            EnterPhase(Phase::ScenarioC_StaggerWarmup);
            break;
        }

        case Phase::ScenarioC_StaggerWarmup: {
            if (g_unpausedAccumSec < 0.35) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                FinishAndScheduleQuit("Player unavailable.");
                return;
            }
            g_telBeforeStagger = CycloneRuntime::GetTelemetry();

            // 1) 0.25 경직 -> 삼키고 사이클론 유지(결정 56)
            const bool setLow = player->SetGraphVariableFloat("staggerMagnitude", 0.25f);
            float readLow = -1.0f;
            (void)player->GetGraphVariableFloat("staggerMagnitude", readLow);
            (void)player->NotifyAnimationGraph("staggerStart");

            const auto telMid = CycloneRuntime::GetTelemetry();
            const bool activeAfterLow = CycloneRuntime::IsActive();
            Check("C4_LowStaggerSwallowed",
                (telMid.staggersSwallowed > g_telBeforeStagger.staggersSwallowed) && activeAfterLow,
                std::format("setLow={}, readLow={:.2f}, swallowed={}, activeAfterLow={}",
                    setLow, readLow, telMid.staggersSwallowed - g_telBeforeStagger.staggersSwallowed, activeAfterLow));

            // 2) 0.85 경직 -> 통과시키며 사이클론 즉시 종료(결정 150 ③)
            const bool setHigh = player->SetGraphVariableFloat("staggerMagnitude", 0.85f);
            float readHigh = -1.0f;
            (void)player->GetGraphVariableFloat("staggerMagnitude", readHigh);
            (void)player->NotifyAnimationGraph("staggerStart");
            CycloneRuntime::SetAutoTestHeld(false);
            LogInfo(std::format("High stagger injected: setHigh={}, readHigh={:.2f}.", setHigh, readHigh));

            EnterPhase(Phase::ScenarioC_StaggerCleanupWait);
            break;
        }

        case Phase::ScenarioC_StaggerCleanupWait: {
            if (g_unpausedAccumSec < 0.3) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto telAfter = CycloneRuntime::GetTelemetry();
            const bool active = CycloneRuntime::IsActive();
            const float var09 = (player && player->AsActorValueOwner())
                ? player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kVariable09)
                : -1.0f;

            Check("C5_HighStaggerAbortsCyclone",
                (telAfter.staggersAborted > g_telBeforeStagger.staggersAborted) && !active && std::abs(var09) < 1e-3f,
                std::format("aborted={}, active={}, var09={:.3f}",
                    telAfter.staggersAborted - g_telBeforeStagger.staggersAborted, active, var09));

            BeginNonHostileTest(player);
            break;
        }

        case Phase::ScenarioC_NonHostileSpawnWait: {
            if (g_unpausedAccumSec < 0.3) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto npc = g_foxHandle.get();
            if (!player || !npc || npc->IsDead()) {
                Check("C6_NonHostileFirstHitZeroXp", false, "Spawned neutral NPC (TestJeremyRegular) unavailable before first hit.");
                BeginScenarioD();
                break;
            }
            g_foxFormId = npc->GetFormID();
            const bool wasHostile = npc->IsHostileToActor(player);
            if (wasHostile) {
                Check("C6_NonHostileFirstHitZeroXp", false, "Neutral NPC (TestJeremyRegular) was unexpectedly hostile before first hit.");
                BeginScenarioD();
                break;
            }
            // 실제 무기 타격(DealDamage -> ApplyHitHook -> RecordFirstHitHostility(false) -> HandleHealthDamageHook)
            CallDealDamage(player, npc.get(), nullptr, false);
            EnterPhase(Phase::ScenarioC_NonHostileFinishWait);
            break;
        }

        case Phase::ScenarioC_NonHostileFinishWait: {
            if (g_unpausedAccumSec < 0.2) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto npc = g_foxHandle.get();
            if (player && npc && !npc->IsDead()) {
                (void)npc->DoDamage(500.0f, player, true);
            }
            EnterPhase(Phase::ScenarioC_NonHostileXpWait);
            break;
        }

        case Phase::ScenarioC_NonHostileXpWait: {
            const auto rec = g_killTracker ? g_killTracker->GetActorRecordSnapshot(g_foxFormId) : Core::ActorKillRecordSnapshot{};
            // TESDeathEvent(dead=true) 확정(rewardClaimed)까지 최대 5초 대기한 뒤 0.2초 여유를 두고 판정한다.
            if ((!rec.rewardClaimed || g_unpausedAccumSec < 0.5) && g_unpausedAccumSec < 5.0 && PhaseElapsedSec() < 8.0) {
                break;
            }
            auto npc = g_foxHandle.get();
            const bool npcDead = !npc || npc->IsDead();
            const auto stAfter = SaveLoad::GetStateCopy();
            const bool zeroXp = (stAfter.exileLevel == g_exileLevelBeforeFox) &&
                (stAfter.exileXp == g_exileXpBeforeFox);
            const bool hookVerified = rec.found && rec.firstHostilityRecorded &&
                !rec.wasHostileOnFirstHit && rec.playerContributed && rec.rewardClaimed;

            Check("C6_NonHostileFirstHitZeroXp", npcDead && zeroXp && hookVerified,
                std::format("npcDead={}, wait={:.2f}s, hook(found={}, firstRec={}, wasHostile={}, contrib={}, claimed={}), exile Lv{} XP {} -> Lv{} XP {}",
                    npcDead, g_unpausedAccumSec, rec.found, rec.firstHostilityRecorded, rec.wasHostileOnFirstHit,
                    rec.playerContributed, rec.rewardClaimed,
                    g_exileLevelBeforeFox, g_exileXpBeforeFox, stAfter.exileLevel, stAfter.exileXp));

            BeginScenarioD();
            break;
        }

        // ================= SCENARIO D (Stage 3 Passive Tree) =================
        case Phase::ScenarioD_BaselineWait: {
            if (g_unpausedAccumSec < 0.35 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* av = player ? player->AsActorValueOwner() : nullptr;
            const auto snapBefore = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
            if (!player || !av || !snapBefore || !g_treeData || !g_config) {
                FinishAndScheduleQuit("Scenario D baseline unavailable.");
                return;
            }
            g_treeStrBefore = snapBefore->str;
            g_treePoeLifeBefore = snapBefore->poeMaxLife;
            g_treePermHealthBefore = static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth));

            // D2: UiBridge JSON 명령으로 Marauder(1) 선택 + Attack Mastery notable(12809) 경로 + 31628(+16 Life) 할당
            SendUiCmd(json{ { "op", "treeSelectClass" }, { "classId", 1 } });
            auto stMid = SaveLoad::GetStateCopy();
            const auto pathTo12809 = Core::PassiveTree::ComputeShortestPath(*g_treeData, stMid.tree, 12809);
            if (!pathTo12809.empty()) {
                SendUiCmd(json{ { "op", "treeAllocatePath" }, { "nodes", pathTo12809 } });
            }
            SendUiCmd(json{ { "op", "treeAllocatePath" }, { "nodes", json::array({ 31628 }) } });

            const auto stAfter = SaveLoad::GetStateCopy();
            const auto spent = Core::PassiveTree::ComputeSpentNormalPoints(*g_treeData, stAfter.tree);
            const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(*g_treeData, stAfter.tree, stAfter.exileLevel, *g_config);
            const auto hasNode = [&](std::uint32_t nid) {
                return std::binary_search(stAfter.tree.allocatedNodes.begin(), stAfter.tree.allocatedNodes.end(), nid);
            };
            const auto expectedAvail = 49 + std::max<std::int32_t>(0, g_config->bonusPassivePoints) - spent;
            const bool d2Ok = stAfter.tree.classSelected && stAfter.tree.classId == 1 &&
                hasNode(12809) && hasNode(31628) && hasNode(50904) && spent >= 3 && avail == expectedAvail;
            Check("D2_SelectClassAndAllocatePath", d2Ok,
                std::format("classId={}, spent={}, avail={} (exp {}), has12809={}, has31628={}, has50904={}",
                    static_cast<int>(stAfter.tree.classId), spent, avail, expectedAvail,
                    hasNode(12809), hasNode(31628), hasNode(50904)));

            EnterPhase(Phase::ScenarioD_AllocateAndPoolWait);
            break;
        }

        case Phase::ScenarioD_AllocateAndPoolWait: {
            if (g_unpausedAccumSec < 0.35 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* av = player ? player->AsActorValueOwner() : nullptr;
            const auto snapAfter = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
            if (!player || !av || !snapAfter || !g_treeData || !g_config) {
                FinishAndScheduleQuit("Scenario D pool check unavailable.");
                return;
            }

            // D3: StatEngine 힘/생명력 증가 및 실제 스카이림 플레이어 최대 체력(kPermanent) 증가 확인
            const double permHealthAfter = static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth));
            const bool d3Ok = (snapAfter->str > g_treeStrBefore) &&
                (snapAfter->poeMaxLife > g_treePoeLifeBefore + 1.0) &&
                (permHealthAfter > g_treePermHealthBefore + 1.0);
            Check("D3_StatReflectionIntoSkyrimPools", d3Ok,
                std::format("str {}->{}, poeLife {:.1f}->{:.1f}, skyrimPermHP {:.1f}->{:.1f}",
                    g_treeStrBefore, snapAfter->str, g_treePoeLifeBefore, snapAfter->poeMaxLife,
                    g_treePermHealthBefore, permHealthAfter));

            // D4: 마스터리 선택(8629 -> 28524) -> 두 번째 Attack Mastery(8556, notable 20835) 할당 ->
            //     중복 effectId(28524) 거절 -> 대체 효과(39154) 허용 -> 중간 브리지 단절 회수 거절 ->
            //     리프 notable(20835) 회수 시 마스터리 8556 자동 회수 검증
            SendUiCmd(json{ { "op", "treeSelectMastery" }, { "masteryNodeId", 8629 }, { "effectId", 28524 } });
            auto stM1 = SaveLoad::GetStateCopy();
            const bool m1Selected = stM1.tree.masterySelections.contains(8629) && stM1.tree.masterySelections.at(8629) == 28524;

            const auto pathTo20835 = Core::PassiveTree::ComputeShortestPath(*g_treeData, stM1.tree, 20835);
            if (!pathTo20835.empty()) {
                SendUiCmd(json{ { "op", "treeAllocatePath" }, { "nodes", pathTo20835 } });
            }
            // 중복 effectId 28524 거절 검증
            SendUiCmd(json{ { "op", "treeSelectMastery" }, { "masteryNodeId", 8556 }, { "effectId", 28524 } });
            const auto stDup = SaveLoad::GetStateCopy();
            const bool dupRejected = !stDup.tree.masterySelections.contains(8556);

            // 다른 효과 39154 선택 후 리프 notable(20835) 회수 -> 마스터리 8556 자동 회수
            SendUiCmd(json{ { "op", "treeSelectMastery" }, { "masteryNodeId", 8556 }, { "effectId", 39154 } });
            const auto stM2 = SaveLoad::GetStateCopy();
            const bool m2Selected = stM2.tree.masterySelections.contains(8556) && stM2.tree.masterySelections.at(8556) == 39154;

            // 4단계 기본값 FreePassiveRefund=false 환경에서도 D4 회수 검증이 통과하도록 임시 후회의 오브 5개를 지급한다.
            (void)CraftingRuntime::GrantCurrency(player, Core::CurrencyKind::Regret, 5);

            // 중간 브리지 노드(pathTo20835의 첫 노드 또는 31628) 단절 회수 거절 검증
            const std::uint32_t bridgeId = (pathTo20835.size() >= 2) ? pathTo20835[0] : 31628u;
            SendUiCmd(json{ { "op", "treeRefundNode" }, { "nodeId", bridgeId } });
            const auto stBridge = SaveLoad::GetStateCopy();
            const bool bridgeKept = std::binary_search(
                stBridge.tree.allocatedNodes.begin(), stBridge.tree.allocatedNodes.end(), bridgeId);

            // 리프 notable 20835 회수 -> 마스터리 8556 자동 회수, 8629는 유지
            SendUiCmd(json{ { "op", "treeRefundNode" }, { "nodeId", 20835 } });
            const auto stAuto = SaveLoad::GetStateCopy();
            const bool leafRefunded = !std::binary_search(
                stAuto.tree.allocatedNodes.begin(), stAuto.tree.allocatedNodes.end(), 20835u);
            const bool autoRefundedM2 = !stAuto.tree.masterySelections.contains(8556) &&
                stAuto.tree.masterySelections.contains(8629);

            Check("D4_MasteryAndDisconnectGuard",
                m1Selected && dupRejected && m2Selected && bridgeKept && leafRefunded && autoRefundedM2,
                std::format("m1={}, dupRej={}, m2={}, bridgeKept(id={})={}, leafRef={}, autoRefM2={}",
                    m1Selected, dupRejected, m2Selected, bridgeId, bridgeKept, leafRefunded, autoRefundedM2));

            // D5 준비: Chaos Inoculation + 임시 ES 50 적용 후 주 갱신 TickPlayerPools가 안전 체력 하한·ES 풀을 반영하는지 확인
            Core::Layer1Input ciL1;
            ciL1.exileLevel = 50;
            ciL1.className = "Marauder";
            ciL1.slots = stAuto.slots;
            ciL1.flatEnergyShield = 50.0;
            ciL1.passives.chaosInoculation = true;
            g_statEngine->SetLayer1(ciL1);
            (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);

            EnterPhase(Phase::ScenarioD_KeystoneCiVerify);
            break;
        }

        case Phase::ScenarioD_KeystoneCiVerify: {
            if (g_unpausedAccumSec < 0.3 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* av = player ? player->AsActorValueOwner() : nullptr;
            const auto snapCi = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
            if (!player || !av || !snapCi || !g_dataPack || !g_config) {
                FinishAndScheduleQuit("Scenario D keystone verify unavailable.");
                return;
            }

            // 1) Chaos Inoculation: 카오스 피해 0 + 안전 체력 하한(<= 25.5, >= 1.0) + ES 풀 유지 + 플레이어 생존
            const double ciPermHp = static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth));
            const double chaosDmg = Core::ConvertDefenseResistDamage(100.0, Core::DamageType::Chaos, 50, *snapCi, *g_dataPack, *g_config);
            const bool esIntactUnderCi = !g_esPool || g_esPool->Snapshot().max > 0.0;
            const bool ciOk = snapCi->chaosInoculation && snapCi->chaosImmune &&
                std::abs(snapCi->poeMaxLife - 1.0) < 1e-6 &&
                chaosDmg == 0.0 && esIntactUnderCi && !player->IsDead() && ciPermHp >= 1.0 && ciPermHp <= 25.5;

            // 2) Blood Magic + MoM 상호작용: 체력 비용 환산 + 1 HP 자살 방지 가드 + Blood Magic 시 MoM 비활성(momRatio == 0)
            Core::Layer1Input bmL1;
            bmL1.exileLevel = 50;
            bmL1.className = "Marauder";
            bmL1.slots = SaveLoad::GetStateCopy().slots;
            bmL1.passives.bloodMagic = true;
            bmL1.passives.moreLifeMult = 1.1;
            bmL1.passives.mindOverMatterRatio = 0.40;
            g_statEngine->SetLayer1(bmL1);
            (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            const auto snapBm = g_statEngine->GetSnapshot();
            Core::SkillSystem bmSys;
            const auto blockedAtLowHp = snapBm
                ? bmSys.TryTriggerOnCrit(0, *snapBm, bmL1.slots, 0.0, 100.0, *g_dataPack, *g_config, 1.2, 200.0)
                : std::nullopt;
            const auto allowedAtFullHp = snapBm
                ? bmSys.TryTriggerOnCrit(0, *snapBm, bmL1.slots, 0.0, 100.0, *g_dataPack, *g_config, 200.0, 200.0)
                : std::nullopt;
            const bool bmOk = snapBm && snapBm->bloodMagic && snapBm->poeMaxMana == 0.0 &&
                snapBm->momRatio == 0.0 &&
                !blockedAtLowHp.has_value() &&
                allowedAtFullHp.has_value() && allowedAtFullHp->usesHealthCost && allowedAtFullHp->skyrimHealthCost > 0.0;

            // 3) Mind Over Matter: StatEngine momRatio=0.40 스냅샷 + 실제 플레이어 매지카/인게임 난이도 배율 정렬 흡수
            Core::Layer1Input momL1;
            momL1.exileLevel = 50;
            momL1.className = "Marauder";
            momL1.slots = bmL1.slots;
            momL1.passives.mindOverMatterRatio = 0.40;
            g_statEngine->SetLayer1(momL1);
            (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            const auto snapMom = g_statEngine->GetSnapshot();
            const double diffMult = SkillExecution::GetDifficultyMultiplier(true);
            const double curMagicka = std::max(50.0, static_cast<double>(av->GetActorValue(RE::ActorValue::kMagicka)));
            const auto momRes = snapMom
                ? Core::AbsorbWithMindOverMatter(50.0, diffMult, snapMom->momRatio, curMagicka)
                : Core::MindOverMatterAbsorbResult{};
            const double expectedMomAbsorb = std::min(curMagicka, 50.0 * diffMult * 0.40);
            const bool momOk = snapMom && std::abs(snapMom->momRatio - 0.40) < 1e-6 &&
                std::abs(momRes.absorbedMagicka - expectedMomAbsorb) < 0.05 &&
                momRes.remainingRawDamage < 50.0;

            Check("D5_KeystonesRuntime", ciOk && bmOk && momOk,
                std::format("ciOk(hp={:.1f},es={},chaos={:.1f})={}, bmOk(mom0={},blockLow={},allowFull={})={}, momOk(ratio={:.2f},diff={:.2f},abs={:.1f})={}",
                    ciPermHp, esIntactUnderCi, chaosDmg, ciOk,
                    snapBm ? (snapBm->momRatio == 0.0) : false,
                    !blockedAtLowHp.has_value(), allowedAtFullHp.has_value(), bmOk,
                    snapMom ? snapMom->momRatio : 0.0, diffMult, momRes.absorbedMagicka, momOk));

            // 원래 패시브 트리 스탯으로 즉시 복원
            SaveLoad::SyncStateToStatEngine();
            (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            RestorePlayerVitals(player, true, true);

            // D6: 창 활성 및 Prisma DOM 준비 시에만 트리 탭·검색 포커스 검증(시나리오 A와 동일 가드, 검수 Major #1)
            const bool gameActive = (!main || main->gameActive);
            if (!gameActive || !UiBridge::IsDomReady()) {
                LogWarn(std::format("D6_TreeTabAndSearchFocus skipped (gameActive={}, domReady={}).",
                    gameActive, UiBridge::IsDomReady()));
                BeginScenarioD_AscendancyAndSave();
                break;
            }
            g_d6OpenedOnTree = UiBridge::TryOpenPanelOnMainThread("tree") && (UiBridge::ActiveTab() == "tree");
            SendUiCmd(json{ { "op", "treeSearchFocus" }, { "focused", true } });
            EnterPhase(Phase::ScenarioD_SearchFocusWait);
            break;
        }

        case Phase::ScenarioD_SearchFocusWait: {
            if (PhaseElapsedSec() < 0.5) {
                break;
            }
            const auto* controls = RE::ControlMap::GetSingleton();
            const std::int32_t teCountDuring = controls ? controls->GetRuntimeData().textEntryCount : -1;
            const bool imeAvail = SimpleImeBridge::Available();
            const bool imeActive = SimpleImeBridge::TextEntryActive();
            g_d6ImeVerified = !imeAvail || (imeActive && teCountDuring >= 1);
            g_d6SurvivedSearchFocus = UiBridge::IsPanelOpen() &&
                (UiBridge::ActiveTab() == "tree") &&
                UiBridge::IsSearchFocused();
            LogInfo(std::format("D6 search focus probe: panelOpen={}, imeAvail={}, imeActive={}, textEntryCount={}",
                UiBridge::IsPanelOpen(), imeAvail, imeActive, teCountDuring));

            SendUiCmd(json{ { "op", "switchTab" }, { "tab", "skills" } });
            g_d6SwitchedToSkills = UiBridge::IsPanelOpen() &&
                (UiBridge::ActiveTab() == "skills") &&
                !UiBridge::IsSearchFocused();
            UiBridge::ClosePanelOnMainThread();
            EnterPhase(Phase::ScenarioD_TabCloseWait);
            break;
        }

        case Phase::ScenarioD_TabCloseWait: {
            // 검수 Minor #2: CI 복원·패널 닫힘 뒤 unpaused 틱 최소 0.15초를 보장해 TickPlayerPools 원복 완료 후 D8/D7 진행
            if ((g_unpausedAccumSec < 0.15 || PhaseElapsedSec() < 0.35) && PhaseElapsedSec() < 3.0) {
                break;
            }
            const auto* controls = RE::ControlMap::GetSingleton();
            const std::int32_t textEntryCount = controls ? controls->GetRuntimeData().textEntryCount : -1;
            const bool closedCleanly = !UiBridge::IsPanelOpen() && UiBridge::IsHudShown() && (textEntryCount == 0);
            Check("D6_TreeTabAndSearchFocus",
                g_d6OpenedOnTree && g_d6SurvivedSearchFocus && g_d6ImeVerified && g_d6SwitchedToSkills && closedCleanly,
                std::format("openTree={}, survivedFocus={}, imeOk={}, switchSkills={}, closed={}, hud={}, textEntryCount={}",
                    g_d6OpenedOnTree, g_d6SurvivedSearchFocus, g_d6ImeVerified, g_d6SwitchedToSkills,
                    !UiBridge::IsPanelOpen(), UiBridge::IsHudShown(), textEntryCount));

            BeginScenarioD_AscendancyAndSave();
            break;
        }

        case Phase::ScenarioD_SavePump: {
            bool matched = false;
            {
                std::scoped_lock lock(g_msgMutex);
                matched = g_saveMatched;
            }
            if (matched) {
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                LogInfo(std::format("Scenario D temp save '{}' confirmed by kSaveGame (attempts={}).",
                    g_cfg.tempSave, g_saveAttempts));
                EnterPhase(Phase::ScenarioD_SaveWaitBeforeLoad);
                break;
            }
            if (PhaseElapsedSec() > 25.0) {
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                Check("D7_TreeCosaveRoundTrip", false,
                    std::format("Timed out waiting for Scenario D kSaveGame('{}') after {} attempts.",
                        g_cfg.tempSave, g_saveAttempts));
                FinishAndScheduleQuit("Scenario D save timeout.");
                break;
            }
            const auto now = Clock::now();
            if (now >= g_nextSaveRetryAt) {
                if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
                    ++g_saveAttempts;
                    mgr->Save(g_cfg.tempSave.c_str());
                }
                g_nextSaveRetryAt = now + std::chrono::seconds(2);
            }
            break;
        }

        case Phase::ScenarioD_SaveWaitBeforeLoad: {
            if (PhaseElapsedSec() < 1.0) {
                break;
            }
            LogInfo(std::format("Reloading temp save '{}' to verify TREE co-save round-trip...", g_cfg.tempSave));
            if (!RequestLoadSave(g_cfg.tempSave)) {
                Check("D7_TreeCosaveRoundTrip", false, "BGSSaveLoadManager singleton unavailable.");
                FinishAndScheduleQuit("Scenario D reload request failed.");
                break;
            }
            EnterPhase(Phase::ScenarioD_AwaitReload);
            break;
        }

        case Phase::ScenarioD_AwaitReload: {
            bool signaled = false;
            bool ok = false;
            {
                std::scoped_lock lock(g_msgMutex);
                signaled = g_postLoadSignaled;
                ok = g_postLoadSucceeded;
            }
            if (signaled && !ok) {
                Check("D7_TreeCosaveRoundTrip", false, "kPostLoadGame reported failure on Scenario D reload.");
                FinishAndScheduleQuit("Scenario D reload failed.");
                return;
            }
            if (signaled && ok && IsPlayerReadyInWorld()) {
                EnterPhase(Phase::ScenarioD_ReloadSettle);
            } else if (PhaseElapsedSec() > 90.0) {
                Check("D7_TreeCosaveRoundTrip", false, "Timed out waiting for Scenario D temp save reload.");
                FinishAndScheduleQuit("Scenario D reload timeout.");
            }
            break;
        }

        case Phase::ScenarioD_ReloadSettle: {
            if (g_unpausedAccumSec < 2.0 && PhaseElapsedSec() < 8.0) {
                break;
            }
            const auto stReloaded = SaveLoad::GetStateCopy();
            const auto snapReloaded = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
            const bool d7Ok = stReloaded.tree.classSelected &&
                stReloaded.tree.classId == 1 &&
                stReloaded.tree.ascendancyId == "Juggernaut" &&
                stReloaded.tree.allocatedNodes == g_d7ExpectedNodes &&
                stReloaded.tree.masterySelections == g_d7ExpectedMasteries &&
                (stReloaded.tree.ascendancyMilestonesBitmask & 0x1u) != 0 &&
                snapReloaded && snapReloaded->str > g_treeStrBefore;
            Check("D7_TreeCosaveRoundTrip", d7Ok,
                std::format("clsSel={}, classId={}, asc='{}', nodes={}/{}, masteries={}/{}, msMask=0x{:X}, str={}",
                    stReloaded.tree.classSelected, static_cast<int>(stReloaded.tree.classId),
                    stReloaded.tree.ascendancyId,
                    stReloaded.tree.allocatedNodes.size(), g_d7ExpectedNodes.size(),
                    stReloaded.tree.masterySelections.size(), g_d7ExpectedMasteries.size(),
                    stReloaded.tree.ascendancyMilestonesBitmask,
                    snapReloaded ? snapReloaded->str : 0u));

            BeginScenarioE();
            break;
        }

        // ================= SCENARIO E (Stage 4 Items, Crafting, Gear Sync, World Drops & UI) =================
        case Phase::ScenarioE_Start: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !g_statEngine || !g_dataPack || !g_treeData || !g_config) {
                Check("E1_ItemsDataAndEspForms", false, "Core singletons unavailable.");
                FinishAndScheduleQuit("Scenario E aborted.");
                return;
            }

            // E1: items_3_30.json 로드 + 화폐 12종 + 토큰 80종 + 빛기둥 2종 ESP 폼 + 시작 화폐 플래그 검증
            std::size_t orbFound = 0;
            for (const auto fid : FormIds::kCurrencyOrbForms) {
                if (FormIds::Lookup<RE::TESObjectMISC>(fid)) ++orbFound;
            }
            std::size_t tokFound = 0;
            for (std::size_t off = 0; off < FormIds::kSealedTokenCount; ++off) {
                const auto fid = FormIds::kSealedTokenBase + static_cast<RE::FormID>(off);
                if (FormIds::Lookup<RE::TESObjectMISC>(fid)) ++tokFound;
            }
            const bool beamsFound = (FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamRare) != nullptr) &&
                (FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamCurrency) != nullptr);
            const auto stInitE = SaveLoad::GetStateCopy();
            const bool e1Ok = g_itemData && g_itemData->IsLoaded() &&
                g_itemData->AllAffixes().size() > 700 &&
                g_itemData->AllCurrencies().size() == Core::kCurrencyKindCount &&
                orbFound == Core::kCurrencyKindCount &&
                tokFound == FormIds::kSealedTokenCount &&
                beamsFound && stInitE.starterCurrencyGiven;
            Check("E1_ItemsDataAndEspForms", e1Ok,
                std::format("loaded={}, affixes={}, orbs={}/12, tokens={}/80, beams={}, starterCurr={}",
                    g_itemData && g_itemData->IsLoaded(),
                    g_itemData ? g_itemData->AllAffixes().size() : 0,
                    orbFound, tokFound, beamsFound, stInitE.starterCurrencyGiven));

            if (!e1Ok) {
                FinishAndScheduleQuit("Scenario E aborted because Stage 4 data or ESP forms were missing.");
                return;
            }

            // E2: LootTable 보스 드랍 판정(확률 편차 배제를 위해 5회 보스 표본 합산) + T3 희귀 갑옷 토큰 감정
            std::mt19937_64 detRng(424242u);
            std::size_t bossDropTotal = 0;
            for (int r = 0; r < 5; ++r) {
                bossDropTotal += Core::LootTable::RollKillDrops(50, true, false, *g_config, *g_itemData, detRng).size();
            }
            const bool rollValid = bossDropTotal > 0;
            (void)CraftingRuntime::GrantCurrency(player, Core::CurrencyKind::ScrollOfWisdom, 2);
            (void)CraftingRuntime::GrantToken(player, 3, Core::ItemRarity::Rare, Core::TokenSlotGroup::Body, 1);
            const auto wisBefore = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::ScrollOfWisdom);
            const auto idRes = CraftingRuntime::IdentifyToken(3, Core::ItemRarity::Rare, Core::TokenSlotGroup::Body);
            const auto wisAfter = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::ScrollOfWisdom);
            const auto trackedRec = idRes.ok ? ItemTracker::FindByKey(idRes.resultKey) : std::nullopt;
            const bool e2Ok = rollValid && idRes.ok && idRes.resultKey.IsValid() &&
                trackedRec.has_value() &&
                trackedRec->rarity == Core::ItemRarity::Rare &&
                trackedRec->itemLevel >= 41 && trackedRec->itemLevel <= 60 &&
                trackedRec->affixes.size() >= 4 && trackedRec->affixes.size() <= 6 &&
                wisAfter == wisBefore - 1;
            if (idRes.ok) {
                g_eIdentifiedKey = idRes.resultKey;
            }
            Check("E2_LootRollAndTokenIdentify", e2Ok,
                std::format("bossDrops5={}(ok={}), idOk={}, uid={}, rarity={}, iLvl={}, affixes={}, wis {}->{}",
                    bossDropTotal, rollValid, idRes.ok, idRes.resultKey.uniqueId,
                    trackedRec ? static_cast<int>(trackedRec->rarity) : -1,
                    trackedRec ? trackedRec->itemLevel : 0u,
                    trackedRec ? trackedRec->affixes.size() : 0,
                    wisBefore, wisAfter));

            // E3: 화폐 오브 전체 왕복(불법 Chaos 거절 -> Transmutation -> Alteration -> Augmentation -> Regal -> Exalted -> Divine -> Chaos -> Scouring[iLvl 보존] -> Alchemy)
            auto* dagger = RE::TESForm::LookupByID<RE::TESObjectWEAP>(kIronDaggerId);
            bool e3Ok = false;
            std::string e3Detail;
            if (dagger) {
                player->AddObjectToContainer(dagger, nullptr, 1, nullptr);
                for (auto kind : {
                         Core::CurrencyKind::Transmutation,
                         Core::CurrencyKind::Augmentation,
                         Core::CurrencyKind::Alteration,
                         Core::CurrencyKind::Regal,
                         Core::CurrencyKind::Alchemy,
                         Core::CurrencyKind::Chaos,
                         Core::CurrencyKind::Exalted,
                         Core::CurrencyKind::Scouring,
                         Core::CurrencyKind::Divine }) {
                    (void)CraftingRuntime::GrantCurrency(player, kind, 10);
                }
                const auto chaosBeforeIllegal = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Chaos);
                const auto illegalChaos = CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Chaos, kIronDaggerId, 0, false);
                const auto chaosAfterIllegal = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Chaos);
                const bool illegalGuarded = !illegalChaos.ok && (chaosAfterIllegal == chaosBeforeIllegal);

                const auto rTrans = CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Transmutation, kIronDaggerId, 0, false);
                const std::uint16_t uid = rTrans.resultKey.uniqueId;
                const std::uint32_t origIlvl = rTrans.resultRecord.itemLevel;
                bool magicChainOk = rTrans.ok && uid != 0 && rTrans.resultRecord.rarity == Core::ItemRarity::Magic;

                // Alteration 실행 및 Augmentation(1어픽스면 성공, 2어픽스면 거절 후 0차감) 검증
                auto rAlt = magicChainOk
                    ? CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Alteration, kIronDaggerId, uid, false)
                    : CraftActionResult{};
                magicChainOk = magicChainOk && rAlt.ok && rAlt.resultRecord.rarity == Core::ItemRarity::Magic;
                bool augOk = false;
                if (magicChainOk) {
                    const std::size_t affBeforeAug = rAlt.resultRecord.affixes.size();
                    const auto augBefore = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Augmentation);
                    const auto rAug = CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Augmentation, kIronDaggerId, uid, false);
                    const auto augAfter = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Augmentation);
                    if (affBeforeAug < 2) {
                        augOk = rAug.ok && rAug.resultRecord.affixes.size() == affBeforeAug + 1 && augAfter == augBefore - 1;
                    } else {
                        augOk = !rAug.ok && augAfter == augBefore;
                    }
                }

                const auto rRegal = (magicChainOk && augOk)
                    ? CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Regal, kIronDaggerId, uid, false)
                    : CraftActionResult{};
                const auto rExalt = rRegal.ok
                    ? CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Exalted, kIronDaggerId, uid, false)
                    : CraftActionResult{};
                const auto rDivine = rExalt.ok
                    ? CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Divine, kIronDaggerId, uid, false)
                    : CraftActionResult{};
                const auto rChaos = rDivine.ok
                    ? CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Chaos, kIronDaggerId, uid, false)
                    : CraftActionResult{};
                const auto rScour = rChaos.ok
                    ? CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Scouring, kIronDaggerId, uid, false)
                    : CraftActionResult{};
                const bool scourIlvlKept = rScour.ok &&
                    rScour.resultRecord.rarity == Core::ItemRarity::Normal &&
                    rScour.resultRecord.affixes.empty() &&
                    rScour.resultRecord.itemLevel == origIlvl;
                const auto rAlch = scourIlvlKept
                    ? CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Alchemy, kIronDaggerId, uid, false)
                    : CraftActionResult{};

                e3Ok = illegalGuarded && magicChainOk && augOk &&
                    rRegal.ok && rRegal.resultRecord.rarity == Core::ItemRarity::Rare &&
                    rExalt.ok && rExalt.resultRecord.affixes.size() == rRegal.resultRecord.affixes.size() + 1 &&
                    rDivine.ok && rDivine.resultRecord.affixes.size() == rExalt.resultRecord.affixes.size() &&
                    rChaos.ok && rChaos.resultRecord.affixes.size() >= 4 &&
                    scourIlvlKept &&
                    rAlch.ok && rAlch.resultRecord.rarity == Core::ItemRarity::Rare &&
                    rAlch.resultRecord.affixes.size() >= 4 &&
                    rAlch.resultRecord.itemLevel == origIlvl;
                if (e3Ok) {
                    g_eCraftedDaggerKey = rAlch.resultKey;
                }
                e3Detail = std::format(
                    "illegalGuard={}, trans={}, alt={}, aug={}, regal={}, exalt={}, divine={}, chaos={}, scourIlvl({}=={})={}, alch={}",
                    illegalGuarded, rTrans.ok, rAlt.ok, augOk, rRegal.ok, rExalt.ok,
                    rDivine.ok, rChaos.ok, rScour.resultRecord.itemLevel, origIlvl, scourIlvlKept, rAlch.ok);
            } else {
                e3Detail = "IronDagger (0x0001397E) lookup failed";
            }
            Check("E3_CurrencyOrbsRoundTripAndGuard", e3Ok, e3Detail);

            // E4 준비: 기존 투구/서클릿 등 Helmet 슬롯 선점 장비를 먼저 해제해 충돌 차단(검수 Major #6)
            auto* equipMgr = RE::ActorEquipManager::GetSingleton();
            if (equipMgr) {
                for (int attempt = 0; attempt < 3; ++attempt) {
                    EquipmentSync::RefreshFromPlayerNow();
                    const auto curMeta = EquipmentSync::GetCurrentGearMeta();
                    const auto& hMeta = curMeta[static_cast<std::size_t>(Core::EquipSlot::Helmet)];
                    if (!hMeta.occupied) {
                        break;
                    }
                    if (auto* prevHelm = RE::TESForm::LookupByID<RE::TESBoundObject>(hMeta.baseFormId)) {
                        equipMgr->UnequipObject(player, prevHelm, nullptr, 1, nullptr, false, true, false, true);
                    } else {
                        break;
                    }
                }
                EquipmentSync::RefreshFromPlayerNow();
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            }

            const auto snapBeforeGear = g_statEngine->GetSnapshot();
            auto* av = player->AsActorValueOwner();
            g_eStrBefore = snapBeforeGear ? snapBeforeGear->str : 0u;
            g_ePoeLifeBefore = snapBeforeGear ? snapBeforeGear->poeMaxLife : 0.0;
            g_eFireResBefore = snapBeforeGear ? snapBeforeGear->fireResist : 0.0;
            g_ePermHpBefore = av ? static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth)) : 0.0;

            auto* helmet = RE::TESForm::LookupByID<RE::TESObjectARMO>(kIronHelmetId);
            if (helmet && equipMgr) {
                player->AddObjectToContainer(helmet, nullptr, 1, nullptr);
                RE::ExtraDataList* helmExtra = nullptr;
                g_eHelmetKey = ItemTracker::EnsureUniqueId(player, helmet, nullptr, &helmExtra);
                if (g_eHelmetKey.IsValid()) {
                    Core::PoEItemRecord hRec;
                    hRec.rarity = Core::ItemRarity::Rare;
                    // 검수 Critical #1: Lv50 Marauder(힘 ~122) 요구치 충족을 위해 iLvl 48(reqLevel=48, reqStr=101) 사용
                    hRec.itemLevel = 48;
                    hRec.baseFormId = kIronHelmetId;
                    hRec.slotGroup = Core::TokenSlotGroup::Helmet;
                    hRec.defenseSubtype = EquipmentSync::ClassifyDefenseSubtype(helmet);
                    // items_3_30.json에 존재하는 실제 어픽스 ID 및 법정 롤 범위 값 사용
                    hRec.affixes = {
                        { "IncreasedLife8", { 120.0 } },
                        { "FireResist6", { 40.0 } },
                        { "Strength5", { 30.0 } }
                    };
                    ItemTracker::Upsert(g_eHelmetKey, hRec, false);
                    equipMgr->EquipObject(player, helmet, helmExtra, 1, nullptr, false, true, false, true);
                    EquipmentSync::RefreshFromPlayerNow();
                    (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                }
            }
            EnterPhase(Phase::ScenarioE_EquipPoolWait);
            break;
        }

        case Phase::ScenarioE_EquipPoolWait: {
            if (g_unpausedAccumSec < 0.4 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* av = player ? player->AsActorValueOwner() : nullptr;
            const auto snapAfterGear = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
            if (!player || !av || !snapAfterGear) {
                FinishAndScheduleQuit("Scenario E4 player/snapshot unavailable.");
                return;
            }

            const double permHpAfter = static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth));
            const bool wornReflected = (snapAfterGear->str >= g_eStrBefore + 25) &&
                (snapAfterGear->poeMaxLife > g_ePoeLifeBefore + 80.0) &&
                (snapAfterGear->fireResist > g_eFireResBefore + 30.0) &&
                (permHpAfter > g_ePermHpBefore + 1.0);

            // 순환 자기충족 방지 + 젬 요구치 평가 검증: 요구 힘 300이면서 자기 자신이 힘 +300을 주는 장비는 비활성 유지
            Core::StatEngine circEngine;
            Core::Layer1Input circL1;
            circL1.exileLevel = 10;
            circL1.className = "Scion";
            circL1.slots[0].occupied = true;
            circL1.slots[0].gem.instanceId = 1;
            circL1.slots[0].gem.gemId = "Cyclone";
            circL1.slots[0].gem.level = 20;  // Lv20 Cyclone requires high Dex -> unmet on base Scion(20)
            circL1.slots[0].gem.socketCount = 3;
            auto& circBody = circL1.equippedGear[static_cast<std::size_t>(Core::EquipSlot::Body)];
            circBody.occupied = true;
            circBody.isCrafted = true;
            circBody.reqStr = 300;
            circBody.stats.flatStr = 300.0;
            circBody.stats.flatLife = 500.0;
            circEngine.SetLayer1(circL1);
            (void)circEngine.RebuildIfDirty(*g_dataPack, *g_config);
            const auto circSnap = circEngine.GetSnapshot();
            const bool circularBlocked = circSnap &&
                circSnap->gearSlotReqFailed[static_cast<std::size_t>(Core::EquipSlot::Body)] &&
                !circSnap->gearSlotActive[static_cast<std::size_t>(Core::EquipSlot::Body)] &&
                circSnap->str < 100 &&
                !circSnap->slotStats[0].requirementMet;

            Check("E4_EquipGearStatSyncAndCircularGuard", wornReflected && circularBlocked,
                std::format("str {}->{}, life {:.1f}->{:.1f}, fireRes {:.0f}->{:.0f}, permHp {:.1f}->{:.1f}, circBlocked={}",
                    g_eStrBefore, snapAfterGear->str, g_ePoeLifeBefore, snapAfterGear->poeMaxLife,
                    g_eFireResBefore, snapAfterGear->fireResist, g_ePermHpBefore, permHpAfter, circularBlocked));

            // E5: 쥬얼러의 오브(스킬 젬 3->4->5 + 5 초과 거절 + 발동 젬 내부 주문 젬 3->4) & 후회의 오브 연동 회수
            (void)CraftingRuntime::GrantCurrency(player, Core::CurrencyKind::Jeweller, 5);
            const auto jw0 = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Jeweller);
            const auto j1 = CraftingRuntime::ApplyJewellerToGem(0, 0);
            const auto j2 = CraftingRuntime::ApplyJewellerToGem(0, 0);
            const auto jwBeforeCap = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Jeweller);
            const auto jCap = CraftingRuntime::ApplyJewellerToGem(0, 0);
            const auto jwAfterCap = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Jeweller);

            const auto stForJew = SaveLoad::GetStateCopy();
            const std::uint64_t spellInstId = (stForJew.slots[1].occupied && !stForJew.slots[1].gem.socketedGems.empty())
                ? stForJew.slots[1].gem.socketedGems[0].instanceId
                : 0;
            const auto jSp = (spellInstId != 0)
                ? CraftingRuntime::ApplyJewellerToGem(1, spellInstId)
                : CraftActionResult{};
            Core::GemInstance dummySup;
            dummySup.gemId = "SupportBrutality";
            dummySup.socketCount = 0;
            const bool supRejected =
                Core::ItemCrafting::ApplyJewellerToGem(dummySup, Core::GemKind::Support).code == Core::CraftResultCode::UnsupportedTarget;

            const auto stAfterJew = SaveLoad::GetStateCopy();
            const bool jewOk = j1.ok && j2.ok && !jCap.ok && (jwAfterCap == jwBeforeCap) &&
                jSp.ok && supRejected &&
                stAfterJew.slots[0].occupied && stAfterJew.slots[0].gem.socketCount == 5 &&
                stAfterJew.slots[1].occupied && !stAfterJew.slots[1].gem.socketedGems.empty() &&
                stAfterJew.slots[1].gem.socketedGems[0].socketCount == 4 &&
                jw0 - 3 == CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Jeweller);

            // 후회의 오브 연동 패시브 회수: 0개일 때 실패 -> 1개 지급 후 성공 및 차감(검수 Minor #8: FreePassiveRefund=true 분기 대응)
            const auto remRegret = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Regret);
            if (remRegret > 0) {
                (void)CraftingRuntime::ConsumeCurrency(player, Core::CurrencyKind::Regret, remRegret);
            }
            const std::uint32_t expectedRefundCost = g_config->freePassiveRefund ? 0u : 1u;
            std::uint32_t leafNodeId = 0;
            for (const auto nid : stAfterJew.tree.allocatedNodes) {
                const auto* nd = g_treeData->FindNode(nid);
                if (nd && nd->ascendancyId.empty() &&
                    Core::PassiveTree::GetRefundNodeCost(*g_treeData, stAfterJew.tree, nid, *g_config) == expectedRefundCost) {
                    auto probeTree = stAfterJew.tree;
                    if (Core::PassiveTree::RefundNode(*g_treeData, probeTree, nid, stAfterJew.exileLevel, *g_config, expectedRefundCost).ok) {
                        leafNodeId = nid;
                        break;
                    }
                }
            }
            bool regretOk = false;
            if (leafNodeId != 0) {
                if (!g_config->freePassiveRefund) {
                    const auto failZero = CraftingRuntime::RefundPassiveNodeWithRegret(leafNodeId);
                    (void)CraftingRuntime::GrantCurrency(player, Core::CurrencyKind::Regret, 1);
                    const auto okOne = CraftingRuntime::RefundPassiveNodeWithRegret(leafNodeId);
                    const auto regretEnd = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Regret);
                    regretOk = !failZero.ok && okOne.ok && regretEnd == 0;
                } else {
                    const auto okFree = CraftingRuntime::RefundPassiveNodeWithRegret(leafNodeId);
                    regretOk = okFree.ok;
                }
            }
            Check("E5_JewellerAndRegretRuntime", jewOk && regretOk,
                std::format("jewOk(slot0={}, spell={}, capRej={})={}, regretOk(leaf={})={}",
                    stAfterJew.slots[0].occupied ? stAfterJew.slots[0].gem.socketCount : 0,
                    (stAfterJew.slots[1].occupied && !stAfterJew.slots[1].gem.socketedGems.empty())
                        ? stAfterJew.slots[1].gem.socketedGems[0].socketCount : 0,
                    !jCap.ok, jewOk, leafNodeId, regretOk));

            // E6 준비: 동일 베이스(IronWarAxe) 2개 중 1개만 제작 -> 제작품만 바닥 드랍 -> 픽업 후 격리 보존 검증
            auto* axe = RE::TESForm::LookupByID<RE::TESObjectWEAP>(kIronWarAxeId);
            g_eDupOrigKey = {};
            g_eDroppedRefHandle = {};
            g_eDroppedWorldStamped = false;
            if (axe) {
                player->AddObjectToContainer(axe, nullptr, 2, nullptr);
                (void)CraftingRuntime::GrantCurrency(player, Core::CurrencyKind::Transmutation, 1);
                const auto craftAxe = CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Transmutation, kIronWarAxeId, 0, false);
                if (craftAxe.ok && craftAxe.resultKey.IsValid()) {
                    g_eDupOrigKey = craftAxe.resultKey;
                    g_eDupExpectedRec = craftAxe.resultRecord;
                    auto* axeExtra = ItemTracker::FindExtraListByKey(player, axe, g_eDupOrigKey);
                    if (axeExtra) {
                        g_eDroppedRefHandle = player->DropObject(axe, axeExtra, 1);
                    }
                }
            }
            EnterPhase(Phase::ScenarioE_DropWait);
            break;
        }

        case Phase::ScenarioE_DropWait: {
            if (g_unpausedAccumSec < 0.25 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto droppedRef = g_eDroppedRefHandle.get();
            if (player && droppedRef && g_eDupOrigKey.IsValid()) {
                // 검수 Critical #2: 바닥 드랍 항목은 isWorld=true, key={droppedRef->GetFormID(), stampUid}로 저장되므로 스냅샷에서 직접 대조
                const auto* xUid = droppedRef->extraList.GetByType<RE::ExtraUniqueID>();
                if (xUid && xUid->uniqueID >= 0xC000) {
                    const RE::FormID groundRefId = droppedRef->GetFormID();
                    const auto snapItems = ItemTracker::TakeSnapshotForSave();
                    for (const auto& e : snapItems) {
                        if (e.isWorld && e.key.ownerFormId == groundRefId && e.key.uniqueId == xUid->uniqueID &&
                            e.record.rarity == g_eDupExpectedRec.rarity &&
                            e.record.affixes.size() == g_eDupExpectedRec.affixes.size()) {
                            g_eDroppedWorldStamped = true;
                            break;
                        }
                    }
                }
                player->PickUpObject(droppedRef.get(), 1, false, false);
            }
            EnterPhase(Phase::ScenarioE_PickupWait);
            break;
        }

        case Phase::ScenarioE_PickupWait: {
            if (g_unpausedAccumSec < 0.45 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            std::int32_t trackedAxeCount = 0;
            std::int32_t untouchedAxeCount = 0;
            std::string axeSummary;
            if (player && g_eDupOrigKey.IsValid()) {
                const auto bag = CraftingRuntime::ListPlayerCraftableItems(player);
                for (const auto& entry : bag) {
                    if (entry.baseFormId != kIronWarAxeId) {
                        continue;
                    }
                    if (!axeSummary.empty()) axeSummary += ",";
                    axeSummary += std::format("(uid={},cnt={},rec={})", entry.uniqueId, entry.count, entry.hasPoERecord);
                    if (entry.hasPoERecord && entry.uniqueId != 0 &&
                        entry.record.rarity == g_eDupExpectedRec.rarity &&
                        entry.record.affixes.size() == g_eDupExpectedRec.affixes.size()) {
                        trackedAxeCount += entry.count;
                    } else if (!entry.hasPoERecord && entry.count >= 1) {
                        untouchedAxeCount += entry.count;
                    }
                }
            }
            const bool foundTrackedAxe = (trackedAxeCount == 1);
            const bool foundPlainAxe = (untouchedAxeCount >= 1);
            Check("E6_ItemTrackerDuplicateBaseIsolation",
                g_eDroppedWorldStamped && foundTrackedAxe && foundPlainAxe,
                std::format("worldStamped={}, trackedCnt={}, plainCnt={}, entries=[{}]",
                    g_eDroppedWorldStamped, trackedAxeCount, untouchedAxeCount, axeSummary));

            // E7 준비: 시체 1구 소환·즉사(검수 Minor #9: nullptr 공격자로 DeathListener 간섭 차단) -> 바닥 물리 드랍+빛기둥 생성 -> 세이브·로드 왕복
            WorldDropTracker::ReturnAllToCorpsesNow();
            g_eCorpseHandle = {};
            if (player) {
                g_eCorpseHandle = SpawnNpcInFrontOfPlayer(player, kEncSkeeverId, 75.0f, 0.0f, 0.0f, false);
                if (auto sk = g_eCorpseHandle.get()) {
                    (void)sk->DoDamage(1200.0f, nullptr, true);
                }
            }
            EnterPhase(Phase::ScenarioE_CorpseWait);
            break;
        }

        case Phase::ScenarioE_CorpseWait: {
            if (g_unpausedAccumSec < 0.4 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto corpse = g_eCorpseHandle.get();
            const auto rareTokLocal = FormIds::SealedTokenLocalFormId(3, Core::ItemRarity::Rare, Core::TokenSlotGroup::Body);
            auto* rareTokObj = FormIds::Lookup<RE::TESObjectMISC>(rareTokLocal);
            bool spawnedDrop = false;
            if (corpse && rareTokObj) {
                // SpawnPhysicalDropFromContainer가 내부에서 AddObjectToContainer -> RemoveItem(kDropping)을 원자 수행함
                spawnedDrop = WorldDropTracker::SpawnPhysicalDropFromContainer(corpse.get(), rareTokObj, 2, false);
            }
            LogInfo(std::format("Scenario E7: physical world drop spawned={}, activeCount={}.",
                spawnedDrop, WorldDropTracker::ActiveCount()));

            // E8 코세이브 왕복 대조용 현재 추적 아이템 전체 스냅샷 저장
            g_eExpectedTrackedItems = ItemTracker::TakeSnapshotForSave();

            g_saveAttempts = 0;
            g_nextSaveRetryAt = Clock::now();
            {
                std::scoped_lock lock(g_msgMutex);
                g_expectedSaveStem = g_cfg.tempSave;
                g_saveMatched = false;
            }
            EnterPhase(Phase::ScenarioE_SavePump);
            break;
        }

        case Phase::ScenarioE_SavePump: {
            bool matched = false;
            {
                std::scoped_lock lock(g_msgMutex);
                matched = g_saveMatched;
            }
            if (matched) {
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                LogInfo(std::format("Scenario E temp save '{}' confirmed by kSaveGame (attempts={}).",
                    g_cfg.tempSave, g_saveAttempts));
                EnterPhase(Phase::ScenarioE_SaveWaitBeforeLoad);
                break;
            }
            if (PhaseElapsedSec() > 25.0) {
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                Check("E7_WorldDropCosaveAndReturn", false, "Timed out waiting for Scenario E kSaveGame.");
                Check("E8_ItemCosaveRoundTripAndGearUi", false, "Skipped due to Scenario E save timeout.");
                FinishAndScheduleQuit("Scenario E save timeout.");
                break;
            }
            const auto now = Clock::now();
            if (now >= g_nextSaveRetryAt) {
                if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
                    ++g_saveAttempts;
                    mgr->Save(g_cfg.tempSave.c_str());
                }
                g_nextSaveRetryAt = now + std::chrono::seconds(2);
            }
            break;
        }

        case Phase::ScenarioE_SaveWaitBeforeLoad: {
            if (PhaseElapsedSec() < 1.0) {
                break;
            }
            LogInfo(std::format("Reloading temp save '{}' to verify ITEM + WDRP co-save round-trip...", g_cfg.tempSave));
            if (!RequestLoadSave(g_cfg.tempSave)) {
                Check("E7_WorldDropCosaveAndReturn", false, "BGSSaveLoadManager singleton unavailable.");
                Check("E8_ItemCosaveRoundTripAndGearUi", false, "Skipped due to reload failure.");
                FinishAndScheduleQuit("Scenario E reload request failed.");
                break;
            }
            EnterPhase(Phase::ScenarioE_AwaitReload);
            break;
        }

        case Phase::ScenarioE_AwaitReload: {
            bool signaled = false;
            bool ok = false;
            {
                std::scoped_lock lock(g_msgMutex);
                signaled = g_postLoadSignaled;
                ok = g_postLoadSucceeded;
            }
            if (signaled && !ok) {
                Check("E7_WorldDropCosaveAndReturn", false, "kPostLoadGame reported failure on Scenario E reload.");
                Check("E8_ItemCosaveRoundTripAndGearUi", false, "Skipped due to reload failure.");
                FinishAndScheduleQuit("Scenario E reload failed.");
                return;
            }
            if (signaled && ok && IsPlayerReadyInWorld()) {
                EnterPhase(Phase::ScenarioE_ReloadSettle);
            } else if (PhaseElapsedSec() > 90.0) {
                Check("E7_WorldDropCosaveAndReturn", false, "Timed out waiting for Scenario E reload.");
                Check("E8_ItemCosaveRoundTripAndGearUi", false, "Skipped due to reload timeout.");
                FinishAndScheduleQuit("Scenario E reload timeout.");
            }
            break;
        }

        case Phase::ScenarioE_ReloadSettle: {
            if (g_unpausedAccumSec < 2.0 && PhaseElapsedSec() < 8.0) {
                break;
            }

            // E7 검증: 로드 직후 'WDRP' 복원 -> 고아 빛기둥 0개 -> ReturnAllToCorpsesNow() 후 활성 0 & 고아 0
            const auto activeAfterReload = WorldDropTracker::ActiveCount();
            const auto orphansBeforeReturn = WorldDropTracker::SweepOrphanBeamsNearPlayer();
            WorldDropTracker::ReturnAllToCorpsesNow();
            const auto activeAfterReturn = WorldDropTracker::ActiveCount();
            const auto orphansAfterReturn = WorldDropTracker::SweepOrphanBeamsNearPlayer();
            const bool e7Ok = (activeAfterReload >= 1) &&
                (orphansBeforeReturn == 0) &&
                (activeAfterReturn == 0) &&
                (orphansAfterReturn == 0);
            Check("E7_WorldDropCosaveAndReturn", e7Ok,
                std::format("activeAfterReload={}, orphansBefore={}, activeAfterReturn={}, orphansAfter={}",
                    activeAfterReload, orphansBeforeReturn, activeAfterReturn, orphansAfterReturn));

            // E8 검증: 'ITEM' v1 코세이브 복원 + Prisma UI 3번째 탭(장비·제작) 페이로드 및 탭 열기/닫기
            const auto stReloadedE = SaveLoad::GetStateCopy();
            const auto restoredHelmet = ItemTracker::FindByKey(g_eHelmetKey);
            const auto restoredIdent = ItemTracker::FindByKey(g_eIdentifiedKey);
            const auto restoredDagger = ItemTracker::FindByKey(g_eCraftedDaggerKey);
            const auto restoredCount = ItemTracker::CountTracked();
            g_e8CosaveOk = stReloadedE.starterCurrencyGiven &&
                restoredCount == g_eExpectedTrackedItems.size() &&
                restoredHelmet.has_value() && restoredHelmet->rarity == Core::ItemRarity::Rare &&
                restoredHelmet->affixes.size() == 3 &&
                restoredIdent.has_value() && restoredIdent->rarity == Core::ItemRarity::Rare &&
                restoredDagger.has_value() && restoredDagger->rarity == Core::ItemRarity::Rare;

            g_e8PayloadOk = false;
            std::size_t eqCount = 0;
            std::size_t curCount = 0;
            bool helmCardHasRare = false;
            try {
                const auto panelJson = json::parse(UiBridge::BuildPanelJsonForTestOnMainThread());
                if (panelJson.contains("gear") && panelJson["gear"].is_object()) {
                    const auto& gj = panelJson["gear"];
                    const bool loaded = gj.value("loaded", false);
                    eqCount = gj.contains("equippedSlots") && gj["equippedSlots"].is_array() ? gj["equippedSlots"].size() : 0;
                    curCount = gj.contains("currencies") && gj["currencies"].is_array() ? gj["currencies"].size() : 0;
                    if (eqCount == Core::kEquipSlotCount) {
                        const auto& hSlot = gj["equippedSlots"][static_cast<std::size_t>(Core::EquipSlot::Helmet)];
                        if (hSlot.value("occupied", false) && hSlot.contains("card") && hSlot["card"].is_object()) {
                            helmCardHasRare = (hSlot["card"].value("rarityBadge", "") == "[희귀]") &&
                                hSlot["card"].contains("affixLines") && hSlot["card"]["affixLines"].size() == 3;
                        }
                    }
                    g_e8PayloadOk = loaded && (eqCount == Core::kEquipSlotCount) &&
                        (curCount == Core::kCurrencyKindCount) && helmCardHasRare;
                }
            } catch (...) {
                g_e8PayloadOk = false;
            }

            g_e8Detail = std::format("tracked={}/{}, helm={}, id={}, dag={}, eqSlots={}, cur={}, helmCardRare={}",
                restoredCount, g_eExpectedTrackedItems.size(),
                restoredHelmet.has_value(), restoredIdent.has_value(), restoredDagger.has_value(),
                eqCount, curCount, helmCardHasRare);

            const bool gameActive = (!main || main->gameActive);
            if (gameActive && UiBridge::IsDomReady()) {
                g_e8OpenedGearTab = UiBridge::TryOpenPanelOnMainThread("gear") && (UiBridge::ActiveTab() == "gear");
                EnterPhase(Phase::ScenarioE_GearTabCloseWait);
            } else {
                Check("E8_ItemCosaveRoundTripAndGearUi", g_e8CosaveOk && g_e8PayloadOk,
                    std::format("{}, uiTabSkipped(gameActive={}, domReady={})", g_e8Detail, gameActive, UiBridge::IsDomReady()));
                FinishAndScheduleQuit("All Stage 4 AutoTest scenarios (A, B, C, D, E) completed.");
            }
            break;
        }

        case Phase::ScenarioE_GearTabCloseWait: {
            if (PhaseElapsedSec() < 0.35) {
                break;
            }
            UiBridge::ClosePanelOnMainThread();
            const bool closedClean = !UiBridge::IsPanelOpen() && UiBridge::IsHudShown();
            Check("E8_ItemCosaveRoundTripAndGearUi", g_e8CosaveOk && g_e8PayloadOk && g_e8OpenedGearTab && closedClean,
                std::format("{}, openGearTab={}, closedClean={}", g_e8Detail, g_e8OpenedGearTab, closedClean));
            FinishAndScheduleQuit("All Stage 4 AutoTest scenarios (A, B, C, D, E) completed.");
            break;
        }

        default:
            break;
        }
    }
}
