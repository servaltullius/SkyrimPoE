#include "PCH.h"
#include "runtime/AutoTest.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/CombatHooks.h"
#include "runtime/CraftingRuntime.h"
#include "runtime/CycloneRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/ItemTracker.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/MapPackRuntime.h"
#include "runtime/ModState.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/ProjectileRuntime.h"
#include "runtime/ProxySpellRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SimpleImeBridge.h"
#include "runtime/SkillExecution.h"
#include "runtime/TrueHudBridge.h"
#include "runtime/UiBridge.h"
#include "runtime/UniqueIdUtil.h"
#include "runtime/WorldDropTracker.h"

#include "core/AilmentSystem.h"
#include "core/DamageConversion.h"
#include "core/ItemAffixParser.h"
#include "core/ItemCrafting.h"
#include "core/ItemFormatter.h"
#include "core/LootTable.h"
#include "core/MapSystem.h"
#include "core/PassiveTree.h"
#include "core/SkillSystem.h"
#include "core/UiBuildCheck.h"
#include "core/WorldStampLink.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// 걷기 탐침이 게임 창이 앞에 있는지 보려고 쓴다. CommonLib의 REX 선언 방식을 따른다(Windows 헤더를 넣지 않음).
REX_W32_IMPORT(REX::W32::HWND, GetForegroundWindow);
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
        using AutoTestMarker::AutoTestConfig;
        using AutoTestMarker::kCleanupFinalSuffix;
        using AutoTestMarker::kHCheckpointSuffix;
        using AutoTestMarker::SameSaveStem;
        using AutoTestMarker::StripSaveStem;
        using AutoTestMarker::ToLowerAscii;
        constexpr unsigned kHCheckpointSaveCandidates = 16;
        constexpr unsigned kHCheckpointRestoreAttempts = 2;

        // houseCARL로 대조 검증한 바닐라 Skyrim.esm 폼 ID
        constexpr RE::FormID kIronSwordId = 0x00012EB7;      // IronSword
        constexpr RE::FormID kIronDaggerId = 0x0001397E;     // IronDagger (E3 오브 왕복용)
        constexpr RE::FormID kIronWarAxeId = 0x00013790;     // IronWarAxe (E6 중복 베이스 격리용)
        constexpr RE::FormID kIronHelmetId = 0x00012E4D;     // IronHelmet (E4 장비 스탯 반영용)
        constexpr RE::FormID kEncSkeeverId = 0x00023AB7;     // EncSkeever (Skyrim.esm 순정, Aggressive, Lv1)
        constexpr RE::FormID kNeutralNpcId = 0x0010D13E;     // AADeleteWhenDoneTestJeremyRegular (순정, 무팩션·Unaggressive·NoCrime)
        constexpr RE::FormID kFireboltId = 0x00012FD0;       // Skyrim.esm Firebolt (프로토타입 6 대조 주문)
        constexpr float kTestIsolationRadius = 1200.0f;
        constexpr auto kFrenzyPlugin = "BerserkerFrenzy.esp"sv;
        constexpr RE::FormID kFrenzyEffectLocalId = 0x800;

        using DealDamageFn = void*(RE::Actor* attacker, RE::Actor* target, RE::Projectile* projectile, bool leftHand);
        // DllMain 로더 락 안에서 Address Library를 열지 않도록 함수 내부 static으로 지연 해석한다.
        void CallDealDamage(RE::Actor* attacker, RE::Actor* target, RE::Projectile* projectile, bool leftHand)
        {
            static const REL::Relocation<DealDamageFn> fn{ REL::RelocationID(37673, 38627) };
            (void)fn(attacker, target, projectile, leftHand);
        }

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
            ScenarioF_Start,
            ScenarioF_ProjectileWait,
            ScenarioF_F6TargetWait,
            ScenarioF_SavePump,
            ScenarioF_SaveWaitBeforeLoad,
            ScenarioF_AwaitReload,
            ScenarioF_ReloadSettle,
            ScenarioG_Start,
            ScenarioG_GemDropWait,
            ScenarioG_GemPickupWait,
            ScenarioG_GemRedropSetupWait,
            ScenarioG_GemRedropWait,
            ScenarioG_GemRedropPickupWait,
            ScenarioG_SaveBoundaryDropWait,
            ScenarioG_SavePump,
            ScenarioG_SaveWaitBeforeLoad,
            ScenarioG_AwaitReload,
            ScenarioG_ReloadSettle,
            ScenarioH_Start,
            ScenarioH_IndoorStartWait,
            ScenarioH_DungeonStart,
            ScenarioH_DungeonEntryWait,
            ScenarioH_DungeonCloseWait,
            ScenarioH_DungeonRecoveryWait,
            ScenarioH_OutdoorStartWait,
            ScenarioH_MapEntryWait,
            ScenarioH_FallRecoveryWait,
            ScenarioH_DeathInterceptTrigger,
            ScenarioH_DeathEscapeWait,
            ScenarioH_UnauthorizedEntryWait,
            ScenarioH_SafeEscapeWait,
            ScenarioH_ReenterStart,
            ScenarioH_ReenterWait,
            ScenarioH_BossKillWait,
            ScenarioH_PostMapAbandonWait,
            ScenarioH_CellResetWait,
            ScenarioH_NoContributionMapEntryWait,
            ScenarioH_NoContributionMapKillWait,
            ScenarioH_NoContributionMapReturnWait,
            ScenarioH_Arena4Prerequisites,
            ScenarioH_Arena4NegativeStart,
            ScenarioH_Arena4EntryWait,
            ScenarioH_Arena4NegativeKillWait,
            ScenarioH_Arena4NegativeReturnWait,
            ScenarioH_Arena4Phase2Wait,
            ScenarioH_Arena4Phase3Wait,
            ScenarioH_Arena4KillWait,
            ScenarioH_Arena4ReturnWait,
            ScenarioH_PostArenaReturnWait,
            ScenarioH_CosaveMapEntryWait,
            ScenarioH_SavePump,
            ScenarioH_SaveWaitBeforeLoad,
            ScenarioH_AwaitReload,
            ScenarioH_ReloadSettle,
            ScenarioH_Contract,
            ScenarioH_UninstallPrepare,
            ScenarioH_UninstallVerifyWait,
            EnemyEsOnly,
            QuitDelay,
            Done,
            SaveOnlyContent,
            CleanupContent,
            CleanupAwaitCommit,
            CleanupSaveFinal,
            CleanupReloadFinal
        };

        const char* PhaseName(Phase p) noexcept
        {
            switch (p) {
            case Phase::Inactive: return "Inactive";
            case Phase::SaveOnlyContent: return "SaveOnlyContent";
            case Phase::CleanupContent: return "CleanupContent";
            case Phase::CleanupAwaitCommit: return "CleanupAwaitCommit";
            case Phase::CleanupSaveFinal: return "CleanupSaveFinal";
            case Phase::CleanupReloadFinal: return "CleanupReloadFinal";
            case Phase::AwaitMainMenu: return "AwaitMainMenu";
            case Phase::MenuDelay: return "MenuDelay";
            case Phase::AwaitBaselineLoad: return "AwaitBaselineLoad";
            case Phase::BaselineSettle: return "BaselineSettle";
            case Phase::ScenarioA_Setup: return "ScenarioA_Setup";
            case Phase::ScenarioA_PanelOpenWait: return "ScenarioA_PanelOpenWait";
            case Phase::ScenarioA_PanelCloseWait: return "ScenarioA_PanelCloseWait";
            case Phase::ScenarioA_SavePump: return "ScenarioA_SavePump";
            case Phase::ScenarioA_SaveWaitBeforeLoad: return "ScenarioA_SaveWaitBeforeLoad";
            case Phase::ScenarioA_AwaitReload: return "ScenarioA_AwaitReload";
            case Phase::ScenarioA_ReloadSettle: return "ScenarioA_ReloadSettle";
            case Phase::ScenarioB_PrepareWeapon: return "ScenarioB_PrepareWeapon";
            case Phase::ScenarioB_SpawnWait: return "ScenarioB_SpawnWait";
            case Phase::ScenarioB_Channeling: return "ScenarioB_Channeling";
            case Phase::ScenarioB_AwaitStop: return "ScenarioB_AwaitStop";
            case Phase::ScenarioB_KillWait: return "ScenarioB_KillWait";
            case Phase::ScenarioC_EsTrigger: return "ScenarioC_EsTrigger";
            case Phase::ScenarioC_EsDelay1s: return "ScenarioC_EsDelay1s";
            case Phase::ScenarioC_EsRecharge3s: return "ScenarioC_EsRecharge3s";
            case Phase::ScenarioC_PrepareStagger: return "ScenarioC_PrepareStagger";
            case Phase::ScenarioC_StaggerWarmup: return "ScenarioC_StaggerWarmup";
            case Phase::ScenarioC_StaggerCleanupWait: return "ScenarioC_StaggerCleanupWait";
            case Phase::ScenarioC_NonHostileSpawnWait: return "ScenarioC_NonHostileSpawnWait";
            case Phase::ScenarioC_NonHostileFinishWait: return "ScenarioC_NonHostileFinishWait";
            case Phase::ScenarioC_NonHostileXpWait: return "ScenarioC_NonHostileXpWait";
            case Phase::ScenarioD_BaselineWait: return "ScenarioD_BaselineWait";
            case Phase::ScenarioD_AllocateAndPoolWait: return "ScenarioD_AllocateAndPoolWait";
            case Phase::ScenarioD_KeystoneCiVerify: return "ScenarioD_KeystoneCiVerify";
            case Phase::ScenarioD_SearchFocusWait: return "ScenarioD_SearchFocusWait";
            case Phase::ScenarioD_TabCloseWait: return "ScenarioD_TabCloseWait";
            case Phase::ScenarioD_SavePump: return "ScenarioD_SavePump";
            case Phase::ScenarioD_SaveWaitBeforeLoad: return "ScenarioD_SaveWaitBeforeLoad";
            case Phase::ScenarioD_AwaitReload: return "ScenarioD_AwaitReload";
            case Phase::ScenarioD_ReloadSettle: return "ScenarioD_ReloadSettle";
            case Phase::ScenarioE_Start: return "ScenarioE_Start";
            case Phase::ScenarioE_EquipPoolWait: return "ScenarioE_EquipPoolWait";
            case Phase::ScenarioE_DropWait: return "ScenarioE_DropWait";
            case Phase::ScenarioE_PickupWait: return "ScenarioE_PickupWait";
            case Phase::ScenarioE_CorpseWait: return "ScenarioE_CorpseWait";
            case Phase::ScenarioE_SavePump: return "ScenarioE_SavePump";
            case Phase::ScenarioE_SaveWaitBeforeLoad: return "ScenarioE_SaveWaitBeforeLoad";
            case Phase::ScenarioE_AwaitReload: return "ScenarioE_AwaitReload";
            case Phase::ScenarioE_ReloadSettle: return "ScenarioE_ReloadSettle";
            case Phase::ScenarioE_GearTabCloseWait: return "ScenarioE_GearTabCloseWait";
            case Phase::ScenarioF_Start: return "ScenarioF_Start";
            case Phase::ScenarioF_ProjectileWait: return "ScenarioF_ProjectileWait";
            case Phase::ScenarioF_F6TargetWait: return "ScenarioF_F6TargetWait";
            case Phase::ScenarioF_SavePump: return "ScenarioF_SavePump";
            case Phase::ScenarioF_SaveWaitBeforeLoad: return "ScenarioF_SaveWaitBeforeLoad";
            case Phase::ScenarioF_AwaitReload: return "ScenarioF_AwaitReload";
            case Phase::ScenarioF_ReloadSettle: return "ScenarioF_ReloadSettle";
            case Phase::ScenarioG_Start: return "ScenarioG_Start";
            case Phase::ScenarioG_GemDropWait: return "ScenarioG_GemDropWait";
            case Phase::ScenarioG_GemPickupWait: return "ScenarioG_GemPickupWait";
            case Phase::ScenarioG_GemRedropSetupWait: return "ScenarioG_GemRedropSetupWait";
            case Phase::ScenarioG_GemRedropWait: return "ScenarioG_GemRedropWait";
            case Phase::ScenarioG_GemRedropPickupWait: return "ScenarioG_GemRedropPickupWait";
            case Phase::ScenarioG_SaveBoundaryDropWait: return "ScenarioG_SaveBoundaryDropWait";
            case Phase::ScenarioG_SavePump: return "ScenarioG_SavePump";
            case Phase::ScenarioG_SaveWaitBeforeLoad: return "ScenarioG_SaveWaitBeforeLoad";
            case Phase::ScenarioG_AwaitReload: return "ScenarioG_AwaitReload";
            case Phase::ScenarioG_ReloadSettle: return "ScenarioG_ReloadSettle";
            case Phase::ScenarioH_Start: return "ScenarioH_Start";
            case Phase::ScenarioH_IndoorStartWait: return "ScenarioH_IndoorStartWait";
            case Phase::ScenarioH_DungeonStart: return "ScenarioH_DungeonStart";
            case Phase::ScenarioH_DungeonEntryWait: return "ScenarioH_DungeonEntryWait";
            case Phase::ScenarioH_DungeonCloseWait: return "ScenarioH_DungeonCloseWait";
            case Phase::ScenarioH_DungeonRecoveryWait: return "ScenarioH_DungeonRecoveryWait";
            case Phase::ScenarioH_OutdoorStartWait: return "ScenarioH_OutdoorStartWait";
            case Phase::ScenarioH_MapEntryWait: return "ScenarioH_MapEntryWait";
            case Phase::ScenarioH_FallRecoveryWait: return "ScenarioH_FallRecoveryWait";
            case Phase::ScenarioH_DeathInterceptTrigger: return "ScenarioH_DeathInterceptTrigger";
            case Phase::ScenarioH_DeathEscapeWait: return "ScenarioH_DeathEscapeWait";
            case Phase::ScenarioH_UnauthorizedEntryWait: return "ScenarioH_UnauthorizedEntryWait";
            case Phase::ScenarioH_SafeEscapeWait: return "ScenarioH_SafeEscapeWait";
            case Phase::ScenarioH_ReenterStart: return "ScenarioH_ReenterStart";
            case Phase::ScenarioH_ReenterWait: return "ScenarioH_ReenterWait";
            case Phase::ScenarioH_BossKillWait: return "ScenarioH_BossKillWait";
            case Phase::ScenarioH_PostMapAbandonWait: return "ScenarioH_PostMapAbandonWait";
            case Phase::ScenarioH_CellResetWait: return "ScenarioH_CellResetWait";
            case Phase::ScenarioH_NoContributionMapEntryWait: return "ScenarioH_NoContributionMapEntryWait";
            case Phase::ScenarioH_NoContributionMapKillWait: return "ScenarioH_NoContributionMapKillWait";
            case Phase::ScenarioH_NoContributionMapReturnWait: return "ScenarioH_NoContributionMapReturnWait";
            case Phase::ScenarioH_Arena4Prerequisites: return "ScenarioH_Arena4Prerequisites";
            case Phase::ScenarioH_Arena4NegativeStart: return "ScenarioH_Arena4NegativeStart";
            case Phase::ScenarioH_Arena4EntryWait: return "ScenarioH_Arena4EntryWait";
            case Phase::ScenarioH_Arena4NegativeKillWait: return "ScenarioH_Arena4NegativeKillWait";
            case Phase::ScenarioH_Arena4NegativeReturnWait: return "ScenarioH_Arena4NegativeReturnWait";
            case Phase::ScenarioH_Arena4Phase2Wait: return "ScenarioH_Arena4Phase2Wait";
            case Phase::ScenarioH_Arena4Phase3Wait: return "ScenarioH_Arena4Phase3Wait";
            case Phase::ScenarioH_Arena4KillWait: return "ScenarioH_Arena4KillWait";
            case Phase::ScenarioH_Arena4ReturnWait: return "ScenarioH_Arena4ReturnWait";
            case Phase::ScenarioH_PostArenaReturnWait: return "ScenarioH_PostArenaReturnWait";
            case Phase::ScenarioH_CosaveMapEntryWait: return "ScenarioH_CosaveMapEntryWait";
            case Phase::ScenarioH_SavePump: return "ScenarioH_SavePump";
            case Phase::ScenarioH_SaveWaitBeforeLoad: return "ScenarioH_SaveWaitBeforeLoad";
            case Phase::ScenarioH_AwaitReload: return "ScenarioH_AwaitReload";
            case Phase::ScenarioH_ReloadSettle: return "ScenarioH_ReloadSettle";
            case Phase::ScenarioH_Contract: return "ScenarioH_Contract";
            case Phase::ScenarioH_UninstallPrepare: return "ScenarioH_UninstallPrepare";
            case Phase::ScenarioH_UninstallVerifyWait: return "ScenarioH_UninstallVerifyWait";
            case Phase::EnemyEsOnly: return "EnemyEsOnly";
            case Phase::QuitDelay: return "QuitDelay";
            case Phase::Done: return "Done";
            default: return "Unknown";
            }
        }

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        Core::EnergyShieldPool* g_esPool = nullptr;
        Core::KillTracker* g_killTracker = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        Core::Config* g_config = nullptr;
        bool g_savedEnforceGemReq = true;
        bool g_hasSavedEnforceGemReq = false;
        Core::SkillSystem* g_skillSystem = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;

        // 5단계 시나리오 F 전용 상태
        std::uint64_t g_fProjLaunchedBefore = 0;
        std::uint64_t g_fProjImpactsBefore = 0;
        RE::ActorHandle g_fTargetHandle{};

        // 결정 185 적 ES 표시 단축 시험(-Only es). A(레어, ES 300)·B(매직, ES 300)·C(ES 없음)를 AI를 끈 채 세우고
        // 플레이어 직접 피해로 "마지막 교전 적"을 바꿔 가며 UiBridge가 마지막으로 보낸 표시 값을 본다. 조준점 경로는 보지 않는다.
        constexpr std::array<const char*, 7> kEnemyEsCheckNames{ "ES1_EngagedRareTarget", "ES2_SwitchToSameEsTarget",
            "ES3_NoEsEngagedHides", "ES6_IncomingHitTarget", "ES7_DamageOverTimeKeepsTarget", "ES4_DeadTargetHides",
            "ES5_EngagedHoldExpires" };
        constexpr int kEnemyEsOnlyChecks = static_cast<int>(kEnemyEsCheckNames.size());
        constexpr double kScenarioASaveTimeoutSec = 25.0;
        enum class SaveCheck : std::size_t { Content, Baseline, Write, Reload, State, Preserved };
        constexpr std::array kSaveCheckNames{ "S0_ContentCheckPassed", "S1_LoadBaselineNormal",
            "S2_SaveWroteGeneration", "S3_ReloadHasGeneration", "S4_StateRoundTrip", "S5_PreservedStoreEmpty" };
        std::array<bool, kSaveCheckNames.size()> g_saveChecksReported{};

        struct SaveOnlySnapshot
        {
            PersistentState state;
            std::size_t trackedGems = 0;
            std::size_t trackedItems = 0;
        };
        SaveOnlySnapshot g_saveOnlyBefore;

        enum class CleanupCheck : std::size_t { Content, Candidate, Committed, Items, Engine, State, Write, Reload };
        constexpr std::array kCleanupCheckNames{ "C0_ContentCheckPassed", "C1_LoadLegacyCandidate", "C2_CleanupCommitted",
            "C3_ItemsReturned", "C4_EngineReverted", "C5_StateCleared", "C6_SaveWritesTarget", "C7_ReloadNormal" };
        constexpr double kCleanupCommitTimeoutSec = 90.0;

        struct SaveFiles
        {
            fs::path ess;
            fs::path cosave;
            std::optional<fs::file_time_type> essBefore;
            std::optional<fs::file_time_type> cosaveBefore;
        };

        struct CleanupTestState
        {
            std::array<bool, kCleanupCheckNames.size()> reported{};
            // The baseline is derived from the source candidate while the engine still holds its old inventory.
            std::map<RE::FormID, std::pair<std::int32_t, std::size_t>> expectedItems;
            std::size_t expectedReturns = 0;
            std::size_t expectedWithoutForm = 0;
            std::optional<Clock::time_point> lastEligibleTick;
            double eligibleSeconds = 0.0;
            std::optional<MigrationDiagnostics> committed;
            std::array<std::pair<bool, std::string>, 3> outcomeChecks{};
            std::optional<SaveFiles> finalFiles;
            fs::file_time_type finalSaveRequestedAt{};
            std::map<RE::FormID, std::int32_t> starterItemsBeforeSave;
        };
        CleanupTestState g_cleanupTest;
        constexpr double kEnemyEsWaitSec = 2.0;
        constexpr double kEnemyEsTestMaxEs = 300.0;
        // 결정 185의 유지 시간. 제품 상수(Core::kEnemyEsEngagedHoldSec)가 바뀌면 시험이 잡도록 따로 둔다.
        constexpr double kEnemyEsExpectedHoldSec = 5.0;
        struct EnemyEsTestState
        {
            int step = 0;
            double stepStarted = 0.0;
            RE::ActorHandle a{};
            RE::ActorHandle b{};
            RE::ActorHandle c{};
            Core::EnemyEsView viewAfterA{};
            bool lastEngageOk = false;
            bool incomingStarted = false;
            bool shownBeforeIncoming = false;
            int incomingTries = 0;
            bool igniteSeen = false;
            bool dotStoleEngagement = false;
            bool visibleAt3s = false;
            std::set<std::string_view> reported;
        };
        EnemyEsTestState g_esTest{};

        // 적 ES 단축 시험의 액터를 죽이고 끈다(정상 끝·실패·시간 초과 모두. 이 시험은 저장하지 않음).
        void CleanupEnemyEsActors()
        {
            for (auto* handle : { &g_esTest.a, &g_esTest.b, &g_esTest.c }) {
                if (auto actor = handle->get()) {
                    if (!actor->IsDead()) {
                        actor->KillImmediate();
                    }
                    actor->Disable();
                }
                *handle = {};
            }
        }
        Core::InventoryItemKey g_fStarforgeKey{};
        std::uint32_t g_fSocketNodeId = 0;
        std::uint16_t g_fExpectedAuraMask = 0;
        double g_fExpectedCharmCharges = 28.0;
        bool g_fGuardAbsorbedOk = false;
        bool g_fProxySpellsSyncedOk = false;
        bool g_fUiEquipGemsOk = false;

        // 검수 검증 시나리오 G (C-1 4시나리오 + U1 + Q-1/Q-5/Q-6) 전용 상태
        std::uint64_t g_gGemInstanceId = 0;
        std::size_t g_gGemSupportCount = 0;
        RE::ObjectRefHandle g_gGemDropHandle{};
        RE::FormID g_gGemFirstDropRefId = 0;
        std::uint16_t g_gGemFirstDropStamp = 0;
        bool g_gGemStamped1 = false;
        bool g_gGemRelinked1 = false;
        bool g_gGemReequipped1 = false;
        bool g_gGemRedropStamped = false;
        bool g_gGemRedropRelinked = false;
        bool g_gSaveBoundaryWasWorldBeforeSave = false;
        RE::FormID g_gQ1ActorFormId = 0;
        float g_gQ1SpeedBeforeSave = 0.0f;

        // 6단계 시나리오 H (지도 장치·아틀라스·4차 결투장·안전 제거) 전용 상태
        RE::FormID g_hPreMapCellId = 0;
        RE::FormID g_hPreMapWsId = 0;
        RE::NiPoint3 g_hPreMapPosition{};
        bool g_hOutdoorMap = false;
        bool g_hIndoorReturnOk = false;
        std::string g_hIndoorReturnDetail;
        constexpr RE::FormID kHIndoorStartMarker = 0x00032AFB;   // Skyrim.esm QASmoke
        constexpr RE::FormID kHOutdoorStartMarker = 0x000215D4;  // Skyrim.esm COCMarkerWhiterun
        constexpr std::uint8_t kHMapTier = 10;
        constexpr std::uint8_t kHEntryPortals = 5;
        constexpr std::uint8_t kHReentryPortals = 4;
        constexpr std::uint16_t kHAtlasTierBit = 1u << (kHMapTier - 1u);
        constexpr std::uint32_t kHMapRewardLevel = 77;
        constexpr float kHSpawnMinDistance = 600.0f;
        constexpr double kHSpawnSettleSec = 0.5;
        constexpr double kHOriginWaitSec = 30.0;
        constexpr double kHDeathTriggerWaitSec = 10.0;
        constexpr float kHDeathProbeHealth = 2.0f;
        constexpr float kHDeathProbeHealthTolerance = 0.5f;
        constexpr double kHGuardClearSec = 30.0;
        constexpr double kHCombatStopWaitSec = 10.0;
        constexpr double kHAffixDamagePct = 25.0;
        constexpr double kHAffixReducedTakenPct = 20.0;
        constexpr double kHAffixIiqPct = 22.0;  // MonsterDamage 12 + MonsterLife 10
        constexpr double kHAffixProbeHit = 100.0;
        constexpr double kHDamageTolerance = 1e-4;
        constexpr double kHPlayerLifeTolerance = 1e-3;
        constexpr double kHArenaHitDamage = 99999.0;
        // 시험 환경은 전설 난이도(받는 피해 ×4)라 결투장 2페이즈 보스·증원이 한 대에 플레이어를 쓰러뜨리고, 사망 귀환이 회차를
        // 끝내 H6·H7이 보스를 볼 수 없었다(실행 13·14). 결투장 시험 동안만 최대 체력을 이만큼 임시로 올린다.
        constexpr float kHArenaPlayerHealthBuffer = 100000.0f;
        constexpr double kHMapBossHitDamage = 50000.0;
        constexpr double kHContributionHitDamage = 1.0;
        constexpr double kHBossHitIntervalSec = 0.25;
        // H19·H20 연타 전 준비(2026-11-24): 근접 타격 함수(AE 38627)는 공격자가 전투 중이면 전투 그룹의 대상 목록(AE 33230)에
        // 대상이 있을 때만 피해를 적용하므로, 보스가 플레이어를 전투 대상으로 잡고(마법은 시선까지) 난 뒤 연타한다.
        // 기다리는 동안은 무적으로 실제 공격을 막는다. 같은 날 전체 시험에서 준비가 된 상태(전투 중·대상 플레이어·거리 10)에서도
        // H19가 같은 모양으로 실패해, 이것이 H19의 원인은 아니었다. 준비 상태 로그를 남기는 진단으로 둔다.
        constexpr double kHBurstReadyWaitSec = 8.0;
        constexpr double kHArenaFloorFirst = 0.665;
        constexpr double kHArenaFloorSecond = 0.335;
        constexpr double kHArenaHealthRatioTolerance = 0.015;
        constexpr float kHArenaSaveTimerMax = 0.60f;
        constexpr float kHArenaSaveTimerMin = 0.20f;
        constexpr float kHArenaSaveTimerTolerance = 1e-3f;
        constexpr double kHSaveWaitSec = 25.0;
        constexpr double kHSaveBeforeLoadSec = 1.0;
        constexpr double kHReloadWaitSec = 90.0;
        constexpr double kHReloadSettleSec = 2.0;
        constexpr double kHReloadSettleLimitSec = 8.0;
        constexpr auto kHSaveRetryDelay = std::chrono::seconds(2);
        constexpr std::uint8_t kHArenaSecondPhase = 2;
        constexpr std::size_t kHArenaInitialReinforcements = 2;
        constexpr std::int32_t kHArenaExpectedAscendancyPoints = 8;
        constexpr std::uint32_t kHArenaExpectedBaseSpirit = 220;
        constexpr std::uint32_t kHAtlasPointBaselineLevel = 1;
        constexpr auto kHOutdoorReturnCheck = "H4_MapDeathInterceptAndPortalReturn";
        constexpr auto kHAffixCheck = "H2_MapAffixesAfterEntry";
        constexpr auto kHDungeonCheck = "H3_MapEntryTeleportAndAsyncSpawn";
        constexpr auto kHArenaCheck = "H6_Arena4ThreePhaseBossAndSpirit220";
        constexpr auto kHArenaCosaveCheck = "H7_ArenaPhase2CosaveRoundTrip";
        constexpr auto kHCompanionCheck = "H13_Arena4CompanionKillNoReward";
        constexpr auto kHBypassCheck = "H13_Arena4EarlyKillNoReward";
        constexpr auto kHPortalCheck = "H15_SixPortalEntries";
        constexpr auto kHNormalReturnCheck = "H16_NormalReturnKeepsPortals";
        constexpr auto kHLastPortalCheck = "H17_LastPortalSaveLoadDeath";
        constexpr auto kHArenaDeathCheck = "H18_ArenaSingleAttemptDeath";
        constexpr auto kHPhysicalBurstCheck = "H19_PhysicalLethalBurst";
        constexpr auto kHMagicBurstCheck = "H20_MagicLethalBurst";
        constexpr auto kHIniOffCheck = "H21_MapDeathInterceptDisabled";
        constexpr auto kHOpenWorldCheck = "H22_OpenWorldDeathNotIntercepted";
        constexpr auto kHCleanupCheck = "H23_MapCleanup20Runs";
        constexpr auto kHReentrySpawnsCheck = "H24_ReentryPreservesSpawns";
        constexpr auto kHFreshRunCheck = "H25_FreshRunAfterCellReset";
        constexpr auto kHBossRouteCheck = "H26_BossAnchorLanding";
        constexpr auto kHExitRouteCheck = "H27_AutoExitFallProbes";
        constexpr auto kHExitZoneReturnCheck = "H28_ExitZonePortalReturn";
        constexpr auto kHExitZoneRefusalCheck = "H29_ExitZoneRefusedBeforeBossKill";
        constexpr std::size_t kOriginalHCheckCount = 17;
        constexpr std::array<std::string_view, kOriginalHCheckCount + 15> kHChecks = {
            "H1_Stage6Esp192FormsAndMapKey", "H2_MapCraftingAndItemLevel", kHAffixCheck, kHDungeonCheck,
            kHOutdoorReturnCheck, "H5_MapReenterBossKillAndAtlasPoint", kHArenaCheck,
            "H7_MapsAndTreeCosaveRoundTrip", kHArenaCosaveCheck, "H8_SafeUninstallCleanupAndNoReapply",
            "H9_UnauthorizedEntryAndSafeEscape", "H10_MapFallRecovery", "H11_ClosedMapCellResetDispatch",
            "H12_Arena4Prerequisites", kHBypassCheck, kHCompanionCheck, "H14_MapBossWithoutContribution",
            kHPortalCheck, kHNormalReturnCheck, kHLastPortalCheck, kHArenaDeathCheck, kHPhysicalBurstCheck,
            kHMagicBurstCheck, kHIniOffCheck, kHOpenWorldCheck, kHCleanupCheck, kHReentrySpawnsCheck,
            kHFreshRunCheck, kHBossRouteCheck, kHExitRouteCheck, kHExitZoneReturnCheck, kHExitZoneRefusalCheck
        };
        std::array<bool, kHChecks.size()> g_hChecksReported{};
        std::optional<bool> g_hDeathInterceptBefore;
        std::optional<bool> g_hRouteGodModeBefore;
        // 결투장 체력 여유를 넣기 전의 플레이어 임시 체력 보정값. 값이 있으면 여유가 걸려 있다.
        std::optional<float> g_hArenaHealthBufferBase;
        struct HAffixProbe
        {
            double outgoing = 0.0;
            double incoming = 0.0;
            double iiq = 0.0;
        };
        std::optional<HAffixProbe> g_hPlainAffixProbe;
        std::size_t g_hDungeonIndex = 0;
        std::uint8_t g_hDungeonTier = 0;
        bool g_hDungeonsOk = true;
        std::string g_hDungeonsDetail;
        bool g_hDungeonOk = false;
        bool g_hDungeonMeasured = false;
        bool g_hDungeonCloseRetried = false;
        std::optional<Clock::time_point> g_hDungeonClosedAt;
        std::string g_hDungeonDetail;
        bool g_hDungeonResetExpected = false;
        std::string g_hArenaSpawnsDetail;
        std::optional<bool> g_hArenaSpawnsOk;
        std::optional<Clock::time_point> g_hBossWaitStartedAt;
        std::optional<Clock::time_point> g_hBossMoveRequestedAt;
        std::optional<Clock::time_point> g_hBossReadySince;
        std::optional<Clock::time_point> g_hDeathAttackStartedAt;
        std::string g_hDeathTriggerDetail;
        std::uint64_t g_hDeathRunId = 0;
        std::int32_t g_hEarnedTributeBagCount = 0;
        bool g_hEarnedTributeAvailable = false;
        bool g_hMapBossDamageKill = false;
        bool g_hArenaDamageKill = false;
        double g_hNextBossHitSec = 0.0;
        enum class HArenaTrial { EarnedTribute, Bypass, Companion };
        HArenaTrial g_hArenaTrial = HArenaTrial::EarnedTribute;
        std::optional<std::uint32_t> g_hMaskBeforeNegativeArena;
        std::size_t g_hArenaGateIndex = 0;
        bool g_hArenaGateOk = true;
        std::string g_hArenaGateDetail;
        bool g_hNegativeTributeOutstanding = false;
        std::int32_t g_hNegativeTributeBaseline = 0;
        std::uint8_t g_hCompanionAttackPhase = 0;
        bool g_hCompanionAttackPowerSet = false;
        RE::ActorHandle g_hArenaCompanion;
        RE::FormID g_hCompanionBase = 0;
        RE::FormID g_hCompanionCell = 0;
        std::optional<Clock::time_point> g_hCompanionCleanupStarted;
        bool g_hCompanionDisabled = false;
        bool g_hCompanionCleanupOk = false;
        constexpr double kHCompanionCleanupWaitSec = 15.0;
        bool g_hArenaCosaveInProgress = false;
        bool g_hArenaCosaveAttempted = false;
        bool g_hWatchArenaCoSave = false;  // g_msgMutex
        std::optional<float> g_hArenaTimerAtSave;  // g_msgMutex
        std::optional<float> g_hArenaTimerAtLoad;  // g_msgMutex
        double g_hArenaHealthBeforeSave = 0.0;
        double g_hPreMapPoeLife = 0.0;
        RE::FormID g_hEntryItemId = 0;
        Core::InventoryItemKey g_hEntryItemKey{};
        std::int32_t g_hEntryItemCount = 0;
        bool g_hEntryPendingWithoutConsumption = false;
        bool g_hUnauthorizedEntryObserved = false;
        // 최대 설정(입장 120초, 귀환 5회 + 마커 1회)에 비활성화 대기와 로딩 여유 시간을 더한다.
        constexpr double kMapEntryWaitSec = 150.0;
        constexpr double kMapReturnWaitSec = 780.0 + Core::kMapSpawnDisableWaitTimeoutSec;
        constexpr double kMapFallWaitSec = 10.0;
        // 제품이 보스 처치 반영을 기다리는 상한(Core::kBossDeathProcessWaitSec, 10초)보다 길게 둔다.
        constexpr double kMapBossKillWaitSec = 12.0;
        constexpr double kMapBossLoadWaitSec = 15.0;
        constexpr double kArenaPhase3InvulnerabilityWaitSec = 8.0;
        // 셀이 떨어진 뒤 보내고 VM 콜백까지 기다린다(콜백 시간 초과 10초로 다시 보내는 경우 포함).
        constexpr double kMapResetWaitSec = 60.0;
        constexpr int kExpectedAutoTestChecks = 82;  // A~F 45건, G 5건, H 32건
        static_assert(kExpectedAutoTestChecks == 50 + kHChecks.size());
        RE::FormID g_hMapCellId = 0;
        std::size_t g_hResetCountBeforeMap = 0;
        std::size_t g_hResetCompletedBeforeMap = 0;
        bool g_hExpectMapReset = false;
        std::uint64_t g_hFallRunId = 0;
        std::size_t g_hFallRecoveriesBefore = 0;
        std::uint8_t g_hFallPortals = 0;
        std::int32_t g_hAvailPtsBeforeBoss = 0;
        RE::FormID g_hMapBossFormId = 0;
        RE::FormID g_hTributeCellId = 0;
        struct HTributeCounts
        {
            struct FloorDrop
            {
                std::int32_t count;
                RE::ObjectRefHandle handle;
            };
            std::int64_t bag = 0;
            std::int64_t corpse = 0;
            std::int64_t floor = 0;
            std::unordered_map<RE::FormID, FloorDrop> floorDrops;
            [[nodiscard]] std::int64_t Total() const { return bag + corpse + floor; }
        };
        std::optional<HTributeCounts> g_hTributeCountBeforeBoss;
        bool g_hDeathEscapeIntercepted = false;
        int g_hDeathInterceptHitsSent = 0;
        constexpr auto kH14MapTier = Core::kMinMapTier;
        constexpr auto kH14AtlasTierBit = static_cast<std::uint16_t>(1u << (kH14MapTier - 1u));
        std::optional<Core::AtlasState> g_hAtlasBeforeNoContributionKill;
        bool g_hArenaBypassPrimed = false;
        std::int32_t g_hTributesBeforeArena = 0;
        std::uint64_t g_hXpBeforeNegativeKill = 0;
        std::uint32_t g_hLevelBeforeNegativeKill = 0;
        struct HGemProgress
        {
            std::uint64_t instanceId;
            std::uint8_t level;
            std::uint64_t xp;
            bool operator==(const HGemProgress&) const = default;
        };
        std::vector<HGemProgress> g_hGemsBeforeNegativeKill;
        std::int64_t g_hCorpseItemsBeforeNegativeKill = 0;
        std::array<Core::CharmSlotState, Core::kMaxCharmSlots> g_hCharmsBeforeNegativeKill{};
        constexpr float kHCompanionSpawnDistance = 70.0f;
        constexpr float kHCompanionReach = 200.0f;
        constexpr float kHCompanionUnarmedDamage = 99999.0f;
        constexpr double kHCompanionKillWaitSec = 20.0;
        constexpr double kHArenaPhaseHealthUnchangedTolerance = 1e-3;
        std::uint32_t g_hSpiritBeforeArena4 = 0;
        std::int32_t g_hAscPtsBeforeArena4 = 0;
        RE::FormID g_hArenaBossFormId = 0;
        bool g_hArenaReturnRequested = false;
        Core::ActiveMapState g_hArenaReturnState;
        std::optional<bool> g_hArenaResultBeforeReturn;
        std::string g_hArenaResultDetail;
        bool g_hArenaPhase1Clamped = false;
        bool g_hArenaInvulnBlocked = false;
        bool g_hArenaPhase2Clamped = false;
        std::uint8_t g_hArenaLastLoggedTransition = Core::kArena4FirstPhase;
        Core::AtlasState g_hAtlasBeforeCosave;
        std::uint32_t g_hMilestonesBeforeCosave = 0;
        Core::ActiveMapState g_hRunBeforeCosave;
        double g_hHpDeltaBeforeUninstall = 0.0;
        double g_hMpDeltaBeforeUninstall = 0.0;
        float g_hPermHpBeforeUninstall = 0.0f;
        float g_hPermMpBeforeUninstall = 0.0f;
        bool g_hUninstallResultOk = false;
        std::unordered_map<RE::TESBoundObject*, std::int32_t> g_hUninstallBagCounts;

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
        bool g_runError = false;
        bool g_globalTimeoutReported = false;

        // SKSE 메시징 스레드 <-> 주 갱신 스레드 공유 상태
        std::mutex g_msgMutex;
        bool g_postLoadSignaled = false;
        bool g_postLoadSucceeded = false;
        std::string g_expectedLoadStem;
        bool g_preLoadMatched = false;
        std::string g_expectedSaveStem;
        bool g_saveMatched = false;
        std::uint64_t g_saveSequenceAtMessage = 0;
        std::optional<SaveLoad::SaveDiagnostics> g_saveAtNextMessage;
        std::optional<SaveFiles> g_hSaveFiles;

        // 세이브 재시도 상태(프로토타입 5 검증: NPC 대화 등으로 Save()가 무시되면 2초마다 재시도)
        Clock::time_point g_nextSaveRetryAt{};
        int g_saveAttempts = 0;

        // 시나리오 B 추적 상태
        std::vector<RE::ActorHandle> g_spawnedSkeevers;
        int g_extraSkeeverWavesSpawned = 0;
        // B6: B가 만든 쥐의 사망 확정(보상 청구)이 모두 끝난 첫 시각. 확정 뒤 경험치는 한 프레임 늦게 큐에서 반영된다.
        std::optional<double> g_bKillsSettledAtSec;
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
        // 재검수(2026-11-21 자동 시험 실측): 시나리오 B의 늦은 사망 확정·레벨업이 C 도중 스탯을 다시 빌드하면 임시 ES가
        // 바뀌어 C2가 실행마다 갈렸다(스키버 추가 소환이 많을수록). C는 그 처리가 가라앉은 뒤 ES를 다시 주입하고 시작한다.
        bool g_cEsSettled = false;
        double g_cEsSettledAt = 0.0;
        int g_cEsRestarts = 0;  // C1~C2 사이에 스탯이 다시 빌드돼 ES 최대치가 바뀌면 C를 다시 시작한 횟수(Codex 지적 25)
        constexpr double kCEsSettleSec = 3.0;
        // B6: 쥐 사망 확정 뒤 경험치 반영(큐)까지의 여유와, 확정을 기다리는 상한(게임 시간, 일시정지 제외).
        constexpr double kBKillXpApplySec = 0.5;
        constexpr double kBKillSettleCapSec = 15.0;
        double g_esAfterHit = 0.0;
        CycloneTelemetry g_telBeforeStagger{};
        RE::ActorHandle g_foxHandle{};
        RE::FormID g_foxFormId = 0;
        std::uint32_t g_exileLevelBeforeFox = 1;
        std::uint64_t g_exileXpBeforeFox = 0;


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
            const auto hCheck = std::ranges::find(kHChecks, name);
            if (hCheck != kHChecks.end()) {
                auto& reported = g_hChecksReported[static_cast<std::size_t>(hCheck - kHChecks.begin())];
                if (std::exchange(reported, true)) {
                    LogWarn(std::format("{} was reported twice.", name));
                    return;
                }
            }
            if (ok) {
                ++g_passCount;
                WriteLine("PASS", std::format("{}: {}", name, detail));
            } else {
                ++g_failCount;
                WriteLine("FAIL", std::format("{}: {}", name, detail));
            }
        }

        bool SaveCheckReported(SaveCheck check)
        {
            return g_saveChecksReported[static_cast<std::size_t>(check)];
        }

        void CheckSave(SaveCheck check, bool ok, const std::string& detail)
        {
            const auto index = static_cast<std::size_t>(check);
            g_saveChecksReported[index] = true;
            Check(kSaveCheckNames[index], ok, detail);
        }

        void CheckCleanup(CleanupCheck check, bool ok, const std::string& detail)
        {
            const auto index = static_cast<std::size_t>(check);
            if (!std::exchange(g_cleanupTest.reported[index], true)) {
                Check(kCleanupCheckNames[index], ok, detail);
            }
        }

        void ReportCleanupCommitChecks(std::uint32_t saveThread)
        {
            if (!g_cleanupTest.committed || g_cleanupTest.reported[static_cast<std::size_t>(CleanupCheck::Committed)]) {
                return;
            }
            const auto& result = *g_cleanupTest.committed;
            CheckCleanup(CleanupCheck::Committed, result.step == Core::CleanupStep::Complete && result.starterGranted,
                std::format("{}; OnSave thread={}", result.Describe(),
                    saveThread == 0 ? "unobserved" : std::to_string(saveThread)));
            for (std::size_t i = 0; i < g_cleanupTest.outcomeChecks.size(); ++i) {
                const auto& [passed, detail] = g_cleanupTest.outcomeChecks[i];
                CheckCleanup(static_cast<CleanupCheck>(static_cast<std::size_t>(CleanupCheck::Items) + i), passed, detail);
            }
        }

        const char* SaveDispositionName(Core::SaveDisposition disposition)
        {
            switch (disposition) {
            case Core::SaveDisposition::Normal: return "Normal";
            case Core::SaveDisposition::FirstInstall: return "FirstInstall";
            case Core::SaveDisposition::CleanupFromLegacy: return "CleanupFromLegacy";
            case Core::SaveDisposition::MigrateGeneration: return "MigrateGeneration";
            case Core::SaveDisposition::Preserve: return "Preserve";
            case Core::SaveDisposition::ResumeMigration: return "ResumeMigration";
            }
            return "Unknown";
        }

        bool CheckSaveOnlyLoad(SaveCheck check, bool succeeded, const std::string& stem)
        {
            const auto load = SaveLoad::GetLastLoadDiagnostics();
            const auto pack = SaveLoad::GetContentGeneration();
            const auto mode = ModState::Get();
            const bool normal = mode->mode == ModMode::Normal;
            const bool normalDecision = load.decision && load.decision->disposition == Core::SaveDisposition::Normal;
            const auto* generation = load.generation ? &*load.generation : nullptr;
            const bool valid = generation && generation->status == Core::GenerationRecordStatus::Valid;
            const bool missing = generation && generation->status == Core::GenerationRecordStatus::Missing;
            const char* recordStatus = valid ? "valid" : missing ? "missing" : generation ? "damaged" : "unavailable";
            const auto describeGeneration = [](const Core::SaveGeneration& identity) {
                return std::format("game='{}', generation={}, buildId='{}'",
                    identity.game, identity.contentGeneration, identity.buildId);
            };
            const bool baseline = check == SaveCheck::Baseline;
            const bool legacyZero = missing && pack && pack->contentGeneration == 0;
            const bool matchesPack = valid && pack && generation->value.game == pack->game &&
                generation->value.contentGeneration == pack->contentGeneration && generation->value.buildId == pack->buildId;
            const bool generationOk = baseline ? (legacyZero || valid) : matchesPack;
            const bool ok = succeeded && normal && normalDecision && load.collectionComplete && generationOk;
            CheckSave(check, ok, std::format(
                "save='{}', kPostLoadGame={}, decision={} ({}), normalMode={} ({}), "
                "GENR={} [{}], pack=[{}], receivedLoad={}, collectionComplete={}{}",
                stem, succeeded, load.decision ? SaveDispositionName(load.decision->disposition) : "unavailable",
                load.decision ? load.decision->reason : "", normal, mode->reason, recordStatus,
                generation && !generation->value.game.empty() ? describeGeneration(generation->value) : "unavailable",
                pack ? describeGeneration(*pack) : "unavailable", load.receivedLoad, load.collectionComplete,
                ok && baseline ? (legacyZero ? " (missing GENR, normal generation 0)" : " (valid GENR, normal load)") : ""));
            return ok;
        }

        SaveOnlySnapshot CaptureSaveOnlySnapshot()
        {
            SaveOnlySnapshot snapshot;
            SaveLoad::RunSerializedTransaction([&] {
                snapshot.state = SaveLoad::GetStateCopy();
                snapshot.trackedGems = GemInventory::CountTracked();
                snapshot.trackedItems = ItemTracker::CountTracked();
            });
            return snapshot;
        }

        void CheckSaveOnlyRoundTrip()
        {
            const auto after = CaptureSaveOnlySnapshot();
            const auto& before = g_saveOnlyBefore.state;
            const auto& state = after.state;
            std::string differences;
            const auto compare = [&](std::string_view name, const auto& original, const auto& restored) {
                if (original != restored) {
                    std::format_to(std::back_inserter(differences), "{}{}: '{}' -> '{}'",
                        differences.empty() ? "" : ", ", name, original, restored);
                }
            };
            compare("exileLevel", before.exileLevel, state.exileLevel);
            compare("exileXp", before.exileXp, state.exileXp);
            compare("unallocatedPassivePoints", before.unallocatedPassivePoints, state.unallocatedPassivePoints);
            compare("className", before.className, state.className);
            compare("classSelected", before.tree.classSelected, state.tree.classSelected);
            compare("classId", before.tree.classId, state.tree.classId);
            compare("allocatedNodes.size", before.tree.allocatedNodes.size(), state.tree.allocatedNodes.size());
            compare("trackedGems", g_saveOnlyBefore.trackedGems, after.trackedGems);
            compare("trackedItems", g_saveOnlyBefore.trackedItems, after.trackedItems);
            compare("atlas.completedTierMask", before.atlas.completedTierMask, state.atlas.completedTierMask);
            compare("atlas.totalMapsCleared", before.atlas.totalMapsCleared, state.atlas.totalMapsCleared);
            compare("appliedHealthDelta", before.appliedHealthDelta, state.appliedHealthDelta);
            compare("appliedMagickaDelta", before.appliedMagickaDelta, state.appliedMagickaDelta);
            compare("activeAuraMask", before.activeAuraMask, state.activeAuraMask);
            for (std::size_t i = 0; i < state.slots.size(); ++i) {
                const auto& original = before.slots[i];
                const auto& restored = state.slots[i];
                if (original.occupied != restored.occupied || (original.occupied && original.gem.gemId != restored.gem.gemId)) {
                    std::format_to(std::back_inserter(differences), "{}slots[{}].gemId: {} -> {}",
                        differences.empty() ? "" : ", ", i,
                        original.occupied ? std::format("'{}'", original.gem.gemId) : "<empty>",
                        restored.occupied ? std::format("'{}'", restored.gem.gemId) : "<empty>");
                }
            }
            CheckSave(SaveCheck::State, differences.empty(),
                differences.empty() ? "All captured state fields matched." : differences);
        }

        void CheckSaveOnlyPreservedStore()
        {
            const bool normal = ModState::AllowsGameplay();
            const auto records = SaveLoad::GetPreservedRecordCount();
            CheckSave(SaveCheck::Preserved, normal && records == 0,
                std::format("normalMode={}, preservedRecords={}", normal, records));
        }

        // 지도 이동이 끝나(요청 없음·3D 준비) 기대 상태를 기다리기 시작한 시각(단계 경과 초). 음수면 아직 이동 중.
        double g_mapTravelSettledAtSec = -1.0;

        bool VerifyUninstallBagCounts(RE::PlayerCharacter* player)
        {
            bool matches = player != nullptr && !g_hUninstallBagCounts.empty();
            if (!player) {
                return false;
            }
            for (const auto& [base, expected] : g_hUninstallBagCounts) {
                const auto actual = player->GetItemCount(base);
                if (actual != expected) {
                    LogInfo(std::format("H8 inventory: base=0x{:08X}, expected={}, actual={}",
                        base->GetFormID(), expected, actual));
                    matches = false;
                }
            }
            return matches;
        }

        bool ExerciseUninstallWithMissingForm(RE::PlayerCharacter* player)
        {
            auto* av = player ? player->AsActorValueOwner() : nullptr;
            const auto before = SaveLoad::GetStateCopy();
            const auto charms = g_charmSystem ? g_charmSystem->Slots() : before.charmSlots;
            const auto withSupport = std::ranges::find_if(before.slots, [](const auto& slot) {
                return slot.occupied && !slot.gem.socketedGems.empty();
            });
            if (!av || withSupport == before.slots.end() || before.socketedJewels.empty() ||
                std::ranges::none_of(charms, [](const auto& slot) { return slot.occupied; })) {
                return false;
            }
            const auto testSlot = static_cast<std::size_t>(withSupport - before.slots.begin());
            g_hHpDeltaBeforeUninstall = before.appliedHealthDelta;
            g_hMpDeltaBeforeUninstall = before.appliedMagickaDelta;
            g_hPermHpBeforeUninstall = av->GetPermanentActorValue(RE::ActorValue::kHealth);
            g_hPermMpBeforeUninstall = av->GetPermanentActorValue(RE::ActorValue::kMagicka);
            g_hUninstallBagCounts.clear();

            bool allFormsFound = true;
            const auto expectReturn = [&](RE::TESBoundObject* base) {
                if (!base) {
                    allFormsFound = false;
                    return;
                }
                auto [it, added] = g_hUninstallBagCounts.try_emplace(base);
                if (added) {
                    it->second = player->GetItemCount(base);
                }
                ++it->second;
            };
            const auto expectGem = [&](const auto& self, const Core::GemInstance& gem) -> void {
                expectReturn(FormIds::Lookup<RE::TESObjectMISC>(FormIds::FindLocalFormIdForGem(gem.gemId)));
                for (const auto& socketed : gem.socketedGems) {
                    self(self, socketed);
                }
            };
            for (const auto& slot : before.slots) {
                if (slot.occupied) {
                    expectGem(expectGem, slot.gem);
                }
            }
            for (const auto& [node, jewel] : before.socketedJewels) {
                (void)node;
                auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(jewel.baseFormId);
                expectReturn(base ? base : FormIds::Lookup<RE::TESObjectMISC>(FormIds::JewelLocalFormId(jewel.jewelColor)));
            }
            for (const auto& charm : charms) {
                if (charm.occupied) {
                    expectReturn(FormIds::Lookup<RE::TESObjectMISC>(FormIds::CharmLocalFormId(charm.kind)));
                }
            }
            if (!allFormsFound) {
                return false;
            }

            // H7 저장 뒤에만 폼이 없는 보조 젬을 추가한다. 실물 반환 수와 완료 여부에는 영향을 주지 않는다.
            constexpr std::string_view kUnavailableSupport = "AutoTest_H8_MissingSupport";
            SaveLoad::UpdateState([&](PersistentState& state) {
                Core::GemInstance unavailable;
                unavailable.gemId = kUnavailableSupport;
                state.slots[testSlot].gem.socketedGems.push_back(std::move(unavailable));
            });
            const auto cleanup = MapDeviceRuntime::RunSafeUninstallCleanup(player);
            g_hUninstallResultOk = cleanup.ok &&
                cleanup.message.contains("돌려줄 수 없음(폼 유실) · 젬 1개, 주얼 0개, 참 0개.");
            LogInfo(cleanup.message);
            return true;
        }

        void EnterPhase(Phase next)
        {
            g_phase = next;
            g_phaseEnteredAt = Clock::now();
            g_mapTravelSettledAtSec = -1.0;
            g_hBossWaitStartedAt.reset();
            g_hBossMoveRequestedAt.reset();
            g_hBossReadySince.reset();
            g_hDungeonClosedAt.reset();
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

        void RestoreH14AtlasTier()
        {
            const auto before = std::exchange(g_hAtlasBeforeNoContributionKill, std::nullopt);
            if (before) {
                SaveLoad::UpdateState([&](PersistentState& st) {
                    const auto originalBit = before->completedTierMask & kH14AtlasTierBit;
                    st.atlas.completedTierMask = static_cast<std::uint16_t>(
                        (st.atlas.completedTierMask & ~kH14AtlasTierBit) | originalBit);
                });
            }
        }

        void CaptureHCompanionOwnership(RE::FormID cellId)
        {
            g_hCompanionBase = kNeutralNpcId;
            g_hCompanionCell = cellId;
            g_hCompanionCleanupStarted.reset();
            g_hCompanionDisabled = false;
            g_hCompanionCleanupOk = false;
        }

        void BeginHCompanionCleanup()
        {
            if (g_hArenaCompanion && !g_hCompanionCleanupStarted) {
                g_hCompanionCleanupStarted = Clock::now();
            }
        }

        bool IsHCompanionCleanupPending()
        {
            return g_hCompanionCleanupStarted.has_value();
        }

        bool WasHCompanionCleanupSuccessful()
        {
            return g_hCompanionCleanupOk;
        }

        void PumpHCompanionCleanup(bool canModify)
        {
            if (!g_hCompanionCleanupStarted) {
                return;
            }
            const auto actor = canModify ? g_hArenaCompanion.get() : RE::NiPointer<RE::Actor>{};
            const auto* base = actor ? actor->GetBaseObject() : nullptr;
            const auto* cell = actor ? actor->GetParentCell() : nullptr;
            if (actor && base && cell && base->GetFormID() == g_hCompanionBase && cell->GetFormID() == g_hCompanionCell) {
                if (!g_hCompanionDisabled) {
                    actor->Disable();
                    g_hCompanionDisabled = true;
                    return;
                }
                if (actor->IsDisabled()) {
                    actor->SetDelete(true);
                    g_hCompanionCleanupOk = true;
                }
            }
            if (!g_hCompanionCleanupOk &&
                std::chrono::duration<double>(Clock::now() - *g_hCompanionCleanupStarted).count() < kHCompanionCleanupWaitSec) {
                return;
            }
            if (!g_hCompanionCleanupOk) {
                LogInfo("H13 companion cleanup expired without proving ownership and disabling the actor.");
            }
            g_hCompanionCleanupStarted.reset();
            g_hArenaCompanion = {};
        }

        void ClearHNegativeArenaFixture(RE::PlayerCharacter* player)
        {
            if (g_hNegativeTributeOutstanding) {
                if (auto* item = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
                    player && item && !MapDeviceRuntime::GetTravelStatus(player).kind) {
                    if (MapDeviceRuntime::CountPlayerTributes(player) > g_hNegativeTributeBaseline) {
                        player->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    }
                    g_hNegativeTributeOutstanding = MapDeviceRuntime::CountPlayerTributes(player) > g_hNegativeTributeBaseline;
                }
            }
            BeginHCompanionCleanup();
        }

        void ReportMissingHChecks(const std::string& reason)
        {
            // 적 ES와 세이브 단축 실행은 H를 돌지 않는다.
            if (g_cfg.enemyEsOnly || g_cfg.saveOnly) {
                return;
            }
            for (std::size_t i = 0; i < kHChecks.size(); ++i) {
                const std::string_view name = kHChecks[i];
                if (g_cfg.routeOnly && name != kHBossRouteCheck && name != kHExitRouteCheck) {
                    continue;
                }
                if (!g_hChecksReported[i]) {
                    Check(kHChecks[i], false, std::format("Skipped in {}. {}", PhaseName(g_phase), reason));
                }
            }
        }

        bool ScheduleHCheckpointRestore(const std::string& reason);
        void ClearHArenaHealthBuffer(RE::PlayerCharacter* player);

        // CommonLib의 PlayerCharacter::SetGodMode는 이 함수를 멤버처럼 (this, enable)로 부르지만, AE 1.6.1170의 40500은
        // 첫 인자(cl)를 무적 플래그 전역에 쓰는 정적 함수다(2026-11-23 실행 파일 바이트: mov byte ptr [rip+...], cl; ret).
        // 그래서 켜려던 값 대신 플레이어 포인터의 하위 바이트가 들어간다. IsGodMode가 읽는 같은 전역에 직접 쓴다.
        void SetPlayerGodModeFlag(bool enable)
        {
            static REL::Relocation<bool*> flag{ RELOCATION_ID(517711, 404238) };
            *flag = enable;
        }

        bool RestoreHGodMode(RE::PlayerCharacter* player)
        {
            if (!g_hRouteGodModeBefore) {
                return true;
            }
            if (!player) {
                return false;
            }
            SetPlayerGodModeFlag(*g_hRouteGodModeBefore);
            if (RE::PlayerCharacter::IsGodMode() != *g_hRouteGodModeBefore) {
                return false;
            }
            g_hRouteGodModeBefore.reset();
            return true;
        }

        bool CleanUpScenariosBeforeFinish(const std::string& reason)
        {
            if (g_cfg.enemyEsOnly) {
                CleanupEnemyEsActors();
            }
            // 체크포인트 복원으로 돌아가기 전에 뺀다. 불러온 저장은 결투장 전 것이라 여유가 없다.
            ClearHArenaHealthBuffer(RE::PlayerCharacter::GetSingleton());
            if (ScheduleHCheckpointRestore(reason)) {
                return false;
            }
            CycloneRuntime::SetAutoTestHeld(false);
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                CycloneRuntime::StopNowOnMainThread(player);
            }
            UiBridge::ClosePanelOnMainThread();
            ReportMissingHChecks(reason);
            RestoreH14AtlasTier();
            ClearHNegativeArenaFixture(RE::PlayerCharacter::GetSingleton());
            if (const auto enabled = std::exchange(g_hDeathInterceptBefore, std::nullopt); enabled && g_config) {
                std::atomic_ref<bool>(g_config->mapDeathInterceptEnabled).store(*enabled, std::memory_order_relaxed);
            }
            (void)RestoreHGodMode(RE::PlayerCharacter::GetSingleton());
            if (const auto mask = std::exchange(g_hMaskBeforeNegativeArena, std::nullopt)) {
                SaveLoad::UpdateState([mask](PersistentState& st) { st.tree.ascendancyMilestonesBitmask = *mask; });
            }
            const bool restoreTestStats = !MapDeviceRuntime::IsUninstallCleanupArmed();
            if (restoreTestStats) {
                RevertTempEnergyShield();
            }
            // U2 확정(2026-11-20): AutoTest 실행 중 임시 비활성화한 enforceGemRequirements를 원래 INI 값으로 원복
            if (g_config && g_hasSavedEnforceGemReq) {
                g_config->enforceGemRequirements = g_savedEnforceGemReq;
                g_hasSavedEnforceGemReq = false;
            }
            // 검수 Major #4: 비정상 종료/타임아웃 시에도 임시 키스톤(CI/BM 등) 주입이 남지 않도록 원상태로 복원
            if (restoreTestStats) {
                SaveLoad::SyncStateToStatEngine();
                if (g_statEngine && g_dataPack && g_config && g_dataPack->IsLoaded()) {
                    (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                }
            }
            return true;
        }

        void FinishAndScheduleQuit(const std::string& reason)
        {
            if (g_phase == Phase::QuitDelay || g_phase == Phase::Done) {
                return;
            }
            if (g_cfg.cleanupOnly) {
                SaveLoad::SetCleanupTestTarget(false);
                ReportCleanupCommitChecks(0);
                for (std::size_t i = 0; i < kCleanupCheckNames.size(); ++i) {
                    CheckCleanup(static_cast<CleanupCheck>(i), false,
                        std::format("Skipped in {}. {}", PhaseName(g_phase), reason));
                }
            } else if (g_cfg.saveOnly) {
                for (std::size_t i = 0; i < kSaveCheckNames.size(); ++i) {
                    if (!g_saveChecksReported[i]) {
                        Check(kSaveCheckNames[i], false, std::format("Skipped in {}. {}", PhaseName(g_phase), reason));
                    }
                }
            } else if (!CleanUpScenariosBeforeFinish(reason)) {
                return;
            }
            {
                std::scoped_lock lock(g_msgMutex);
                g_expectedLoadStem.clear();
                g_preLoadMatched = false;
                g_expectedSaveStem.clear();
                g_saveMatched = false;
                g_hWatchArenaCoSave = false;
            }

            if (!reason.empty()) {
                LogInfo(reason);
            }
            // T-1 + 6단계: 기대 건수 및 WARN=0까지 충족해야 RESULT=PASS 인정
            // 경로 탐침만 도는 조사 실행은 H26·H27 두 건만 기대한다.
            const int expectedChecks = g_cfg.cleanupOnly ? static_cast<int>(kCleanupCheckNames.size()) :
                g_cfg.saveOnly ? static_cast<int>(kSaveCheckNames.size()) :
                g_cfg.routeOnly ? 2 : g_cfg.enemyEsOnly ? kEnemyEsOnlyChecks : kExpectedAutoTestChecks;
            const char* modeDescription = g_cfg.cleanupOnly ? " (cleanup-only migration run)" :
                g_cfg.saveOnly ? " (save-only round-trip run)" :
                g_cfg.routeOnly ? " (route-only investigation run)" : g_cfg.enemyEsOnly ? " (enemy-ES investigation run)" : "";
            const bool overallOk = !g_runError && g_failCount == 0 && g_warnCount == 0 && g_passCount == expectedChecks;
            const auto summary = std::format(
                "=== SUMMARY: PASS={}/{} FAIL={} WARN={} RESULT={}{} ===",
                g_passCount, expectedChecks, g_failCount, g_warnCount, overallOk ? "PASS" : "FAIL", modeDescription);
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

        void StartHContractAfterCosave(RE::PlayerCharacter* player);

        void ContinueHAfterCosaveFailure(const std::string& detail)
        {
            Check(g_hArenaCosaveInProgress ? kHArenaCosaveCheck : "H7_MapsAndTreeCosaveRoundTrip", false, detail);
            {
                std::scoped_lock lock(g_msgMutex);
                g_expectedSaveStem.clear();
                g_hWatchArenaCoSave = false;
            }
            g_hArenaCosaveInProgress = false;
            StartHContractAfterCosave(RE::PlayerCharacter::GetSingleton());
        }

        fs::path SaveDirectory()
        {
            // Keep the game's INI-relative path, including MO2's virtual profile save directory.
            const auto* local = RE::GetINISetting("sLocalSavePath:General");
            const char* subdir = local && local->GetType() == RE::Setting::Type::kString ? local->GetString() : nullptr;
            return g_reportPath.parent_path().parent_path() / (subdir && *subdir ? subdir : "Saves");
        }

        bool ReadSaveTimestamp(const fs::path& path, std::optional<fs::file_time_type>& timestamp)
        {
            std::error_code error;
            const auto status = fs::status(path, error);
            if (status.type() == fs::file_type::not_found) {
                timestamp.reset();
                return true;
            }
            if (error || !fs::is_regular_file(status)) {
                return false;
            }
            const auto modified = fs::last_write_time(path, error);
            if (error) {
                return false;
            }
            timestamp = modified;
            return true;
        }

        std::optional<SaveFiles> ObserveSaveFiles(const fs::path& stem)
        {
            auto base = SaveDirectory() / stem;
            SaveFiles files{ base, std::move(base), {}, {} };
            files.ess += ".ess";
            files.cosave += ".skse";
            if (!ReadSaveTimestamp(files.ess, files.essBefore) || !ReadSaveTimestamp(files.cosave, files.cosaveBefore)) {
                return std::nullopt;
            }
            return files;
        }

        bool SaveFileUpdated(const fs::path& path, std::optional<fs::file_time_type> before,
            std::optional<fs::file_time_type> requestedAt = std::nullopt)
        {
            std::optional<fs::file_time_type> modified;
            return ReadSaveTimestamp(path, modified) && modified && modified != before &&
                (!requestedAt || *modified >= *requestedAt);
        }

        bool BeginHVerifiedSave(const std::string& stem)
        {
            g_hSaveFiles = ObserveSaveFiles(stem);
            std::scoped_lock lock(g_msgMutex);
            g_expectedSaveStem = stem;
            g_saveMatched = false;
            return g_hSaveFiles.has_value();
        }

        bool IsHSaveWritten(const std::string& stem)
        {
            {
                std::scoped_lock lock(g_msgMutex);
                if (!g_saveMatched || g_expectedSaveStem != stem || !g_hSaveFiles) {
                    return false;
                }
            }
            const auto& files = *g_hSaveFiles;
            return SaveFileUpdated(files.ess, files.essBefore) && SaveFileUpdated(files.cosave, files.cosaveBefore);
        }

        std::optional<std::string> FindUnusedHCheckpointStem()
        {
            const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
            for (unsigned i = 0; i < kHCheckpointSaveCandidates; ++i) {
                auto stem = std::format("{}{}_{}_{}", g_cfg.tempSave, kHCheckpointSuffix, timestamp, i);
                if (ToLowerAscii(stem) == ToLowerAscii(g_cfg.baselineSave)) {
                    continue;
                }
                const auto files = ObserveSaveFiles(stem);
                if (files && !files->essBefore && !files->cosaveBefore) {
                    return stem;
                }
            }
            return std::nullopt;
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

        // H27 진단: 경로 탐침이 멈출 때 플레이어가 왜 준비되지 않았는지 남긴다(실행 15의 embershard 출구 −150은 관찰 표본 39개 뒤
        // 준비 상태가 풀리지 않아 계약이 중단됐다).
        std::string DescribeHRoutePlayerState(RE::PlayerCharacter* player, bool paused, const MapTravelStatus& travel)
        {
            auto* ui = RE::UI::GetSingleton();
            const auto* cell = player ? player->GetParentCell() : nullptr;
            const auto pos = player ? player->GetPosition() : RE::NiPoint3{};
            return std::format(
                "paused={}, 3D={}, dead={}, menus(main={}, loading={}, fader={}), travel={}, inPackCell={}, cell=0x{:08X}, pos={:.0f},{:.0f},{:.0f}, fallRecoveries={}",
                paused, player && player->Is3DLoaded(), player && player->IsDead(),
                ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME), ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME),
                ui && ui->IsMenuOpen(RE::FaderMenu::MENU_NAME),
                travel.kind ? static_cast<int>(*travel.kind) : -1, travel.inPackCell, cell ? cell->GetFormID() : 0u,
                pos.x, pos.y, pos.z, MapDeviceRuntime::GetFallRecoveryCount());
        }

        // 안전 탈출의 고정 도착 마커(화이트런 마구간 착지, Tamriel) 근처에 있는지. H9와 H27이 같은 판정을 쓴다.
        struct HSafeEscapeArrival
        {
            bool inTamriel = false;
            double distance = -1.0;
            [[nodiscard]] bool AtMarker() const noexcept
            {
                return inTamriel && distance >= 0.0 && distance <= MapDeviceRuntime::GetTravelArrivalRadius();
            }
        };

        HSafeEscapeArrival ObserveHSafeEscapeArrival(RE::PlayerCharacter* player)
        {
            constexpr RE::FormID kWhiterunStablesLanding = 0x0007287B;
            constexpr RE::FormID kTamriel = 0x0000003C;
            HSafeEscapeArrival arrival;
            const auto* marker = RE::TESForm::LookupByID<RE::TESObjectREFR>(kWhiterunStablesLanding);
            const auto* world = player ? player->GetWorldspace() : nullptr;
            arrival.inTamriel = world && world->GetFormID() == kTamriel;
            if (marker && player) {
                arrival.distance = (player->GetPosition() - marker->GetPosition()).Length();
            }
            return arrival;
        }

        // 게임 창이 앞에 있는지. 다른 창이 앞에 있으면 자동 이동 입력이 이동으로 이어지지 않았다(전체 시험 2026-11-24).
        // 엔진의 활성 표시(Main의 gameActive)가 실제 포커스를 따라가는지는 확인하지 못했으므로, 운영체제의 전면 창이 게임 창인지도 함께 본다.
        struct GameWindowState
        {
            bool engineActive = false;
            bool foreground = false;
            [[nodiscard]] bool InFront() const noexcept { return engineActive && foreground; }
        };

        GameWindowState GetGameWindowState()
        {
            GameWindowState state;
            if (const auto* main = RE::Main::GetSingleton()) {
                state.engineActive = main->GetRuntimeData().gameActive;
                state.foreground = main->wnd != nullptr && W32_IMPL_GetForegroundWindow() == main->wnd;
            }
            return state;
        }

        // 걷기 탐침 진단: 카메라·방향·자동 이동·조작 상태. 전체 시험 2026-11-24에서 걷기가 문 쪽으로 가지 않은 원인을 가르려고 남긴다.
        std::string DescribeHWalkState(RE::PlayerCharacter* player, float walkYaw)
        {
            const auto* camera = RE::PlayerCamera::GetSingleton();
            const auto* cameraState = camera ? camera->currentState.get() : nullptr;
            const auto* controls = RE::PlayerControls::GetSingleton();
            const auto* controlMap = RE::ControlMap::GetSingleton();
            const auto* actorState = player ? player->AsActorState() : nullptr;
            const auto window = GetGameWindowState();
            return std::format(
                "window(engineActive={}, foreground={}), camera(state={}, firstPerson={}, thirdPerson={}), heading={:.3f}, walkYaw={:.3f}, autoMove={}, "
                "controls(movement={}, looking={}), moving={}, running={}, sneaking={}, weaponDrawn={}, aiEnabled={}",
                window.engineActive, window.foreground,
                cameraState ? static_cast<int>(cameraState->id) : -1, camera && camera->IsInFirstPerson(),
                camera && camera->IsInThirdPerson(), player ? player->GetAngleZ() : 0.0f, walkYaw,
                controls && controls->data.autoMove, controlMap && controlMap->IsMovementControlsEnabled(),
                controlMap && controlMap->IsLookingControlsEnabled(), player && player->IsMoving(),
                player && player->IsRunning(), player && player->IsSneaking(), actorState && actorState->IsWeaponDrawn(),
                player && player->IsAIEnabled());
        }

        // 이동이 끝난 뒤 스폰·상태 반영을 기다리는 여유. 이동 요청이 실패로 사라졌으면 상한까지 기다리지 않는다.
        constexpr double kMapTravelSettleSec = 10.0;

        bool WaitForMapTravel(RE::PlayerCharacter* player, bool expectedState, double timeoutSec)
        {
            const double now = PhaseElapsedSec();
            if (now >= timeoutSec) {
                return false;
            }
            if (MapDeviceRuntime::GetTravelStatus(player).kind || !IsPlayerReadyInWorld()) {
                g_mapTravelSettledAtSec = -1.0;
                return true;
            }
            if (expectedState) {
                return false;
            }
            if (g_mapTravelSettledAtSec < 0.0) {
                g_mapTravelSettledAtSec = now;
            }
            return now - g_mapTravelSettledAtSec < kMapTravelSettleSec;
        }

        enum class HBossReadiness { Waiting, Ready, Unavailable };

        HBossReadiness PrepareHBoss(RE::PlayerCharacter* player, RE::Actor* boss, bool approach = false)
        {
            const auto now = Clock::now();
            if (!g_hBossWaitStartedAt) {
                g_hBossWaitStartedAt = now;
            }
            if (boss && (boss->IsDead() || boss->IsDisabled() || boss->IsDeleted())) {
                return HBossReadiness::Unavailable;
            }
            auto* values = boss ? boss->AsActorValueOwner() : nullptr;
            const auto health = values ? values->GetPermanentActorValue(RE::ActorValue::kHealth) : 0.0f;
            const bool withinReach = player && boss && player->GetParentCell() == boss->GetParentCell() &&
                (player->GetPosition() - boss->GetPosition()).Length() <= kHCompanionReach;
            if (player && boss && !g_hBossMoveRequestedAt && (!boss->Is3DLoaded() || (approach && !withinReach))) {
                player->MoveTo(boss);
                g_hBossMoveRequestedAt = now;
                return HBossReadiness::Waiting;
            }
            if (player && boss && player->GetParentCell() == boss->GetParentCell() &&
                boss->Is3DLoaded() && std::isfinite(health) && health > 0.0f &&
                IsPlayerReadyInWorld() && (!approach || withinReach)) {
                if (!g_hBossReadySince) {
                    g_hBossReadySince = now;
                }
                if (std::chrono::duration<double>(now - *g_hBossReadySince).count() >= kHSpawnSettleSec) {
                    return HBossReadiness::Ready;
                }
            } else {
                g_hBossReadySince.reset();
            }
            const auto since = g_hBossMoveRequestedAt.value_or(*g_hBossWaitStartedAt);
            return std::chrono::duration<double>(now - since).count() < kMapBossLoadWaitSec
                ? HBossReadiness::Waiting : HBossReadiness::Unavailable;
        }

        struct HSpawnLayout
        {
            std::vector<RE::FormID> ids;
            std::size_t bosses = 0;
            std::size_t adds = 0;
            std::size_t invalid = 0;
            float nearest = std::numeric_limits<float>::infinity();
            bool matchesRegistry = false;

            [[nodiscard]] bool Meets(std::size_t expectedAdds) const
            {
                return matchesRegistry && bosses == 1 && adds == expectedAdds && invalid == 0 &&
                    nearest >= kHSpawnMinDistance;
            }
        };

        HSpawnLayout ObserveHSpawnLayout(const MapPackRuntime::DungeonForms* forms, const Core::ActiveMapState& run)
        {
            HSpawnLayout observed;
            if (!forms || !forms->cell || !forms->cell->IsAttached() || !forms->entryMarker) {
                return observed;
            }
            const auto& addBases = run.isArena4 ? forms->addNpcs : forms->packNpcs;
            const auto entry = forms->entryMarker->GetPosition();
            forms->cell->ForEachReference([&](RE::TESObjectREFR* ref) {
                auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
                if (!actor || (actor->GetFormID() >> 24) != 0xFF || actor->GetParentCell() != forms->cell ||
                    actor->IsDeleted() || actor->IsDisabled()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto* base = actor->GetBaseObject();
                if (base == forms->bossNpc) {
                    ++observed.bosses;
                    if (actor->GetFormID() != run.bossRefFormId || !SkillExecution::IsBossOrDragon(actor)) {
                        ++observed.invalid;
                    }
                } else if (std::ranges::find(addBases, base) != addBases.end()) {
                    ++observed.adds;
                } else {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                observed.ids.push_back(actor->GetFormID());
                const float distance = (actor->GetPosition() - entry).Length();
                if (actor->IsDead() || !std::isfinite(distance)) {
                    ++observed.invalid;
                } else {
                    observed.nearest = std::min(observed.nearest, distance);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            auto registered = run.spawnedRefFormIds;
            std::ranges::sort(registered);
            std::ranges::sort(observed.ids);
            observed.matchesRegistry = registered == observed.ids;
            return observed;
        }

        std::optional<HAffixProbe> ProbeHMapAffixes(
            RE::Actor* boss, const Core::ActiveMapState& run, RE::FormID playerCellId)
        {
            if (!boss || boss->IsDead() || run.tier != kHMapTier) {
                return std::nullopt;
            }
            const auto kill = MapDeviceRuntime::GetMapKillContext(run.bossRefFormId, playerCellId);
            if (!kill.isBoss || kill.runId != run.runId || kill.rewardBlocked || kill.runFailed) {
                return std::nullopt;
            }
            const auto out = MonsterRuntime::GetOutgoingModifier(run.bossRefFormId);
            // 지도 보스의 Armoured 프로필은 고정이다. ES를 우회하고 엔진에 피해를 주지 않는다.
            const auto in = MonsterRuntime::ApplyPlayerHitToMonster(
                run.bossRefFormId, kHAffixProbeHit, Core::DamageType::Physical, true, boss);
            return HAffixProbe{ out.damageMultiplier, in.remainingHealthDamage, kill.bonusIiqPct };
        }

        bool MoveToHOrigin(RE::PlayerCharacter* player, RE::FormID markerId)
        {
            auto* marker = RE::TESForm::LookupByID<RE::TESObjectREFR>(markerId);
            if (!player || !marker || marker->IsDeleted()) {
                return false;
            }
            player->MoveTo(marker);
            return true;
        }

        bool AtHOrigin(RE::PlayerCharacter* player, RE::FormID markerId, bool outdoor)
        {
            const auto* marker = RE::TESForm::LookupByID<RE::TESObjectREFR>(markerId);
            const auto* cell = player ? player->GetParentCell() : nullptr;
            if (!marker || !cell || cell->IsInteriorCell() == outdoor || !IsPlayerReadyInWorld()) {
                return false;
            }
            return (outdoor ? player->GetWorldspace() && player->GetWorldspace() == marker->GetWorldspace()
                            : cell == marker->GetParentCell()) &&
                (player->GetPosition() - marker->GetPosition()).Length() <= MapDeviceRuntime::GetTravelArrivalRadius();
        }

        void CloseHRun(RE::PlayerCharacter* player)
        {
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            if (!travel.kind && IsPlayerReadyInWorld()) {
                if (MapDeviceRuntime::IsActiveMapRun()) {
                    (void)MapDeviceRuntime::AbandonActiveMap(player);
                } else if (travel.unauthorized) {
                    (void)MapDeviceRuntime::SafeEscape(player);
                }
            }
        }

        std::optional<HTributeCounts> CountMapBossTributes(
            RE::PlayerCharacter* player, RE::Actor* boss, const RE::TESObjectCELL* cell)
        {
            auto* tribute = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
            if (!player || !boss || !cell || !tribute) {
                return std::nullopt;
            }
            HTributeCounts result;
            result.bag = MapDeviceRuntime::CountPlayerTributes(player);
            const auto counts = boss->GetInventoryCounts([tribute](RE::TESBoundObject& item) { return &item == tribute; });
            if (const auto it = counts.find(tribute); it != counts.end()) {
                result.corpse = it->second;
            }
            cell->ForEachReference([&](RE::TESObjectREFR* ref) {
                if (ref && !ref->IsDeleted() && !ref->IsDisabled() && ref->GetBaseObject() == tribute) {
                    const auto count = std::max<std::int32_t>(1, ref->extraList.GetCount());
                    result.floor += count;
                    result.floorDrops.emplace(ref->GetFormID(), HTributeCounts::FloorDrop{ count, ref->CreateRefHandle() });
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            return result;
        }

        double BossHealthRatio(RE::Actor* boss)
        {
            const auto* values = boss ? boss->AsActorValueOwner() : nullptr;
            const double max = values ? values->GetPermanentActorValue(RE::ActorValue::kHealth) : 0.0;
            return max > 0.0 ? values->GetActorValue(RE::ActorValue::kHealth) / max
                : std::numeric_limits<double>::quiet_NaN();
        }

        struct HArenaBossObservation
        {
            double health = std::numeric_limits<double>::quiet_NaN();
            double permanentHealth = std::numeric_limits<double>::quiet_NaN();
            bool alive = false;
            std::optional<Core::MonsterProfile> profile;
            Arena4BossPhaseStatus phase;

            [[nodiscard]] double HealthRatio() const
            {
                return permanentHealth > 0.0 ? health / permanentHealth : std::numeric_limits<double>::quiet_NaN();
            }
        };

        HArenaBossObservation ObserveHArenaBoss(RE::Actor* boss, Arena4BossPhaseStatus phase)
        {
            HArenaBossObservation result;
            result.phase = phase;
            result.alive = boss && !boss->IsDead();
            if (const auto* values = boss ? boss->AsActorValueOwner() : nullptr) {
                result.health = values->GetActorValue(RE::ActorValue::kHealth);
                result.permanentHealth = values->GetPermanentActorValue(RE::ActorValue::kHealth);
                result.profile = MonsterRuntime::GetProfileCopy(g_hArenaBossFormId);
            }
            return result;
        }

        std::string DescribeHArenaBoss(const HArenaBossObservation& boss)
        {
            const auto shield = boss.profile
                ? std::format("{:.2f}/{:.2f}", boss.profile->currentEsSkyrim, boss.profile->maxEsSkyrim)
                : "unavailable";
            return std::format(
                "boss=0x{:08X}, alive={}, health={:.2f}, permanentMaxHealth={:.2f}, ratio={:.4f}, monsterES={}, phase(requested={}, completed={}), invulnerable={}",
                g_hArenaBossFormId, boss.alive, boss.health, boss.permanentHealth, boss.HealthRatio(), shield,
                boss.phase.requested, boss.phase.completed, boss.phase.invulnerable);
        }

        bool ArenaBossAtFloor(const HArenaBossObservation& boss, double floor, std::uint8_t from, std::uint8_t to)
        {
            const auto ratio = boss.HealthRatio();
            return boss.alive && ratio >= floor - kHArenaHealthRatioTolerance && ratio <= floor + kHArenaHealthRatioTolerance &&
                boss.phase.requested == to && boss.phase.completed == from;
        }

        void LogHArenaTransition(RE::Actor* boss, Arena4BossPhaseStatus phase, std::uint8_t expected)
        {
            if (phase.completed == expected && g_hArenaLastLoggedTransition != expected) {
                LogInfo(std::format("H6 phase {} transition complete: {}", expected, DescribeHArenaBoss(ObserveHArenaBoss(boss, phase))));
                g_hArenaLastLoggedTransition = expected;
            }
        }

        std::int64_t CountArenaCorpseItems(RE::Actor* boss)
        {
            std::int64_t count = 0;
            if (boss) {
                const auto loot = boss->GetInventoryCounts([](RE::TESBoundObject& item) {
                    return FormIds::ResolveLocalFormIdIfOurs(item.GetFormID()) != 0;
                });
                for (const auto& [item, amount] : loot) {
                    if (item && amount > 0) {
                        count += amount;
                    }
                }
            }
            return count;
        }

        const char* NegativeArenaCheck()
        {
            return g_hArenaTrial == HArenaTrial::Bypass ? kHBypassCheck : kHCompanionCheck;
        }

        std::vector<HGemProgress> ObserveHGemProgress(const PersistentState& state)
        {
            std::vector<HGemProgress> result;
            const auto gather = [&](const auto& self, const Core::GemInstance& gem) -> void {
                result.push_back({ gem.instanceId, gem.level, gem.xp });
                for (const auto& socketed : gem.socketedGems) {
                    self(self, socketed);
                }
            };
            for (const auto& slot : state.slots) {
                if (slot.occupied) {
                    gather(gather, slot.gem);
                }
            }
            return result;
        }

        void BeginHArenaNegativeKill(RE::Actor* boss)
        {
            const auto state = SaveLoad::GetStateCopy();
            g_hXpBeforeNegativeKill = state.exileXp;
            g_hLevelBeforeNegativeKill = state.exileLevel;
            g_hGemsBeforeNegativeKill = ObserveHGemProgress(state);
            g_hCorpseItemsBeforeNegativeKill = CountArenaCorpseItems(boss);
            g_hCharmsBeforeNegativeKill = g_charmSystem ? g_charmSystem->Slots()
                : std::array<Core::CharmSlotState, Core::kMaxCharmSlots>{};
        }

        bool StartHCosave(Core::ActiveMapState run, bool arena)
        {
            if (!BeginHVerifiedSave(g_cfg.tempSave)) {
                return false;
            }
            g_hArenaCosaveInProgress = arena;
            const auto state = SaveLoad::GetStateCopy();
            g_hAtlasBeforeCosave = state.atlas;
            g_hMilestonesBeforeCosave = state.tree.ascendancyMilestonesBitmask;
            g_hRunBeforeCosave = std::move(run);
            {
                std::scoped_lock lock(g_msgMutex);
                g_hWatchArenaCoSave = arena;
                g_hArenaTimerAtSave.reset();
                g_hArenaTimerAtLoad.reset();
            }
            if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
                mgr->Save(g_cfg.tempSave.c_str());
            }
            g_saveAttempts = 1;
            g_nextSaveRetryAt = Clock::now() + kHSaveRetryDelay;
            EnterPhase(Phase::ScenarioH_SavePump);
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

        void SendUiCmd(json cmd)
        {
            const auto content = SaveLoad::GetContentGeneration();
            cmd["buildId"] = content ? content->buildId : std::string{};
            cmd["uiProtocol"] = Core::UiBuildCheck::kUiProtocolVersion;
            UiBridge::ExecuteCommandForTestOnMainThread(cmd.dump());
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

        // 결투장 시험의 매 틱에 부른다. 처음 부를 때 지금의 임시 체력 보정을 기준으로 잡고, 기준+여유보다 모자란 만큼만 더한다.
        // 그래서 여러 번 불러도 한 번만 걸리고, 여유가 없는 저장을 불러온 뒤에도 다시 채워진다.
        void EnsureHArenaHealthBuffer(RE::PlayerCharacter* player)
        {
            auto* av = player ? player->AsActorValueOwner() : nullptr;
            if (!av) {
                return;
            }
            const float temporary = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth);
            const bool first = !g_hArenaHealthBufferBase;
            if (first) {
                g_hArenaHealthBufferBase = temporary;
            }
            const float missing = *g_hArenaHealthBufferBase + kHArenaPlayerHealthBuffer - temporary;
            if (missing > 0.5f) {
                av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth, missing);
                LogInfo(std::format("H arena health buffer {} (+{:.0f}; temporary health {:.0f} -> {:.0f}).",
                    first ? "applied" : "refilled", missing, temporary, temporary + missing));
            }
            RestorePlayerVitals(player, true, true);
        }

        // 여유를 뺀다. 지금 남아 있는 만큼만(최대 여유량) 빼므로 여유가 없는 저장을 불러온 뒤에 불러도 체력을 깎지 않는다.
        // 받은 피해를 먼저 채워, 최대 체력이 줄 때 현재 체력이 0 아래로 내려가지 않게 한다.
        void ClearHArenaHealthBuffer(RE::PlayerCharacter* player)
        {
            if (!g_hArenaHealthBufferBase) {
                return;
            }
            auto* av = player ? player->AsActorValueOwner() : nullptr;
            if (!av) {
                return;
            }
            const float base = *std::exchange(g_hArenaHealthBufferBase, std::nullopt);
            RestorePlayerVitals(player, false, true);
            const float temporary = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth);
            const float excess = std::min(temporary - base, kHArenaPlayerHealthBuffer);
            if (excess > 0.5f) {
                av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth, -excess);
            }
            LogInfo(std::format("H arena health buffer removed ({:.0f}; temporary health {:.0f} -> base {:.0f}).",
                std::max(excess, 0.0f), temporary, base));
        }

        MapActionResult OpenHMap(RE::PlayerCharacter* player, std::uint8_t tier, bool fixedAffixes)
        {
            auto* item = FormIds::Lookup<RE::TESObjectMISC>(FormIds::MapItemLocalFormId(tier));
            if (!player || !item) {
                return { false, "Map item or player unavailable." };
            }
            RestorePlayerVitals(player, true, true);
            player->StopCombat();
            const auto* cell = player->GetParentCell();
            const auto* world = player->GetWorldspace();
            g_hPreMapCellId = cell ? cell->GetFormID() : 0;
            g_hPreMapWsId = world ? world->GetFormID() : 0;
            g_hPreMapPosition = player->GetPosition();
            const auto stats = g_statEngine->GetSnapshot();
            g_hPreMapPoeLife = stats ? stats->poeMaxLife : 0.0;
            Core::InventoryItemKey key{};
            if (fixedAffixes) {
                key = ItemTracker::AddObjectWithTrackedUniqueId(player, item, UniqueIdUtil::CreateGameExtraDataList());
                if (!key.IsValid()) {
                    return { false, "Cannot create a tracked map fixture." };
                }
                Core::PoEItemRecord record{};
                record.baseFormId = item->GetFormID();
                record.rarity = Core::ItemRarity::Rare;
                record.itemLevel = Core::MapSystem::MonsterLevelForTier(tier);
                record.slotGroup = Core::TokenSlotGroup::Map;
                record.affixes = {
                    { "MapMod_MonsterDamage", { kHAffixDamagePct } },
                    { "MapMod_MonsterLife", { kHAffixReducedTakenPct } }
                };
                ItemTracker::Upsert(key, record);
            } else {
                player->AddObjectToContainer(item, nullptr, 1, nullptr);
            }
            g_hEntryItemId = item->GetFormID();
            g_hEntryItemKey = key;
            g_hEntryItemCount = player->GetItemCount(item);
            const auto opened = MapDeviceRuntime::OpenNormalMap(player, key, g_hEntryItemId);
            g_hEntryPendingWithoutConsumption = opened.ok &&
                MapDeviceRuntime::GetTravelStatus(player).kind == Core::MapTravelKind::OpenMap &&
                !MapDeviceRuntime::IsActiveMapRun() && player->GetItemCount(item) == g_hEntryItemCount &&
                (!key.IsValid() || ItemTracker::FindByKey(key).has_value());
            return opened;
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
                    // 난이도 배율(전설 0.4)과 시험용 추가 체력을 넘어 한 번에 죽도록 남은 체력의 10배 이상을 준다.
                    const auto* values = actor->AsActorValueOwner();
                    const float health = values ? values->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
                    (void)actor->DoDamage(std::max(1200.0f, health * 10.0f), player, true);
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
                g_expectedLoadStem = g_cfg.cleanupOnly ? saveStem : ToLowerAscii(saveStem);
                g_preLoadMatched = false;
                g_postLoadSignaled = false;
                g_postLoadSucceeded = false;
            }
            if (g_cfg.cleanupOnly) {
                SaveLoad::ExpectCleanupTestLoad(saveStem);
            }
            // 누락 모드 경고 모달로 멈추지 않도록 a_checkForMods=false로 로드한다.
            mgr->Load(saveStem.c_str(), false);
            return true;
        }

        bool IsCleanupTarget(const Core::SaveGeneration& generation)
        {
            return generation.game == "poe2" && generation.contentGeneration == 1;
        }

        bool ObserveCleanupBaseline(RE::PlayerCharacter* player, bool succeeded)
        {
            const auto load = SaveLoad::GetLastLoadDiagnostics();
            const auto candidate = SaveLoad::GetMigrationCandidate(player);
            const auto mode = ModState::Get();
            const bool pending = mode->mode == ModMode::CleanupPending;
            const bool legacy = load.decision && load.decision->disposition == Core::SaveDisposition::CleanupFromLegacy;
            if (!succeeded || !load.receivedLoad || !load.collectionComplete || !pending || !legacy || !candidate) {
                CheckCleanup(CleanupCheck::Candidate, false, std::format(
                    "save='{}', success={}, collected={}, decision={}, CleanupPending={}, reason={}",
                    g_cfg.baselineSave, succeeded, load.collectionComplete,
                    load.decision ? SaveDispositionName(load.decision->disposition) : "unavailable", pending, mode->reason));
                return false;
            }
            std::array<std::size_t, 4> returnKinds{};
            g_cleanupTest.expectedReturns = candidate->returns.size();
            for (const auto& entry : candidate->returns) {
                ++returnKinds[static_cast<std::size_t>(entry.kind)];
                if (auto* form = MigrationRuntime::ReturnForm(entry)) {
                    auto [it, inserted] = g_cleanupTest.expectedItems.try_emplace(form->GetFormID());
                    if (inserted) {
                        it->second.first = player->GetItemCount(form);
                    }
                    ++it->second.second;
                } else {
                    ++g_cleanupTest.expectedWithoutForm;
                }
            }
            std::string baselineCounts;
            for (const auto& [form, count] : g_cleanupTest.expectedItems) {
                baselineCounts += std::format(" {:08X}={} (+{})", form, count.first, count.second);
            }
            constexpr auto permanent = RE::ACTOR_VALUE_MODIFIER::kPermanent;
            CheckCleanup(CleanupCheck::Candidate, true, std::format(
                "save='{}', decision=CleanupFromLegacy, CleanupPending=true, {}, inventory=[{} ], "
                "returns(gem/support/jewel/charm)={}/{}/{}/{}, unresolvedReturnForms={}, permanent(health/magicka)={:.6f}/{:.6f}, "
                "delta={:.6f}/{:.6f}, mapRun={}, inMapCell={}",
                g_cfg.baselineSave, candidate->report, baselineCounts, returnKinds[0], returnKinds[1], returnKinds[2], returnKinds[3],
                g_cleanupTest.expectedWithoutForm,
                player->GetActorValueModifier(permanent, RE::ActorValue::kHealth),
                player->GetActorValueModifier(permanent, RE::ActorValue::kMagicka),
                candidate->healthDelta, candidate->magickaDelta, candidate->hasMapRun, candidate->inMapCell));
            return true;
        }

        std::pair<bool, std::string> CheckCleanupItems(RE::PlayerCharacter* player, const MigrationDiagnostics& result)
        {
            const auto totalLost = std::accumulate(result.lostReturns.begin(), result.lostReturns.end(), std::size_t{ 0 });
            if (g_cleanupTest.expectedReturns == 0) {
                return { totalLost == 0 && result.lostReturnsByForm.empty() && result.lostReturnsWithoutForm == 0,
                    "미실행(대상 없음)" };
            }
            bool matched = g_cleanupTest.expectedWithoutForm == result.lostReturnsWithoutForm;
            std::size_t totalIncrease = 0;
            std::size_t countedLosses = result.lostReturnsWithoutForm;
            auto detail = std::format("expected={}, unresolved(expected={}, lost={})", g_cleanupTest.expectedReturns,
                g_cleanupTest.expectedWithoutForm, result.lostReturnsWithoutForm);
            for (const auto& [id, baseline] : g_cleanupTest.expectedItems) {
                auto* form = RE::TESForm::LookupByID<RE::TESObjectMISC>(id);
                const auto lost = result.lostReturnsByForm.find(id);
                const auto lostCount = lost == result.lostReturnsByForm.end() ? 0 : lost->second;
                const auto increase = form ? player->GetItemCount(form) - baseline.first : -1;
                matched &= increase >= 0 && static_cast<std::size_t>(increase) + lostCount == baseline.second;
                totalIncrease += static_cast<std::size_t>(std::max(0, increase));
                countedLosses += lostCount;
                detail += std::format(" {:08X}(expected={}, increase={}, lost={})", id, baseline.second, increase, lostCount);
            }
            matched &= std::ranges::all_of(result.lostReturnsByForm,
                [](const auto& entry) { return g_cleanupTest.expectedItems.contains(entry.first); });
            matched &= countedLosses == totalLost && totalIncrease + totalLost == g_cleanupTest.expectedReturns;
            detail += std::format(" totalIncrease={}, totalLost={}", totalIncrease, totalLost);
            return { matched, std::move(detail) };
        }

        std::pair<bool, std::string> CheckCleanupEngine(RE::PlayerCharacter* player, const MigrationDiagnostics& result)
        {
            const auto current = MigrationRuntime::ObserveEngineResidue(player);
            const auto healthChange = result.healthAfter - result.healthBefore;
            const auto magickaChange = result.magickaAfter - result.magickaBefore;
            const bool deltas = std::abs(healthChange + result.healthDelta) <= Core::kLegacyDeltaTolerance &&
                std::abs(magickaChange + result.magickaDelta) <= Core::kLegacyDeltaTolerance;
            return { deltas && result.engineResidue.Empty() && current.Empty(),
                std::format("healthChange={:.6f} expected={:.6f}, magickaChange={:.6f} expected={:.6f}, "
                    "afterRevert(Variable09={}, proxySpells={}, proxyPowers={}), current(Variable09={}, proxySpells={}, proxyPowers={})",
                    healthChange, -result.healthDelta, magickaChange, -result.magickaDelta,
                    result.engineResidue.variable09, result.engineResidue.proxySpells, result.engineResidue.proxyPowers,
                    current.variable09, current.proxySpells, current.proxyPowers) };
        }

        std::pair<bool, std::string> CheckCleanupState()
        {
            const auto state = SaveLoad::GetStateCopy();
            const auto slots = std::ranges::count_if(state.slots, [](const auto& slot) { return slot.occupied; });
            const bool virtualSlotsEmpty = state.virtualRing2Key == Core::InventoryItemKey{} && state.virtualRing2BaseFormId == 0 &&
                state.virtualBeltKey == Core::InventoryItemKey{} && state.virtualBeltBaseFormId == 0;
            const auto charmOccupied = [](const auto& slot) { return slot.occupied; };
            const auto charms = std::ranges::count_if(state.charmSlots, charmOccupied);
            const auto liveCharms = g_charmSystem ? std::ranges::count_if(g_charmSystem->Slots(), charmOccupied) : charms;
            const auto trackedGems = GemInventory::CountTracked();
            const auto trackedItems = ItemTracker::CountTracked();
            const bool atlasReset = state.atlas.completedTierMask == 0 && state.atlas.totalMapsCleared == 0 &&
                state.atlas.highestTierCleared == 0 && !state.atlas.firstTributeGiven;
            const bool starter = state.starterGemsGiven && state.starterCurrencyGiven && state.starterStage5GemsGiven;
            const bool ok = state.exileLevel == 1 && state.exileXp == 0 && state.tree.allocatedNodes.empty() &&
                slots == 0 && state.activeAuraMask == 0 && virtualSlotsEmpty && charms == 0 && liveCharms == 0 &&
                state.socketedJewels.empty() && trackedGems == 0 && trackedItems == 0 && atlasReset && starter;
            return { ok, std::format("level={}, xp={}, nodes={}, skills={}, auras={}, virtualSlotsEmpty={}, charms={}/{}, jewels={}, "
                    "trackedGems={}, trackedItems={}, atlasReset={}, starterFlags={}/{}/{}", state.exileLevel, state.exileXp,
                    state.tree.allocatedNodes.size(), slots, state.activeAuraMask, virtualSlotsEmpty, charms, liveCharms,
                    state.socketedJewels.size(), trackedGems, trackedItems, atlasReset, state.starterGemsGiven,
                    state.starterCurrencyGiven, state.starterStage5GemsGiven) };
        }

        std::map<RE::FormID, std::int32_t> CaptureCleanupStarterItems(RE::PlayerCharacter* player)
        {
            std::map<RE::FormID, std::int32_t> counts;
            const auto capture = [&](RE::FormID localId) {
                auto* form = FormIds::Lookup<RE::TESObjectMISC>(localId);
                counts.emplace(localId, form ? player->GetItemCount(form) : -1);
            };
            for (const auto& entry : FormIds::kAllGemForms) {
                capture(entry.localFormId);
            }
            using enum Core::CurrencyKind;
            for (const auto kind : { ScrollOfWisdom, Transmutation, Augmentation, Alteration,
                    Regal, Alchemy, Chaos, Scouring, Regret, Jeweller }) {
                capture(FormIds::CurrencyLocalFormId(kind));
            }
            return counts;
        }

        std::pair<bool, std::string> CheckCleanupStarterItems(RE::PlayerCharacter* player)
        {
            const auto after = CaptureCleanupStarterItems(player);
            const auto& before = g_cleanupTest.starterItemsBeforeSave;
            bool equal = before.size() == after.size();
            std::string differences;
            for (const auto& [id, count] : after) {
                const auto previous = before.find(id);
                const auto savedCount = previous != before.end() ? previous->second : -1;
                if (savedCount < 0 || count != savedCount) {
                    equal = false;
                    differences += std::format(" {:06X}={}->{}", id, savedCount, count);
                }
            }
            return { equal, std::format("starterInventoryEqual={}, forms={}, differences=[{} ]", equal, after.size(), differences) };
        }

        void CloseDialogueWhileAwaitingSave(RE::UI* ui)
        {
            // 시험 세이브 주변 NPC가 말을 걸어 대화 메뉴가 열리면 저장이 막혀 시나리오 전체가 건너뛰어졌다(실행 20, E7).
            // 시험은 대화를 쓰지 않으므로 저장을 기다리는 동안 열린 대화 메뉴는 닫는다.
            if (ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
                bool awaitingSave = false;
                {
                    std::scoped_lock lock(g_msgMutex);
                    // 저장이 확인된 뒤(g_saveMatched)에는 정상 대화를 닫지 않는다(Codex 검토 2026-11-24).
                    awaitingSave = !g_expectedSaveStem.empty() && !g_saveMatched;
                }
                // 닫기 요청은 메뉴가 실제로 닫힐 때까지 몇 틱 걸리므로 1초에 한 번만 보내고 남긴다.
                static Clock::time_point lastDialogueClose{};
                if (awaitingSave && Clock::now() - lastDialogueClose >= std::chrono::seconds(1)) {
                    if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                        lastDialogueClose = Clock::now();
                        queue->AddMessage(RE::DialogueMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                        LogInfo(std::format("Closed a Dialogue Menu that blocked the pending test save in {}.", PhaseName(g_phase)));
                    }
                }
            }
        }

        void TickCleanupOnly()
        {
            const auto now = Clock::now();
            if (std::chrono::duration<double>(now - g_startedAt).count() > g_cfg.globalTimeoutSec) {
                FinishAndScheduleQuit(std::format("Cleanup AutoTest timed out in {}.", PhaseName(g_phase)));
                return;
            }
            auto* ui = RE::UI::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            const bool eligible = MigrationRuntime::CanTick(player);
            const auto finalSave = g_cfg.tempSave + kCleanupFinalSuffix;
            const auto loadSignal = [] {
                std::scoped_lock lock(g_msgMutex);
                return std::pair{ g_postLoadSignaled, g_postLoadSucceeded };
            };
            switch (g_phase) {
            case Phase::CleanupContent: {
                const auto content = SaveLoad::GetContentGeneration();
                const auto target = SaveLoad::GetCleanupTestTarget();
                const bool ok = content && target && IsCleanupTarget(*target) && ModState::AllowsGameplay();
                CheckCleanup(CleanupCheck::Content, ok, std::format("contentCheckPassed={}, target={}/{}",
                    content.has_value(), target ? target->game : "unavailable", target ? target->contentGeneration : 0));
                if (!ok || g_cfg.baselineSave.empty()) {
                    FinishAndScheduleQuit("Cleanup AutoTest requires verified content and a baseline save.");
                } else {
                    EnterPhase(Phase::AwaitMainMenu);
                }
                break;
            }
            case Phase::AwaitMainMenu:
                if ((ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) || IsPlayerReadyInWorld()) {
                    EnterPhase(Phase::MenuDelay);
                }
                break;
            case Phase::MenuDelay:
                if (PhaseElapsedSec() >= 2.0) {
                    if (RequestLoadSave(g_cfg.baselineSave)) {
                        EnterPhase(Phase::AwaitBaselineLoad);
                    } else {
                        FinishAndScheduleQuit("Could not request the cleanup baseline load.");
                    }
                }
                break;
            case Phase::AwaitBaselineLoad: {
                const auto [signaled, succeeded] = loadSignal();
                if (!signaled || (succeeded && !eligible)) {
                    if (PhaseElapsedSec() > 90.0) {
                        FinishAndScheduleQuit("Cleanup baseline load did not become ready.");
                    }
                    break;
                }
                if (!ObserveCleanupBaseline(player, succeeded)) {
                    FinishAndScheduleQuit("Baseline load did not produce the legacy candidate.");
                    break;
                }
                SaveLoad::ReleaseForTest();
                g_cleanupTest.lastEligibleTick = now;
                EnterPhase(Phase::CleanupAwaitCommit);
                break;
            }
            case Phase::CleanupAwaitCommit: {
                if (eligible && g_cleanupTest.lastEligibleTick) {
                    const auto elapsed = std::chrono::duration<double>(now - *g_cleanupTest.lastEligibleTick).count();
                    g_cleanupTest.eligibleSeconds += std::clamp(elapsed, 0.0, 0.25);
                }
                g_cleanupTest.lastEligibleTick = eligible ? std::optional{ now } : std::nullopt;
                if (g_cleanupTest.eligibleSeconds > kCleanupCommitTimeoutSec || ModState::Get()->mode == ModMode::Preserve) {
                    FinishAndScheduleQuit(std::format("Cleanup commit failed after {:.2f}s of eligible time. {}",
                        g_cleanupTest.eligibleSeconds, ModState::Get()->reason));
                    break;
                }
                if (!ModState::AllowsGameplay() || !eligible) {
                    break;
                }
                g_cleanupTest.committed = SaveLoad::GetLastMigrationDiagnostics();
                if (!g_cleanupTest.committed || g_cleanupTest.committed->step != Core::CleanupStep::Complete) {
                    FinishAndScheduleQuit("Normal mode was reached without a completed cleanup.");
                    break;
                }
                g_cleanupTest.outcomeChecks[0] = CheckCleanupItems(player, *g_cleanupTest.committed);
                g_cleanupTest.outcomeChecks[1] = CheckCleanupEngine(player, *g_cleanupTest.committed);
                g_cleanupTest.outcomeChecks[2] = CheckCleanupState();
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem = finalSave;
                    g_saveMatched = false;
                    g_saveAtNextMessage.reset();
                }
                g_nextSaveRetryAt = now;
                EnterPhase(Phase::CleanupSaveFinal);
                break;
            }
            case Phase::CleanupSaveFinal: {
                CloseDialogueWhileAwaitingSave(ui);
                bool matched = false;
                std::uint64_t before = 0;
                auto write = SaveLoad::GetLastSaveDiagnostics();
                {
                    std::scoped_lock lock(g_msgMutex);
                    matched = g_saveMatched;
                    before = g_saveSequenceAtMessage;
                    if (g_saveAtNextMessage) {
                        write = *g_saveAtNextMessage;
                    }
                }
                const bool timedOut = PhaseElapsedSec() > kScenarioASaveTimeoutSec || !ModState::AllowsGameplay();
                if (matched && write.sequence != before) {
                    ReportCleanupCommitChecks(write.sequence == before + 1 ? write.threadId : 0);
                    const bool noMigration = std::ranges::find(write.recordTypes, Core::kRecordMigration) == write.recordTypes.end();
                    const bool stamp = write.generation && IsCleanupTarget(*write.generation);
                    const auto tickThread = g_cleanupTest.committed->tickThreadId;
                    const bool sameThread = write.threadId != 0 && write.threadId == tickThread;
                    const bool callbackWritten = write.sequence == before + 1 && write.complete && write.failedRecords == 0 &&
                        noMigration && stamp && sameThread;
                    const auto& files = g_cleanupTest.finalFiles;
                    const auto requestedAt = g_cleanupTest.finalSaveRequestedAt;
                    const bool essUpdated = files && SaveFileUpdated(files->ess, files->essBefore, requestedAt);
                    const bool cosaveUpdated = files && SaveFileUpdated(files->cosave, files->cosaveBefore, requestedAt);
                    const bool written = callbackWritten && essUpdated && cosaveUpdated;
                    if (callbackWritten && !written && !timedOut) {
                        break;
                    }
                    std::string records;
                    for (const auto type : write.recordTypes) {
                        records += std::format(" {}{}{}{}", static_cast<char>(type >> 24), static_cast<char>(type >> 16),
                            static_cast<char>(type >> 8), static_cast<char>(type));
                    }
                    CheckCleanup(CleanupCheck::Write, written, std::format(
                        "save='{}', sequence={}->{}, complete={}, failedRecords={}, records=[{} ], GENR={}/{}, "
                        "tick thread={}, OnSave thread={}, sameThread={}, essUpdated={}, skseUpdated={}",
                        finalSave, before, write.sequence, write.complete, write.failedRecords, records,
                        write.generation ? write.generation->game : "unavailable",
                        write.generation ? write.generation->contentGeneration : 0, tickThread, write.threadId, sameThread,
                        essUpdated, cosaveUpdated));
                    if (written && RequestLoadSave(finalSave)) {
                        EnterPhase(Phase::CleanupReloadFinal);
                    } else {
                        FinishAndScheduleQuit("Could not verify the final save or request its reload.");
                    }
                } else if (timedOut) {
                    FinishAndScheduleQuit("Final co-save write did not complete.");
                } else if (!matched && eligible && now >= g_nextSaveRetryAt) {
                    g_nextSaveRetryAt = now + std::chrono::seconds(2);
                    if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
                        const fs::path stem{ std::u8string(finalSave.begin(), finalSave.end()) };
                        g_cleanupTest.finalFiles = ObserveSaveFiles(stem);
                        if (!g_cleanupTest.finalFiles) {
                            FinishAndScheduleQuit("Could not read the final ESS and SKSE timestamps before saving.");
                            break;
                        }
                        g_cleanupTest.starterItemsBeforeSave = CaptureCleanupStarterItems(player);
                        g_cleanupTest.finalSaveRequestedAt = fs::file_time_type::clock::now();
                        mgr->Save(finalSave.c_str());
                    }
                }
                break;
            }
            case Phase::CleanupReloadFinal: {
                const auto [signaled, succeeded] = loadSignal();
                if (!signaled || (succeeded && !eligible)) {
                    if (PhaseElapsedSec() > 90.0) {
                        FinishAndScheduleQuit("Final save reload did not become ready.");
                    }
                    break;
                }
                const auto load = SaveLoad::GetLastLoadDiagnostics();
                const auto [stateReset, stateDetail] = CheckCleanupState();
                const bool stamp = load.generation && load.generation->status == Core::GenerationRecordStatus::Valid &&
                    IsCleanupTarget(load.generation->value);
                const bool normal = load.decision && load.decision->disposition == Core::SaveDisposition::Normal &&
                    ModState::AllowsGameplay();
                const auto [starterItemsEqual, starterItemsDetail] = succeeded ? CheckCleanupStarterItems(player) :
                    std::pair{ false, std::string("starter inventory unavailable") };
                CheckCleanup(CleanupCheck::Reload, succeeded && load.receivedLoad && load.collectionComplete &&
                    stamp && normal && stateReset && starterItemsEqual, std::format(
                    "save='{}', success={}, collected={}, GENR=poe2/1:{}, normal={}, {}, {}",
                    finalSave, succeeded, load.collectionComplete, stamp, normal, stateDetail, starterItemsDetail));
                FinishAndScheduleQuit("Cleanup AutoTest finished.");
                break;
            }
            default:
                FinishAndScheduleQuit("Unexpected cleanup AutoTest phase.");
                break;
            }
        }


        // Default budget covers the portal sequence, 20 closures/reattachments and 17 landings.
        // 2026-11-23 실행 9: 계약 시작부터 정리 20회 끝까지 970초가 걸려 1050초로는 착지 탐침 17곳을 마치지 못했다.
        constexpr double kHContractBaseBudgetSec = 1500.0;
        constexpr double kHContractLongTravelBudgetMultiplier = 2.0;
        constexpr double kHContractTravelMarginSec = 20.0;
        constexpr double kHContractResetSec = 45.0;
        constexpr double kHRouteObserveSec = 3.0;
        constexpr double kHRouteProbeSec = 10.0;
        constexpr unsigned kHRouteMinHeightSamples = 8;
        constexpr std::size_t kHRouteProbesPerExit = 3;
        constexpr float kHRouteExitOffset = 150.0f;
        constexpr float kHRouteMaxDrop = 300.0f;
        constexpr float kHRouteArrivalAbove = 128.0f;
        constexpr float kHRouteArrivalXY = 192.0f;
        // 접근 자리 후보 삼각형의 문과의 높이 차 상한. Pinemoon은 문 근처 바닥이 문보다 150~162 낮다(2026-11-23
        // 내비메쉬 대조. 160이면 538유닛 떨어진 삼각형이 잡혔다).
        constexpr float kHRouteApproachMaxDz = 200.0f;
        // 출구 탐침은 떨어진 뒤 낙하 복귀나 착지까지 지켜봐야 하므로 더 길게 본다.
        constexpr double kHRouteExitProbeSec = 20.0;
        constexpr float kHRouteLandingStableZ = 1.0f;
        // 같은 셀 안의 위치 변경은 이 거리 안에서만 한다. 입구에서 약 8300유닛을 한 번에 옮기자 로딩 화면 없이 Mist·Fader가 열려
        // 닫히지 않았다(실행 22). 더 멀면 먼저 내비메쉬 위의 접근 자리로 엔진 이동을 한 뒤 짧게 옮긴다.
        constexpr float kHRouteShortHop = 1000.0f;
        // 로딩 화면 없이 Mist·Fader가 이만큼 열려 있으면 엔진 연출이 멈춘 것으로 보고 닫는다(실행 15·19·21·22).
        constexpr double kHRouteMistStallSec = 5.0;
        // 걷기 탐침이 접근 자리에서 문 쪽으로 걷는 최대 시간과, 접근 자리에 섰다고 보는 거리.
        constexpr double kHRouteWalkSec = 12.0;
        constexpr float kHRouteWalkStartRadius = 64.0f;
        // 게임 창이 뒤에 있으면 엔진이 자동 이동 입력을 이동으로 바꾸지 않았다(전체 시험 2026-11-24, 사용자가 다른 창 사용).
        // 그때는 위치를 문 쪽으로 조금씩 옮긴다(사용자 결정 "창이 뒤면 위치 이동"). 출구 구역 판정은 같고, 벽·지형 충돌은 보지 않는다.
        // 위치 이동 속도(유닛/초). 플레이어 달리기 속도 정도로 둔다. 한 틱 이동량이 낙하 복귀의 순간이동 판정에 걸리지 않을 만큼 작다.
        constexpr float kHRouteWalkStepSpeed = 250.0f;
        // 접근 자리는 출구 구역(목록의 exitZoneRadius) 밖에 이만큼 더 떨어진 걸을 수 있는 자리여야 한다(결정 177 ③ 재보완).
        constexpr float kHRouteApproachZoneClearance = 150.0f;
        // H28: 포털 시험의 몇 번째 입장 뒤 귀환을 출구 구역으로 하는지(나머지 귀환은 직접 포털 귀환).
        constexpr std::size_t kHExitZoneReturnEntry = 1;
        // H29: 결투장 출구에 세워 두는 시간(출구 구역의 거절 뒤 재판정 1초의 두 배 넘게)
        constexpr double kHExitZoneHoldSec = 2.5;
        constexpr float kHContractLowHealth = 2.0f;
        constexpr float kHContractMagicMagnitude = 1000.0f;
        constexpr float kHContractMagicEffectiveness = 1.0f;
        constexpr unsigned kHDeathBurstHits = 3;
        constexpr unsigned kHCleanupCycles = 20;
        constexpr std::uint8_t kHRegressionMapTier = 1;
        constexpr std::array<std::uint8_t, 6> kHExpectedPortalSequence{ 5, 4, 3, 2, 1, 0 };  // Decision 166.
        constexpr std::array<std::pair<std::string_view, std::size_t>, 5> kHExpectedAutoExits{{
            { "embershard", 1 }, { "pinemoon", 1 }, { "ironbind", 1 }, { "raldbthar", 0 }, { "shrouded_grove", 1 }
        }};

        double HContractTravelSec()
        {
            const auto settings = MapDeviceRuntime::GetTravelSettings();
            return settings.timeoutSec * (settings.maxAttempts + 1) + kHContractTravelMarginSec +
                Core::kMapSpawnDisableWaitTimeoutSec;
        }

        enum class HContractStage
        {
            PrepareCheckpoint, SaveCheckpoint, ClosePriorRun, OutdoorProbe, IndoorOrigin, Open, Entry, ExitZoneHold,
            Return, Reenter, SaveLastPortal, ExhaustedReentry, LoadLastPortal, Damage, DeathReturn,
            PartialKill, Close, Reset, VerifyEmptyCell, LeaveVerifiedCell, RecoverCleanup, RoutePrepare,
            RouteStart, RouteEntry, RouteObserve, RouteRecover, CloseRoutes,
            RestoreCheckpoint, AwaitCheckpoint
        };

        enum class HContractTrial { Portals, Arena, Physical, Magic, Cleanup };

        struct HMapSpawn
        {
            RE::FormID id;
            RE::FormID base;
            RE::ActorHandle handle;
            bool dead;
            bool operator==(const HMapSpawn& other) const
            {
                return id == other.id && base == other.base && dead == other.dead;
            }
        };

        // 결정 177 ③과 재보완(2026-11-24 사용자 확정 "구역 판정으로"): 출구는 런타임 출구 구역이 판정한다.
        // Boss: 보스 앵커에 서 있어야 함. Approach: 출구 구역 밖에서 가장 가까운 걸을 수 있는 내비메쉬 삼각형(셀에 들어간 뒤 계산)에
        // 서 있어야 함. Walk: 접근 자리에서 문 쪽으로 걸어 출구 구역이 안전 탈출을 걸어야 함. Exit: 출구 자리와 그 앞뒤(구역 안).
        // 출구 구역이 안전 탈출을 걸어야 함.
        enum class HRouteKind { Boss, Approach, Exit, Walk };

        struct HRouteSpot
        {
            const Core::MapDungeonDef* dungeon;
            RE::NiPoint3 position;
            RE::FormID exitId;  // zero for the boss anchor
            float exitOffset;
            HRouteKind kind = HRouteKind::Boss;
            bool positionResolved = true;
        };

        struct HCheckpoint
        {
            RE::FormID cell = 0;
            RE::FormID world = 0;
            RE::NiPoint3 position{};
            Core::AtlasState atlas;
            std::uint32_t milestones = 0;
            std::array<std::int32_t, FormIds::kMapItemForms.size()> maps{};
            std::vector<Core::PoEItemRecord> trackedMaps;
            std::int32_t tributes = 0;
        };

        HCheckpoint ObserveHCheckpoint(RE::PlayerCharacter* player)
        {
            HCheckpoint checkpoint;
            if (const auto* cell = player->GetParentCell()) {
                checkpoint.cell = cell->GetFormID();
            }
            if (const auto* world = player->GetWorldspace()) {
                checkpoint.world = world->GetFormID();
            }
            checkpoint.position = player->GetPosition();
            const auto state = SaveLoad::GetStateCopy();
            checkpoint.atlas = state.atlas;
            checkpoint.milestones = state.tree.ascendancyMilestonesBitmask;
            for (std::size_t i = 0; i < checkpoint.maps.size(); ++i) {
                if (auto* item = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kMapItemForms[i])) {
                    checkpoint.maps[i] = player->GetItemCount(item);
                }
            }
            for (const auto& entry : ItemTracker::TakeSnapshotForSave()) {
                if (!entry.isWorld && entry.key.ownerFormId == player->GetFormID() &&
                    entry.record.slotGroup == Core::TokenSlotGroup::Map) {
                    auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(entry.record.baseFormId);
                    if (base && ItemTracker::FindExtraListByKey(player, base, entry.key)) {
                        checkpoint.trackedMaps.push_back(entry.record);
                    }
                }
            }
            checkpoint.tributes = MapDeviceRuntime::CountPlayerTributes(player);
            return checkpoint;
        }

        bool HCheckpointMatches(const HCheckpoint& observed, const HCheckpoint& saved)
        {
            const auto sameMap = [](const Core::PoEItemRecord& a, const Core::PoEItemRecord& b) {
                return a.baseFormId == b.baseFormId && a.rarity == b.rarity && a.itemLevel == b.itemLevel &&
                    a.implicitAffixes == b.implicitAffixes && a.affixes == b.affixes;
            };
            return observed.cell == saved.cell && observed.world == saved.world &&
                (observed.position - saved.position).Length() <= MapDeviceRuntime::GetTravelArrivalRadius() &&
                observed.atlas.completedTierMask == saved.atlas.completedTierMask &&
                observed.atlas.totalMapsCleared == saved.atlas.totalMapsCleared &&
                observed.atlas.highestTierCleared == saved.atlas.highestTierCleared &&
                observed.atlas.firstTributeGiven == saved.atlas.firstTributeGiven &&
                observed.milestones == saved.milestones && observed.maps == saved.maps && observed.tributes == saved.tributes &&
                std::ranges::is_permutation(observed.trackedMaps, saved.trackedMaps, sameMap);
        }

        struct HContractState
        {
            HContractStage stage = HContractStage::PrepareCheckpoint;
            HContractTrial trial = HContractTrial::Portals;
            Clock::time_point started = Clock::now();
            Clock::time_point stageStarted = started;
            std::string checkpointStem;
            HCheckpoint checkpoint;
            bool checkpointSaved = false;
            bool restoreFinished = false;
            unsigned restoreAttempts = 0;
            std::string finishReason;
            bool openWorldRejected = false;
            bool zeroPortalRejected = false;
            std::uint64_t runId = 0;
            std::uint8_t entryCount = 0;
            unsigned normalReturns = 0;
            unsigned cleanupCycles = 0;
            std::uint64_t lastCleanupRunId = 0;
            bool cleanupOk = true;
            bool cleanupCycleOk = true;
            std::vector<HMapSpawn> priorCleanupSpawns;
            std::vector<HMapSpawn> cleanupSpawns;
            bool reentering = false;
            std::size_t resetCompletedBefore = 0;
            std::optional<Clock::time_point> cleanupReattachedAt;
            std::uint8_t portalsBeforeDeath = 0;
            std::uint64_t deathRequestsBefore = 0;
            std::uint64_t deathClampsBefore = 0;
            std::uint64_t deathRepeatClampsBefore = 0;
            Clock::time_point firstDamageAt{};
            bool deathMarkedFailed = false;
            bool movedToBoss = false;
            double attackWindowStarted = -1.0;
            double burstReadySince = -1.0;
            bool burstStartedCombat = false;
            unsigned damageRequests = 0;
            unsigned magicCastsSent = 0;
            std::optional<float> magicHealthBeforeBurst;
            float magicMaxHealthLoss = 0.0f;
            double nextDamageAt = 0.0;
            std::int32_t tributeBefore = 0;
            Core::ActiveMapState lastPortalSave;
            Core::ActiveMapState enteredRun;
            std::size_t expectedCorpses = 0;
            std::vector<HMapSpawn> spawnsBeforeReturn;
            std::vector<HRouteSpot> route;
            std::size_t routeIndex = 0;
            std::size_t bossSpotsPassed = 0;
            std::size_t approachSpotsPassed = 0;
            std::size_t exitSpotsPassed = 0;
            bool exitCountsMatch = true;
            double routeObservedSec = 0.0;
            float lowestZ = 0.0f;
            bool routeArrivalSeen = false;
            unsigned routeHeightSamples = 0;
            bool routeFell = false;
            std::optional<float> routeLandingZ;
            double routeLandingStableSec = 0.0;
            std::optional<float> routeRecoveryZ;
            double routeRecoveryStableSec = 0.0;
            std::size_t fallRecoveryBefore = 0;
            // 먼 출구로 가려고 접근 자리로 엔진 이동을 요청했는지(같은 탐침 안에서 한 번).
            bool routeStaged = false;
            // 로딩 화면 없이 Mist·Fader가 열린 시각, 닫아 준 횟수, 그 때문에 다시 시도한 탐침 번호.
            std::optional<Clock::time_point> routeMistSince;
            std::size_t routeMistStalls = 0;
            std::optional<std::size_t> routeRetriedIndex;
            // 걷기 탐침의 방향(라디안)과 통과 수.
            float walkYaw = 0.0f;
            std::size_t walkSpotsPassed = 0;
            // 걷기 탐침 동안 자동 이동을 건 틱(준비됨)과 걸지 못한 틱(준비 안 됨) 수. 실패 진단에 남긴다.
            std::size_t walkReadyTicks = 0;
            std::size_t walkNotReadyTicks = 0;
            // 걷기 탐침이 1인칭·자동 이동을 건 뒤 아직 풀지 않았는지
            bool walkActive = false;
            // 걷기 탐침: 준비된 상태로 자동 이동을 건 시간, 시작 위치, 위치 이동 방식으로 바꿨는지와 그 출발점·누적 거리, 바꾼 걷기 수
            double walkReadySec = 0.0;
            // 걷기 탐침의 게임 시간(틱의 deltaSec 합). 시간 초과는 실제 시간이 아니라 이것으로 본다(창이 뒤에서 멈춰 있던 시간 제외).
            double walkTickSec = 0.0;
            // 출구 구역이 안전 탈출을 건 탐침 뒤, 실제로 고정 도착 마커에 닿았는지(Codex 검토 2026-11-24: 요청만으로 통과를 세지 않음).
            bool routeEscapeExpected = false;
            std::size_t routeEscapeArrived = 0;
            std::size_t routeEscapeMissed = 0;
            bool walkStepMode = false;
            RE::NiPoint3 walkStepFrom;
            float walkStepDistance = 0.0f;
            std::size_t walkStepped = 0;
            // 결정 177 ③ 재보완: 탐침을 놓기 전의 출구 구역 이동 수, 경로 탐침 동안 로딩 없이 Mist·Fader가 열린 횟수(엔진 자동 문 발동의
            // 흔적, 0이어야 함), 경로 탐침 시작 때의 "켜져 있던 출구 문을 다시 끈 수".
            std::size_t exitZoneBefore = 0;
            bool routeMistOpen = false;
            std::size_t routeMistOpenings = 0;
            std::size_t exitDoorReenabledBefore = 0;
            // H28: 출구 구역으로 귀환시키는 중인지와 그 전의 출구 구역 이동 수
            bool exitZoneReturnPending = false;
            std::size_t exitZoneReturnBefore = 0;
            // H29(결정 182): 포털이 남은 입장과 마지막 포털 입장에서 출구 구역 귀환 거절 여부, 결투장 입장 때의 거절 여부와
            // 결투장 출구에 세운 뒤의 이동 수(결투장 출구 대기 단계에서 함께 판정)
            std::optional<bool> exitZoneRefusedWithPortals;
            std::optional<bool> exitZoneRefusedAtLastPortal;
            bool exitZoneRefusedAtArenaEntry = false;
            std::size_t exitZoneHoldBefore = 0;
            // 걷기 탐침이 1인칭으로 바꾸기 전 3인칭이었는지(경로 탐침이 끝나면 되돌림)
            bool routeRestoreThirdPerson = false;
            // 정리 시험을 시작할 때의 남은 스폰 청소 누적 수(결정 178 ⑦ 보완). 셀 초기화가 놓친 액터를 제품이 치운 횟수를 보고한다.
            std::size_t leftoverSweepBefore = 0;
        };

        HContractState g_hContract;

        void EnterHContract(HContractStage stage)
        {
            constexpr std::array names{
                "PrepareCheckpoint", "SaveCheckpoint", "ClosePriorRun", "OutdoorProbe", "IndoorOrigin", "Open", "Entry",
                "ExitZoneHold", "Return", "Reenter", "SaveLastPortal", "ExhaustedReentry", "LoadLastPortal", "Damage", "DeathReturn",
                "PartialKill", "Close", "Reset", "VerifyEmptyCell", "LeaveVerifiedCell", "RecoverCleanup", "RoutePrepare",
                "RouteStart", "RouteEntry", "RouteObserve", "RouteRecover", "CloseRoutes",
                "RestoreCheckpoint", "AwaitCheckpoint"
            };
            static_assert(names.size() == static_cast<std::size_t>(HContractStage::AwaitCheckpoint) + 1);
            g_hContract.stage = stage;
            g_hContract.stageStarted = Clock::now();
            LogInfo(std::format("H contract: {}.", names[static_cast<std::size_t>(stage)]));
        }

        bool BeginHContractSave(const std::string& stem, HContractStage stage)
        {
            if (!BeginHVerifiedSave(stem)) {
                return false;
            }
            g_nextSaveRetryAt = Clock::now();
            EnterHContract(stage);
            return true;
        }

        bool PumpHContractSave(const std::string& stem)
        {
            const auto now = Clock::now();
            if (IsHSaveWritten(stem)) {
                return true;
            }
            if (now >= g_nextSaveRetryAt) {
                if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
                    mgr->Save(stem.c_str());
                }
                g_nextSaveRetryAt = now + kHSaveRetryDelay;
            }
            return false;
        }

        bool ScheduleHCheckpointRestore(const std::string& reason)
        {
            if (!g_hContract.checkpointSaved || g_hContract.restoreFinished) {
                return false;
            }
            if (g_phase == Phase::ScenarioH_Contract && g_hContract.stage >= HContractStage::RestoreCheckpoint) {
                return true;
            }
            g_hContract.finishReason = reason;
            ReportMissingHChecks(reason);
            ClearHNegativeArenaFixture(RE::PlayerCharacter::GetSingleton());
            EnterPhase(Phase::ScenarioH_Contract);
            EnterHContract(HContractStage::RestoreCheckpoint);
            return true;
        }

        void AbortHContract(const std::string& reason)
        {
            // 걷기 탐침 도중에 중단되면 자동 이동과 1인칭이 남지 않게 한다(quit=false로 게임이 계속되는 경우).
            if (auto* controls = RE::PlayerControls::GetSingleton(); controls && controls->data.autoMove) {
                controls->data.autoMove = false;
            }
            if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && std::exchange(g_hContract.routeRestoreThirdPerson, false)) {
                (void)camera->ForceThirdPerson();
            }
            g_runError = true;
            LogInfo(reason);
            FinishAndScheduleQuit(reason);
        }

        void StartHContractAfterCosave(RE::PlayerCharacter* player)
        {
            // 결투장 저장 왕복이 실패해 여기로 오면 2페이즈 보스가 살아 있는 결투장 안일 수 있다. 체력 여유는 ClosePriorRun이
            // 회차 밖을 확인한 뒤에 뺀다.
            CloseHRun(player);
            g_hContract.started = Clock::now();
            EnterPhase(Phase::ScenarioH_Contract);
            EnterHContract(HContractStage::ClosePriorRun);
        }

        bool IsHMapSpawn(const RE::Actor* actor, const MapPackRuntime::DungeonForms& forms)
        {
            if (!actor || (actor->GetFormID() >> 24) != 0xFF || actor->GetParentCell() != forms.cell ||
                actor->IsDeleted() || actor->IsDisabled()) {
                return false;
            }
            const auto* base = actor->GetBaseObject();
            return base == forms.bossNpc || std::ranges::find(forms.packNpcs, base) != forms.packNpcs.end() ||
                std::ranges::find(forms.addNpcs, base) != forms.addNpcs.end();
        }

        std::optional<std::vector<HMapSpawn>> ObserveHMapSpawns(const MapPackRuntime::DungeonForms& forms)
        {
            if (!forms.cell || !forms.cell->IsAttached()) {
                return std::nullopt;
            }
            std::vector<HMapSpawn> spawns;
            forms.cell->ForEachReference([&](RE::TESObjectREFR* reference) {
                if (auto* actor = reference ? reference->As<RE::Actor>() : nullptr; IsHMapSpawn(actor, forms)) {
                    spawns.push_back({ actor->GetFormID(), actor->GetBaseObject()->GetFormID(), actor->GetHandle(), actor->IsDead() });
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            std::ranges::sort(spawns, {}, &HMapSpawn::id);
            return spawns;
        }

        bool HAllSpawnsBelongToRun(const std::vector<HMapSpawn>& spawns, const Core::ActiveMapState& run)
        {
            auto expected = Core::MapSystem::MapSpawnCleanupIds(run);
            std::ranges::sort(expected);
            return !spawns.empty() && std::ranges::equal(spawns, expected, {}, &HMapSpawn::id);
        }

        // H23 진단: 지금 회차 등록에 없는 액터와, 등록됐는데 셀의 살아 있는 지도 스폰으로 보이지 않는 FormID(꺼짐·삭제·다른 셀 포함)를
        // 적는다. 실행 14에서 남은 액터가 누구인지 몰랐다.
        std::string DescribeHSpawnMismatch(const std::vector<HMapSpawn>& spawns, const Core::ActiveMapState* run)
        {
            std::vector<RE::FormID> expected;
            if (run) {
                expected = Core::MapSystem::MapSpawnCleanupIds(*run);
                std::ranges::sort(expected);
            }
            std::string out;
            std::string present;
            for (const auto& spawn : spawns) {
                if (std::ranges::binary_search(expected, spawn.id)) {
                    present += std::format("{}0x{:08X}(base=0x{:08X}, dead={})", present.empty() ? "" : ",", spawn.id, spawn.base,
                        spawn.dead);
                    continue;
                }
                const auto actor = spawn.handle.get();
                const auto* cell = actor ? actor->GetParentCell() : nullptr;
                const auto pos = actor ? actor->GetPosition() : RE::NiPoint3{};
                out += std::format("{}0x{:08X}(base=0x{:08X}, dead={}, handle={}, cell=0x{:08X}, 3D={}, pos={:.0f},{:.0f},{:.0f})",
                    out.empty() ? "" : ", ", spawn.id, spawn.base, spawn.dead, static_cast<bool>(actor),
                    cell ? cell->GetFormID() : 0u, actor && actor->Is3DLoaded(), pos.x, pos.y, pos.z);
            }
            std::string missing;
            for (const auto id : expected) {
                if (std::ranges::none_of(spawns, [id](const HMapSpawn& spawn) { return spawn.id == id; })) {
                    missing += std::format("{}0x{:08X}", missing.empty() ? "" : ",", id);
                }
            }
            return std::format("unregistered=[{}], registered-present=[{}], registered-but-absent=[{}]", out, present, missing);
        }

        bool HHasPriorSpawn(const std::vector<HMapSpawn>& current, const std::vector<HMapSpawn>& prior)
        {
            return std::ranges::any_of(prior, [&](const HMapSpawn& old) {
                const auto oldActor = old.handle.get();
                return std::ranges::any_of(current, [&](const HMapSpawn& spawn) {
                    if (spawn.id != old.id || spawn.base != old.base) {
                        return false;
                    }
                    // A recycled FormID is acceptable if the handles distinguish the two references.
                    const auto actor = spawn.handle.get();
                    return !old.handle || !actor || actor.get() == oldActor.get();
                });
            });
        }

        bool HHasOtherNearbyHostile(RE::PlayerCharacter* player, const RE::Actor* boss)
        {
            bool found = false;
            player->GetParentCell()->ForEachReference([&](RE::TESObjectREFR* ref) {
                auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
                if (actor && actor != boss && actor != player && !actor->IsDead() && !actor->IsDisabled() &&
                    !actor->IsDeleted() && actor->IsHostileToActor(player) &&
                    (actor->GetPosition() - player->GetPosition()).Length() <= kTestIsolationRadius) {
                    found = true;
                    return RE::BSContainer::ForEachResult::kStop;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            return found;
        }

        // 2026-11-23 실행 8: 플레이어 쪽 StopCombat만으로는 10초 동안 전투가 풀리지 않았다. 플레이어와 싸우는 액터 전부의
        // 전투·경보도 멈춘다(Papyrus StopCombatAlarm이 부르는 엔진 함수).
        void StopPlayerCombatAndAlarms(RE::PlayerCharacter* player)
        {
            if (!player) {
                return;
            }
            player->StopCombat();
            if (auto* lists = RE::ProcessLists::GetSingleton()) {
                lists->StopCombatAndAlarmOnActor(player, false);
            }
        }

        // 플레이어를 전투 대상으로 잡고 있는 처리 중 액터(FormID, 셀, 거리). 재입장 전 전투가 안 풀릴 때 진단용.
        std::string DescribePlayerCombatants(RE::PlayerCharacter* player)
        {
            auto* lists = RE::ProcessLists::GetSingleton();
            if (!player || !lists) {
                return "unavailable";
            }
            std::string out;
            std::size_t count = 0;
            lists->ForAllActors([&](RE::Actor* actor) {
                if (!actor || actor == player || actor->IsDead() || !actor->IsCombatTarget(player)) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto* cell = actor->GetParentCell();
                const bool sameCell = cell && cell == player->GetParentCell();
                out += std::format("{}0x{:08X}(cell=0x{:08X}, {})", count == 0 ? "" : ", ", actor->GetFormID(),
                    cell ? cell->GetFormID() : 0u,
                    sameCell ? std::format("dist={:.0f}", (actor->GetPosition() - player->GetPosition()).Length())
                             : std::string("other cell"));
                ++count;
                return RE::BSContainer::ForEachResult::kContinue;
            });
            return count == 0 ? std::string("none found") : out;
        }

        bool HAtSavedReturn(RE::PlayerCharacter* player, const Core::ActiveMapState& run)
        {
            if (!player || !player->GetParentCell()) {
                return false;
            }
            if (player->GetParentCell()->IsInteriorCell()) {
                return player->GetParentCell()->GetFormID() == run.returnCellFormId;
            }
            const auto* world = player->GetWorldspace();
            const RE::NiPoint3 position{ run.returnPos[0], run.returnPos[1], run.returnPos[2] };
            return world && world->GetFormID() == run.returnWorldspaceFormId &&
                (player->GetPosition() - position).Length() <= MapDeviceRuntime::GetTravelArrivalRadius();
        }

        bool ApplyHFirebolt(RE::Actor* caster, RE::PlayerCharacter* player)
        {
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(kFireboltId);
            auto* magicCaster = caster->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
            if (!spell || !magicCaster) {
                return false;
            }
            magicCaster->CastSpellImmediate(
                spell, false, player, kHContractMagicEffectiveness, false, kHContractMagicMagnitude, caster);
            return true;
        }

        void ObserveHMagicHealthLoss(HContractState& test, const RE::ActorValueOwner* values)
        {
            if (test.magicHealthBeforeBurst && values) {
                const auto currentHealth = values->GetActorValue(RE::ActorValue::kHealth);
                test.magicMaxHealthLoss = std::max(test.magicMaxHealthLoss, *test.magicHealthBeforeBurst - currentHealth);
            }
        }

        bool HObservedMagicDamage(const HContractState& test, std::uint64_t clamps)
        {
            return test.magicCastsSent > 0 && (clamps > 0 || test.magicMaxHealthLoss > 0.0f);
        }

        std::string DescribeHMagicDelivery(const HContractState& test, std::uint64_t clamps)
        {
            return std::format("CastSpellImmediate calls={}/{}, last pre-burst HP={:.2f}, max HP loss={:.2f}, magic clamps={}",
                test.magicCastsSent, test.damageRequests,
                test.magicHealthBeforeBurst.value_or(std::numeric_limits<float>::quiet_NaN()), test.magicMaxHealthLoss, clamps);
        }

        void BeginHDamageTrial(const Core::ActiveMapState& run)
        {
            auto& test = g_hContract;
            test.damageRequests = 0;
            test.magicCastsSent = 0;
            test.magicHealthBeforeBurst.reset();
            test.magicMaxHealthLoss = 0.0f;
            test.movedToBoss = false;
            test.attackWindowStarted = -1.0;
            test.burstReadySince = -1.0;
            test.burstStartedCombat = false;
            test.firstDamageAt = {};
            test.portalsBeforeDeath = run.portalsRemaining;
            test.nextDamageAt = 0.0;
            if (!CombatHooks::AreAllHooksInstalled()) {
                const char* check = test.trial == HContractTrial::Portals ? kHLastPortalCheck :
                    test.trial == HContractTrial::Arena ? kHArenaDeathCheck :
                    test.trial == HContractTrial::Magic ? kHMagicBurstCheck : kHPhysicalBurstCheck;
                Check(check, false, "Combat hook installation is incomplete; no lethal damage was attempted.");
                EnterHContract(HContractStage::Close);
                return;
            }
            EnterHContract(HContractStage::Damage);
        }

        void FailHCleanupCycle(const std::string& reason)
        {
            g_hContract.cleanupCycleOk = false;
            LogInfo(std::format("H cleanup cycle {}: {}", g_hContract.cleanupCycles + 1, reason));
        }

        void AdvanceHCleanupCycle()
        {
            auto& test = g_hContract;
            test.cleanupOk = test.cleanupOk && test.cleanupCycleOk;
            test.priorCleanupSpawns = test.cleanupCycleOk ? std::move(test.cleanupSpawns) : std::vector<HMapSpawn>{};
            test.lastCleanupRunId = test.runId;
            ++test.cleanupCycles;
            test.cleanupCycleOk = true;
            test.cleanupSpawns.clear();
            test.runId = 0;
            if (test.cleanupCycles == kHCleanupCycles) {
                Check(kHCleanupCheck, test.cleanupOk,
                    std::format("{} cleanup cycles attempted; each must close, reset and show no active actors after reattachment "
                                "(leftover actors swept on reattach since H24: {}).",
                        test.cleanupCycles, MapDeviceRuntime::GetLeftoverSweepCleanedCount() - test.leftoverSweepBefore));
                EnterHContract(HContractStage::RoutePrepare);
            } else {
                EnterHContract(HContractStage::Open);
            }
        }

        void RecoverHCleanupCycle(const std::string& reason)
        {
            FailHCleanupCycle(reason);
            if (!RequestLoadSave(g_hContract.checkpointStem)) {
                AbortHContract("H cleanup recovery load could not be requested.");
                return;
            }
            EnterHContract(HContractStage::RecoverCleanup);
        }

        // 자동 출구에서 수평으로 가장 가까운 걸을 수 있는 내비메쉬 삼각형의 중심(높이 차 kHRouteApproachMaxDz 이하이고,
        // 문에서 3차원으로 minDistance 이상: 출구 구역 밖). 셀이 붙어 있어야 내비메쉬가 메모리에 있다.
        std::optional<RE::NiPoint3> FindExitApproachPoint(RE::TESObjectCELL* cell, const RE::NiPoint3& door, float minDistance)
        {
            auto* navMeshes = cell ? cell->GetRuntimeData().navMeshes : nullptr;
            if (!navMeshes) {
                return std::nullopt;
            }
            std::optional<RE::NiPoint3> best;
            float bestDistance = std::numeric_limits<float>::infinity();
            for (const auto& navPtr : navMeshes->navMeshes) {
                const auto* nav = navPtr.get();
                if (!nav) {
                    continue;
                }
                const auto& vertices = nav->vertices;
                for (const auto& tri : nav->triangles) {
                    if (tri.triangleFlags.any(RE::BSNavmeshTriangle::TriangleFlag::kDeleted) ||
                        tri.vertices[0] >= vertices.size() || tri.vertices[1] >= vertices.size() ||
                        tri.vertices[2] >= vertices.size()) {
                        continue;
                    }
                    const auto& a = vertices[tri.vertices[0]].location;
                    const auto& b = vertices[tri.vertices[1]].location;
                    const auto& c = vertices[tri.vertices[2]].location;
                    const RE::NiPoint3 centroid{ (a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f };
                    if (!std::isfinite(centroid.z) || std::abs(centroid.z - door.z) > kHRouteApproachMaxDz ||
                        (centroid - door).Length() < minDistance) {
                        continue;
                    }
                    const float distance = std::hypot(centroid.x - door.x, centroid.y - door.y);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        best = centroid;
                    }
                }
            }
            return best;
        }

        // 걷기 탐침이 건 자동 이동과 1인칭을 푼다. 1인칭 3D가 보이는 상태로 로딩하면 엔진의 1인칭 컬링
        // (AE 36563 Main::unk_FPCulling)이 그리는 도중 사라진 1인칭 3D를 건드려 CTD가 났다(전체 시험 2026-11-24).
        // routeRestoreThirdPerson은 지우지 않는다. 다음 걷기 탐침이 다시 1인칭으로 바꾸고, 경로가 끝날 때도 한 번 더 되돌린다.
        void StopHWalk()
        {
            auto& test = g_hContract;
            test.walkActive = false;
            test.walkReadyTicks = 0;
            test.walkNotReadyTicks = 0;
            if (auto* controls = RE::PlayerControls::GetSingleton()) {
                controls->data.autoMove = false;
            }
            if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && test.routeRestoreThirdPerson && !camera->IsInThirdPerson()) {
                (void)camera->ForceThirdPerson();
            }
        }

        void RecordHRouteSpot(bool ok, std::string_view reason, bool awaitRecovery = false)
        {
            auto& test = g_hContract;
            const auto& spot = test.route[test.routeIndex];
            const char* label = spot.kind == HRouteKind::Boss ? "boss" : spot.kind == HRouteKind::Approach ? "approach"
                : spot.kind == HRouteKind::Walk ? "walk" : "exit";
            if (spot.kind == HRouteKind::Walk) {
                if (!ok && test.walkActive) {
                    // 자동 이동을 끄기 전의 상태를 남긴다(걷기를 시작하기 전의 실패에는 남길 것이 없다).
                    LogInfo(std::format("H route walk diagnostics: {}, readyTicks={}, notReadyTicks={}.",
                        DescribeHWalkState(RE::PlayerCharacter::GetSingleton(), test.walkYaw), test.walkReadyTicks,
                        test.walkNotReadyTicks));
                }
                StopHWalk();
            }
            LogInfo(std::format("H route {} {} 0x{:06X} offset={:+.0f}: {} ({}; startZ={:.1f}, minZ={:.1f}, samples={})",
                spot.dungeon->key, label, spot.exitId, spot.exitOffset,
                ok ? "PASS" : "FAIL", reason, spot.position.z, test.lowestZ, test.routeHeightSamples));
            if (ok) {
                ++(spot.kind == HRouteKind::Boss ? test.bossSpotsPassed
                    : spot.kind == HRouteKind::Approach ? test.approachSpotsPassed
                    : spot.kind == HRouteKind::Walk ? test.walkSpotsPassed : test.exitSpotsPassed);
            }
            // 걷기·출구 탐침의 통과는 출구 구역이 안전 탈출을 건 것이다. 실제 도착은 RouteRecover가 확인한다.
            test.routeEscapeExpected = ok && awaitRecovery && (spot.kind == HRouteKind::Walk || spot.kind == HRouteKind::Exit);
            // 접근 자리 이동 요청 표시가 다음 탐침으로 넘어가면 그 탐침이 자기 이동을 요청하지 않는다(Codex 검토 2026-11-24).
            test.routeStaged = false;
            test.routeRecoveryZ.reset();
            test.routeRecoveryStableSec = 0.0;
            ++test.routeIndex;
            EnterHContract(awaitRecovery ? HContractStage::RouteRecover : HContractStage::RouteStart);
        }

        void TickHContract(bool paused, double deltaSec)
        {
            auto& test = g_hContract;
            using Stage = HContractStage;
            using Trial = HContractTrial;
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - test.stageStarted).count();
            const double travelSec = HContractTravelSec();
            const double contractBudgetSec = std::max(kHContractBaseBudgetSec, travelSec * kHContractLongTravelBudgetMultiplier);
            if (test.stage >= Stage::RouteStart && test.stage <= Stage::RouteRecover) {
                // 엔진이 로딩 화면 없이 Mist·Fader 연출을 시작하고 페이드인을 하지 않는 일이 있었다(실행 15·19·21·22, 원인 미확정).
                // 그대로 두면 준비 판정이 풀리지 않아 계약 전체가 멈춘다. 기록하고 두 메뉴를 닫은 뒤, 관찰 중이던 탐침은 한 번 다시 시도한다.
                auto* routeUi = RE::UI::GetSingleton();
                const bool mistStall = routeUi && routeUi->IsMenuOpen(RE::MistMenu::MENU_NAME) &&
                    routeUi->IsMenuOpen(RE::FaderMenu::MENU_NAME) && !routeUi->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
                // 결정 177 ③ 재보완: 출구 문이 꺼져 있으므로 로딩 없는 Mist·Fader 열림(엔진 자동 문 발동)은 없어야 한다.
                if (mistStall && !test.routeMistOpen) {
                    ++test.routeMistOpenings;
                    LogInfo(std::format("H route saw Mist/Fader open without loading (spot {}/{}).", test.routeIndex + 1, test.route.size()));
                }
                test.routeMistOpen = mistStall;
                if (!mistStall) {
                    test.routeMistSince.reset();
                } else if (!test.routeMistSince) {
                    test.routeMistSince = now;
                } else if (std::chrono::duration<double>(now - *test.routeMistSince).count() >= kHRouteMistStallSec) {
                    test.routeMistSince.reset();
                    ++test.routeMistStalls;
                    if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
                        queue->AddMessage(RE::MistMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                        queue->AddMessage(RE::FaderMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                    }
                    LogInfo(std::format("H route dismissed an engine Mist/Fader stall in {} (spot {}/{}).",
                        static_cast<int>(test.stage), test.routeIndex + 1, test.route.size()));
                    if (test.stage == Stage::RouteObserve && test.routeRetriedIndex != test.routeIndex) {
                        test.routeRetriedIndex = test.routeIndex;
                        test.routeStaged = false;
                        // 걷기 탐침이면 자동 이동·1인칭을 풀고 다시 시작한다(풀지 않으면 접근 자리로 옮기는 동안 다시 걸어 나간다).
                        if (test.walkActive) {
                            StopHWalk();
                        }
                        EnterHContract(Stage::RouteStart);
                        return;
                    }
                }
            }
            if (test.stage >= Stage::ClosePriorRun && test.stage < Stage::RestoreCheckpoint &&
                std::chrono::duration<double>(now - test.started).count() >= contractBudgetSec) {
                AbortHContract("H contract exceeded its time budget.");
                return;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const auto run = MapDeviceRuntime::GetActiveMapStateCopy();
            const bool ready = player && !paused && IsPlayerReadyInWorld() && !travel.kind && !player->IsDead();
            const bool outside = ready && !travel.inPackCell;
            const auto& manifest = MapPackRuntime::Manifest();
            const auto* dungeon = test.trial == Trial::Arena ? &manifest.arena : manifest.ForTier(kHRegressionMapTier);
            const auto* forms = MapPackRuntime::FindForms(dungeon);
            const bool entered = ready && forms && player->GetParentCell() == forms->cell &&
                run.active && MapDeviceRuntime::IsPlayerInActiveMapCell();
            const bool failedTrial = test.trial == Trial::Portals || test.trial == Trial::Arena;
            const auto failTrial = [&](const std::string& reason) {
                if (test.trial == Trial::Cleanup) {
                    FailHCleanupCycle(reason);
                    EnterHContract(Stage::Close);
                } else {
                    AbortHContract(reason);
                }
            };

            switch (test.stage) {
            case Stage::PrepareCheckpoint:
                if (outside && !run.active) {
                    const auto stem = FindUnusedHCheckpointStem();
                    if (!stem || !BeginHContractSave(*stem, Stage::SaveCheckpoint)) {
                        AbortHContract("H could not allocate an unused checkpoint save name.");
                        break;
                    }
                    test.checkpointStem = *stem;
                    test.checkpoint = ObserveHCheckpoint(player);
                } else if (elapsed >= travelSec) {
                    AbortHContract("H could not confirm a ready player outside any map run before checkpoint creation.");
                }
                break;

            case Stage::SaveCheckpoint:
                if (PumpHContractSave(test.checkpointStem)) {
                    test.checkpointSaved = true;
                    LogInfo(std::format("H pre-scenario checkpoint written: {}.", test.checkpointStem));
                    EnterPhase(Phase::ScenarioH_Start);
                } else if (elapsed >= kHSaveWaitSec) {
                    AbortHContract("H checkpoint save/co-save did not reach disk.");
                }
                break;

            case Stage::ClosePriorRun:
                if (outside && !run.active) {
                    ClearHArenaHealthBuffer(player);
                    if (!MoveToHOrigin(player, kHOutdoorStartMarker)) {
                        AbortHContract("H contract could not move to the outdoor probe.");
                        break;
                    }
                    EnterHContract(Stage::OutdoorProbe);
                } else if (elapsed >= travelSec) {
                    AbortHContract("H contract could not confirm closure of the H7 run.");
                } else if (ready) {
                    CloseHRun(player);
                }
                break;

            case Stage::OutdoorProbe:
                if (outside && AtHOrigin(player, kHOutdoorStartMarker, true) && !run.active) {
                    test.openWorldRejected = !MapDeviceRuntime::WouldInterceptLethalHit(
                        true, kHContractLowHealth, kHContractLowHealth, g_queue->CurrentGeneration());
                    if (!MoveToHOrigin(player, kHIndoorStartMarker)) {
                        AbortHContract("H contract could not move to its indoor origin.");
                        break;
                    }
                    EnterHContract(Stage::IndoorOrigin);
                } else if (elapsed >= kHOriginWaitSec) {
                    AbortHContract("H contract could not confirm the outdoor probe position.");
                }
                break;

            case Stage::IndoorOrigin:
                if (outside && AtHOrigin(player, kHIndoorStartMarker, false)) {
                    EnterHContract(Stage::Open);
                } else if (elapsed >= kHOriginWaitSec) {
                    AbortHContract("H contract could not confirm its indoor origin.");
                }
                break;

            case Stage::Open: {
                if (!forms || elapsed >= travelSec) {
                    // 실행 21 H23 4·5회차: 83초 동안 열지 못했고 그 뒤 바깥에서 쓰러져 엔진이 마지막 저장을 불러왔다(추정).
                    // 무엇이 막았는지 남긴다.
                    failTrial(std::format("H contract could not prepare the next map or arena entry (outside={}, runActive={}, resetPending={}, inCombat={}, combatants={}).",
                        outside, run.active, forms && MapDeviceRuntime::GetCellResetStatus(forms->cellId).pending,
                        player && player->IsInCombat(), DescribePlayerCombatants(player)));
                    break;
                }
                if (!outside || run.active || MapDeviceRuntime::GetCellResetStatus(forms->cellId).pending) {
                    break;
                }
                if (player->IsInCombat()) {
                    // 플레이어만 멈추면 상대의 전투·경보가 남아 다시 걸렸다. 재입장 전 처리와 같이 상대까지 멈춘다.
                    StopPlayerCombatAndAlarms(player);
                    break;
                }
                test.runId = 0;
                test.reentering = false;
                test.resetCompletedBefore = MapDeviceRuntime::GetCellResetStatus(forms->cellId).completed;
                MapActionResult result;
                if (test.trial == Trial::Arena) {
                    auto* tribute = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
                    if (!tribute) {
                        AbortHContract("H contract arena tribute form was unavailable.");
                        break;
                    }
                    SaveLoad::UpdateState([](PersistentState& state) {
                        state.tree.ascendancyMilestonesBitmask =
                            (state.tree.ascendancyMilestonesBitmask | Core::kArena4PrerequisiteMask) &
                            ~Core::kArena4MilestoneBit;
                    });
                    const auto baseline = MapDeviceRuntime::CountPlayerTributes(player);
                    player->AddObjectToContainer(tribute, nullptr, 1, nullptr);
                    test.tributeBefore = MapDeviceRuntime::CountPlayerTributes(player);
                    if (test.tributeBefore != baseline + 1) {
                        AbortHContract("H contract could not add exactly one arena tribute.");
                        break;
                    }
                    result = MapDeviceRuntime::OpenArena4(player);
                } else {
                    result = OpenHMap(player, kHRegressionMapTier, false);
                }
                if (!result.ok) {
                    failTrial(std::format("H contract entry was rejected: {}", result.message));
                    break;
                }
                EnterHContract(Stage::Entry);
                break;
            }

            case Stage::Entry: {
                if (!entered || !run.bossRefFormId || run.spawnedRefFormIds.empty()) {
                    if (elapsed >= travelSec) {
                        failTrial("H contract did not reach an authorized, populated map or arena.");
                    }
                    break;
                }
                const auto expected = run.isArena4 ? 0u :
                    kHExpectedPortalSequence[test.trial == Trial::Portals ? test.entryCount : test.reentering ? 1u : 0u];
                if (run.isArena4 != (test.trial == Trial::Arena) || run.portalsRemaining != expected ||
                    run.failed || run.bossKilled ||
                    (test.reentering && run.runId != test.runId)) {
                    failTrial(std::format("H contract entry state mismatch: run={}, portals={}/{}.",
                        run.runId, run.portalsRemaining, expected));
                    break;
                }
                test.runId = run.runId;
                test.enteredRun = run;
                if (test.trial == Trial::Cleanup) {
                    const auto spawns = ObserveHMapSpawns(*forms);
                    if (!spawns) {
                        if (elapsed >= travelSec) {
                            failTrial("H cleanup could not observe the attached populated cell.");
                        }
                        break;
                    }
                    if (test.reentering) {
                        Check(kHReentrySpawnsCheck, *spawns == test.spawnsBeforeReturn,
                            std::format("Active actors (including corpses) before/after reentry={}/{}.",
                                test.spawnsBeforeReturn.size(), spawns->size()));
                        EnterHContract(Stage::Close);
                    } else {
                        const bool priorActorSurvived = HHasPriorSpawn(*spawns, test.priorCleanupSpawns);
                        bool onlyCurrent = run.runId != test.lastCleanupRunId && !priorActorSurvived && HAllSpawnsBelongToRun(*spawns, run);
                        if (!onlyCurrent && run.runId != test.lastCleanupRunId && elapsed < travelSec) {
                            // 결정 178 ⑦ 보완: 셀 초기화가 놓친 앞선 회차 액터를 제품이 스폰 직후 찾아 Disable을 요청했으면(작업 풀 예약이라
                            // 한두 프레임 뒤 꺼짐) 꺼질 때까지 기다린다. 제품이 찾지 못한 액터가 섞였으면 그대로 실패로 본다(실행 18 7회차).
                            auto unswept = *spawns;
                            std::erase_if(unswept, [](const HMapSpawn& spawn) { return MapDeviceRuntime::IsLeftoverSweepPending(spawn.id); });
                            if (unswept.size() != spawns->size() && !HHasPriorSpawn(unswept, test.priorCleanupSpawns) &&
                                HAllSpawnsBelongToRun(unswept, run)) {
                                break;
                            }
                        }
                        if (test.cleanupCycles == 1) {
                            Check(kHFreshRunCheck, onlyCurrent && !test.priorCleanupSpawns.empty(),
                                std::format("Run {} -> {}, prior actors={}, current actors={}, prior actor survived={}; registration matched={}; leftover actors swept={}",
                                    test.lastCleanupRunId, run.runId, test.priorCleanupSpawns.size(), spawns->size(), priorActorSurvived,
                                    HAllSpawnsBelongToRun(*spawns, run),
                                    MapDeviceRuntime::GetLeftoverSweepCleanedCount() - test.leftoverSweepBefore));
                        }
                        test.cleanupSpawns = *spawns;
                        if (!onlyCurrent) {
                            failTrial(std::format("H cleanup found missing or foreign-run actors in the cell (run {}, last cleanup run {}, prior survived={}): {}.",
                                run.runId, test.lastCleanupRunId, priorActorSurvived, DescribeHSpawnMismatch(*spawns, &run)));
                            break;
                        }
                        if (test.cleanupCycles != 0) {
                            EnterHContract(Stage::Close);
                            break;
                        }
                        const auto pack = spawns->size() - 1;
                        if (pack < 2 || std::ranges::any_of(*spawns, &HMapSpawn::dead)) {
                            failTrial("H cleanup needs a living boss and at least two living pack actors.");
                            break;
                        }
                        test.expectedCorpses = pack / 2;
                        auto remaining = test.expectedCorpses;
                        for (const auto& spawn : *spawns) {
                            if (remaining && spawn.id != run.bossRefFormId) {
                                if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(spawn.id)) {
                                    actor->KillImmediate();
                                    --remaining;
                                }
                            }
                        }
                        EnterHContract(Stage::PartialKill);
                    }
                    break;
                }
                if (test.trial != Trial::Portals) {
                    if (test.trial == Trial::Arena && MapDeviceRuntime::CountPlayerTributes(player) != test.tributeBefore - 1) {
                        AbortHContract("H arena entry did not consume one tribute.");
                        break;
                    }
                    if (test.trial == Trial::Arena) {
                        // H29(결정 182): 결투 중(포털 0, 보스 생존)에 결투장 출구에 세워 출구 구역이 귀환을 걸지 않는지 본다(아래 ExitZoneHold).
                        test.exitZoneRefusedAtArenaEntry = MapDeviceRuntime::ExitZoneReturnRefusal().has_value();
                        if (dungeon->autoExits.empty()) {
                            AbortHContract("H29 needs an automatic exit in the arena.");
                            break;
                        }
                        const auto& exit = dungeon->autoExits.front();
                        test.exitZoneHoldBefore = MapDeviceRuntime::GetExitZoneTravelCount();
                        // 보스가 다가와도 출구에 서 있는 동안 쓰러지지 않게 한다(중단 때는 AbortHContract가 되돌림).
                        if (!g_hRouteGodModeBefore) {
                            g_hRouteGodModeBefore = RE::PlayerCharacter::IsGodMode();
                        }
                        SetPlayerGodModeFlag(true);
                        LogInfo(std::format("H29: placing the player on arena exit 0x{:06X} in run {}.", exit.localId, run.runId));
                        player->SetPosition(RE::NiPoint3{ exit.position[0], exit.position[1], exit.position[2] }, true);
                        EnterHContract(Stage::ExitZoneHold);
                        break;
                    }
                    BeginHDamageTrial(run);
                    break;
                }
                ++test.entryCount;
                LogInfo(std::format("H portal entry {}: {} remaining, run={}.", test.entryCount, run.portalsRemaining, run.runId));
                if (test.entryCount == 1) {
                    const auto generation = g_queue->CurrentGeneration();
                    const bool disabled = MapDeviceRuntime::WouldInterceptLethalHit(
                        false, kHContractLowHealth, kHContractLowHealth, generation);
                    const bool enabled = MapDeviceRuntime::WouldInterceptLethalHit(
                        true, kHContractLowHealth, kHContractLowHealth, generation);
                    Check(kHIniOffCheck, !disabled && enabled,
                        std::format("Authorized cell: setting off={}, same position with setting on={}.", disabled, enabled));
                    test.openWorldRejected = test.openWorldRejected && enabled;
                }
                if (test.entryCount == kHExitZoneReturnEntry) {
                    test.exitZoneRefusedWithPortals = MapDeviceRuntime::ExitZoneReturnRefusal().has_value();
                } else if (test.entryCount == kHExpectedPortalSequence.size()) {
                    test.exitZoneRefusedAtLastPortal = MapDeviceRuntime::ExitZoneReturnRefusal().has_value();
                }
                if (test.entryCount == kHExpectedPortalSequence.size()) {
                    test.lastPortalSave = MapDeviceRuntime::TakeSnapshotForSave();
                    if (!BeginHContractSave(g_cfg.tempSave, Stage::SaveLastPortal)) {
                        AbortHContract("H could not observe the last-portal save files before writing.");
                    }
                } else if (test.entryCount == kHExitZoneReturnEntry) {
                    // H28(결정 177 ③ 재보완): 허가된 회차에서 출구 구역에 들면 포털 귀환이어야 한다(회차와 포털 수 유지, 아래 Return이 확인).
                    const auto* runDungeon = MapPackRuntime::Manifest().ForRun(run);
                    if (!runDungeon || runDungeon->autoExits.empty()) {
                        AbortHContract("H exit zone return needs a regression map with an automatic exit.");
                        break;
                    }
                    const auto& exit = runDungeon->autoExits.front();
                    test.exitZoneReturnBefore = MapDeviceRuntime::GetExitZoneTravelCount();
                    test.exitZoneReturnPending = true;
                    LogInfo(std::format("H28: placing the player on exit 0x{:06X} of {} in run {}.", exit.localId, runDungeon->key, run.runId));
                    player->SetPosition(RE::NiPoint3{ exit.position[0], exit.position[1], exit.position[2] }, true);
                    EnterHContract(Stage::Return);
                } else if (const auto result = MapDeviceRuntime::ReturnPortal(player); result.ok) {
                    EnterHContract(Stage::Return);
                } else {
                    AbortHContract(std::format("H normal return was rejected: {}", result.message));
                }
                break;
            }

            case Stage::PartialKill: {
                if (!entered || !forms) {
                    failTrial("H partial kill lost the authorized map.");
                    break;
                }
                auto spawns = ObserveHMapSpawns(*forms);
                if (spawns && HAllSpawnsBelongToRun(*spawns, run) &&
                    static_cast<std::size_t>(std::ranges::count(*spawns, true, &HMapSpawn::dead)) == test.expectedCorpses &&
                    std::ranges::none_of(*spawns, [&](const HMapSpawn& spawn) {
                        return spawn.dead && spawn.id == run.bossRefFormId;
                    })) {
                    test.spawnsBeforeReturn = std::move(*spawns);
                    const auto result = MapDeviceRuntime::ReturnPortal(player);
                    if (result.ok) {
                        EnterHContract(Stage::Return);
                    } else {
                        failTrial(std::format("H preservation return was rejected: {}", result.message));
                    }
                } else if (elapsed >= kMapBossKillWaitSec) {
                    failTrial("H preservation could not confirm the partial pack kill.");
                }
                break;
            }

            case Stage::ExitZoneHold: {
                // 출구 구역은 거절 뒤 1초마다 다시 판정하므로 두 번 넘게 판정할 시간을 둔다. 이동이 걸리면 entered가 풀리거나 이동 수가 는다.
                const auto travels = MapDeviceRuntime::GetExitZoneTravelCount() - test.exitZoneHoldBefore;
                if (elapsed < kHExitZoneHoldSec && travels == 0) {
                    break;
                }
                const bool held = entered && run.active && run.runId == test.runId && run.isArena4 && run.portalsRemaining == 0 &&
                    !run.failed && !run.bossKilled && !run.endRequested && travels == 0;
                const auto show = [](const std::optional<bool>& value) {
                    return value ? (*value ? "refused" : "allowed") : "not observed";
                };
                Check(kHExitZoneRefusalCheck,
                    test.exitZoneRefusedWithPortals == false && test.exitZoneRefusedAtLastPortal == true &&
                        test.exitZoneRefusedAtArenaEntry && held,
                    std::format("Exit zone return with portals={}, at the last portal={}, arena entry={}; on the arena exit for {:.1f}s: "
                                "exit-zone travels={}, run {} kept={} (portals {}, failed={}, bossKilled={}).",
                        show(test.exitZoneRefusedWithPortals), show(test.exitZoneRefusedAtLastPortal),
                        test.exitZoneRefusedAtArenaEntry ? "refused" : "allowed", elapsed, travels, run.runId, held,
                        run.portalsRemaining, run.failed, run.bossKilled));
                const bool godModeRestored = RestoreHGodMode(player);
                if (!held || !forms || !forms->entryMarker) {
                    failTrial("H29 lost the arena run while standing on the exit.");
                    break;
                }
                if (!godModeRestored) {
                    AbortHContract("H29 could not restore god mode after the arena exit hold.");
                    break;
                }
                player->MoveTo(forms->entryMarker);
                BeginHDamageTrial(run);
                break;
            }

            case Stage::Return:
                if (outside && HAtSavedReturn(player, test.enteredRun) && run.active && run.runId == test.runId &&
                    !run.failed && run.portalsRemaining == test.enteredRun.portalsRemaining) {
                    if (std::exchange(test.exitZoneReturnPending, false)) {
                        const auto zoneTravels = MapDeviceRuntime::GetExitZoneTravelCount() - test.exitZoneReturnBefore;
                        Check(kHExitZoneReturnCheck, zoneTravels == 1,
                            std::format("Authorized exit zone return: exit-zone travels={}, run {} kept with {} portals.",
                                zoneTravels, run.runId, run.portalsRemaining));
                    }
                    if (test.trial == Trial::Portals) {
                        ++test.normalReturns;
                        if (test.normalReturns == 1) {
                            const bool intercepted = MapDeviceRuntime::WouldInterceptLethalHit(
                                true, kHContractLowHealth, kHContractLowHealth, g_queue->CurrentGeneration());
                            Check(kHOpenWorldCheck, test.openWorldRejected && !intercepted,
                                std::format("Inactive and authorized-cell controls passed={}, open run outside intercepted={}.",
                                    test.openWorldRejected, intercepted));
                        }
                        if (test.normalReturns == kHExpectedPortalSequence.size() - 1) {
                            Check(kHNormalReturnCheck, true,
                                std::format("{} normal returns preserved the active run and its portal count.", test.normalReturns));
                        }
                    }
                    EnterHContract(Stage::Reenter);
                } else if (elapsed >= travelSec) {
                    failTrial(std::format("H normal return did not preserve the run and its {} portals (observed {}).",
                        test.enteredRun.portalsRemaining, run.portalsRemaining));
                }
                break;

            case Stage::Reenter:
                if (elapsed >= kHCombatStopWaitSec) {
                    failTrial(std::format("H reentry could not clear combat. Combatants: {}", DescribePlayerCombatants(player)));
                } else if (outside && !player->IsInCombat()) {
                    const auto result = MapDeviceRuntime::ReenterActiveMap(player);
                    if (result.ok) {
                        test.reentering = true;
                        EnterHContract(Stage::Entry);
                    } else {
                        failTrial(std::format("H positive reentry was rejected: {}", result.message));
                    }
                } else if (outside) {
                    StopPlayerCombatAndAlarms(player);
                }
                break;

            case Stage::SaveLastPortal:
                if (PumpHContractSave(g_cfg.tempSave)) {
                    if (!MoveToHOrigin(player, kHIndoorStartMarker)) {
                        AbortHContract("H could not step outside the active zero-portal run.");
                        break;
                    }
                    EnterHContract(Stage::ExhaustedReentry);
                } else if (elapsed >= kHSaveWaitSec) {
                    AbortHContract("H last-portal save/co-save did not reach disk.");
                }
                break;

            case Stage::ExhaustedReentry:
                if (outside && !player->IsInCombat() && AtHOrigin(player, kHIndoorStartMarker, false)) {
                    if (run.active && run.runId == test.runId && run.portalsRemaining == 0 && !run.failed) {
                        const auto rejected = MapDeviceRuntime::ReenterActiveMap(player);
                        test.zeroPortalRejected = !rejected.ok && rejected.entryDenial == MapEntryDenial::NoPortals &&
                            !MapDeviceRuntime::GetTravelStatus(player).kind;
                    }
                } else if (elapsed < travelSec) {
                    if (outside && player->IsInCombat()) {
                        StopPlayerCombatAndAlarms(player);
                    }
                    break;
                }
                Check(kHPortalCheck, test.entryCount == kHExpectedPortalSequence.size() && test.zeroPortalRejected,
                    std::format("Entries={}, zero-portal reentry rejected while the run was active={}.", test.entryCount, test.zeroPortalRejected));
                if (RequestLoadSave(g_cfg.tempSave)) {
                    EnterHContract(Stage::LoadLastPortal);
                } else {
                    AbortHContract("H last-portal reload request failed.");
                }
                break;

            case Stage::LoadLastPortal: {
                bool loaded = false;
                bool failed = false;
                {
                    std::scoped_lock lock(g_msgMutex);
                    loaded = g_postLoadSignaled && g_postLoadSucceeded;
                    failed = g_postLoadSignaled && !g_postLoadSucceeded;
                }
                const auto& saved = test.lastPortalSave;
                const auto versions = SaveLoad::GetLoadedMapRecordVersions();
                if (loaded && entered && saved.playerAuthorized && run.runId == saved.runId && run.portalsRemaining == 0 &&
                    run.dungeonKey == saved.dungeonKey && !run.bossKilled && !run.failed &&
                    run.bossRefFormId == saved.bossRefFormId && run.returnCellFormId == saved.returnCellFormId &&
                    run.returnWorldspaceFormId == saved.returnWorldspaceFormId &&
                    run.returnPos == saved.returnPos && run.returnAngle == saved.returnAngle && versions.maps == SaveLoad::kMapsVersion) {
                    test.resetCompletedBefore = MapDeviceRuntime::GetCellResetStatus(forms->cellId).completed;
                    test.enteredRun = run;
                    BeginHDamageTrial(run);
                } else if (failed || elapsed >= kHReloadWaitSec) {
                    AbortHContract("H reload did not restore the authorized last-portal run and its return target.");
                }
                break;
            }

            case Stage::Damage: {
                const bool magic = test.trial == Trial::Magic;
                auto* av = player ? player->AsActorValueOwner() : nullptr;
                if (magic) {
                    ObserveHMagicHealthLoss(test, av);
                }
                if (MapDeviceRuntime::IsDeathEscapeInFlight() && test.damageRequests != 0) {
                    test.deathMarkedFailed = run.failed;
                    EnterHContract(Stage::DeathReturn);
                    break;
                }
                if (!player || player->IsDead() ||
                    (test.attackWindowStarted < 0.0 ? elapsed >= kMapBossLoadWaitSec + kHBurstReadyWaitSec :
                        elapsed - test.attackWindowStarted >= kHDeathTriggerWaitSec)) {
                    // 연타 준비 중에 켠 무적을 되돌린다(켜지 않았으면 아무것도 하지 않음).
                    (void)RestoreHGodMode(player);
                    if (magic) {
                        const auto clamps = test.damageRequests == 0 ? 0 :
                            CombatHooks::GetMapDeathDamageClampCount(true) - test.deathClampsBefore;
                        const auto reason = HObservedMagicDamage(test, clamps)
                            ? "Firebolt damage observed without a death escape"
                            : "시험 주입 실패 (no Firebolt damage observed)";
                        Check(kHMagicBurstCheck, false, std::format("{}; {}", reason, DescribeHMagicDelivery(test, clamps)));
                        if (player && !player->IsDead()) {
                            RestorePlayerVitals(player, true, true);
                            EnterHContract(Stage::Close);
                        } else {
                            AbortHContract("H20 could not close the trial run with a living player.");
                        }
                    } else {
                        AbortHContract(std::format("H lethal damage did not trigger the escape (requests={}).", test.damageRequests));
                    }
                    break;
                }
                if (MapDeviceRuntime::IsDeathEscapeInFlight()) {
                    (void)RestoreHGodMode(player);
                    if (magic) {
                        Check(kHMagicBurstCheck, false, "시험 주입 실패 (death escape was in flight before the first Firebolt cast).");
                        EnterHContract(Stage::Close);
                    } else {
                        AbortHContract("H death escape was already in flight before the test's first hit.");
                    }
                    break;
                }
                if (!entered || elapsed < test.nextDamageAt) {
                    break;
                }
                auto* boss = RE::TESForm::LookupByID<RE::Actor>(run.bossRefFormId);
                if (!boss || boss->IsDead()) {
                    break;
                }
                if (!boss->Is3DLoaded()) {
                    if (!test.movedToBoss) {
                        player->MoveTo(boss);
                        test.movedToBoss = true;
                    }
                    break;
                }
                if (!av) {
                    break;
                }
                if (!failedTrial && HHasOtherNearbyHostile(player, boss)) {
                    Check(magic ? kHMagicBurstCheck : kHPhysicalBurstCheck, false,
                        "Another hostile actor was within the test's isolation radius; no lethal damage was applied on this tick.");
                    EnterHContract(Stage::Close);
                    break;
                }
                if (!failedTrial && test.damageRequests == 0) {
                    if (test.burstReadySince < 0.0) {
                        test.burstReadySince = elapsed;
                        if (!g_hRouteGodModeBefore) {
                            g_hRouteGodModeBefore = RE::PlayerCharacter::IsGodMode();
                        }
                        SetPlayerGodModeFlag(true);
                    }
                    const double waited = elapsed - test.burstReadySince;
                    const auto combatTarget = boss->GetActorRuntimeData().currentCombatTarget.get();
                    const bool targetsPlayer = boss->IsInCombat() && combatTarget && combatTarget.get() == player;
                    bool sightArg = false;
                    const bool sight = !magic || boss->HasLineOfSight(player, sightArg);
                    if (!targetsPlayer && !test.burstStartedCombat) {
                        boss->StartCombat(player);
                        test.burstStartedCombat = true;
                    }
                    if (!(targetsPlayer && sight) && waited < kHBurstReadyWaitSec) {
                        break;
                    }
                    LogInfo(std::format("{} burst readiness after {:.1f}s: boss inCombat={}, targets player={}, line of sight={}, "
                                        "distance={:.0f}, startCombat sent={}.",
                        magic ? "H20" : "H19", waited, boss->IsInCombat(), targetsPlayer, sight,
                        boss->GetPosition().GetDistance(player->GetPosition()), test.burstStartedCombat));
                    if (!RestoreHGodMode(player)) {
                        AbortHContract("H could not restore god mode before the lethal burst.");
                        break;
                    }
                }
                if (test.attackWindowStarted < 0.0) {
                    test.attackWindowStarted = elapsed;
                }
                if (g_esPool) {
                    g_esPool->Clear();
                }
                if (g_skillSystem) {
                    (void)g_skillSystem->TickGuardShield(kHOriginWaitSec, false);
                }
                const float hp = av->GetActorValue(RE::ActorValue::kHealth);
                if (hp > kHContractLowHealth) {
                    av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, kHContractLowHealth - hp);
                }
                if (test.damageRequests == 0) {
                    test.deathRequestsBefore = MapDeviceRuntime::GetDeathEscapeRequestCount();
                    test.deathClampsBefore = CombatHooks::GetMapDeathDamageClampCount(magic);
                    test.deathRepeatClampsBefore = CombatHooks::GetMapDeathRepeatClampCount(magic);
                    test.firstDamageAt = Clock::now();
                }
                if (magic) {
                    test.magicHealthBeforeBurst = av->GetActorValue(RE::ActorValue::kHealth);
                }
                // Submit the burst in one tick. Firebolt projectiles can apply damage on later ticks.
                const unsigned hits = failedTrial ? 1u : kHDeathBurstHits;
                for (unsigned i = 0; i < hits && !player->IsDead(); ++i) {
                    ++test.damageRequests;
                    if (magic) {
                        test.magicCastsSent += ApplyHFirebolt(boss, player) ? 1u : 0u;
                    } else {
                        CallDealDamage(boss, player, nullptr, false);
                    }
                }
                if (magic) {
                    ObserveHMagicHealthLoss(test, av);
                }
                if (MapDeviceRuntime::IsDeathEscapeInFlight()) {
                    test.deathMarkedFailed = MapDeviceRuntime::GetActiveMapStateCopy().failed;
                    EnterHContract(Stage::DeathReturn);
                }
                test.nextDamageAt = elapsed + kHBossHitIntervalSec;
                break;
            }

            case Stage::DeathReturn: {
                const bool returned = outside && !MapDeviceRuntime::IsDeathEscapeInFlight();
                if (!returned && elapsed < travelSec) {
                    break;
                }
                if (!returned) {
                    // 전체 시험 2026-11-24 06:14: 결투장 사망 귀환이 원위치에 도착했는데도 82초 동안 "돌아옴"이 되지 않았다. 무엇이 막았는지 남긴다.
                    LogInfo(std::format("H death return wait ended; ready={}, deathEscapeInFlight={}, player state: {}",
                        ready, MapDeviceRuntime::IsDeathEscapeInFlight(), DescribeHRoutePlayerState(player, paused, travel)));
                }
                if (test.trial == Trial::Portals && returned && player->IsInCombat() && elapsed < travelSec) {
                    player->StopCombat();
                    break;
                }
                const auto acceptance = MapDeviceRuntime::GetLastDeathEscapeRequest();
                const auto requests = acceptance.count - test.deathRequestsBefore;
                const bool magic = test.trial == Trial::Magic;
                const auto clamped = CombatHooks::GetMapDeathDamageClampCount(magic) - test.deathClampsBefore;
                const auto repeats = CombatHooks::GetMapDeathRepeatClampCount(magic) - test.deathRepeatClampsBefore;
                const bool acceptedTestAttack = test.damageRequests != 0 && acceptance.acceptedAt >= test.firstDamageAt &&
                    acceptance.attackerId == test.enteredRun.bossRefFormId;
                // Repeat magic clamps can also occur after a physical hit requests the escape.
                const bool magicRequestedEscape = magic && test.magicCastsSent > 0 && clamped > repeats;
                const bool sameOrigin = HAtSavedReturn(player, test.enteredRun);
                const bool ok = returned && sameOrigin && requests == 1 && acceptedTestAttack &&
                    (!magic || magicRequestedEscape) &&
                    (failedTrial ? clamped >= 1 : repeats >= 1) && test.deathMarkedFailed == failedTrial &&
                    (failedTrial ? !run.active : run.active && run.runId == test.runId &&
                        run.portalsRemaining == test.portalsBeforeDeath && !run.failed);
                auto detail = std::format("damage requests={}, damage clamped={}, repeat clamps={}, escape requests={}, test attack accepted={}, failed={}, active={}, portals={}/{}, returned={}, origin={}",
                    test.damageRequests, clamped, repeats, requests, acceptedTestAttack, test.deathMarkedFailed, run.active,
                    run.portalsRemaining, test.portalsBeforeDeath, returned, sameOrigin);
                if (magic) {
                    detail += std::format("; {}, initial magic clamp={}", DescribeHMagicDelivery(test, clamped), magicRequestedEscape);
                    if (!HObservedMagicDamage(test, clamped)) {
                        detail = "시험 주입 실패 (no Firebolt damage observed); " + detail;
                    }
                }
                if (test.trial == Trial::Portals) {
                    Check(kHLastPortalCheck, ok, detail);
                } else if (test.trial == Trial::Arena) {
                    const bool milestoneLocked =
                        (SaveLoad::GetStateCopy().tree.ascendancyMilestonesBitmask & Core::kArena4MilestoneBit) == 0;
                    const auto tributes = MapDeviceRuntime::CountPlayerTributes(player);
                    Check(kHArenaDeathCheck, ok && milestoneLocked && tributes == test.tributeBefore - 1,
                        std::format("{}; fourth milestone locked={}, tributes={} (before entry {}).",
                            detail, milestoneLocked, tributes, test.tributeBefore));
                } else {
                    Check(magic ? kHMagicBurstCheck : kHPhysicalBurstCheck, ok, detail);
                }
                if (!returned || (failedTrial && run.active)) {
                    AbortHContract("H death return did not leave a usable outside-map state.");
                } else {
                    RestorePlayerVitals(player, true, true);
                    EnterHContract(Stage::Close);
                }
                break;
            }

            case Stage::Close:
                if (outside && !run.active) {
                    if (test.trial == Trial::Cleanup && !test.runId) {
                        AdvanceHCleanupCycle();
                    } else {
                        EnterHContract(Stage::Reset);
                    }
                } else if (elapsed >= travelSec) {
                    if (test.trial == Trial::Cleanup) {
                        RecoverHCleanupCycle("Could not close the trial run; restoring a usable outside-map state.");
                    } else {
                        AbortHContract("H contract could not close the trial run.");
                    }
                } else if (ready) {
                    CloseHRun(player);
                }
                break;

            case Stage::Reset: {
                if (!outside || run.active) {
                    if (elapsed >= kHContractResetSec) {
                        if (test.trial == Trial::Cleanup) {
                            RecoverHCleanupCycle("Lost outside-map state during reset.");
                        } else {
                            AbortHContract("H contract lost the outside-map position during the cell reset.");
                        }
                    }
                    break;
                }
                if (!forms) {
                    if (test.trial == Trial::Cleanup) {
                        RecoverHCleanupCycle("The cleanup cell is unavailable for reset and reattachment.");
                    } else {
                        AbortHContract("The trial cell is unavailable for reset.");
                    }
                    break;
                }
                const auto reset = MapDeviceRuntime::GetCellResetStatus(forms->cellId);
                if (reset.pending || reset.completed <= test.resetCompletedBefore) {
                    if (elapsed >= kHContractResetSec) {
                        if (test.trial == Trial::Cleanup) {
                            RecoverHCleanupCycle("No completed Reset callback; prior actor state remains unverified.");
                        } else {
                            AbortHContract("H contract did not receive the closed cell's Reset callback.");
                        }
                    }
                    break;
                }
                switch (test.trial) {
                case Trial::Portals: test.trial = Trial::Arena; break;
                case Trial::Arena: test.trial = Trial::Physical; break;
                case Trial::Physical: test.trial = Trial::Magic; break;
                case Trial::Magic:
                    test.trial = Trial::Cleanup;
                    test.runId = 0;
                    test.leftoverSweepBefore = MapDeviceRuntime::GetLeftoverSweepCleanedCount();
                    break;
                case Trial::Cleanup:
                    test.cleanupReattachedAt.reset();
                    player->MoveTo(forms->entryMarker);
                    EnterHContract(Stage::VerifyEmptyCell);
                    break;
                }
                if (test.stage == Stage::Reset) {
                    EnterHContract(Stage::Open);
                }
                break;
            }

            case Stage::VerifyEmptyCell:
                if (ready && !run.active && player->GetParentCell() == forms->cell && forms->cell->IsAttached()) {
                    if (!test.cleanupReattachedAt) {
                        test.cleanupReattachedAt = now;
                    }
                    if (std::chrono::duration<double>(now - *test.cleanupReattachedAt).count() >= kHSpawnSettleSec) {
                        const auto residual = ObserveHMapSpawns(*forms);
                        if (!residual) {
                            test.cleanupReattachedAt.reset();
                            break;
                        }
                        if (!residual->empty()) {
                            // 남은 액터가 방금 닫은 회차(입장 때 등록)에 있던 것인지 가른다. "registered-but-absent"는 꺼졌거나
                            // 지워졌거나 셀을 떠난 것으로, 정상 정리면 여기에 회차 전부가 들어간다.
                            FailHCleanupCycle(std::format("{} active actors remained in the reattached cell after closing run {}: {}.",
                                residual->size(), test.enteredRun.runId, DescribeHSpawnMismatch(*residual, &test.enteredRun)));
                        } else {
                            LogInfo(std::format("H cleanup cycle {}/{}: attached cell observed with zero active actors after Reset.",
                                test.cleanupCycles + 1, kHCleanupCycles));
                        }
                        CloseHRun(player);
                        EnterHContract(Stage::LeaveVerifiedCell);
                        break;
                    }
                } else {
                    test.cleanupReattachedAt.reset();
                }
                if (elapsed >= travelSec) {
                    RecoverHCleanupCycle("Could not confirm cell reattachment; active residuals remain unverified.");
                }
                break;

            case Stage::LeaveVerifiedCell:
                if (outside && !run.active) {
                    AdvanceHCleanupCycle();
                } else if (elapsed >= travelSec) {
                    RecoverHCleanupCycle("Could not leave the reattached cell.");
                } else if (ready) {
                    CloseHRun(player);
                }
                break;

            case Stage::RecoverCleanup: {
                bool loaded = false;
                bool failed = false;
                {
                    std::scoped_lock lock(g_msgMutex);
                    loaded = g_postLoadSignaled && g_postLoadSucceeded;
                    failed = g_postLoadSignaled && !g_postLoadSucceeded;
                }
                if (loaded && outside && !run.active) {
                    test.runId = 0;
                    AdvanceHCleanupCycle();
                } else if (failed || elapsed >= kHReloadWaitSec) {
                    AbortHContract("H cleanup recovery load failed; attempting final checkpoint restoration.");
                }
                break;
            }

            case Stage::RoutePrepare: {
                if (!ready) {
                    if (elapsed >= travelSec) {
                        AbortHContract("H route setup could not obtain a ready player.");
                    }
                    break;
                }
                // 단축 실행은 체크포인트 없이 기준 세이브에서 바로 탐침한다. 활성 회차가 있으면 탐침이 허가 없이 돈다는 전제가 깨지고,
                // 끝낼 때 그 회차를 포기하게 되므로 시작하지 않는다(Codex 검토 2026-11-24).
                if (g_cfg.routeOnly && run.active) {
                    AbortHContract("The route-only run needs a baseline save without an active map run.");
                    break;
                }
                std::vector<const Core::MapDungeonDef*> dungeons;
                for (const auto& map : manifest.maps) {
                    dungeons.push_back(&map);
                }
                dungeons.push_back(&manifest.arena);
                for (const auto& [key, expectedExits] : kHExpectedAutoExits) {
                    const auto found = std::ranges::find_if(dungeons, [key](const auto* dungeon) { return dungeon->key == key; });
                    const auto* routeForms = found == dungeons.end() ? nullptr : MapPackRuntime::FindForms(*found);
                    if (!routeForms || !routeForms->bossAnchor || !routeForms->entryMarker) {
                        LogInfo(std::format("H route markers missing for {}.", key));
                        test.exitCountsMatch = false;
                        continue;
                    }
                    const auto* routeDungeon = *found;
                    test.exitCountsMatch = test.exitCountsMatch && routeDungeon->autoExits.size() == expectedExits;
                    test.route.push_back({ routeDungeon, routeForms->bossAnchor->GetPosition(), 0, 0.0f, HRouteKind::Boss, true });
                    for (const auto& exit : routeDungeon->autoExits) {
                        const RE::NiPoint3 door{ exit.position[0], exit.position[1], exit.position[2] };
                        // 접근 자리는 셀에 들어간 뒤 내비메쉬로 정한다. 그때까지 문 위치를 기준점으로 둔다.
                        test.route.push_back({ routeDungeon, door, exit.localId, 0.0f, HRouteKind::Approach, false });
                        // 걷기 탐침: 목표는 문 자리. 출발은 같은 출구의 접근 자리(앞의 접근 탐침이 정함, 출구 구역 밖).
                        // 실제 플레이처럼 걸어서 출구 구역에 들면 우리 이동이 시작돼야 한다(결정 177 ③ 재보완).
                        test.route.push_back({ routeDungeon, door, exit.localId, 0.0f, HRouteKind::Walk, true });
                        for (float offset : { 0.0f, kHRouteExitOffset, -kHRouteExitOffset }) {
                            test.route.push_back({ routeDungeon, {
                                exit.position[0] + std::sin(exit.rotationZ) * offset,
                                exit.position[1] + std::cos(exit.rotationZ) * offset,
                                exit.position[2] }, exit.localId, offset, HRouteKind::Exit, true });
                        }
                    }
                    LogInfo(std::format("H route {}: automatic exits={}/{}.", key, routeDungeon->autoExits.size(), expectedExits));
                }
                test.exitDoorReenabledBefore = MapDeviceRuntime::GetExitDoorReenabledCount();
                if (auto* camera = RE::PlayerCamera::GetSingleton()) {
                    test.routeRestoreThirdPerson = camera->IsInThirdPerson();
                }
                test.routeMistOpenings = 0;
                test.routeMistOpen = false;
                test.walkStepped = 0;
                test.routeEscapeExpected = false;
                test.routeEscapeArrived = 0;
                test.routeEscapeMissed = 0;
                g_hRouteGodModeBefore = RE::PlayerCharacter::IsGodMode();
                SetPlayerGodModeFlag(true);
                if (!RE::PlayerCharacter::IsGodMode()) {
                    Check(kHBossRouteCheck, false, "God mode could not be verified before the boss landing probes.");
                    Check(kHExitRouteCheck, false, "God mode could not be verified before the exit landing probes.");
                    EnterHContract(Stage::CloseRoutes);
                } else {
                    EnterHContract(Stage::RouteStart);
                }
                break;
            }

            case Stage::RouteStart: {
                if (test.routeIndex == test.route.size()) {
                    constexpr auto expectedBosses = kHExpectedAutoExits.size();
                    constexpr auto expectedExits = [] {
                        std::size_t total = 0;
                        for (const auto& dungeon : kHExpectedAutoExits) {
                            total += dungeon.second;
                        }
                        return total;
                    }();
                    constexpr auto expectedExitProbes = expectedExits * kHRouteProbesPerExit;
                    Check(kHBossRouteCheck, test.bossSpotsPassed == expectedBosses,
                        std::format("Boss anchor placements passed {}/{}.", test.bossSpotsPassed, expectedBosses));
                    // 결정 177 ③과 재보완: 출구 구역 밖 접근 자리는 서 있어야 하고, 걸어서 출구 구역에 들거나 출구 자리·앞뒤에 놓이면
                    // 출구 구역이 안전 탈출을 해야 한다(탐침은 허가 없이 돈다). 출구 문은 꺼져 있어 엔진 연출이 열리지 않아야 한다.
                    const auto reenabledDoors = MapDeviceRuntime::GetExitDoorReenabledCount() - test.exitDoorReenabledBefore;
                    Check(kHExitRouteCheck, test.exitCountsMatch && test.approachSpotsPassed == expectedExits &&
                            test.walkSpotsPassed == expectedExits && test.exitSpotsPassed == expectedExitProbes &&
                            test.routeEscapeMissed == 0 && test.routeEscapeArrived == test.walkSpotsPassed + test.exitSpotsPassed &&
                            test.routeMistOpenings == 0 && reenabledDoors == 0,
                        std::format("Exit approaches standable {}/{}; walks taken by the exit zone {}/{} (stepped because the game window was behind: {}); "
                                    "exit probes taken by the exit zone {}/{}; safe escapes arrived at the fixed marker {} (missed {}); "
                                    "per-dungeon exit counts matched={}; Mist/Fader openings without loading={} (stalls dismissed={}); "
                                    "enabled exit doors disabled again={}.",
                            test.approachSpotsPassed, expectedExits, test.walkSpotsPassed, expectedExits, test.walkStepped, test.exitSpotsPassed,
                            expectedExitProbes, test.routeEscapeArrived, test.routeEscapeMissed, test.exitCountsMatch, test.routeMistOpenings,
                            test.routeMistStalls, reenabledDoors));
                    EnterHContract(Stage::CloseRoutes);
                    break;
                }
                const auto& spot = test.route[test.routeIndex];
                const auto* routeForms = MapPackRuntime::FindForms(spot.dungeon);
                if (!ready) {
                    if (elapsed >= travelSec) {
                        AbortHContract("H route could not recover a ready player between placements.");
                    }
                    break;
                }
                if (!spot.exitId || player->GetParentCell() != routeForms->cell) {
                    player->MoveTo(routeForms->entryMarker);
                }
                EnterHContract(Stage::RouteEntry);
                break;
            }

            case Stage::RouteEntry: {
                const auto& spot = test.route[test.routeIndex];
                const auto* routeForms = MapPackRuntime::FindForms(spot.dungeon);
                const bool inCell = ready && player->GetParentCell() == routeForms->cell;
                const bool atEntry = inCell &&
                    (player->GetPosition() - routeForms->entryMarker->GetPosition()).Length() <=
                        MapDeviceRuntime::GetTravelArrivalRadius();
                if (inCell && spot.kind == HRouteKind::Walk) {
                    const auto approach = std::ranges::find_if(test.route, [&spot](const HRouteSpot& other) {
                        return other.kind == HRouteKind::Approach && other.dungeon == spot.dungeon &&
                            other.exitId == spot.exitId && other.positionResolved;
                    });
                    if (approach == test.route.end()) {
                        RecordHRouteSpot(false, "no resolved approach point to start the walk from");
                        break;
                    }
                    if ((player->GetPosition() - approach->position).Length() > kHRouteWalkStartRadius) {
                        if (!test.routeStaged) {
                            if (!MapDeviceRuntime::MovePlayerToSpace(player, routeForms->cell, nullptr, approach->position, player->GetAngle())) {
                                RecordHRouteSpot(false, "could not move to the approach point to start the walk");
                                break;
                            }
                            test.routeStaged = true;
                        } else if (elapsed >= travelSec) {
                            RecordHRouteSpot(false, "the approach point was not reached before the walk");
                        }
                        break;
                    }
                    test.routeStaged = false;
                    // 스카이림 방향: 0이 +Y(북), 시계 방향으로 증가. 앞으로 = (sin, cos).
                    test.walkYaw = std::atan2(spot.position.x - approach->position.x, spot.position.y - approach->position.y);
                    player->SetHeading(test.walkYaw);
                    // 1인칭으로 바꾸기 전 상태를 남긴다(바꾼 뒤에 찍으면 원래 시점을 알 수 없다, Codex 검토 2026-11-24).
                    const auto stateBeforeWalk = DescribeHWalkState(player, test.walkYaw);
                    if (auto* camera = RE::PlayerCamera::GetSingleton()) {
                        // 3인칭에서는 이동 방향이 카메라를 따르므로 1인칭으로 고정한다.
                        (void)camera->ForceFirstPerson();
                    }
                    test.fallRecoveryBefore = MapDeviceRuntime::GetFallRecoveryCount();
                    test.exitZoneBefore = MapDeviceRuntime::GetExitZoneTravelCount();
                    test.lowestZ = player->GetPosition().z;
                    test.routeHeightSamples = 0;
                    test.routeFell = false;
                    test.walkReadyTicks = 0;
                    test.walkNotReadyTicks = 0;
                    test.walkActive = true;
                    test.walkReadySec = 0.0;
                    test.walkTickSec = 0.0;
                    test.walkStepMode = false;
                    test.walkStepDistance = 0.0f;
                    LogInfo(std::format("H route {} walk 0x{:06X}: starting at ({:.0f}, {:.0f}, {:.1f}) toward the door ({:.0f} units); before first person: {}.",
                        spot.dungeon->key, spot.exitId, approach->position.x, approach->position.y, approach->position.z,
                        std::hypot(spot.position.x - approach->position.x, spot.position.y - approach->position.y),
                        stateBeforeWalk));
                    EnterHContract(Stage::RouteObserve);
                    break;
                }
                if (inCell && (spot.exitId || atEntry)) {
                    if (!spot.positionResolved) {
                        auto& mutableSpot = test.route[test.routeIndex];
                        // 셀이 막 붙었으면 내비메쉬가 아직 없을 수 있다. 이동 대기 시간 안에서는 기다린다.
                        const auto* loadedNavs = player->GetParentCell()->GetRuntimeData().navMeshes;
                        if (!loadedNavs || loadedNavs->navMeshes.empty()) {
                            if (elapsed >= travelSec) {
                                RecordHRouteSpot(false, "the cell navmesh was not loaded");
                            }
                            break;
                        }
                        const auto keepOut = static_cast<float>(MapPackRuntime::Manifest().exitZoneRadius) + kHRouteApproachZoneClearance;
                        const auto approach = FindExitApproachPoint(player->GetParentCell(), mutableSpot.position, keepOut);
                        if (!approach) {
                            RecordHRouteSpot(false, "no walkable navmesh triangle near the exit outside the exit zone");
                            break;
                        }
                        LogInfo(std::format("H route {} exit 0x{:06X} approach point ({:.0f}, {:.0f}, {:.1f}), {:.0f} units from the door (3D, keep-out {:.0f}).",
                            mutableSpot.dungeon->key, mutableSpot.exitId, approach->x, approach->y, approach->z,
                            (*approach - mutableSpot.position).Length(), keepOut));
                        mutableSpot.position = *approach;
                        mutableSpot.positionResolved = true;
                    }
                    test.fallRecoveryBefore = MapDeviceRuntime::GetFallRecoveryCount();
                    test.exitZoneBefore = MapDeviceRuntime::GetExitZoneTravelCount();
                    test.lowestZ = spot.position.z;
                    test.routeArrivalSeen = false;
                    test.routeHeightSamples = 0;
                    test.routeObservedSec = 0.0;
                    test.routeFell = false;
                    test.routeLandingZ.reset();
                    test.routeLandingStableSec = 0.0;
                    // 출구 탐침 자리는 내비메쉬 밖의 허공일 수 있다. 엔진 MoveTo로 그런 자리에 놓으면 약 0.6초 뒤 엔진이 가장 가까운
                    // 내비메쉬로 다시 옮기며 로딩용 연출(Mist·Fader)을 열고, 로딩 화면이 오지 않아 페이더가 닫히지 않는 일이 있었다
                    // (실행 19·21, 가설). 같은 셀 안의 출구 탐침은 위치만 바꿔(SetPosition) 실제로 떨어지게 한다. 다른 셀로 갈 때는 MoveTo.
                    const bool samePlacementCell = spot.kind == HRouteKind::Exit && player->GetParentCell() == routeForms->cell;
                    if (samePlacementCell && (player->GetPosition() - spot.position).Length() > kHRouteShortHop) {
                        // 먼 출구: 같은 출구의 접근 자리(내비메쉬 위)로 먼저 엔진 이동을 하고, 가까워진 뒤 짧게 옮긴다.
                        if (!test.routeStaged) {
                            const auto approach = std::ranges::find_if(test.route, [&spot](const HRouteSpot& other) {
                                return other.kind == HRouteKind::Approach && other.dungeon == spot.dungeon &&
                                    other.exitId == spot.exitId && other.positionResolved;
                            });
                            if (approach == test.route.end() ||
                                !MapDeviceRuntime::MovePlayerToSpace(player, routeForms->cell, nullptr, approach->position, player->GetAngle())) {
                                RecordHRouteSpot(false, "could not stage at the exit approach point");
                                break;
                            }
                            test.routeStaged = true;
                        } else if (elapsed >= travelSec) {
                            RecordHRouteSpot(false, "the exit approach point was not reached before the probe");
                        }
                        break;
                    }
                    test.routeStaged = false;
                    bool placed = true;
                    if (samePlacementCell) {
                        player->SetPosition(spot.position, true);
                    } else {
                        placed = MapDeviceRuntime::MovePlayerToSpace(player, routeForms->cell, nullptr, spot.position, player->GetAngle());
                    }
                    if (!placed) {
                        RecordHRouteSpot(false, "placement request rejected");
                    } else {
                        EnterHContract(Stage::RouteObserve);
                    }
                } else if (elapsed >= travelSec) {
                    RecordHRouteSpot(false, "entry marker not reached");
                }
                break;
            }

            case Stage::RouteObserve: {
                const auto& spot = test.route[test.routeIndex];
                const auto* routeForms = MapPackRuntime::FindForms(spot.dungeon);
                const bool exitProbe = spot.kind == HRouteKind::Exit;
                if (spot.kind == HRouteKind::Walk) {
                    if (!player) {
                        // 준비 판정은 플레이어가 없는 틱을 허용한다. 위치를 읽기 전에 막는다(Codex 검토 2026-11-24).
                        ++test.walkNotReadyTicks;
                        break;
                    }
                    if (std::isfinite(deltaSec) && deltaSec > 0.0) {
                        test.walkTickSec += deltaSec;
                    }
                    auto* walkUi = RE::UI::GetSingleton();
                    const bool mistNow = walkUi && walkUi->IsMenuOpen(RE::MistMenu::MENU_NAME) &&
                        walkUi->IsMenuOpen(RE::FaderMenu::MENU_NAME) && !walkUi->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
                    const auto pos = player->GetPosition();
                    const float doorDistance = (pos - spot.position).Length();
                    test.lowestZ = std::min(test.lowestZ, pos.z);
                    // 결정 177 ③ 재보완: 걸어서 출구 구역에 들면 우리 이동(이 탐침은 허가가 없어 안전 탈출)이 시작돼야 한다.
                    // 엔진 연출(Mist·Fader)은 열리지 않아야 한다(경로 전체에서 따로 셈).
                    const bool byExitZone = MapDeviceRuntime::GetExitZoneTravelCount() != test.exitZoneBefore;
                    if (travel.kind || byExitZone || MapDeviceRuntime::GetFallRecoveryCount() != test.fallRecoveryBefore) {
                        const bool ok = byExitZone && travel.kind == Core::MapTravelKind::SafeEscape;
                        RecordHRouteSpot(ok, std::format("{} after {:.1f}s (travel={}, {:.0f} units from the door, mist={}, {})",
                            ok ? "the exit zone started our safe escape"
                               : byExitZone ? "the exit zone started a travel that is not a safe escape"
                                            : "another travel or the fall recovery took over before the exit zone",
                            elapsed, travel.kind ? static_cast<int>(*travel.kind) : -1, doorDistance, mistNow,
                            test.walkStepMode ? "stepped" : "walked"), true);
                        break;
                    }
                    // 우리 이동이 아닌 엔진 쪽 로딩·셀 변경·연출(켜진 출구 문 등)이 시작되면 1인칭이 남은 채 로딩되지 않게 바로 끝낸다.
                    const bool loadingNow = walkUi && walkUi->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
                    if (mistNow || loadingNow || player->GetParentCell() != routeForms->cell) {
                        RecordHRouteSpot(false, std::format("an engine {} started during the walk after {:.1f}s ({:.0f} units from the door)",
                            mistNow ? "Mist/Fader opening" : loadingNow ? "loading screen" : "cell change", elapsed, doorDistance), true);
                        break;
                    }
                    if (ready) {
                        const bool tickOk = std::isfinite(deltaSec) && deltaSec > 0.0;
                        if (!test.walkStepMode) {
                            // 사용자 결정 "창이 뒤면 위치 이동": 창이 앞에 있는데 걷지 못하면 위치 이동으로 가리지 않고 시간 초과 실패로 남긴다.
                            if (const auto window = GetGameWindowState(); !window.InFront()) {
                                test.walkStepMode = true;
                                test.walkStepFrom = pos;
                                test.walkStepDistance = 0.0f;
                                ++test.walkStepped;
                                LogInfo(std::format("H route {} walk 0x{:06X}: the game window is behind another window, switching to stepped movement "
                                                    "after {:.1f}s ({:.0f} units from the door); {}.",
                                    spot.dungeon->key, spot.exitId, test.walkReadySec, doorDistance, DescribeHWalkState(player, test.walkYaw)));
                                if (auto* controls = RE::PlayerControls::GetSingleton()) {
                                    controls->data.autoMove = false;
                                }
                            }
                        }
                        if (test.walkStepMode) {
                            // 출발점에서 문 자리로 곧게 옮긴다(3차원 구 판정이므로 높이도 함께 옮김).
                            if (tickOk) {
                                test.walkStepDistance += kHRouteWalkStepSpeed * static_cast<float>(deltaSec);
                            }
                            const auto path = spot.position - test.walkStepFrom;
                            const float length = path.Length();
                            const float t = length > 0.0f ? std::min(1.0f, test.walkStepDistance / length) : 1.0f;
                            player->SetPosition(test.walkStepFrom + path * t, true);
                        } else {
                            if (auto* controls = RE::PlayerControls::GetSingleton()) {
                                controls->data.autoMove = true;
                            }
                        }
                        player->SetHeading(test.walkYaw);
                        ++test.walkReadyTicks;
                        if (tickOk) {
                            test.walkReadySec += deltaSec;
                        }
                    } else {
                        ++test.walkNotReadyTicks;
                    }
                    // 시간 초과는 게임 시간으로 본다. 창이 뒤에서 틱이 멈췄던 동안은 위치 이동 기회를 받지 못했다(Codex 검토 2026-11-24).
                    if (test.walkTickSec >= kHRouteWalkSec) {
                        RecordHRouteSpot(false, std::format("{} {:.0f}s without the exit zone taking over; ended at ({:.0f}, {:.0f}, {:.1f}), {:.0f} units from the door",
                            test.walkStepMode ? "stepped" : "walked", kHRouteWalkSec, pos.x, pos.y, pos.z, doorDistance), true);
                    }
                    break;
                }
                const bool byExitZone = MapDeviceRuntime::GetExitZoneTravelCount() != test.exitZoneBefore;
                if (travel.kind || byExitZone) {
                    // 결정 177 ③ 재보완: 탐침은 회차 없이(허가 없이) 돈다. 출구 자리와 앞뒤 150유닛은 출구 구역 안이므로 출구 구역이
                    // 같은 셀 안정 판정 직후(낙하 복귀보다 먼저) 안전 탈출을 걸어야 한다.
                    const bool ok = exitProbe && byExitZone && travel.kind == Core::MapTravelKind::SafeEscape;
                    RecordHRouteSpot(ok, ok ? "the exit zone started our safe escape"
                        : !exitProbe ? "a travel started during the placement"
                        : byExitZone ? "the exit zone started a travel that is not a safe escape"
                                     : "a travel other than the exit zone started", true);
                    break;
                }
                if (MapDeviceRuntime::GetFallRecoveryCount() != test.fallRecoveryBefore) {
                    // 출구 탐침 자리는 출구 구역 안이라 낙하 복귀가 먼저 받으면 출구 구역이 놓친 것이다. 입구 도착은 RouteRecover가 확인한다.
                    RecordHRouteSpot(false, exitProbe ? "fall recovery took over before the exit zone" : "fall recovery intervened", true);
                    break;
                }
                if (ready) {
                    if (player->GetParentCell() != routeForms->cell) {
                        RecordHRouteSpot(false, "cell changed during placement", true);
                        break;
                    }
                    const auto pos = player->GetPosition();
                    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
                        RecordHRouteSpot(false, "non-finite player position");
                        break;
                    }
                    const auto dx = pos.x - spot.position.x;
                    const auto dy = pos.y - spot.position.y;
                    const bool closeXY = std::hypot(dx, dy) <= kHRouteArrivalXY;
                    if (!test.routeArrivalSeen && closeXY && pos.z <= spot.position.z + kHRouteArrivalAbove) {
                        test.routeArrivalSeen = true;
                    }
                    if (test.routeArrivalSeen) {
                        ++test.routeHeightSamples;
                        test.lowestZ = std::min(test.lowestZ, pos.z);
                        if (std::isfinite(deltaSec) && deltaSec > 0.0) {
                            test.routeObservedSec += deltaSec;
                        }
                    }
                    if (spot.position.z - test.lowestZ > kHRouteMaxDrop) {
                        if (!exitProbe) {
                            RecordHRouteSpot(false, "fell more than 300 units", true);
                            break;
                        }
                        // 출구 탐침은 낙하 복귀나 착지까지 계속 본다.
                        test.routeFell = true;
                    }
                    if (test.routeFell) {
                        // 문턱(navmeshMinZ − 여유) 위에서 높이가 멈추면 아래층 바닥에 착지한 것으로 본다.
                        const auto settings = MapDeviceRuntime::GetTravelSettings();
                        const double threshold = static_cast<double>(spot.dungeon->navmeshMinZ) - settings.fallMargin;
                        // 직전 표본이 아니라 안정 구간을 시작한 높이와 비교한다. 천천히 계속 내려가는 경우를 착지로 보지 않는다(Codex 검토).
                        const bool stable = test.routeLandingZ && std::abs(pos.z - *test.routeLandingZ) <= kHRouteLandingStableZ;
                        if (!stable) {
                            test.routeLandingZ = pos.z;
                        }
                        test.routeLandingStableSec = stable && std::isfinite(deltaSec) && deltaSec > 0.0
                            ? test.routeLandingStableSec + deltaSec : 0.0;
                        if (test.routeLandingStableSec >= kHRouteObserveSec) {
                            // 결정 177 ③ 재보완: 출구 탐침 자리는 출구 구역 안이라 떨어져 멈출 때까지 출구 구역이 받지 않았으면 실패다.
                            const bool neverBelow = test.lowestZ >= threshold;
                            const bool aboveThreshold = pos.z >= threshold;
                            RecordHRouteSpot(false,
                                std::format("{} at Z={:.1f} without the exit zone taking over (drop {:.0f}, fall threshold {:.1f})",
                                    !neverBelow ? "went below the fall threshold, then stopped"
                                    : aboveThreshold ? "landed on a lower floor" : "settled below the fall threshold",
                                    pos.z, spot.position.z - pos.z, threshold), true);
                            break;
                        }
                    } else if (test.routeHeightSamples >= kHRouteMinHeightSamples && test.routeObservedSec >= kHRouteObserveSec) {
                        // 출구 탐침 자리에 서 있기만 하면 출구 구역이 놓친 것이다.
                        RecordHRouteSpot(!exitProbe, exitProbe ? "stood at the exit without the exit zone taking over"
                                                               : "placement reached and observed");
                        break;
                    }
                }
                if (elapsed >= (exitProbe ? kHRouteExitProbeSec : kHRouteProbeSec)) {
                    LogInfo("H route probe timed out; player state: " + DescribeHRoutePlayerState(player, paused, travel));
                    RecordHRouteSpot(false, test.routeFell ? "fell and neither recovered nor settled in time"
                                                           : "placement or observation timed out", test.routeFell);
                }
                break;
            }

            case Stage::RouteRecover: {
                const auto* priorDungeon = test.route[test.routeIndex - 1].dungeon;
                const auto* priorForms = MapPackRuntime::FindForms(priorDungeon);
                const auto settings = MapDeviceRuntime::GetTravelSettings();
                if (ready) {
                    constexpr float kStableZTolerance = 1.0f;
                    const auto pos = player->GetPosition();
                    const bool atEntry = (pos - priorForms->entryMarker->GetPosition()).Length() <= settings.arrivalRadius;
                    const bool recoveryTriggered = MapDeviceRuntime::GetFallRecoveryCount() != test.fallRecoveryBefore;
                    const bool validZ = std::isfinite(pos.z) && pos.z >= priorDungeon->navmeshMinZ - settings.fallMargin;
                    const bool stableZ = test.routeRecoveryZ && std::abs(pos.z - *test.routeRecoveryZ) <= kStableZTolerance;
                    test.routeRecoveryZ = pos.z;
                    if (!recoveryTriggered && validZ && stableZ && std::isfinite(deltaSec) && deltaSec > 0.0) {
                        test.routeRecoveryStableSec += deltaSec;
                    } else {
                        test.routeRecoveryStableSec = 0.0;
                    }
                    // 다른 셀로 옮겨졌으면 지도 팩 밖이어야 한다(안전 탈출의 도착). 다른 팩 셀이면 기다리다 시간 초과로 중단한다.
                    const bool leftPack = player->GetParentCell() != priorForms->cell && !travel.inPackCell;
                    if (leftPack || (recoveryTriggered && validZ && atEntry) || test.routeRecoveryStableSec >= kHSpawnSettleSec) {
                        if (std::exchange(test.routeEscapeExpected, false)) {
                            // 출구 구역이 건 안전 탈출은 고정 도착 마커에 닿아야 한다. 팩 안에 남았거나 다른 곳에 내렸으면 놓친 것이다.
                            const auto arrival = ObserveHSafeEscapeArrival(player);
                            if (leftPack && arrival.AtMarker()) {
                                ++test.routeEscapeArrived;
                            } else {
                                ++test.routeEscapeMissed;
                                LogInfo(std::format("H route safe escape after spot {}/{} did not arrive at the fixed marker (leftPack={}, inTamriel={}, "
                                                    "markerDistance={:.0f}); player state: {}",
                                    test.routeIndex, test.route.size(), leftPack, arrival.inTamriel, arrival.distance,
                                    DescribeHRoutePlayerState(player, paused, travel)));
                            }
                        }
                        EnterHContract(Stage::RouteStart);
                        break;
                    }
                }
                if (elapsed >= travelSec) {
                    if (std::exchange(test.routeEscapeExpected, false)) {
                        ++test.routeEscapeMissed;
                    }
                    LogInfo("H route recovery wait ended; player state: " + DescribeHRoutePlayerState(player, paused, travel));
                    AbortHContract("H route fall recovery did not settle before the next landing probe.");
                }
                break;
            }

            case Stage::CloseRoutes:
                if (auto* controls = RE::PlayerControls::GetSingleton(); controls && controls->data.autoMove) {
                    controls->data.autoMove = false;
                }
                if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && std::exchange(test.routeRestoreThirdPerson, false)) {
                    (void)camera->ForceThirdPerson();
                }
                if (outside && !run.active && RestoreHGodMode(player)) {
                    if (g_cfg.routeOnly) {
                        FinishAndScheduleQuit("Route-only investigation run finished.");
                        break;
                    }
                    EnterPhase(Phase::ScenarioH_UninstallPrepare);
                } else if (elapsed >= travelSec) {
                    AbortHContract("H route exit or God mode restoration did not settle before H8.");
                } else if (ready) {
                    CloseHRun(player);
                }
                break;

            case Stage::RestoreCheckpoint:
                if (IsHCompanionCleanupPending()) {
                    break;
                }
                (void)RestoreHGodMode(player);
                if (travel.kind && elapsed < travelSec) {
                    break;
                }
                if (test.restoreAttempts >= kHCheckpointRestoreAttempts) {
                    test.restoreFinished = true;
                    g_runError = true;
                    FinishAndScheduleQuit("H checkpoint could not be restored and verified after two attempts.");
                    break;
                }
                ++test.restoreAttempts;
                if (RequestLoadSave(test.checkpointStem)) {
                    EnterHContract(Stage::AwaitCheckpoint);
                } else {
                    LogInfo("H checkpoint restore request failed.");
                }
                break;

            case Stage::AwaitCheckpoint: {
                bool loaded = false;
                bool failed = false;
                {
                    std::scoped_lock lock(g_msgMutex);
                    loaded = g_postLoadSignaled && g_postLoadSucceeded;
                    failed = g_postLoadSignaled && !g_postLoadSucceeded;
                }
                if (loaded && outside && !run.active) {
                    if (!RestoreHGodMode(player) || !HCheckpointMatches(ObserveHCheckpoint(player), test.checkpoint) ||
                        MapDeviceRuntime::IsUninstallCleanupArmed()) {
                        if (elapsed >= kHReloadSettleLimitSec) {
                            LogInfo("H restored state did not match the checkpoint after settling.");
                            EnterHContract(Stage::RestoreCheckpoint);
                        }
                        break;
                    }
                    test.restoreFinished = true;
                    g_hNegativeTributeOutstanding = false;
                    g_hArenaHealthBufferBase.reset();
                    g_hMaskBeforeNegativeArena.reset();
                    g_hAtlasBeforeNoContributionKill.reset();
                    LogInfo("H checkpoint restored and verified, including tracked map rarity and affixes.");
                    FinishAndScheduleQuit(test.finishReason);
                } else if (failed || elapsed >= kHReloadWaitSec) {
                    LogInfo("H checkpoint restore load failed or timed out.");
                    EnterHContract(Stage::RestoreCheckpoint);
                }
                break;
            }
            }
        }

        void LogScenarioIsolation(std::string_view scenario)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* tes = RE::TES::GetSingleton();
            if (!player || !tes) {
                return;
            }
            std::vector<RE::NiPointer<RE::Actor>> nearbyActors;
            tes->ForEachReferenceInRange(player, kTestIsolationRadius, [&](RE::TESObjectREFR* ref) {
                if (auto* actor = ref->As<RE::Actor>(); actor && actor != player) {
                    nearbyActors.emplace_back(actor);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            std::size_t foreignHostiles = 0;
            for (const auto& actor : nearbyActors) {
                if (actor->IsDead() || actor->IsDisabled() || actor->IsDeleted() || !actor->IsHostileToActor(player)) {
                    continue;
                }
                if (std::ranges::any_of(g_spawnedSkeevers, [&](const auto& handle) { return handle.get().get() == actor.get(); })) {
                    continue;
                }
                const auto* base = actor->GetBaseObject();
                const auto* name = actor->GetDisplayFullName();
                const auto distance = (actor->GetPosition() - player->GetPosition()).Length();
                LogInfo(std::format("Scenario {} external hostile: form=0x{:08X}, base=0x{:08X}, distance={:.1f}, name='{}'",
                    scenario, actor->GetFormID(), base ? base->GetFormID() : 0, distance, name ? name : ""));
                ++foreignHostiles;
            }
            if (foreignHostiles != 0) {
                LogWarn(std::format("Scenario {} 격리 위반(시험 무효) (hostiles={}, radius={:.0f}).",
                    scenario, foreignHostiles, kTestIsolationRadius));
            }
        }

        void BeginScenarioB()
        {
            LogScenarioIsolation("B");
            EnterPhase(Phase::ScenarioB_PrepareWeapon);
        }

        void BeginScenarioC_Es()
        {
            if (g_cEsRestarts == 0) {
                LogScenarioIsolation("C");
            }
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
            g_cEsSettled = false;
            g_cEsSettledAt = 0.0;
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
        Core::Config* config,
        Core::SkillSystem* skillSystem,
        Core::CharmSystem* charmSystem)
    {
        g_queue = queue;
        g_statEngine = statEngine;
        g_esPool = esPool;
        g_killTracker = killTracker;
        g_dataPack = dataPack;
        g_treeData = treeData;
        g_itemData = itemData;
        g_config = config;
        g_skillSystem = skillSystem;
        g_charmSystem = charmSystem;

        const auto logDir = SKSE::log::log_directory();
        if (!logDir) {
            return;
        }
        const auto markerPath = *logDir / kMarkerFilename;
        std::error_code ec;
        if (!fs::is_regular_file(markerPath, ec)) {
            return;
        }

        std::ifstream marker(markerPath);
        auto parsed = marker ? AutoTestMarker::ParseMarker(marker) : std::nullopt;
        marker.close();
        if (!parsed) {
            SKSE::log::warn("AutoTest: marker '{}' found but unreadable; ignoring.", markerPath.string());
            return;
        }

        if (parsed->consumeMarker) {
            fs::remove(markerPath, ec);
        }

        g_cfg = std::move(*parsed);
        g_reportPath = *logDir / kReportFilename;
        const auto prevPath = *logDir / kPrevReportFilename;
        if (fs::exists(g_reportPath, ec)) {
            fs::rename(g_reportPath, prevPath, ec);
        }
        g_report.open(g_reportPath, std::ios::out | std::ios::trunc);

        g_passCount = 0;
        g_hChecksReported.fill(false);
        g_saveChecksReported.fill(false);
        g_cleanupTest = {};
        g_failCount = 0;
        g_warnCount = 0;
        g_startedAt = Clock::now();
        // U2 확정(2026-11-20): 출시 기본값 EnforceGemRequirements=true에서 Lv1 기준 세이브의
        // 스타터 젬(Cyclone Lv28 등) 통합 시험이 가능하도록 AutoTest 실행 중에만 임시 false 적용(종료 시 원복)
        if (g_config && !g_cfg.saveOnly && !g_cfg.cleanupOnly) {
            g_savedEnforceGemReq = g_config->enforceGemRequirements;
            g_hasSavedEnforceGemReq = true;
            g_config->enforceGemRequirements = false;
        }
        g_enabled.store(true, std::memory_order_release);

        LogInfo(std::format("SkyrimPoE Stage 6 AutoTest armed (save='{}', temp_save='{}', quit={}, timeout={:.0f}s).",
            g_cfg.baselineSave, g_cfg.tempSave, g_cfg.quitWhenDone, g_cfg.globalTimeoutSec));

        if (g_cfg.cleanupOnly) {
            SaveLoad::SetCleanupTestTarget(true);
            EnterPhase(Phase::CleanupContent);
            return;
        }
        if (g_cfg.saveOnly) {
            EnterPhase(Phase::SaveOnlyContent);
            return;
        }
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

    bool AutoTest::IsCleanupOnly() noexcept
    {
        return IsEnabled() && g_cfg.cleanupOnly;
    }

    void AutoTest::TickSaveOnlyWhileSuspended(RE::Main* main)
    {
        if (IsEnabled() && (g_cfg.saveOnly || g_cfg.cleanupOnly)) {
            Tick(main, 0.0);
        }
    }

    void AutoTest::OnPreLoadGame(std::string_view saveName)
    {
        if (!IsEnabled()) {
            return;
        }
        const bool cleanupMatch = g_cfg.cleanupOnly && SaveLoad::MatchCleanupTestLoad(saveName);
        const auto stem = g_cfg.cleanupOnly ? std::string{} : ToLowerAscii(StripSaveStem(saveName));
        std::scoped_lock lock(g_msgMutex);
        g_preLoadMatched = g_cfg.cleanupOnly ? cleanupMatch : !g_expectedLoadStem.empty() && stem == g_expectedLoadStem;
        g_postLoadSignaled = false;
        g_postLoadSucceeded = false;
    }

    void AutoTest::OnPostLoadGame(bool succeeded)
    {
        if (!IsEnabled()) {
            return;
        }
        std::scoped_lock lock(g_msgMutex);
        if (!std::exchange(g_preLoadMatched, false)) {
            return;
        }
        g_expectedLoadStem.clear();
        if (succeeded && g_hWatchArenaCoSave) {
            g_hArenaTimerAtLoad = MapDeviceRuntime::TakeSnapshotForSave().arenaPhaseInvulnSec;
        }
        g_postLoadSignaled = true;
        g_postLoadSucceeded = succeeded;
    }

    void AutoTest::OnSaveGame(std::string_view saveName)
    {
        if (!IsEnabled()) {
            return;
        }
        const auto stem = StripSaveStem(saveName);
        if (g_cfg.saveOnly || g_cfg.cleanupOnly) {
            // SKSE dispatches kSaveGame before it invokes the co-save writer.
            const auto previous = SaveLoad::GetLastSaveDiagnostics();
            std::scoped_lock lock(g_msgMutex);
            if (g_expectedSaveStem.empty()) {
                return;
            }
            if (g_saveMatched) {
                if (!g_saveAtNextMessage) {
                    g_saveAtNextMessage = previous;
                }
            } else if (g_cfg.cleanupOnly ? SameSaveStem(stem, g_expectedSaveStem) :
                                         ToLowerAscii(stem) == ToLowerAscii(g_expectedSaveStem)) {
                g_saveSequenceAtMessage = previous.sequence;
                g_saveMatched = true;
            }
            return;
        }
        if (stem.empty()) {
            return;
        }
        std::scoped_lock lock(g_msgMutex);
        if (!g_expectedSaveStem.empty() && ToLowerAscii(stem) == ToLowerAscii(g_expectedSaveStem)) {
            if (g_hWatchArenaCoSave && !g_saveMatched) {
                g_hArenaTimerAtSave = MapDeviceRuntime::TakeSnapshotForSave().arenaPhaseInvulnSec;
            }
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

        if (g_cfg.cleanupOnly) {
            try {
                TickCleanupOnly();
            } catch (const std::exception& e) {
                FinishAndScheduleQuit(std::format("Cleanup AutoTest failed: {}", e.what()));
            } catch (...) {
                FinishAndScheduleQuit("Cleanup AutoTest failed with an unknown exception.");
            }
            return;
        }

        const bool saveOnlySuspended = g_cfg.saveOnly && !ModState::AllowsGameplay();
        if (g_phase == Phase::SaveOnlyContent) {
            const auto pack = SaveLoad::GetContentGeneration();
            const auto status = ModState::Get();
            const bool normal = status->mode == ModMode::Normal;
            const bool ok = pack.has_value() && normal;
            CheckSave(SaveCheck::Content, ok, std::format("contentCheckPassed={}, normalMode={}, failureReasons=[{}]",
                pack.has_value(), normal, ok ? "" : status->reason));
            if (!ok) {
                FinishAndScheduleQuit("Startup content or mod mode check failed.");
            } else if (g_cfg.baselineSave.empty()) {
                CheckSave(SaveCheck::Baseline, false, "Marker file missing required 'save=<stem>' entry.");
                FinishAndScheduleQuit("Aborting due to missing baseline save.");
            } else {
                EnterPhase(Phase::AwaitMainMenu);
            }
            return;
        }

        const double totalSec = std::chrono::duration<double>(Clock::now() - g_startedAt).count();
        if (totalSec > g_cfg.globalTimeoutSec && !g_globalTimeoutReported) {
            g_globalTimeoutReported = true;
            g_runError = true;
            const auto detail = std::format("GlobalTimeout exceeded {:.0f}s in {}.", g_cfg.globalTimeoutSec, PhaseName(g_phase));
            WriteLine("ERROR", detail);
            // 걷기 탐침 도중이면 자동 이동·강제 1인칭을 풀고 끝낸다. 체크포인트 불러오기나 조작 반환이 그 상태로 일어나지 않게 한다.
            if (!g_cfg.saveOnly && g_hContract.walkActive) {
                StopHWalk();
            }
            FinishAndScheduleQuit(detail);
            if (g_phase != Phase::ScenarioH_Contract) {
                return;
            }
        }

        // Preserve is also the transient mode during a load. Let its matched post-load message determine the result.
        if (saveOnlySuspended &&
            g_phase != Phase::AwaitBaselineLoad && g_phase != Phase::ScenarioA_AwaitReload) {
            FinishAndScheduleQuit(std::format("Mod left normal mode in {}. {}", PhaseName(g_phase), ModState::Get()->reason));
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!saveOnlySuspended) {
            CloseDialogueWhileAwaitingSave(ui);
        }
        const bool paused = ui && ui->GameIsPaused();
        if (!g_cfg.saveOnly && IsHCompanionCleanupPending()) {
            PumpHCompanionCleanup(!paused && IsPlayerReadyInWorld());
        }
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
                if (g_cfg.saveOnly) {
                    CheckSave(SaveCheck::Baseline, false, "BGSSaveLoadManager singleton unavailable.");
                } else {
                    Check("A0_LoadBaseline", false, "BGSSaveLoadManager singleton unavailable.");
                }
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
            if (g_cfg.saveOnly && signaled && !SaveCheckReported(SaveCheck::Baseline) &&
                !CheckSaveOnlyLoad(SaveCheck::Baseline, ok, g_cfg.baselineSave)) {
                FinishAndScheduleQuit("Baseline save did not load in normal mode with a supported GENR.");
                return;
            }
            if (signaled && !ok) {
                Check("A0_LoadBaseline", false, std::format("kPostLoadGame reported failure for '{}'.", g_cfg.baselineSave));
                FinishAndScheduleQuit("Baseline save failed to load.");
                return;
            }
            if (signaled && ok && IsPlayerReadyInWorld()) {
                LogInfo(g_cfg.saveOnly ? "Baseline save loaded; settling unpaused world." :
                    "Baseline save loaded; settling unpaused world + Prisma DOM.");
                EnterPhase(Phase::BaselineSettle);
            } else if (PhaseElapsedSec() > 90.0) {
                if (!g_cfg.saveOnly) {
                    Check("A0_LoadBaseline", false, "Timed out waiting for baseline save load.");
                } else if (!SaveCheckReported(SaveCheck::Baseline)) {
                    (void)CheckSaveOnlyLoad(SaveCheck::Baseline, false, g_cfg.baselineSave);
                } else {
                    g_runError = true;
                }
                FinishAndScheduleQuit("Baseline load timeout.");
            }
            break;
        }

        case Phase::BaselineSettle: {
            const bool minSettled = (g_unpausedAccumSec >= 2.5);
            const bool domReady = g_cfg.saveOnly || UiBridge::IsDomReady();
            if (!minSettled && PhaseElapsedSec() < 8.0) {
                break;
            }
            if (!domReady && PhaseElapsedSec() < 8.0) {
                break;
            }
            if (g_cfg.saveOnly) {
                g_saveAttempts = 0;
                g_nextSaveRetryAt = Clock::now();
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem = g_cfg.tempSave;
                    g_saveMatched = false;
                    g_saveAtNextMessage.reset();
                }
                EnterPhase(Phase::ScenarioA_SavePump);
                break;
            }
            if (g_cfg.enemyEsOnly) {
                // 결정 185 확인용 단축 실행: 앞 시나리오 없이 적 ES 표시 시험만 돌고 끝낸다(체크포인트 없음, 저장하지 않음).
                LogInfo("Enemy-ES investigation run: skipping scenarios A-H and running ES1-ES7 only.");
                g_esTest = {};
                EnterPhase(Phase::EnemyEsOnly);
                break;
            }
            if (g_cfg.routeOnly) {
                // 조사용 단축 실행: 앞 시나리오와 H 계약 앞부분 없이 경로 탐침 준비로 바로 간다. 체크포인트가 없어 끝에 복원하지 않는다.
                LogInfo("Route-only investigation run: skipping scenarios A-G and the H contract before the route probes.");
                g_hContract.started = Clock::now();
                EnterPhase(Phase::ScenarioH_Contract);
                EnterHContract(HContractStage::RoutePrepare);
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
                if (!g_cfg.saveOnly) {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                LogInfo(std::format("Temp save '{}' confirmed by kSaveGame (attempts={}).", g_cfg.tempSave, g_saveAttempts));
                EnterPhase(Phase::ScenarioA_SaveWaitBeforeLoad);
                break;
            }
            if (PhaseElapsedSec() > kScenarioASaveTimeoutSec) {
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                const auto detail = std::format("Timed out waiting for kSaveGame('{}') after {} attempts.",
                    g_cfg.tempSave, g_saveAttempts);
                if (g_cfg.saveOnly) {
                    CheckSave(SaveCheck::Write, false, detail);
                    FinishAndScheduleQuit("Temporary save did not reach the co-save check.");
                    return;
                }
                Check("A9_SaveLoadRoundTrip", false, detail);
                BeginScenarioB();
                break;
            }
            const auto now = Clock::now();
            if (now >= g_nextSaveRetryAt) {
                if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
                    if (g_cfg.saveOnly) {
                        g_saveOnlyBefore = CaptureSaveOnlySnapshot();
                    }
                    ++g_saveAttempts;
                    mgr->Save(g_cfg.tempSave.c_str());
                }
                g_nextSaveRetryAt = now + std::chrono::seconds(2);
            }
            break;
        }

        case Phase::ScenarioA_SaveWaitBeforeLoad: {
            const double elapsed = PhaseElapsedSec();
            if (elapsed < 1.0) {
                break;
            }
            if (g_cfg.saveOnly) {
                std::uint64_t sequenceAtMessage = 0;
                auto write = SaveLoad::GetLastSaveDiagnostics();
                {
                    std::scoped_lock lock(g_msgMutex);
                    sequenceAtMessage = g_saveSequenceAtMessage;
                    if (g_saveAtNextMessage) {
                        write = *g_saveAtNextMessage;
                    } else if (write.sequence == sequenceAtMessage && elapsed < kScenarioASaveTimeoutSec) {
                        break;
                    }
                    g_expectedSaveStem.clear();
                }
                const bool matchedCallback = write.sequence == sequenceAtMessage + 1;
                const bool written = matchedCallback && write.wroteGeneration && write.failedRecords == 0 && write.complete;
                CheckSave(SaveCheck::Write, written, std::format(
                    "save='{}', matchedCallback={}, sequence={} -> {}, GENR written={}, failedRecords={}, complete={}",
                    g_cfg.tempSave, matchedCallback, sequenceAtMessage, write.sequence,
                    matchedCallback && write.wroteGeneration,
                    matchedCallback ? std::to_string(write.failedRecords) : "unavailable", matchedCallback && write.complete));
                if (!written) {
                    FinishAndScheduleQuit("Temporary co-save write could not be verified.");
                    return;
                }
            }
            LogInfo(std::format("Reloading temp save '{}' to verify SPOE co-save round-trip...", g_cfg.tempSave));
            if (!RequestLoadSave(g_cfg.tempSave)) {
                if (g_cfg.saveOnly) {
                    CheckSave(SaveCheck::Reload, false, "BGSSaveLoadManager singleton unavailable.");
                    FinishAndScheduleQuit("Could not request the temporary save reload.");
                    return;
                }
                Check("A9_SaveLoadRoundTrip", false, "BGSSaveLoadManager singleton unavailable.");
                BeginScenarioB();
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
            if (g_cfg.saveOnly && signaled && !SaveCheckReported(SaveCheck::Reload)) {
                (void)CheckSaveOnlyLoad(SaveCheck::Reload, ok, g_cfg.tempSave);
            }
            if (g_cfg.saveOnly && signaled && (!ok || saveOnlySuspended)) {
                if (ok) {
                    CheckSave(SaveCheck::State, false, "Skipped because the reloaded mod is not in normal mode.");
                    CheckSaveOnlyPreservedStore();
                }
                FinishAndScheduleQuit("Temporary save reload did not return to normal mode.");
                return;
            }
            if (signaled && !ok) {
                Check("A9_SaveLoadRoundTrip", false, "kPostLoadGame reported failure on temp save reload.");
                FinishAndScheduleQuit("Reload failed.");
                return;
            }
            if (signaled && ok && IsPlayerReadyInWorld()) {
                EnterPhase(Phase::ScenarioA_ReloadSettle);
            } else if (PhaseElapsedSec() > 90.0) {
                if (!g_cfg.saveOnly) {
                    Check("A9_SaveLoadRoundTrip", false, "Timed out waiting for temp save reload.");
                } else if (!SaveCheckReported(SaveCheck::Reload)) {
                    (void)CheckSaveOnlyLoad(SaveCheck::Reload, false, g_cfg.tempSave);
                } else {
                    g_runError = true;
                }
                FinishAndScheduleQuit("Reload timeout.");
            }
            break;
        }

        case Phase::ScenarioA_ReloadSettle: {
            if (g_unpausedAccumSec < 2.5 && PhaseElapsedSec() < 8.0) {
                break;
            }
            if (g_cfg.saveOnly) {
                CheckSaveOnlyRoundTrip();
                CheckSaveOnlyPreservedStore();
                FinishAndScheduleQuit("Save-only round-trip run finished.");
                return;
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

            BeginScenarioB();
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
            g_bKillsSettledAtSec.reset();
            EnterPhase(Phase::ScenarioB_KillWait);
            break;
        }

        case Phase::ScenarioB_KillWait: {
            const auto stAfter = SaveLoad::GetStateCopy();
            const bool exileGained = (stAfter.exileLevel > g_exileLevelBeforeB) || (stAfter.exileXp > g_exileXpBeforeB);
            const auto cycLvlNow = stAfter.slots[0].occupied ? stAfter.slots[0].gem.level : 0;
            const auto cycXpNow = stAfter.slots[0].occupied ? stAfter.slots[0].gem.xp : 0;
            const bool gemGained = (cycLvlNow > g_cycGemLvlBeforeB) || (cycXpNow > g_cycGemXpBeforeB);

            // 스카이림 TESDeathEvent(dead=true)는 사망 모션 뒤에 온다. 2026-11-23 실행 9(전설 난이도)에서 5초 안에 확정이 오지 않아
            // B6가 실패하고 늦은 처치 경험치가 C6 측정에 섞였다. B의 쥐 전부의 사망 확정(보상 청구)을 기다린 뒤,
            // 큐에서 경험치가 반영될 시간(0.5초)을 더 주고 C로 넘어간다. 확정이 끝내 안 오는 쥐는 상한에서 끊는다.
            std::size_t unconfirmedKills = 0;
            for (const auto& h : g_spawnedSkeevers) {
                const auto actor = h.get();
                if (!actor || actor->IsDeleted()) {
                    continue;
                }
                if (!g_killTracker || !g_killTracker->GetActorRecordSnapshot(actor->GetFormID()).rewardClaimed) {
                    ++unconfirmedKills;
                }
            }
            if (unconfirmedKills == 0 && !g_bKillsSettledAtSec) {
                g_bKillsSettledAtSec = g_unpausedAccumSec;
            }
            const bool killsSettled = g_bKillsSettledAtSec && g_unpausedAccumSec - *g_bKillsSettledAtSec >= kBKillXpApplySec;
            if (!killsSettled && g_unpausedAccumSec < kBKillSettleCapSec && PhaseElapsedSec() < kBKillSettleCapSec + 3.0) {
                break;
            }

            // 미확정 쥐가 남으면 그 처치 경험치가 C 측정에 섞이므로 B6를 통과시키지 않는다.
            Check("B6_KillTrackerExileAndGemXp", exileGained && gemGained && unconfirmedKills == 0,
                std::format("wait={:.2f}s, unconfirmedKills={}, exile Lv{} XP {} -> Lv{} XP {}, Cyclone Lv{} XP {} -> Lv{} XP {}",
                    g_unpausedAccumSec, unconfirmedKills,
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
            if (!g_cEsSettled) {
                // B의 늦은 사망 확정(약 2초)과 그에 따른 레벨업·스탯 재빌드가 끝나기를 기다린 뒤, 현재 상태로 임시 ES를 다시 주입한다.
                if (g_unpausedAccumSec < kCEsSettleSec && PhaseElapsedSec() < kCEsSettleSec + 3.0) {
                    break;
                }
                if (g_tempEsInjected && g_statEngine && g_dataPack && g_config) {
                    const auto stNow = SaveLoad::GetStateCopy();
                    Core::Layer1Input l1;
                    l1.exileLevel = stNow.exileLevel;
                    l1.className = stNow.className.empty() ? "Scion" : stNow.className;
                    l1.slots = stNow.slots;
                    l1.flatEnergyShield = 100.0;
                    g_statEngine->SetLayer1(l1);
                    (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                }
                g_cEsSettled = true;
                g_cEsSettledAt = g_unpausedAccumSec;
                break;
            }
            const auto esSnap = g_esPool->Snapshot();
            if (esSnap.max <= 0.0 && g_unpausedAccumSec < g_cEsSettledAt + 1.2 && PhaseElapsedSec() < kCEsSettleSec + 6.5) {
                // 직전 시나리오 B의 지연된 스키버 TESDeathEvent가 SyncStateToStatEngine()으로
                // 임시 flatEnergyShield=100을 덮어쓸 수 있으므로 미반영 시 재주입한다.
                if (g_tempEsInjected && g_statEngine && g_dataPack && g_config) {
                    const auto curSnap = g_statEngine->GetSnapshot();
                    if (!curSnap || curSnap->poeMaxEnergyShield <= 0.0) {
                        const auto stNow = SaveLoad::GetStateCopy();
                        Core::Layer1Input l1;
                        l1.exileLevel = stNow.exileLevel;
                        l1.className = stNow.className.empty() ? "Scion" : stNow.className;
                        l1.slots = stNow.slots;
                        l1.flatEnergyShield = 100.0;
                        g_statEngine->SetLayer1(l1);
                        (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                    }
                }
                break;
            }
            g_esMaxForC = esSnap.max;
            if (g_esMaxForC <= 0.0) {
                if (g_cEsRestarts == 0) {  // 재시작했다면 C1은 이미 기록됨(기대 건수를 지키기 위해 한 번만 셈)
                    Check("C1_EsAbsorbDifficultyAligned", false, "esMax remained 0.0 after temporary ES injection.");
                }
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
            const auto c1Detail = std::format("esMax={:.2f}, diffMult={:.2f}, absorbed={:.2f} (exp {:.2f}), esAfter={:.2f}",
                g_esMaxForC, diffMult, res.absorbedEffectiveDamage, expectedAbsorb, g_esAfterHit);
            if (g_cEsRestarts == 0) {
                Check("C1_EsAbsorbDifficultyAligned", c1Ok, c1Detail);
            } else if (!c1Ok) {
                // 재시작한 측정의 C1이 실패하면 숨기지 않는다(같은 이름으로 한 번 더 기록 → 기대 건수 초과로 전체 FAIL).
                Check("C1_EsAbsorbDifficultyAligned", false, c1Detail + " (after C restart)");
            } else {
                LogInfo("Scenario C restart: C1 re-measured OK: " + c1Detail);
            }

            EnterPhase(Phase::ScenarioC_EsDelay1s);
            break;
        }

        case Phase::ScenarioC_EsDelay1s: {
            if (!g_esPool) {
                BeginScenarioC_Stagger();
                break;
            }
            const auto snap = g_esPool->Snapshot();
            // Codex 지적 25: 3초 대기 뒤에도 늦은 레벨업 등으로 스탯이 다시 빌드돼 ES 최대치가 바뀌었다면 이 측정은 무효다.
            // 실패로 세지 않고 C를 처음부터 다시 한다(최대 2회). 다시 한 횟수는 C2 상세에 남긴다.
            if (std::abs(snap.max - g_esMaxForC) > 0.01 && g_cEsRestarts < 2) {
                ++g_cEsRestarts;
                LogInfo(std::format("Scenario C: ES max changed during measurement ({:.2f} -> {:.2f}); restarting C (#{}).",
                    g_esMaxForC, snap.max, g_cEsRestarts));
                BeginScenarioC_Es();
                break;
            }
            // EnergyShieldPool 자체 시계(timeSinceLastHit)를 직접 관측해 프레임 스파이크 편차를 원천 차단한다.
            if (snap.timeSinceLastHit < 1.0 && PhaseElapsedSec() < 5.0) {
                break;
            }
            const bool holdOk = (g_esMaxForC > 0.0) &&
                (snap.timeSinceLastHit >= 1.0) &&
                (snap.timeSinceLastHit < snap.rechargeDelay) &&
                (std::abs(snap.current - g_esAfterHit) < 0.05);
            Check("C2_EsRechargeDelayHoldAt1s", holdOk,
                std::format("t={:.2f}s (< {:.2f}s), es={:.2f} (expected {:.2f}), cRestarts={}",
                    snap.timeSinceLastHit, snap.rechargeDelay, snap.current, g_esAfterHit, g_cEsRestarts));
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

            // E5: 쥬얼러 오브(스킬 젬 3->4->5 + 5 초과 거절 + 발동 젬 내부 주문 젬 3->4) & 후회의 오브 연동 회수
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
                EnterPhase(Phase::ScenarioF_Start);
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
            EnterPhase(Phase::ScenarioF_Start);
            break;
        }

        // ================= SCENARIO F (5단계 통합 검증: F1~F8) =================
        case Phase::ScenarioF_Start: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !g_dataPack || !g_itemData || !g_treeData || !g_statEngine || !g_config) {
                Check("F1_Stage5DataPackAnd174EspForms", false, "Core singletons unavailable.");
                FinishAndScheduleQuit("Scenario F aborted: missing singletons.");
                break;
            }

            // ---- F1: 28종 젬 + ItemData v2 + 174개 ESP 폼 전수 검증 ----
            std::size_t espFound = 0;
            auto countForm = [&](auto* p) { if (p) ++espFound; };
            countForm(FormIds::Lookup<RE::EffectSetting>(FormIds::kProxySpellEffect));
            countForm(FormIds::Lookup<RE::EffectSetting>(FormIds::kProxyPowerEffect));
            for (const auto fid : FormIds::kProxySpells) countForm(FormIds::Lookup<RE::SpellItem>(fid));
            for (const auto fid : FormIds::kProxyPowers) countForm(FormIds::Lookup<RE::SpellItem>(fid));
            for (const auto& e : FormIds::kAllGemForms) countForm(FormIds::Lookup<RE::TESObjectMISC>(e.localFormId));
            countForm(FormIds::Lookup<RE::BGSExplosion>(FormIds::kVfxIceNova));
            countForm(FormIds::Lookup<RE::BGSExplosion>(FormIds::kVfxShockNova));
            for (const auto fid : FormIds::kCurrencyOrbForms) countForm(FormIds::Lookup<RE::TESObjectMISC>(fid));
            for (std::size_t off = 0; off < FormIds::kSealedTokenCount; ++off) {
                countForm(FormIds::Lookup<RE::TESObjectMISC>(FormIds::kSealedTokenBase + static_cast<RE::FormID>(off)));
            }
            countForm(FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamRare));
            countForm(FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamCurrency));
            for (const auto fid : FormIds::kBeltBaseForms) countForm(FormIds::Lookup<RE::TESObjectMISC>(fid));
            for (std::size_t off = 0; off < FormIds::kBeltTokenCount; ++off) {
                countForm(FormIds::Lookup<RE::TESObjectMISC>(FormIds::kBeltTokenBase + static_cast<RE::FormID>(off)));
            }
            for (const auto fid : FormIds::kJewelBaseForms) countForm(FormIds::Lookup<RE::TESObjectMISC>(fid));
            for (const auto fid : FormIds::kCharmForms) countForm(FormIds::Lookup<RE::TESObjectMISC>(fid));
            countForm(FormIds::Lookup<RE::BGSProjectile>(FormIds::kProjFireball));
            countForm(FormIds::Lookup<RE::BGSProjectile>(FormIds::kProjFreezingPulse));
            countForm(FormIds::Lookup<RE::BGSProjectile>(FormIds::kProjSpark));
            countForm(FormIds::Lookup<RE::BGSExplosion>(FormIds::kVfxFireBurst));
            countForm(FormIds::Lookup<RE::TESObjectMISC>(FormIds::kUniqueSealedToken));
            countForm(FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamUnique));

            const bool f1Ok = (g_dataPack->AllGems().size() == 28) &&
                (g_itemData->AllBelts().size() == 5) &&
                (g_itemData->AllCharms().size() == 6) &&
                (g_itemData->AllMonsterMods().size() == 12) &&
                (g_itemData->AllUniques().size() == 16) &&
                (espFound == 174);
            Check("F1_Stage5DataPackAnd174EspForms", f1Ok,
                std::format("gems={}, belts={}, charms={}, mods={}, uniques={}, espForms={}/174",
                    g_dataPack->AllGems().size(), g_itemData->AllBelts().size(),
                    g_itemData->AllCharms().size(), g_itemData->AllMonsterMods().size(),
                    g_itemData->AllUniques().size(), espFound));

            // ---- F2: 논리 10칸 가상 반지2·벨트 장착 + 패시브 주얼 소켓 힘 선합산/회수 ----
            constexpr RE::FormID kSilverRingId = 0x0003B97C;
            auto* ringForm = RE::TESForm::LookupByID<RE::TESObjectARMO>(kSilverRingId);
            if (ringForm) {
                player->AddObjectToContainer(ringForm, nullptr, 2, nullptr);
            }
            const auto eqR2 = CraftingRuntime::EquipVirtualRing2(kSilverRingId, 0);

            (void)CraftingRuntime::GrantBeltBase(player, FormIds::kBeltBaseForms[4], 1);  // Stygian Vise
            auto* stygianMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kBeltBaseForms[4]);
            const auto eqBelt = stygianMisc
                ? CraftingRuntime::EquipVirtualBelt(stygianMisc->GetFormID(), 0)
                : CraftActionResult{};
            // Stygian Vise에 연금술의 오브를 발라 희귀(3참 슬롯 개방)로 업그레이드
            if (eqBelt.ok && eqBelt.resultKey.IsValid()) {
                (void)CraftingRuntime::GrantCurrency(player, Core::CurrencyKind::Alchemy, 1);
                (void)CraftingRuntime::ApplyOrbToItem(
                    Core::CurrencyKind::Alchemy, stygianMisc->GetFormID(), eqBelt.resultKey.uniqueId, true);
            }
            EquipmentSync::RefreshFromPlayerNow();
            (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);

            const auto gearMetaF2 = EquipmentSync::GetCurrentGearMeta();
            const auto bagF2 = CraftingRuntime::ListPlayerCraftableItems(player);
            bool ringInBagF2 = false;
            for (const auto& bi : bagF2) {
                if (bi.baseFormId == kSilverRingId) {
                    ringInBagF2 = true;
                    break;
                }
            }
            const bool r2Ok = eqR2.ok && ringInBagF2 && gearMetaF2[static_cast<std::size_t>(Core::EquipSlot::Ring2)].occupied;
            const bool beltOk = eqBelt.ok && gearMetaF2[static_cast<std::size_t>(Core::EquipSlot::Belt)].occupied;
            const std::uint8_t unlockedCharmsF2 = g_charmSystem ? g_charmSystem->UnlockedSlots() : 0;

            // 가장 가까운 PassiveNodeKind::Socket 노드를 BFS로 찾아 할당 후 힘 주얼 장착/회수/재장착 검증
            g_fSocketNodeId = 0;
            bool jewelStrOk = false;
            bool jewelUnsockOk = false;
            {
                SaveLoad::UpdateState([](PersistentState& st) {
                    if (st.exileLevel < 60) {
                        st.exileLevel = 60;
                    }
                    if (!st.tree.classSelected && g_treeData->IsLoaded()) {
                        (void)Core::PassiveTree::SelectClass(*g_treeData, st.tree, 1);
                    }
                });
                const auto stTree = SaveLoad::GetStateCopy();
                // BFS로 현재 할당 노드/클래스 시작점에서 가장 가까운 주얼 소켓 탐색
                std::unordered_map<std::uint32_t, std::uint32_t> parent;
                std::vector<std::uint32_t> bfs;
                if (const auto* cls = g_treeData->FindClass(stTree.tree.classId)) {
                    for (const auto nb : cls->startNeighbors) {
                        if (!parent.contains(nb)) {
                            bfs.push_back(nb);
                            parent[nb] = 0;
                        }
                    }
                }
                for (const auto nid : stTree.tree.allocatedNodes) {
                    if (!parent.contains(nid)) {
                        bfs.push_back(nid);
                        parent[nid] = 0;
                    }
                }
                std::uint32_t foundSocket = 0;
                for (std::size_t qi = 0; qi < bfs.size() && foundSocket == 0; ++qi) {
                    const std::uint32_t cur = bfs[qi];
                    const auto* n = g_treeData->FindNode(cur);
                    if (!n) continue;
                    if (n->kind == Core::PassiveNodeKind::Socket && n->ascendancyId.empty()) {
                        foundSocket = cur;
                        break;
                    }
                    for (const auto nb : n->adjacency) {
                        if (parent.contains(nb)) continue;
                        const auto* nbn = g_treeData->FindNode(nb);
                        if (!nbn || !nbn->ascendancyId.empty() ||
                            nbn->isMultipleChoice ||
                            nbn->kind == Core::PassiveNodeKind::ClassStart ||
                            nbn->kind == Core::PassiveNodeKind::AscendancyStart ||
                            nbn->kind == Core::PassiveNodeKind::Mastery) {
                            continue;
                        }
                        parent[nb] = cur;
                        bfs.push_back(nb);
                    }
                }

                if (foundSocket != 0) {
                    std::vector<std::uint32_t> path;
                    for (std::uint32_t cur = foundSocket; cur != 0; cur = parent[cur]) {
                        if (!stTree.tree.IsAllocated(cur)) {
                            path.push_back(cur);
                        }
                    }
                    std::reverse(path.begin(), path.end());
                    bool allocOk = stTree.tree.IsAllocated(foundSocket);
                    if (!allocOk && !path.empty()) {
                        SaveLoad::UpdateState([&](PersistentState& st) {
                            const auto ar = Core::PassiveTree::AllocatePath(*g_treeData, st.tree, path, st.exileLevel, *g_config);
                            allocOk = ar.ok || st.tree.IsAllocated(foundSocket);
                        });
                    }
                    if (allocOk) {
                        g_fSocketNodeId = foundSocket;
                        SaveLoad::SyncStateToStatEngine();
                        (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                        const std::uint32_t strBefore = g_statEngine->GetSnapshot() ? g_statEngine->GetSnapshot()->str : 0;

                        // 결정적 +힘 주얼 1개 생성해 가방에 등록 후 소켓 장착
                        (void)CraftingRuntime::GrantJewel(player, 0, 1);
                        auto* crimsonMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::JewelLocalFormId(0));
                        if (crimsonMisc) {
                            const auto jKey = ItemTracker::EnsureUniqueId(player, crimsonMisc, nullptr);
                            if (jKey.IsValid()) {
                                Core::PoEItemRecord jRec;
                                jRec.baseFormId = crimsonMisc->GetFormID();
                                jRec.rarity = Core::ItemRarity::Magic;
                                jRec.itemLevel = 50;
                                jRec.slotGroup = Core::TokenSlotGroup::Jewel;
                                jRec.jewelColor = 0;
                                // items_3_30.json의 힘 주얼 어픽스(StrengthJewel) 탐색
                                for (const auto& af : g_itemData->AllAffixes()) {
                                    if (!af.lines.empty() && af.lines[0].templateEn.find("to Strength") != std::string::npos &&
                                        af.id.find("Jewel") != std::string::npos) {
                                        Core::RolledAffix ra;
                                        ra.affixId = af.id;
                                        for (const auto& ln : af.lines) {
                                            for (const auto& rg : ln.ranges) ra.values.push_back(rg.maxVal);
                                        }
                                        jRec.affixes.push_back(std::move(ra));
                                        break;
                                    }
                                }
                                ItemTracker::Upsert(jKey, jRec, false);
                                const auto sockRes = CraftingRuntime::SocketPassiveJewel(
                                    foundSocket, crimsonMisc->GetFormID(), jKey.uniqueId);
                                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                                const std::uint32_t strAfter = g_statEngine->GetSnapshot() ? g_statEngine->GetSnapshot()->str : 0;
                                jewelStrOk = sockRes.ok && (strAfter > strBefore);

                                // 회수 후 가방 복귀 검증 → F8 코세이브 왕복 검증을 위해 다시 장착
                                const auto unsockRes = CraftingRuntime::UnsocketPassiveJewel(foundSocket);
                                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                                const std::uint32_t strAfterUnsock = g_statEngine->GetSnapshot() ? g_statEngine->GetSnapshot()->str : 0;
                                jewelUnsockOk = unsockRes.ok && unsockRes.resultKey.IsValid() && (strAfterUnsock == strBefore);
                                if (jewelUnsockOk) {
                                    (void)CraftingRuntime::SocketPassiveJewel(
                                        foundSocket, unsockRes.resultBaseFormId, unsockRes.resultKey.uniqueId);
                                    (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                                }
                            }
                        }
                    }
                }
            }
            Check("F2_TenSlotsVirtualGearAndJewelSocket",
                r2Ok && beltOk && (unlockedCharmsF2 >= 2) && jewelStrOk && jewelUnsockOk,
                std::format("ring2={}, belt={}, charmsUnlocked={}, socketNode={}, jewelStr={}, unsock={}",
                    r2Ok, beltOk, unlockedCharmsF2, g_fSocketNodeId, jewelStrOk, jewelUnsockOk));

            // ---- F3: 실제 UiBridge JSON 경로로 5단계 오라/전령 젬 장착 + 정신력 토글 + 한도 초과 차단 + 이정표 확장 ----
            auto equipGemF = [&](std::uint8_t slot, std::string_view gemId) {
                const auto k = FindCleanGemKeyInPlayerInventory(player, gemId);
                SendUiCmd(json{
                    { "op", "equip" }, { "slot", slot },
                    { "ownerFormId", k.ownerFormId }, { "uniqueId", k.uniqueId },
                    { "gemId", gemId }
                });
            };
            auto socketGemF = [&](std::uint8_t slot, std::int32_t spellIdx, std::string_view gemId) {
                const auto k = FindCleanGemKeyInPlayerInventory(player, gemId);
                SendUiCmd(json{
                    { "op", "socket" }, { "slot", slot }, { "spellIdx", spellIdx },
                    { "ownerFormId", k.ownerFormId }, { "uniqueId", k.uniqueId },
                    { "gemId", gemId }
                });
            };

            {
                for (std::uint8_t si : { std::uint8_t{ 3 }, std::uint8_t{ 4 }, std::uint8_t{ 5 },
                                         std::uint8_t{ 6 }, std::uint8_t{ 7 }, std::uint8_t{ 8 } }) {
                    if (SaveLoad::GetStateCopy().slots[si].occupied) {
                        SendUiCmd(json{ { "op", "unequip" }, { "slot", si } });
                    }
                }
                equipGemF(3, "Determination");  // 50
                equipGemF(4, "Grace");          // 50
                equipGemF(5, "HeraldOfIce");    // 25
                equipGemF(8, "Hatred");         // 50

                const auto stF3 = SaveLoad::GetStateCopy();
                const bool uiAuraEquipOk =
                    stF3.slots[3].occupied && stF3.slots[3].gem.gemId == "Determination" && stF3.slots[3].gem.socketCount == 0 &&
                    stF3.slots[4].occupied && stF3.slots[4].gem.gemId == "Grace" && stF3.slots[4].gem.socketCount == 0 &&
                    stF3.slots[5].occupied && stF3.slots[5].gem.gemId == "HeraldOfIce" && stF3.slots[5].gem.socketCount == 0 &&
                    stF3.slots[8].occupied && stF3.slots[8].gem.gemId == "Hatred" && stF3.slots[8].gem.socketCount == 0;

                SaveLoad::SyncStateToStatEngine();
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);

                const auto snapInit = g_statEngine->GetSnapshot();
                const std::uint32_t maxBefore = snapInit ? snapInit->maxSpirit : 0;  // D8에서 1차 이정표 달성 -> 130
                const double armourBeforeAura = snapInit ? snapInit->armour : 0.0;

                const auto onDet = CraftingRuntime::ToggleAuraSlot(3);
                const auto onGra = CraftingRuntime::ToggleAuraSlot(4);
                const auto onHoi = CraftingRuntime::ToggleAuraSlot(5);
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                const auto snapMid = g_statEngine->GetSnapshot();
                // 125/130 상태에서 Hatred(50)는 초과 거절돼야 함
                const auto overHatred = CraftingRuntime::ToggleAuraSlot(8);

                // 2·3차 보스 이정표 해제 -> 최대 정신력 190
                SaveLoad::UpdateState([](PersistentState& st) {
                    st.tree.ascendancyMilestonesBitmask |= 0x7u;
                });
                SaveLoad::SyncStateToStatEngine();
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                const auto snapCapUp = g_statEngine->GetSnapshot();
                const auto onHatredAfter = CraftingRuntime::ToggleAuraSlot(8);
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                const auto snapFinal = g_statEngine->GetSnapshot();
                g_fExpectedAuraMask = SaveLoad::GetStateCopy().activeAuraMask;

                const bool f3Ok = uiAuraEquipOk && onDet.ok && onGra.ok && onHoi.ok &&
                    !overHatred.ok &&
                    snapMid && snapMid->reservedSpirit == 125 && snapMid->armour > armourBeforeAura &&
                    snapCapUp && snapCapUp->maxSpirit >= maxBefore + 60 &&
                    onHatredAfter.ok &&
                    snapFinal && snapFinal->reservedSpirit == 175 && snapFinal->heraldOfIceActive;
                Check("F3_SpiritAurasAndMilestoneCap", f3Ok,
                    std::format("uiEquip={}, maxBefore={}, midRes={}, overRej={}, capUp={}, finalRes={}, hoi={}",
                        uiAuraEquipOk,
                        maxBefore,
                        snapMid ? snapMid->reservedSpirit : 0,
                        !overHatred.ok,
                        snapCapUp ? snapCapUp->maxSpirit : 0,
                        snapFinal ? snapFinal->reservedSpirit : 0,
                        snapFinal ? snapFinal->heraldOfIceActive : false));
            }

            // ---- F4: 참 장착(U4: 신규 장착 시 0 충전 검증) + 충전 후 화염 피격 루비 참 자동 발동 + 처치 충전 ----
            {
                (void)CraftingRuntime::GrantCharm(player, Core::CharmKind::Ruby, 1);
                const auto eqCh = CraftingRuntime::EquipCharm(0, Core::CharmKind::Ruby);
                const double chOnEquip = g_charmSystem ? g_charmSystem->Slots()[0].charges : -1.0;
                // U4 검증 후 자동 발동 시나리오를 위해 테스트용 최대 충전(40) 주입
                if (g_charmSystem) {
                    auto sl0 = g_charmSystem->Slots()[0];
                    sl0.charges = Core::CharmSystem::kMaxCharges;
                    g_charmSystem->SetSlot(0, sl0);
                }
                const double chBefore = g_charmSystem ? g_charmSystem->Slots()[0].charges : 0.0;
                const bool trigFire = g_charmSystem && g_charmSystem->TryTriggerOnIncomingDamage(Core::DamageType::Fire, true);
                const double chAfterTrig = g_charmSystem ? g_charmSystem->Slots()[0].charges : 0.0;
                const auto bon = g_charmSystem ? g_charmSystem->ComputeActiveBonuses() : Core::CharmActiveBonuses{};
                if (g_charmSystem) {
                    g_charmSystem->OnKillRecharge(Core::MonsterRarity::Rare, false, false);
                    SaveLoad::PersistCharmSlotsFromSystem();
                }
                const double chAfterKill = g_charmSystem ? g_charmSystem->Slots()[0].charges : 0.0;
                g_fExpectedCharmCharges = chAfterKill;
                const bool f4Ok = eqCh.ok && (chOnEquip >= 0.0 && chOnEquip < 0.01) && trigFire &&
                    (chBefore >= 39.9) && (std::abs(chAfterTrig - 20.0) < 0.1) &&
                    (bon.fireResist >= 24.9) && (bon.maxFireResist >= 1.9) &&
                    (std::abs(chAfterKill - 28.0) < 0.1);
                Check("F4_CharmsAutoTriggerAndKillRecharge", f4Ok,
                    std::format("eq={}, onEquip={:.0f}, trig={}, charges={:.0f}->{:.0f}->{:.0f}, fireRes=+{:.0f}/+{:.0f}",
                        eqCh.ok, chOnEquip, trigFire, chBefore, chAfterTrig, chAfterKill, bon.fireResist, bon.maxFireResist));
            }

            // ---- F5 준비: 실제 UiBridge JSON 경로로 Fireball + GMP/Pierce/Chain + MoltenShell 장착 & 발동 ----
            {
                equipGemF(6, "Fireball");
                socketGemF(6, -1, "SupportGreaterMultipleProjectiles");
                socketGemF(6, -1, "SupportPierce");
                socketGemF(6, -1, "SupportChain");
                equipGemF(7, "MoltenShell");

                const auto stF5 = SaveLoad::GetStateCopy();
                g_fUiEquipGemsOk =
                    stF5.slots[6].occupied && stF5.slots[6].gem.gemId == "Fireball" &&
                    stF5.slots[6].gem.socketedGems.size() == 3 &&
                    stF5.slots[7].occupied && stF5.slots[7].gem.gemId == "MoltenShell";

                SaveLoad::SyncStateToStatEngine();
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                ProxySpellRuntime::SyncProxySpellsFromState();

                auto* sp7 = FormIds::Lookup<RE::SpellItem>(FormIds::kProxySpells[6]);
                auto* pw8 = FormIds::Lookup<RE::SpellItem>(FormIds::kProxyPowers[7]);
                g_fProxySpellsSyncedOk = g_fUiEquipGemsOk && sp7 && pw8 && player->HasSpell(sp7) && player->HasSpell(pw8);

                RestorePlayerVitals(player, true, true);
                g_fProjLaunchedBefore = ProjectileRuntime::TotalLaunchedCount();
                g_fProjImpactsBefore = ProjectileRuntime::TotalImpactCount();

                // 검수 Major #1: 고체력(+2500) 표적 스키버를 소환해 화염구 5발에 즉사하지 않도록 보호
                g_fTargetHandle = SpawnNpcInFrontOfPlayer(player, kEncSkeeverId, 260.0f, 0.0f, 2500.0f, true);
                (void)ProxySpellRuntime::ExecuteHandCastSlot(6, true);

                const bool msActivated = ProxySpellRuntime::ExecuteGuardPowerSlot(7);
                const auto gsBefore = g_skillSystem ? g_skillSystem->GuardSnapshot() : Core::GuardShieldSnapshot{};
                const auto guardAbs = g_skillSystem
                    ? g_skillSystem->AbsorbWithGuardShield(20.0, 1.0, false)
                    : Core::GuardAbsorbResult{};
                g_fGuardAbsorbedOk = msActivated && gsBefore.active && gsBefore.maxShieldSkyrim > 0.0 &&
                    guardAbs.absorbedEffective > 0.0;
            }

            EnterPhase(Phase::ScenarioF_ProjectileWait);
            break;
        }

        case Phase::ScenarioF_ProjectileWait: {
            if (g_unpausedAccumSec < 0.6 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto launchedDelta = ProjectileRuntime::TotalLaunchedCount() - g_fProjLaunchedBefore;
            const auto impactsDelta = ProjectileRuntime::TotalImpactCount() - g_fProjImpactsBefore;
            Check("F5_ProxySpellHandCastAndMoltenShell",
                g_fProxySpellsSyncedOk && (launchedDelta >= 3) && g_fGuardAbsorbedOk,
                std::format("uiEquip={}, proxySynced={}, launched={}, impacts={}, guardAbsOk={}",
                    g_fUiEquipGemsOk, g_fProxySpellsSyncedOk, launchedDelta, impactsDelta, g_fGuardAbsorbedOk));

            if (player) {
                auto targetPtr = g_fTargetHandle.get();
                if (!targetPtr || targetPtr->IsDead()) {
                    g_fTargetHandle = SpawnNpcInFrontOfPlayer(player, kEncSkeeverId, 160.0f, 0.0f, 2500.0f, true);
                }
            }
            EnterPhase(Phase::ScenarioF_F6TargetWait);
            break;
        }

        case Phase::ScenarioF_F6TargetWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto targetPtr = g_fTargetHandle.get();
            if (targetPtr && !targetPtr->Is3DLoaded() && PhaseElapsedSec() < 2.0) {
                break;
            }

            // ---- F6: 5종 상태이상 + 매직/레어 승격 + 적 ES 100% 흡수 KillTracker 기여 + 헤드헌터 60초 훔치기 ----
            bool f6EsOk = false;
            bool f6AilOk = false;
            bool f6BossCapOk = false;
            bool f6HhOk = false;
            std::string f6AilDetail;
            if (player && targetPtr && !targetPtr->IsDead()) {
                if (auto* target = targetPtr.get()) {
                    const auto tid = target->GetFormID();
                    const std::uint16_t rareMask =
                        static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::EnergyShielded)) |
                        static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted)) |
                        static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::ExtraFire));
                    MonsterRuntime::ForcePromoteForTest(target, Core::MonsterRarity::Rare, rareMask, 80.0);

                    // 30 피해는 적 ES 80에 100% 흡수되며, 동시에 5종 상태이상(점화·냉각/빙결·감전·출혈·중독) 적용
                    const bool directHitOk = SkillExecution::DealDirectSkillDamage(
                        target, 30.0, Core::DamageType::Cold, true, false);
                    const auto profAfter = MonsterRuntime::GetProfileCopy(tid);
                    f6EsOk = directHitOk && profAfter.has_value() && profAfter->currentEsSkyrim < 79.0;

                    Core::DamagePacket ailPkt{};
                    ailPkt.byType[static_cast<std::size_t>(Core::DamageType::Fire)] = { 120.0, 120.0 };
                    ailPkt.byType[static_cast<std::size_t>(Core::DamageType::Cold)] = { 120.0, 120.0 };
                    ailPkt.byType[static_cast<std::size_t>(Core::DamageType::Lightning)] = { 120.0, 120.0 };
                    ailPkt.byType[static_cast<std::size_t>(Core::DamageType::Physical)] = { 100.0, 100.0 };
                    ailPkt.byType[static_cast<std::size_t>(Core::DamageType::Chaos)] = { 100.0, 100.0 };
                    (void)AilmentRuntime::ApplyPlayerHitAilments(
                        target, ailPkt, true, AilmentRuntime::HitKind::Attack, 100.0, false, 100.0, 100.0);

                    const double shockMult = AilmentRuntime::ShockDamageMultiplier(tid);
                    const auto stAil = AilmentRuntime::GetStateCopy(tid);
                    const bool hasIgnite = stAil && stAil->igniteRemaining > 0.0;
                    const bool hasChillOrFreeze = stAil && (stAil->chillRemaining > 0.0 || stAil->freezeRemaining > 0.0);
                    const bool hasShock = stAil && (stAil->shockRemaining > 0.0) && (shockMult > 1.0);
                    const bool hasBleed = stAil && stAil->bleedRemaining > 0.0;
                    const bool hasPoison = stAil && !stAil->poisonStacks.empty();
                    f6AilOk = hasIgnite && hasChillOrFreeze && hasShock && hasBleed && hasPoison;
                    f6AilDetail = std::format("ig={},cf={},sh={},bl={},po={}",
                        hasIgnite, hasChillOrFreeze, hasShock, hasBleed, hasPoison);

                    // 보스 빙결→30% 냉각 상한 코어 검증
                    Core::AilmentSystem bossAil;
                    Core::HitAilmentInput bossIn{};
                    bossIn.coldHitPoe = 5000.0;
                    bossIn.targetAilmentThresholdPoe = 1000.0;
                    bossIn.isCrit = true;
                    bossIn.isBossOrDragon = true;
                    (void)bossAil.ApplyHitAilments(999999u, bossIn);
                    f6BossCapOk = !bossAil.IsFrozen(999999u) && bossAil.ChillSlowRatio(999999u) <= 0.3001 &&
                        bossAil.ChillSlowRatio(999999u) > 0.0;

                    // 헤드헌터 훔치기 + 상태이상 원복 및 테스트 스키버 정리
                    // 검수 Critical #2: KillTracker 기여를 제거해 지연 TESDeathEvent가 참 충전을 변경하지 않도록 차단
                    const auto killedProf = MonsterRuntime::OnMonsterKilled(tid, true);
                    f6HhOk = (killedProf.rarity == Core::MonsterRarity::Rare) &&
                        (MonsterRuntime::ActiveHeadhunterStackCount() >= 1) &&
                        (MonsterRuntime::ActiveHeadhunterModMask() != 0);
                    if (g_killTracker) {
                        g_killTracker->OnActorReset(tid);
                    }
                    AilmentRuntime::OnActorKilled(tid, target, false);
                    (void)target->DoDamage(5000.0f, nullptr, true);
                }
            }
            Check("F6_AilmentsMonsterPromotionAndHeadhunter",
                f6EsOk && f6AilOk && f6BossCapOk && f6HhOk,
                std::format("enemyEs={}, ailments={}({}), bossChillCap={}, hhSteal={}(stacks={})",
                    f6EsOk, f6AilOk, f6AilDetail, f6BossCapOk, f6HhOk, MonsterRuntime::ActiveHeadhunterStackCount()));

            // ---- F7: 고유 토큰 감정 + [고유] 배지 + Divine 전용 가드 + 특수 효과 파싱 ----
            bool f7IdentOk = false;
            bool f7BadgeOk = false;
            bool f7ChaosBlockedOk = false;
            bool f7DivineOk = false;
            bool f7EffectsOk = false;
            g_fStarforgeKey = {};
            if (player) {
                (void)CraftingRuntime::GrantUniqueToken(player, 2);
                (void)CraftingRuntime::GrantCurrency(player, Core::CurrencyKind::ScrollOfWisdom, 2);
                (void)CraftingRuntime::GrantCurrency(player, Core::CurrencyKind::Chaos, 1);
                (void)CraftingRuntime::GrantCurrency(player, Core::CurrencyKind::Divine, 1);

                const auto sfRes = CraftingRuntime::IdentifyUniqueToken("Starforge");
                const auto shavRes = CraftingRuntime::IdentifyUniqueToken("ShavronnesWrappings");
                f7IdentOk = sfRes.ok && shavRes.ok &&
                    sfRes.resultRecord.rarity == Core::ItemRarity::Unique &&
                    shavRes.resultRecord.rarity == Core::ItemRarity::Unique;
                if (sfRes.ok) {
                    g_fStarforgeKey = sfRes.resultKey;
                    const Core::EquippedSlotInput dummy{};
                    const auto card = Core::ItemFormatter::FormatItem(
                        &sfRes.resultRecord, "Starforge", Core::EquipSlot::MainHand, dummy, true, *g_itemData);
                    f7BadgeOk = (card.rarityBadge == "[고유]");

                    const auto chaosBefore = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Chaos);
                    const auto chaosTry = CraftingRuntime::ApplyOrbToItem(
                        Core::CurrencyKind::Chaos, sfRes.resultBaseFormId, sfRes.resultKey.uniqueId, false);
                    const auto chaosAfter = CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Chaos);
                    f7ChaosBlockedOk = !chaosTry.ok && (chaosAfter == chaosBefore);

                    const auto divTry = CraftingRuntime::ApplyOrbToItem(
                        Core::CurrencyKind::Divine, sfRes.resultBaseFormId, sfRes.resultKey.uniqueId, false);
                    f7DivineOk = divTry.ok && (divTry.resultRecord.rarity == Core::ItemRarity::Unique);

                    const auto sfStats = Core::ItemAffixParser::ParseItem(divTry.ok ? divTry.resultRecord : sfRes.resultRecord, *g_itemData);
                    const auto shavStats = Core::ItemAffixParser::ParseItem(shavRes.resultRecord, *g_itemData);
                    f7EffectsOk = sfStats.starforgeShock && sfStats.dealNoElemental && shavStats.shavronneChaosEs;
                }
            }
            Check("F7_UniqueIdentifyDivineGuardAndEffects",
                f7IdentOk && f7BadgeOk && f7ChaosBlockedOk && f7DivineOk && f7EffectsOk,
                std::format("ident={}, badge={}, chaosBlocked={}, divine={}, effects={}",
                    f7IdentOk, f7BadgeOk, f7ChaosBlockedOk, f7DivineOk, f7EffectsOk));

            // ---- F8 준비: SPRT + GEAR + ITEM v2 코세이브 저장 및 재로드 왕복 ----
            SaveLoad::PersistCharmSlotsFromSystem();
            g_fExpectedCharmCharges = g_charmSystem ? g_charmSystem->Slots()[0].charges : 28.0;
            g_saveAttempts = 0;
            g_nextSaveRetryAt = Clock::now();
            {
                std::scoped_lock lock(g_msgMutex);
                g_expectedSaveStem = g_cfg.tempSave;
                g_saveMatched = false;
            }
            EnterPhase(Phase::ScenarioF_SavePump);
            break;
        }

        case Phase::ScenarioF_SavePump: {
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
                g_fExpectedCharmCharges = g_charmSystem ? g_charmSystem->Slots()[0].charges : g_fExpectedCharmCharges;
                LogInfo(std::format("Scenario F temp save '{}' confirmed by kSaveGame (attempts={}, charmCharges={:.1f}).",
                    g_cfg.tempSave, g_saveAttempts, g_fExpectedCharmCharges));
                EnterPhase(Phase::ScenarioF_SaveWaitBeforeLoad);
                break;
            }
            if (PhaseElapsedSec() > 25.0) {
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                Check("F8_Stage5CosaveRoundTripAndUiPayload", false, "Timed out waiting for Scenario F kSaveGame.");
                FinishAndScheduleQuit("Scenario F save timeout.");
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

        case Phase::ScenarioF_SaveWaitBeforeLoad: {
            if (PhaseElapsedSec() < 1.0) {
                break;
            }
            LogInfo(std::format("Reloading temp save '{}' to verify SPRT + GEAR + ITEM v2 co-save round-trip...", g_cfg.tempSave));
            if (!RequestLoadSave(g_cfg.tempSave)) {
                Check("F8_Stage5CosaveRoundTripAndUiPayload", false, "BGSSaveLoadManager singleton unavailable.");
                FinishAndScheduleQuit("Scenario F reload request failed.");
                break;
            }
            EnterPhase(Phase::ScenarioF_AwaitReload);
            break;
        }

        case Phase::ScenarioF_AwaitReload: {
            bool signaled = false;
            bool ok = false;
            {
                std::scoped_lock lock(g_msgMutex);
                signaled = g_postLoadSignaled;
                ok = g_postLoadSucceeded;
            }
            if (signaled && !ok) {
                Check("F8_Stage5CosaveRoundTripAndUiPayload", false, "kPostLoadGame reported failure on Scenario F reload.");
                FinishAndScheduleQuit("Scenario F reload failed.");
                return;
            }
            if (signaled && ok && IsPlayerReadyInWorld()) {
                EnterPhase(Phase::ScenarioF_ReloadSettle);
            } else if (PhaseElapsedSec() > 90.0) {
                Check("F8_Stage5CosaveRoundTripAndUiPayload", false, "Timed out waiting for Scenario F reload.");
                FinishAndScheduleQuit("Scenario F reload timeout.");
            }
            break;
        }

        case Phase::ScenarioF_ReloadSettle: {
            if (g_unpausedAccumSec < 2.0 && PhaseElapsedSec() < 8.0) {
                break;
            }
            const auto st = SaveLoad::GetStateCopy();
            const auto snap = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
            const auto sfRestored = g_fStarforgeKey.IsValid() ? ItemTracker::FindByKey(g_fStarforgeKey) : std::nullopt;

            const bool sprtOk = (st.activeAuraMask == g_fExpectedAuraMask) && (st.activeAuraMask != 0) &&
                snap && (snap->maxSpirit >= 190) && (snap->reservedSpirit == 175);
            const bool gearOk = (st.virtualRing2BaseFormId != 0) && (st.virtualBeltBaseFormId != 0) &&
                st.charmSlots[0].occupied && (st.charmSlots[0].kind == Core::CharmKind::Ruby) &&
                (g_fExpectedCharmCharges >= 27.5) &&
                (std::abs(st.charmSlots[0].charges - g_fExpectedCharmCharges) < 0.5) &&
                (g_fSocketNodeId != 0 && st.socketedJewels.contains(g_fSocketNodeId));
            const bool itemV2Ok = sfRestored.has_value() &&
                sfRestored->rarity == Core::ItemRarity::Unique &&
                sfRestored->uniqueId == "Starforge";

            bool uiPayloadOk = false;
            try {
                const auto pj = json::parse(UiBridge::BuildPanelJsonForTestOnMainThread());
                const bool statsOk = pj.contains("stats") && pj["stats"].value("spiritMax", 0) >= 190 &&
                    pj["stats"].value("spiritReserved", 0) == 175;
                const bool tenSlots = pj.contains("gear") && pj["gear"].contains("equippedSlots") &&
                    pj["gear"]["equippedSlots"].size() == Core::kEquipSlotCount;
                const bool charmsOk = pj.contains("gear") && pj["gear"].contains("charms") &&
                    pj["gear"]["charms"].value("unlockedSlots", 0) >= 2;
                const bool jewelsOk = pj.contains("tree") && pj["tree"].contains("socketedJewels") &&
                    (g_fSocketNodeId != 0 && !pj["tree"]["socketedJewels"].empty());
                uiPayloadOk = statsOk && tenSlots && charmsOk && jewelsOk;
            } catch (...) {
                uiPayloadOk = false;
            }

            Check("F8_Stage5CosaveRoundTripAndUiPayload",
                sprtOk && gearOk && itemV2Ok && uiPayloadOk,
                std::format("sprt={}, gear={}, itemV2={}, uiPayload={}, spirit={}/{}",
                    sprtOk, gearOk, itemV2Ok, uiPayloadOk,
                    snap ? snap->reservedSpirit : 0, snap ? snap->maxSpirit : 0));

            EnterPhase(Phase::ScenarioG_Start);
            break;
        }

        // ---- 시나리오 G: C-1 실게임 4시나리오 + U1 키 충돌 조회 + Q-1/Q-5/Q-6 실측 ----
        case Phase::ScenarioG_Start: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                FinishAndScheduleQuit("Scenario G aborted: null player.");
                break;
            }

            // G3: U1 ControlMap 바닐라 키 비충돌 검증 (0x26 = L vs QuickMagic 0x19 등)
            {
                std::uint32_t quickMagicKey = RE::ControlMap::kInvalid;
                std::uint32_t quickInvKey = RE::ControlMap::kInvalid;
                std::uint32_t quickMapKey = RE::ControlMap::kInvalid;
                std::uint32_t journalKey = RE::ControlMap::kInvalid;
                std::uint32_t treeKey = g_config ? g_config->treePanelKey : 0x26u;
                if (auto* cm = RE::ControlMap::GetSingleton()) {
                    if (auto* ue = RE::UserEvents::GetSingleton()) {
                        quickMagicKey = cm->GetMappedKey(ue->quickMagic, RE::INPUT_DEVICE::kKeyboard);
                        quickInvKey = cm->GetMappedKey(ue->quickInventory, RE::INPUT_DEVICE::kKeyboard);
                        quickMapKey = cm->GetMappedKey(ue->quickMap, RE::INPUT_DEVICE::kKeyboard);
                        journalKey = cm->GetMappedKey(ue->journal, RE::INPUT_DEVICE::kKeyboard);
                    }
                    if (quickMagicKey == RE::ControlMap::kInvalid) {
                        quickMagicKey = cm->GetMappedKey("Quick Magic", RE::INPUT_DEVICE::kKeyboard);
                    }
                }
                const bool collides =
                    (quickMagicKey != RE::ControlMap::kInvalid && quickMagicKey == treeKey) ||
                    (quickInvKey != RE::ControlMap::kInvalid && quickInvKey == treeKey) ||
                    (quickMapKey != RE::ControlMap::kInvalid && quickMapKey == treeKey) ||
                    (journalKey != RE::ControlMap::kInvalid && journalKey == treeKey);
                Check("G3_U1ControlMapKeyCollisionCheck", !collides && treeKey != 0x19u,
                    std::format("treePanelKey=0x{:02X}, quickMagicKey=0x{:02X}, collides={}",
                        treeKey, quickMagicKey, collides));
            }

            // G4: Q-5(바닥 드랍 PickUpObject 직후 플래그 실측) + Q-6(RemovePlainItem 추적품 보호 검증)
            {
                bool q5Deleted = false;
                bool q5Disabled = false;
                bool q5NoCell = false;
                if (auto* dagger = RE::TESForm::LookupByID<RE::TESObjectWEAP>(kIronDaggerId)) {
                    player->AddObjectToContainer(dagger, nullptr, 1, nullptr);
                    const auto dropH = player->DropObject(dagger, nullptr, 1);
                    if (auto dropPtr = dropH.get()) {
                        player->PickUpObject(dropPtr.get(), 1, false, false);
                        q5Deleted = dropPtr->IsDeleted();
                        q5Disabled = dropPtr->IsDisabled();
                        q5NoCell = (dropPtr->GetParentCell() == nullptr);
                    }
                }

                // Q-6: IronDagger 일반+추적 공존 상태에서 UniqueIdUtil::RemovePlainItem(1)이 추적품을 살리고 일반만 1개 제거하는지 검증
                bool q6TrackedCrafted = false;
                bool q6RemoveOk = false;
                bool q6TrackedSurvived = false;
                std::int32_t q6CountBefore = 0;
                std::int32_t q6CountAfter = 0;
                std::size_t q6ExtraBefore = 0;
                std::size_t q6ExtraAfter = 0;
                auto countDagger = [&](std::int32_t& outTotal, std::size_t& outExtraNodes) {
                    outTotal = 0;
                    outExtraNodes = 0;
                    if (auto* changes = player->GetInventoryChanges(true); changes && changes->entryList) {
                        for (auto* entry : *changes->entryList) {
                            if (entry && entry->object && entry->object->GetFormID() == kIronDaggerId) {
                                outTotal = entry->countDelta;
                                if (entry->extraLists) {
                                    for (auto* ex : *entry->extraLists) {
                                        if (ex) ++outExtraNodes;
                                    }
                                }
                                break;
                            }
                        }
                    }
                };
                if (auto* dagger = RE::TESForm::LookupByID<RE::TESObjectWEAP>(kIronDaggerId)) {
                    player->AddObjectToContainer(dagger, nullptr, 2, nullptr);
                    (void)CraftingRuntime::GrantCurrency(player, Core::CurrencyKind::Transmutation, 1);
                    const auto craftDag = CraftingRuntime::ApplyOrbToItem(Core::CurrencyKind::Transmutation, kIronDaggerId, 0, false);
                    if (craftDag.ok && craftDag.resultKey.IsValid()) {
                        q6TrackedCrafted = true;
                        countDagger(q6CountBefore, q6ExtraBefore);
                        q6RemoveOk = UniqueIdUtil::RemovePlainItem(player, dagger, 1);
                        countDagger(q6CountAfter, q6ExtraAfter);
                        q6TrackedSurvived =
                            (ItemTracker::FindExtraListByKey(player, dagger, craftDag.resultKey) != nullptr);
                    }
                }
                const bool g4Ok = q5NoCell && q6TrackedCrafted && q6RemoveOk && q6TrackedSurvived &&
                    (q6CountAfter == q6CountBefore - 1) && (q6ExtraAfter <= q6ExtraBefore);
                Check("G4_Q5PickupFlagsAndQ6NullRemoveProbe", g4Ok,
                    std::format("Q5(del={}, dis={}, noCell={}), Q6(crafted={}, rmOk={}, survived={}, cnt={}->{}, extras={}->{})",
                        q5Deleted, q5Disabled, q5NoCell, q6TrackedCrafted, q6RemoveOk, q6TrackedSurvived,
                        q6CountBefore, q6CountAfter, q6ExtraBefore, q6ExtraAfter));
            }

            // G1 시작: 슬롯 6(Fireball + 보조 젬 3개)을 해제해 인벤토리 데이터 젬 생성 후 바닥 드랍
            g_gGemInstanceId = 0;
            g_gGemSupportCount = 0;
            g_gGemDropHandle = {};
            g_gGemFirstDropRefId = 0;
            g_gGemFirstDropStamp = 0;
            g_gGemStamped1 = false;
            g_gGemRelinked1 = false;
            g_gGemReequipped1 = false;
            g_gGemRedropStamped = false;
            g_gGemRedropRelinked = false;
            g_gSaveBoundaryWasWorldBeforeSave = false;

            const auto stBefore = SaveLoad::GetStateCopy();
            if (stBefore.slots[6].occupied && stBefore.slots[6].gem.gemId == "Fireball") {
                g_gGemInstanceId = stBefore.slots[6].gem.instanceId;
                g_gGemSupportCount = stBefore.slots[6].gem.socketedGems.size();
                SendUiCmd(json{ { "op", "unequip" }, { "slot", 6 } });
                const auto inv = GemInventory::ListPlayerGems(player);
                auto* fbMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::FindLocalFormIdForGem("Fireball"));
                for (const auto& t : inv.trackedGems) {
                    if (t.gem.instanceId == g_gGemInstanceId && fbMisc) {
                        if (auto* extra = ItemTracker::FindExtraListByKey(player, fbMisc, t.key)) {
                            g_gGemDropHandle = player->DropObject(fbMisc, extra, 1);
                        }
                        break;
                    }
                }
            }
            EnterPhase(Phase::ScenarioG_GemDropWait);
            break;
        }

        case Phase::ScenarioG_GemDropWait: {
            if (g_unpausedAccumSec < 0.30 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto dropPtr = g_gGemDropHandle.get();
            if (player && dropPtr && g_gGemInstanceId != 0) {
                const auto* xUid = dropPtr->extraList.GetByType<RE::ExtraUniqueID>();
                const RE::FormID groundId = dropPtr->GetFormID();
                constexpr Core::WorldStampLink::StampBand kGemBand{
                    UniqueIdUtil::kGemWorldStampMin, UniqueIdUtil::kGemWorldStampMax };
                if (xUid && xUid->baseID == groundId && kGemBand.Contains(xUid->uniqueID)) {
                    for (const auto& e : GemInventory::TakeSnapshotForSave()) {
                        if (e.isWorld && e.gem.instanceId == g_gGemInstanceId &&
                            e.key.ownerFormId == groundId && e.key.uniqueId == xUid->uniqueID) {
                            g_gGemStamped1 = true;
                            break;
                        }
                    }
                }
                player->PickUpObject(dropPtr.get(), 1, false, false);
            }
            EnterPhase(Phase::ScenarioG_GemPickupWait);
            break;
        }

        case Phase::ScenarioG_GemPickupWait: {
            if (g_unpausedAccumSec < 0.45 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && g_gGemInstanceId != 0) {
                const auto inv = GemInventory::ListPlayerGems(player);
                for (const auto& t : inv.trackedGems) {
                    if (t.gem.instanceId == g_gGemInstanceId && !t.isWorld &&
                        t.gem.socketedGems.size() == g_gGemSupportCount) {
                        g_gGemRelinked1 = true;
                        SendUiCmd(json{
                            { "op", "equip" },
                            { "slot", 6 },
                            { "gemId", "Fireball" },
                            { "ownerFormId", t.key.ownerFormId },
                            { "uniqueId", t.key.uniqueId }
                        });
                        break;
                    }
                }
                const auto stAfter = SaveLoad::GetStateCopy();
                g_gGemReequipped1 = stAfter.slots[6].occupied &&
                    stAfter.slots[6].gem.instanceId == g_gGemInstanceId &&
                    stAfter.slots[6].gem.socketedGems.size() == g_gGemSupportCount;

                // 재드랍 시험(C-1 시나리오 #4): 다시 해제 -> 1차 바닥 드랍
                g_gGemDropHandle = {};
                if (g_gGemReequipped1) {
                    SendUiCmd(json{ { "op", "unequip" }, { "slot", 6 } });
                    const auto inv2 = GemInventory::ListPlayerGems(player);
                    auto* fbMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::FindLocalFormIdForGem("Fireball"));
                    for (const auto& t : inv2.trackedGems) {
                        if (t.gem.instanceId == g_gGemInstanceId && fbMisc) {
                            if (auto* extra = ItemTracker::FindExtraListByKey(player, fbMisc, t.key)) {
                                g_gGemDropHandle = player->DropObject(fbMisc, extra, 1);
                            }
                            break;
                        }
                    }
                }
            }
            EnterPhase(Phase::ScenarioG_GemRedropSetupWait);
            break;
        }

        case Phase::ScenarioG_GemRedropSetupWait: {
            if (g_unpausedAccumSec < 0.30 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto dropPtr1 = g_gGemDropHandle.get();
            g_gGemDropHandle = {};
            if (player && dropPtr1 && g_gGemInstanceId != 0) {
                const RE::FormID groundId1 = dropPtr1->GetFormID();
                g_gGemFirstDropRefId = groundId1;
                const auto* xUid1 = dropPtr1->extraList.GetByType<RE::ExtraUniqueID>();
                const std::uint16_t stamp1 = xUid1 ? xUid1->uniqueID : 0;
                g_gGemFirstDropStamp = stamp1;
                // 주운 직후(재연결 람다가 돌기 전 같은 프레임에서!) 바로 인벤토리에서 (groundId1, stamp1) 묶음을 찾아 재드랍
                player->PickUpObject(dropPtr1.get(), 1, false, false);
                auto* fbMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::FindLocalFormIdForGem("Fireball"));
                constexpr Core::WorldStampLink::StampBand kGemBand{
                    UniqueIdUtil::kGemWorldStampMin, UniqueIdUtil::kGemWorldStampMax };
                if (fbMisc && kGemBand.Contains(stamp1)) {
                    if (auto* pickedExtra = ItemTracker::FindExtraListByKey(
                            player, fbMisc, Core::InventoryItemKey{ groundId1, stamp1 })) {
                        g_gGemDropHandle = player->DropObject(fbMisc, pickedExtra, 1);
                    }
                }
            }
            EnterPhase(Phase::ScenarioG_GemRedropWait);
            break;
        }

        case Phase::ScenarioG_GemRedropWait: {
            if (g_unpausedAccumSec < 0.30 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto dropPtr2 = g_gGemDropHandle.get();
            if (player && dropPtr2 && g_gGemInstanceId != 0) {
                const RE::FormID groundId2 = dropPtr2->GetFormID();
                const auto* xUid2 = dropPtr2->extraList.GetByType<RE::ExtraUniqueID>();
                constexpr Core::WorldStampLink::StampBand kGemBand{
                    UniqueIdUtil::kGemWorldStampMin, UniqueIdUtil::kGemWorldStampMax };
                // 같은 프레임 재드랍 시 엔진이 삭제된 동적 FF FormID를 재사용할 수 있으므로 (FormID 변경 또는 새 스탬프 부여)로 판정
                if (xUid2 && xUid2->baseID == groundId2 && kGemBand.Contains(xUid2->uniqueID) &&
                    (groundId2 != g_gGemFirstDropRefId || xUid2->uniqueID != g_gGemFirstDropStamp)) {
                    for (const auto& e : GemInventory::TakeSnapshotForSave()) {
                        if (e.isWorld && e.gem.instanceId == g_gGemInstanceId &&
                            e.key.ownerFormId == groundId2 && e.key.uniqueId == xUid2->uniqueID) {
                            g_gGemRedropStamped = true;
                            break;
                        }
                    }
                }
                player->PickUpObject(dropPtr2.get(), 1, false, false);
            }
            EnterPhase(Phase::ScenarioG_GemRedropPickupWait);
            break;
        }

        case Phase::ScenarioG_GemRedropPickupWait: {
            if (g_unpausedAccumSec < 0.45 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && g_gGemInstanceId != 0) {
                const auto inv = GemInventory::ListPlayerGems(player);
                for (const auto& t : inv.trackedGems) {
                    if (t.gem.instanceId == g_gGemInstanceId && !t.isWorld &&
                        t.gem.socketedGems.size() == g_gGemSupportCount) {
                        g_gGemRedropRelinked = true;
                        break;
                    }
                }
            }
            Check("G1_C1GemDropPickupReequipAndRedrop",
                g_gGemStamped1 && g_gGemRelinked1 && g_gGemReequipped1 && g_gGemRedropStamped && g_gGemRedropRelinked,
                std::format("stamp1={}, relink1={}, reequip1={}, redropStamp={}, redropRelink={}, supports={}",
                    g_gGemStamped1, g_gGemRelinked1, g_gGemReequipped1, g_gGemRedropStamped, g_gGemRedropRelinked, g_gGemSupportCount));

            // G2(저장 경계 보완 재연결) + G5(Q-1 감속 세이브 잔존 실측) 동시 준비
            g_gGemDropHandle = {};
            g_gQ1ActorFormId = 0;
            g_gQ1SpeedBeforeSave = 0.0f;
            if (player && g_gGemInstanceId != 0) {
                const auto inv = GemInventory::ListPlayerGems(player);
                auto* fbMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::FindLocalFormIdForGem("Fireball"));
                for (const auto& t : inv.trackedGems) {
                    if (t.gem.instanceId == g_gGemInstanceId && fbMisc) {
                        if (auto* extra = ItemTracker::FindExtraListByKey(player, fbMisc, t.key)) {
                            g_gGemDropHandle = player->DropObject(fbMisc, extra, 1);
                        }
                        break;
                    }
                }

                // Q-1 실측용 스키버 소환 + 냉각(Chill) 감속 적용
                const auto mobHandle = SpawnNpcInFrontOfPlayer(player, kEncSkeeverId, 160.0f, 0.0f, 500.0f, false);
                if (auto mobPtr = mobHandle.get()) {
                    if (auto* mob = mobPtr.get()) {
                        mob->EnableAI(false);
                        Core::DamagePacket coldPkt{};
                        coldPkt.byType[static_cast<std::size_t>(Core::DamageType::Cold)] = { 5000.0, 5000.0 };
                        (void)AilmentRuntime::ApplyPlayerHitAilments(
                            mob, coldPkt, true, AilmentRuntime::HitKind::Spell, 0.0, false);
                        g_gQ1ActorFormId = mob->GetFormID();
                        g_gQ1SpeedBeforeSave = mob->GetActorValueModifier(
                            RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult);
                    }
                }
            }
            EnterPhase(Phase::ScenarioG_SaveBoundaryDropWait);
            break;
        }

        case Phase::ScenarioG_SaveBoundaryDropWait: {
            if (g_unpausedAccumSec < 0.30 && PhaseElapsedSec() < 3.0) {
                break;
            }
            auto* pl = RE::PlayerCharacter::GetSingleton();
            auto dropPtr3 = g_gGemDropHandle.get();
            {
                std::scoped_lock lock(g_msgMutex);
                g_expectedSaveStem = g_cfg.tempSave;
                g_saveMatched = false;
            }
            if (pl && dropPtr3 && g_gGemInstanceId != 0) {
                // 주운 직후 같은 프레임에서 즉시 저장을 요청해(재연결 람다 미실행 상태) 저장 경계 보완 경로를 검증!
                pl->PickUpObject(dropPtr3.get(), 1, false, false);
                for (const auto& e : GemInventory::TakeSnapshotForSave()) {
                    if (e.gem.instanceId == g_gGemInstanceId && e.isWorld) {
                        g_gSaveBoundaryWasWorldBeforeSave = true;
                        break;
                    }
                }
                if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
                    mgr->Save(g_cfg.tempSave.c_str());
                }
            }
            g_saveAttempts = 1;
            g_nextSaveRetryAt = Clock::now() + std::chrono::seconds(2);
            EnterPhase(Phase::ScenarioG_SavePump);
            break;
        }

        case Phase::ScenarioG_SavePump: {
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
                EnterPhase(Phase::ScenarioG_SaveWaitBeforeLoad);
                break;
            }
            if (Clock::now() >= g_nextSaveRetryAt) {
                ++g_saveAttempts;
                LogInfo(std::format("Requesting Scenario G save '{}' (attempt #{})...", g_cfg.tempSave, g_saveAttempts));
                if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
                    mgr->Save(g_cfg.tempSave.c_str());
                } else {
                    Check("G2_C1SaveBoundaryFallbackRelink", false, "BGSSaveLoadManager singleton unavailable.");
                    Check("G5_Q1SaveTempSpeedMultProbe", false, "Save failed.");
                    FinishAndScheduleQuit("Scenario G save request failed.");
                    break;
                }
                g_nextSaveRetryAt = Clock::now() + std::chrono::seconds(2);
            }
            if (PhaseElapsedSec() > 25.0) {
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                Check("G2_C1SaveBoundaryFallbackRelink", false, "Timed out waiting for Scenario G kSaveGame.");
                Check("G5_Q1SaveTempSpeedMultProbe", false, "Save timeout.");
                FinishAndScheduleQuit("Scenario G save timeout.");
            }
            break;
        }

        case Phase::ScenarioG_SaveWaitBeforeLoad: {
            if (PhaseElapsedSec() < 1.0) {
                break;
            }
            if (!RequestLoadSave(g_cfg.tempSave)) {
                Check("G2_C1SaveBoundaryFallbackRelink", false, "Scenario G reload request failed.");
                Check("G5_Q1SaveTempSpeedMultProbe", false, "Reload failed.");
                FinishAndScheduleQuit("Scenario G reload request failed.");
                break;
            }
            EnterPhase(Phase::ScenarioG_AwaitReload);
            break;
        }

        case Phase::ScenarioG_AwaitReload: {
            bool signaled = false;
            bool ok = false;
            {
                std::scoped_lock lock(g_msgMutex);
                signaled = g_postLoadSignaled;
                ok = g_postLoadSucceeded;
            }
            if (signaled && !ok) {
                Check("G2_C1SaveBoundaryFallbackRelink", false, "kPostLoadGame failed on Scenario G reload.");
                Check("G5_Q1SaveTempSpeedMultProbe", false, "Reload failed.");
                FinishAndScheduleQuit("Scenario G reload failed.");
                return;
            }
            if (signaled && ok && IsPlayerReadyInWorld()) {
                EnterPhase(Phase::ScenarioG_ReloadSettle);
            } else if (PhaseElapsedSec() > 90.0) {
                Check("G2_C1SaveBoundaryFallbackRelink", false, "Timed out waiting for Scenario G reload.");
                Check("G5_Q1SaveTempSpeedMultProbe", false, "Reload timeout.");
                FinishAndScheduleQuit("Scenario G reload timeout.");
            }
            break;
        }

        case Phase::ScenarioG_ReloadSettle: {
            if (g_unpausedAccumSec < 2.0 && PhaseElapsedSec() < 8.0) {
                break;
            }
            auto* pl = RE::PlayerCharacter::GetSingleton();
            bool g2Restored = false;
            if (pl && g_gGemInstanceId != 0) {
                const auto inv = GemInventory::ListPlayerGems(pl);
                for (const auto& t : inv.trackedGems) {
                    if (t.gem.instanceId == g_gGemInstanceId && !t.isWorld &&
                        t.gem.socketedGems.size() == g_gGemSupportCount) {
                        g2Restored = true;
                        break;
                    }
                }
                if (!g2Restored) {
                    // G2 진단: 재로드 직후 Fireball 인벤토리 ExtraUniqueID와 GemInventory 스냅샷 덤프
                    std::string diag = "invExtras=[";
                    auto* fbMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::FindLocalFormIdForGem("Fireball"));
                    if (fbMisc) {
                        if (auto* changes = pl->GetInventoryChanges(true); changes && changes->entryList) {
                            for (auto* entry : *changes->entryList) {
                                if (!entry || entry->object != fbMisc) continue;
                                diag += std::format("cntDelta={}", entry->countDelta);
                                if (entry->extraLists) {
                                    for (auto* ex : *entry->extraLists) {
                                        if (!ex) continue;
                                        const auto* u = ex->GetByType<RE::ExtraUniqueID>();
                                        diag += std::format("({:08X}:{:04X},cnt={})",
                                            u ? u->baseID : 0u, u ? u->uniqueID : 0u, ex->GetCount());
                                    }
                                }
                            }
                        }
                    }
                    diag += "] ledger=[";
                    for (const auto& e : GemInventory::TakeSnapshotForSave()) {
                        diag += std::format("(inst={},key={:08X}:{:04X},w={},id={})",
                            e.gem.instanceId, e.key.ownerFormId, e.key.uniqueId, e.isWorld, e.gem.gemId);
                    }
                    diag += "]";
                    LogInfo(std::format("G2 diag: {}", diag));
                }
            }
            Check("G2_C1SaveBoundaryFallbackRelink", g_gSaveBoundaryWasWorldBeforeSave && g2Restored,
                std::format("wasWorldBeforeSave={}, restoredAfterReload={}, supports={}",
                    g_gSaveBoundaryWasWorldBeforeSave, g2Restored, g_gGemSupportCount));

            // G5: Q-1 세이브->재로드 후 SPDM 코세이브 기반 임시 SpeedMult 원복 검증
            bool q1FoundActor = false;
            float q1SpeedAfter = 0.0f;
            if (g_gQ1ActorFormId != 0) {
                if (auto* mob = RE::TESForm::LookupByID<RE::Actor>(g_gQ1ActorFormId)) {
                    q1FoundActor = true;
                    q1SpeedAfter = mob->GetActorValueModifier(
                        RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult);
                }
            }
            Check("G5_Q1SaveTempSpeedMultProbe",
                q1FoundActor && (g_gQ1SpeedBeforeSave < -5.0f) && (std::abs(q1SpeedAfter) < 0.5f),
                std::format("actor=0x{:08X}, found={}, tempSpeedMultBefore={:.1f}, tempSpeedMultAfterReload={:.1f}",
                    g_gQ1ActorFormId, q1FoundActor, g_gQ1SpeedBeforeSave, q1SpeedAfter));

            EnterPhase(Phase::ScenarioH_Start);
            break;
        }

        // ================= SCENARIO H (6단계: 지도 장치·아틀라스·4차 결투장·안전 제거) =================
        case Phase::ScenarioH_Start: {
            if (!g_hContract.checkpointSaved) {
                EnterPhase(Phase::ScenarioH_Contract);
                EnterHContract(HContractStage::PrepareCheckpoint);
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !g_config || !g_itemData || !g_statEngine || !g_treeData) {
                Check("H1_Stage6Esp192FormsAndMapKey", false, "Core singletons unavailable.");
                FinishAndScheduleQuit("Scenario H aborted.");
                break;
            }
            g_hDeathInterceptBefore = std::atomic_ref<bool>(g_config->mapDeathInterceptEnabled)
                .exchange(true, std::memory_order_relaxed);
            RestorePlayerVitals(player, true, true);
            if (player->IsInCombat()) {
                player->StopCombat();
            }
            g_hDeathEscapeIntercepted = false;
            g_hDeathInterceptHitsSent = 0;

            // H1: 6단계 신규 18폼(0x8AE..0x8BF, 총 192 ESL) + MapPanelKey(0x16 U) ControlMap 비충돌 + maps UI JSON
            std::size_t s6Found = 0;
            for (const auto fid : FormIds::kMapItemForms) {
                if (FormIds::Lookup<RE::TESObjectMISC>(fid) != nullptr) ++s6Found;
            }
            if (FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute) != nullptr) ++s6Found;
            if (FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamMap) != nullptr) ++s6Found;

            const std::uint32_t mapKey = g_config->mapPanelKey;
            bool mapKeyCollides = false;
            if (auto* cm = RE::ControlMap::GetSingleton()) {
                constexpr std::array<std::string_view, 6> kVanillaKeys = {
                    "Quick Magic", "Quick Inventory", "Quick Map", "Journal", "Tween Menu", "Wait"
                };
                for (const auto kname : kVanillaKeys) {
                    const auto vk = cm->GetMappedKey(kname, RE::INPUT_DEVICE::kKeyboard);
                    if (vk != RE::ControlMap::kInvalid && vk == mapKey) {
                        mapKeyCollides = true;
                        break;
                    }
                }
            }
            bool mapsJsonOk = false;
            try {
                const auto panelJson = json::parse(UiBridge::BuildPanelJsonForTestOnMainThread());
                mapsJsonOk = panelJson.contains("maps") && panelJson["maps"].is_object() &&
                             panelJson["maps"].contains("atlas") && panelJson["maps"].contains("active") &&
                             panelJson["maps"].contains("bagMaps");
            } catch (...) {
            }
            Check("H1_Stage6Esp192FormsAndMapKey",
                (s6Found == 18u) && (mapKey == 0x16u) && !mapKeyCollides && mapsJsonOk,
                std::format("s6Forms={}/18, mapKey=0x{:02X}, collides={}, mapsJson={}",
                    s6Found, mapKey, mapKeyCollides, mapsJsonOk));

            // H2: T10 지도(0x0008B7) 지급 + 연금술 오브 제작 -> Rare, iLvl=77, MapMod_* 격리 확인
            auto* t10Misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::MapItemLocalFormId(kHMapTier));
            auto* alchOrb = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CurrencyLocalFormId(Core::CurrencyKind::Alchemy));
            bool h2Ok = false;
            std::string h2Detail = "missing forms";
            const RE::FormID craftedMapBaseId = t10Misc ? t10Misc->GetFormID() : 0u;
            if (t10Misc && alchOrb) {
                player->AddObjectToContainer(t10Misc, nullptr, 1, nullptr);
                player->AddObjectToContainer(alchOrb, nullptr, 1, nullptr);
                const auto craftRes = CraftingRuntime::ApplyOrbToItem(
                    Core::CurrencyKind::Alchemy, craftedMapBaseId, 0, false);
                h2Detail = craftRes.message;
                if (craftRes.ok && craftRes.resultKey.IsValid()) {
                    const auto maps = MapDeviceRuntime::ListPlayerMaps(player);
                    const auto map = std::ranges::find_if(maps, [&](const auto& candidate) {
                        return candidate.tracked && candidate.key == craftRes.resultKey;
                    });
                    const auto record = ItemTracker::FindByKey(craftRes.resultKey);
                    if (map != maps.end() && record) {
                        constexpr std::size_t kCraftedRareMinMods = 4;
                        const bool allMapMods = std::ranges::all_of(record->affixes, [](const auto& affix) {
                            return affix.affixId.starts_with("MapMod_");
                        });
                        h2Ok = map->tier == kHMapTier && map->rarity == Core::ItemRarity::Rare &&
                            record->itemLevel == kHMapRewardLevel && allMapMods &&
                            record->affixes.size() >= kCraftedRareMinMods && map->summary.bonusIiqPct > 0.0;
                        h2Detail = std::format("key={:08X}:{:04X}, iLvl={}, mods={}, allMapMods={}, iiq={:.0f}",
                            craftRes.resultKey.ownerFormId, craftRes.resultKey.uniqueId, record->itemLevel,
                            record->affixes.size(), allMapMods, map->summary.bonusIiqPct);
                    }
                }
            }
            Check("H2_MapCraftingAndItemLevel", h2Ok, h2Detail);

            g_hOutdoorMap = false;
            g_hIndoorReturnOk = false;
            g_hIndoorReturnDetail = "Indoor death return did not run.";
            g_hDungeonIndex = 0;
            g_hDungeonsOk = true;
            g_hDungeonsDetail.clear();
            g_hArenaSpawnsOk.reset();
            g_hArenaSpawnsDetail = "No arena spawn observation.";
            g_hArenaGateIndex = 0;
            g_hArenaGateOk = true;
            g_hArenaGateDetail.clear();
            g_hPlainAffixProbe.reset();
            g_hEarnedTributeAvailable = false;
            if (MoveToHOrigin(player, kHIndoorStartMarker)) {
                EnterPhase(Phase::ScenarioH_IndoorStartWait);
            } else {
                g_hIndoorReturnDetail = "Indoor start marker unavailable.";
                EnterPhase(Phase::ScenarioH_DungeonStart);
            }
            break;
        }

        case Phase::ScenarioH_IndoorStartWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (AtHOrigin(player, kHIndoorStartMarker, false) || PhaseElapsedSec() >= kHOriginWaitSec) {
                EnterPhase(Phase::ScenarioH_DungeonStart);
            }
            break;
        }

        case Phase::ScenarioH_DungeonStart: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto& pack = MapPackRuntime::Manifest();
            if (g_hDungeonIndex == pack.maps.size()) {
                g_hDungeonResetExpected = false;
                g_hDungeonCloseRetried = false;
                const auto* forms = MapPackRuntime::FindForms(pack.ForTier(kHMapTier));
                g_hMapCellId = forms ? forms->cellId : 0;
                const auto reset = MapDeviceRuntime::GetCellResetStatus(g_hMapCellId);
                if (reset.pending && PhaseElapsedSec() < kMapResetWaitSec) {
                    break;
                }
                g_hResetCompletedBeforeMap = reset.completed;
                const auto opened = OpenHMap(player, kHMapTier, true);
                if (opened.ok) {
                    EnterPhase(Phase::ScenarioH_MapEntryWait);
                } else {
                    Check(kHAffixCheck, false, opened.message);
                    Check("H10_MapFallRecovery", false, "The fixed-affix map did not open.");
                    g_hIndoorReturnDetail = opened.message;
                    EnterPhase(Phase::ScenarioH_DungeonCloseWait);
                }
                break;
            }
            const auto* dungeon = &pack.maps[g_hDungeonIndex];
            const auto* forms = MapPackRuntime::FindForms(dungeon);
            g_hDungeonOk = false;
            g_hDungeonMeasured = false;
            g_hDungeonCloseRetried = false;
            g_hDungeonResetExpected = false;
            g_hMapCellId = forms ? forms->cellId : 0;
            const auto reset = MapDeviceRuntime::GetCellResetStatus(g_hMapCellId);
            if (reset.pending && PhaseElapsedSec() < kMapResetWaitSec) {
                break;
            }
            g_hResetCompletedBeforeMap = reset.completed;
            g_hDungeonTier = 0;
            // 같은 던전의 무어픽스 T10을 H2 대조군으로도 쓴다.
            if (pack.ForTier(kHMapTier) == dungeon) {
                g_hDungeonTier = kHMapTier;
            } else {
                for (auto tier = Core::kMinMapTier; tier <= Core::kMaxMapTier; ++tier) {
                    if (pack.ForTier(tier) == dungeon) {
                        g_hDungeonTier = tier;
                        break;
                    }
                }
            }
            const auto opened = g_hDungeonTier != 0
                ? OpenHMap(player, g_hDungeonTier, false)
                : MapActionResult{ false, "No selectable tier for the manifest dungeon." };
            g_hDungeonDetail = opened.message;
            EnterPhase(opened.ok ? Phase::ScenarioH_DungeonEntryWait : Phase::ScenarioH_DungeonCloseWait);
            break;
        }

        case Phase::ScenarioH_DungeonEntryWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                RestorePlayerVitals(player, true, true);
            }
            const auto run = MapDeviceRuntime::GetActiveMapStateCopy();
            const auto& pack = MapPackRuntime::Manifest();
            const auto* dungeon = &pack.maps[g_hDungeonIndex];
            const auto* forms = MapPackRuntime::FindForms(dungeon);
            const bool entered = player && forms && player->GetParentCell() == forms->cell &&
                pack.ForRun(run) == dungeon && MapDeviceRuntime::IsPlayerInActiveMapCell();
            const bool spawned = run.bossRefFormId != 0;
            if (WaitForMapTravel(player, entered && spawned, kMapEntryWaitSec)) {
                break;
            }
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool ready = entered && spawned && !travel.kind && IsPlayerReadyInWorld();
            if (!g_hDungeonMeasured) {
                g_hDungeonMeasured = true;
                g_hDungeonResetExpected = run.active;
                const auto layout = ObserveHSpawnLayout(ready ? forms : nullptr, run);
                const auto expectedPack = Core::MapSystem::MapPackActorCount(g_config->mapBasePackCount, 0.0);
                bool scalingOk = g_dataPack && g_dataPack->IsLoaded();
                const auto rewardLevel = Core::MapSystem::MonsterLevelForTier(g_hDungeonTier);
                const auto expectedScaling = scalingOk
                    ? Core::MapSystem::GetMapMonsterMultipliers(rewardLevel, *g_dataPack) : Core::MapMonsterMultipliers{};
                for (const auto id : layout.ids) {
                    const auto scaling = MapDeviceRuntime::GetMapActorScaling(id);
                    scalingOk = scalingOk && scaling.runId == run.runId &&
                        std::abs(scaling.lifeMultiplier - expectedScaling.lifeMultiplier) < kHDamageTolerance &&
                        std::abs(scaling.damageMultiplier - expectedScaling.damageMultiplier) < kHDamageTolerance;
                }
                if (g_dataPack && g_dataPack->IsLoaded()) {
                    (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                }
                const auto snap = g_statEngine->GetSnapshot();
                const bool playerStatsOk = snap && snap->inActiveMap &&
                    std::abs(snap->poeMaxLife - g_hPreMapPoeLife) < kHPlayerLifeTolerance;
                const auto reward = MapDeviceRuntime::GetMapKillContext(run.bossRefFormId, forms ? forms->cellId : 0);
                const bool rewardOk = reward.isBoss && reward.runId == run.runId && !reward.rewardBlocked &&
                    reward.rewardMonsterLevel == rewardLevel && reward.dropMapTier == g_hDungeonTier;
                auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(g_hEntryItemId);
                const bool consumed = player && item && player->GetItemCount(item) == g_hEntryItemCount - 1;
                g_hDungeonOk = ready && layout.Meets(expectedPack) && scalingOk && playerStatsOk && rewardOk &&
                    run.mods.empty() && run.portalsRemaining == kHEntryPortals && g_hEntryPendingWithoutConsumption && consumed;
                g_hDungeonDetail = std::format(
                    "T{} ready={}, bosses={}, pack={}/{}, invalid={}, minDist={:.1f}, registry={}, scaling={}, reward={}, life={}, portals={}, pendingUnchanged={}, consumed={}",
                    g_hDungeonTier, ready, layout.bosses, layout.adds, expectedPack, layout.invalid, layout.nearest,
                    layout.matchesRegistry, scalingOk, rewardOk, playerStatsOk, run.portalsRemaining,
                    g_hEntryPendingWithoutConsumption, consumed);
            }
            if (ready && run.mods.empty() && g_hDungeonTier == kHMapTier) {
                auto* boss = RE::TESForm::LookupByID<RE::Actor>(run.bossRefFormId);
                const auto prepared = PrepareHBoss(player, boss);
                if (prepared == HBossReadiness::Waiting) {
                    break;
                }
                if (prepared == HBossReadiness::Ready) {
                    g_hPlainAffixProbe = ProbeHMapAffixes(boss, run, forms->cellId);
                }
            }
            CloseHRun(player);
            EnterPhase(Phase::ScenarioH_DungeonCloseWait);
            break;
        }

        case Phase::ScenarioH_DungeonCloseWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool closed = !travel.kind && !travel.inPackCell && !MapDeviceRuntime::IsActiveMapRun() &&
                IsPlayerReadyInWorld();
            const auto reset = MapDeviceRuntime::GetCellResetStatus(g_hMapCellId);
            const bool resetDone = !reset.pending &&
                (!g_hDungeonResetExpected || reset.completed > g_hResetCompletedBeforeMap);
            if (!closed && PhaseElapsedSec() < HContractTravelSec()) {
                CloseHRun(player);
                break;
            }
            if (closed && !resetDone) {
                const auto now = Clock::now();
                if (!g_hDungeonClosedAt) {
                    g_hDungeonClosedAt = now;
                }
                if (std::chrono::duration<double>(now - *g_hDungeonClosedAt).count() < kMapResetWaitSec) {
                    break;
                }
            }
            const auto& pack = MapPackRuntime::Manifest();
            if (!closed || !resetDone) {
                if (!g_hDungeonCloseRetried) {
                    g_hDungeonCloseRetried = true;
                    CloseHRun(player);
                    EnterPhase(Phase::ScenarioH_DungeonCloseWait);
                    break;
                }
                g_hDungeonsOk = false;
                const bool sweeping = g_hDungeonIndex < pack.maps.size();
                const auto key = sweeping ? pack.maps[g_hDungeonIndex].key : "fixed-affix map";
                g_hDungeonsDetail += std::format("{} [{}; closed={}, reset={}]; ",
                    key, g_hDungeonDetail, closed, resetDone);
                if (sweeping) {
                    ++g_hDungeonIndex;
                } else {
                    g_hOutdoorMap = true;
                }
                if (RequestLoadSave(g_hContract.checkpointStem)) {
                    LogInfo("Restoring the verified H checkpoint before continuing the dungeon sweep.");
                    EnterPhase(Phase::ScenarioH_DungeonRecoveryWait);
                } else {
                    FinishAndScheduleQuit("Scenario H could not request a checkpoint load after an unsettled dungeon.");
                }
                break;
            }
            if (g_hDungeonIndex < pack.maps.size()) {
                g_hDungeonsOk = g_hDungeonsOk && g_hDungeonOk;
                g_hDungeonsDetail += std::format("{} [{}; closed={}, reset={}]; ",
                    pack.maps[g_hDungeonIndex].key, g_hDungeonDetail, closed, resetDone);
                ++g_hDungeonIndex;
                EnterPhase(Phase::ScenarioH_DungeonStart);
                break;
            }
            // 실내 회차 종료 뒤 새 야외 회차를 열어 귀환 목표를 다시 기록한다.
            g_hOutdoorMap = true;
            if (!closed || !MoveToHOrigin(player, kHOutdoorStartMarker)) {
                Check(kHOutdoorReturnCheck, false, std::format("Indoor: {}. Could not set up outdoor start.", g_hIndoorReturnDetail));
                EnterPhase(Phase::ScenarioH_PostMapAbandonWait);
                break;
            }
            EnterPhase(Phase::ScenarioH_OutdoorStartWait);
            break;
        }

        case Phase::ScenarioH_DungeonRecoveryWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            bool loaded = false;
            bool failed = false;
            {
                std::scoped_lock lock(g_msgMutex);
                loaded = g_postLoadSignaled && g_postLoadSucceeded;
                failed = g_postLoadSignaled && !g_postLoadSucceeded;
            }
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            if (loaded && IsPlayerReadyInWorld() && !travel.kind && !travel.inPackCell &&
                !MapDeviceRuntime::IsActiveMapRun()) {
                const auto marker = g_hOutdoorMap ? kHOutdoorStartMarker : kHIndoorStartMarker;
                if (MoveToHOrigin(player, marker)) {
                    EnterPhase(g_hOutdoorMap ? Phase::ScenarioH_OutdoorStartWait : Phase::ScenarioH_IndoorStartWait);
                } else {
                    FinishAndScheduleQuit("Scenario H could not move to its next origin after dungeon recovery.");
                }
            } else if (failed || PhaseElapsedSec() >= kHReloadWaitSec) {
                FinishAndScheduleQuit("Scenario H could not verify recovery to an outside, inactive dungeon state.");
            }
            break;
        }

        case Phase::ScenarioH_OutdoorStartWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const bool atOrigin = AtHOrigin(player, kHOutdoorStartMarker, true);
            if (!atOrigin && PhaseElapsedSec() < kHOriginWaitSec) {
                break;
            }
            const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForTier(kHMapTier));
            g_hMapCellId = forms ? forms->cellId : 0;
            const auto reset = MapDeviceRuntime::GetCellResetStatus(g_hMapCellId);
            g_hResetCountBeforeMap = reset.dispatched;
            g_hResetCompletedBeforeMap = reset.completed;
            g_hExpectMapReset = false;
            const auto opened = atOrigin ? OpenHMap(player, kHMapTier, false)
                : MapActionResult{ false, "Fixed outdoor start was not reached." };
            if (!opened.ok) {
                Check(kHOutdoorReturnCheck, false, std::format("Indoor: {}. Outdoor: {}", g_hIndoorReturnDetail, opened.message));
                EnterPhase(Phase::ScenarioH_PostMapAbandonWait);
                break;
            }
            EnterPhase(Phase::ScenarioH_MapEntryWait);
            break;
        }

        case Phase::ScenarioH_MapEntryWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto active = MapDeviceRuntime::GetActiveMapStateCopy();
            const auto* dungeon = MapPackRuntime::Manifest().ForTier(kHMapTier);
            const auto* forms = MapPackRuntime::FindForms(dungeon);
            const bool inTargetCell = forms && player && player->GetParentCell() == forms->cell &&
                MapDeviceRuntime::IsPlayerInActiveMapCell();
            const bool spawned = active.bossRefFormId != 0;
            if (WaitForMapTravel(player, inTargetCell && spawned, kMapEntryWaitSec)) {
                break;
            }
            const bool ready = inTargetCell && spawned && !MapDeviceRuntime::GetTravelStatus(player).kind;
            g_hMapBossFormId = ready ? active.bossRefFormId : 0;
            g_hDeathRunId = ready ? active.runId : 0;
            g_hDeathEscapeIntercepted = false;
            g_hDeathInterceptHitsSent = 0;
            if (g_hOutdoorMap) {
                g_hDeathAttackStartedAt.reset();
                g_hDeathTriggerDetail.clear();
                EnterPhase(Phase::ScenarioH_DeathInterceptTrigger);
                break;
            }

            auto* boss = ready ? RE::TESForm::LookupByID<RE::Actor>(active.bossRefFormId) : nullptr;
            const auto prepared = ready ? PrepareHBoss(player, boss) : HBossReadiness::Unavailable;
            if (prepared == HBossReadiness::Waiting) {
                break;
            }
            const auto modified = prepared == HBossReadiness::Ready
                ? ProbeHMapAffixes(boss, active, forms->cellId) : std::nullopt;
            auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(g_hEntryItemId);
            const bool consumed = player && item && player->GetItemCount(item) == g_hEntryItemCount - 1 &&
                g_hEntryItemKey.IsValid() && !ItemTracker::FindByKey(g_hEntryItemKey);
            const auto plain = g_hPlainAffixProbe.value_or(HAffixProbe{});
            const auto fixed = modified.value_or(HAffixProbe{});
            const bool outgoingOk = plain.outgoing > 0.0 && std::abs(
                fixed.outgoing - plain.outgoing * (1.0 + kHAffixDamagePct / 100.0)) < kHDamageTolerance;
            const bool incomingOk = plain.incoming > 0.0 && std::abs(
                fixed.incoming - plain.incoming * (1.0 - kHAffixReducedTakenPct / 100.0)) < kHDamageTolerance;
            const bool iiqOk = std::abs(plain.iiq) < kHDamageTolerance &&
                std::abs(fixed.iiq - plain.iiq - kHAffixIiqPct) < kHDamageTolerance;
            Check(kHAffixCheck, g_hPlainAffixProbe && modified && outgoingOk && incomingOk && iiqOk &&
                g_hEntryPendingWithoutConsumption && consumed,
                std::format("out={:.4f}->{:.4f}, in={:.4f}->{:.4f}, IIQ={:.1f}->{:.1f}, pendingUnchanged={}, consumed={}, pair={}/{}",
                    plain.outgoing, fixed.outgoing, plain.incoming, fixed.incoming, plain.iiq, fixed.iiq,
                    g_hEntryPendingWithoutConsumption, consumed, g_hPlainAffixProbe.has_value(), modified.has_value()));
            g_hDungeonResetExpected = active.active;

            // H10: 같은 셀의 낙하 문턱 아래로 옮긴다. 위치는 다음 프레임 이후에 반영될 수 있으므로,
            // 판정은 낙하 복귀 횟수가 늘고 입구 근처로 돌아왔는지로 한다.
            if (ready && forms->entryMarker) {
                constexpr double kFallProbeDepth = 256.0;
                auto* entry = forms->entryMarker;
                auto belowFloor = entry->GetPosition();
                const auto threshold = dungeon->navmeshMinZ - MapDeviceRuntime::GetFallMargin();
                belowFloor.z = static_cast<float>(threshold - kFallProbeDepth);
                g_hFallRecoveriesBefore = MapDeviceRuntime::GetFallRecoveryCount();
                if (MapDeviceRuntime::MovePlayerToSpace(player, forms->cell, entry->GetWorldspace(), belowFloor, entry->GetAngle())) {
                    g_hFallRunId = active.runId;
                    g_hFallPortals = active.portalsRemaining;
                    EnterPhase(Phase::ScenarioH_FallRecoveryWait);
                    break;
                }
            }
            Check("H10_MapFallRecovery", false, "Could not place the player below the fall threshold in the authorized cell.");
            g_hDeathAttackStartedAt.reset();
            g_hDeathTriggerDetail.clear();
            EnterPhase(Phase::ScenarioH_DeathInterceptTrigger);
            break;
        }

        case Phase::ScenarioH_FallRecoveryWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto active = MapDeviceRuntime::GetActiveMapStateCopy();
            const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(active));
            const bool sameCell = player && forms && forms->cellId == g_hMapCellId && player->GetParentCell() == forms->cell;
            const auto distance = sameCell && forms->entryMarker
                ? (player->GetPosition() - forms->entryMarker->GetPosition()).Length() : -1.0f;
            const bool authorized = MapDeviceRuntime::IsPlayerInActiveMapCell();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool fallDetected = MapDeviceRuntime::GetFallRecoveryCount() > g_hFallRecoveriesBefore;
            // 시험점(문턱 아래)과 겹치지 않는 고정 허용 거리와, 실제로 문턱 위로 올라왔는지로 판정한다.
            constexpr float kFallRecoveredEntryTolerance = 256.0f;
            const auto* fallDungeon = MapPackRuntime::Manifest().ForRun(active);
            const bool aboveThreshold = player && fallDungeon &&
                player->GetPosition().z >= fallDungeon->navmeshMinZ - MapDeviceRuntime::GetFallMargin();
            const bool recovered = fallDetected && sameCell && authorized && aboveThreshold && !travel.kind &&
                IsPlayerReadyInWorld() && distance >= 0.0f && distance <= kFallRecoveredEntryTolerance;
            if (!recovered && PhaseElapsedSec() < kMapFallWaitSec) {
                break;
            }
            Check("H10_MapFallRecovery", recovered && active.runId == g_hFallRunId &&
                active.portalsRemaining == g_hFallPortals && !MapDeviceRuntime::IsDeathEscapeInFlight(),
                std::format("fallDetected={}, aboveThreshold={}, sameCell={}, authorized={}, pending={}, entryDistance={:.2f}, run={}/{}, portals={}/{}",
                    fallDetected, aboveThreshold, sameCell, authorized, travel.kind.has_value(), distance, active.runId, g_hFallRunId,
                    active.portalsRemaining, g_hFallPortals));
            g_hDeathEscapeIntercepted = false;
            g_hDeathInterceptHitsSent = 0;
            g_hDeathAttackStartedAt.reset();
            g_hDeathTriggerDetail.clear();
            EnterPhase(Phase::ScenarioH_DeathInterceptTrigger);
            break;
        }

        case Phase::ScenarioH_DeathInterceptTrigger: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto finishTrigger = [&](std::string detail) {
                g_hDeathTriggerDetail = std::move(detail);
                g_hDeathEscapeIntercepted = MapDeviceRuntime::IsDeathEscapeInFlight();
                if (!g_hDeathEscapeIntercepted && player && MapDeviceRuntime::IsPlayerInActiveMapCell()) {
                    RestorePlayerVitals(player, true, true);
                    (void)MapDeviceRuntime::ReturnPortal(player);
                }
                EnterPhase(Phase::ScenarioH_DeathEscapeWait);
            };
            if (player && player->IsDead()) {
                Check(kHOutdoorReturnCheck, false, "Player died during an H4 lethal-hit trial.");
                FinishAndScheduleQuit("Scenario H interrupted by player death.");
                break;
            }
            if (MapDeviceRuntime::IsDeathEscapeInFlight()) {
                finishTrigger("Death escape accepted.");
                break;
            }
            if (!CombatHooks::AreAllHooksInstalled()) {
                finishTrigger("Combat hooks incomplete. No lethal damage sent.");
                break;
            }
            if (!player || !MapDeviceRuntime::IsPlayerInActiveMapCell()) {
                if (PhaseElapsedSec() >= kMapBossLoadWaitSec) {
                    finishTrigger("Player was unavailable in the authorized map cell.");
                }
                break;
            }
            auto* boss = RE::TESForm::LookupByID<RE::Actor>(g_hMapBossFormId);
            const auto prepared = PrepareHBoss(player, boss, true);
            if (prepared == HBossReadiness::Waiting) {
                break;
            }
            if (prepared == HBossReadiness::Unavailable) {
                finishTrigger("Boss 3D or permanent health was unavailable.");
                break;
            }
            const auto now = Clock::now();
            if (!g_hDeathAttackStartedAt) {
                g_hDeathAttackStartedAt = now;
                g_hNextBossHitSec = PhaseElapsedSec();
            }
            if (std::chrono::duration<double>(now - *g_hDeathAttackStartedAt).count() >= kHDeathTriggerWaitSec) {
                finishTrigger("Lethal-hit window expired.");
                break;
            }
            if (PhaseElapsedSec() < g_hNextBossHitSec) {
                break;
            }
            auto* values = player->AsActorValueOwner();
            if (!values) {
                finishTrigger("Player health values unavailable.");
                break;
            }
            g_hNextBossHitSec = PhaseElapsedSec() + kHBossHitIntervalSec;
            if (g_esPool) {
                g_esPool->Clear();
            }
            if (g_skillSystem) {
                (void)g_skillSystem->TickGuardShield(kHGuardClearSec, false);
            }
            const auto health = values->GetActorValue(RE::ActorValue::kHealth);
            if (health > kHDeathProbeHealth + kHDeathProbeHealthTolerance) {
                values->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth,
                    kHDeathProbeHealth - health);
            }
            CallDealDamage(boss, player, nullptr, false);
            ++g_hDeathInterceptHitsSent;
            if (player->IsDead()) {
                Check(kHOutdoorReturnCheck, false, "The engine attack killed the player.");
                FinishAndScheduleQuit("Scenario H interrupted by player death.");
            } else if (MapDeviceRuntime::IsDeathEscapeInFlight()) {
                finishTrigger("Death escape accepted.");
            }
            break;
        }

        case Phase::ScenarioH_DeathEscapeWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool returnedOut = !travel.inPackCell && !travel.kind && !MapDeviceRuntime::IsPlayerInActiveMapCell();
            const bool escapeCleared = !MapDeviceRuntime::IsDeathEscapeInFlight();
            if (WaitForMapTravel(player, returnedOut && escapeCleared, kMapReturnWaitSec)) {
                break;
            }
            const RE::FormID curCell = (player && player->GetParentCell()) ? player->GetParentCell()->GetFormID() : 0u;
            const RE::FormID curWs = (player && player->GetWorldspace()) ? player->GetWorldspace()->GetFormID() : 0u;
            const auto* cell = player ? player->GetParentCell() : nullptr;
            const auto* origin = RE::TESForm::LookupByID<RE::TESObjectREFR>(
                g_hOutdoorMap ? kHOutdoorStartMarker : kHIndoorStartMarker);
            const auto distance = player ? (player->GetPosition() - g_hPreMapPosition).Length() : -1.0f;
            const bool locMatched = g_hOutdoorMap
                ? cell && !cell->IsInteriorCell() && g_hPreMapWsId != 0 && curWs == g_hPreMapWsId &&
                    distance >= 0.0f && distance <= MapDeviceRuntime::GetTravelArrivalRadius()
                : cell && cell->IsInteriorCell() && origin && cell == origin->GetParentCell() && curCell == g_hPreMapCellId;
            const auto act = MapDeviceRuntime::GetActiveMapStateCopy();
            const bool alive = player && !player->IsDead();
            const bool ok = g_hDeathEscapeIntercepted && g_hDeathInterceptHitsSent > 0 && returnedOut && escapeCleared &&
                locMatched && alive && IsPlayerReadyInWorld() && act.active && act.runId == g_hDeathRunId &&
                act.portalsRemaining == kHEntryPortals;
            const auto detail = std::format(
                "intercepted={}, hits={}, out={}, cleared={}, locMatch={}, cell=0x{:08X}, ws=0x{:08X}, distance={:.1f}, alive={}, portals={}, run={}/{}. {}",
                g_hDeathEscapeIntercepted, g_hDeathInterceptHitsSent, returnedOut, escapeCleared,
                locMatched, curCell, curWs, distance, alive, act.portalsRemaining, act.runId, g_hDeathRunId,
                g_hDeathTriggerDetail);
            if (!g_hOutdoorMap) {
                g_hIndoorReturnOk = ok;
                g_hIndoorReturnDetail = detail;
                CloseHRun(player);
                EnterPhase(Phase::ScenarioH_DungeonCloseWait);
                break;
            }
            Check(kHOutdoorReturnCheck, g_hIndoorReturnOk && ok,
                std::format("Indoor: {}. Outdoor: {} (radius={:.1f}).", g_hIndoorReturnDetail, detail,
                    MapDeviceRuntime::GetTravelArrivalRadius()));

            // H9: 활성 회차의 셀에 이동 요청 없이 넣어, 셀 ID만 같아도 허가되지 않는지 검증한다.
            const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(act));
            auto* entry = forms ? forms->entryMarker : nullptr;
            if (!player || !returnedOut || !escapeCleared || !act.active || !entry ||
                !MapDeviceRuntime::MovePlayerToSpace(player, forms->cell, entry->GetWorldspace(), entry->GetPosition(), entry->GetAngle())) {
                Check("H9_UnauthorizedEntryAndSafeEscape", false, "Could not set up direct movement into the active map cell.");
                EnterPhase(Phase::ScenarioH_ReenterStart);
                break;
            }
            g_hUnauthorizedEntryObserved = false;
            EnterPhase(Phase::ScenarioH_UnauthorizedEntryWait);
            break;
        }

        case Phase::ScenarioH_UnauthorizedEntryWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto act = MapDeviceRuntime::GetActiveMapStateCopy();
            const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(act));
            const bool inRunCell = forms && player && player->GetParentCell() == forms->cell;
            if (player) {
                // 허가가 없으면 사망 가로채기가 꺼진다. H3에서 남은 적에게 실제로 죽지 않게 체력을 유지한다.
                RestorePlayerVitals(player, true, true);
                if (player->IsInCombat()) {
                    player->StopCombat();
                }
            }
            if (WaitForMapTravel(player, inRunCell, kMapEntryWaitSec)) {
                break;
            }
            if (g_dataPack && g_dataPack->IsLoaded()) {
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            }
            const auto snap = g_statEngine->GetSnapshot();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            g_hUnauthorizedEntryObserved = inRunCell && travel.unauthorized &&
                !MapDeviceRuntime::IsPlayerInActiveMapCell() && snap && !snap->inActiveMap;
            const auto escape = MapDeviceRuntime::SafeEscape(player);
            if (!escape.ok) {
                Check("H9_UnauthorizedEntryAndSafeEscape", false, std::format(
                    "inRunCell={}, unauthorized={}, SafeEscape failed: {}", inRunCell, travel.unauthorized, escape.message));
                EnterPhase(Phase::ScenarioH_ReenterStart);
                break;
            }
            EnterPhase(Phase::ScenarioH_SafeEscapeWait);
            break;
        }

        case Phase::ScenarioH_SafeEscapeWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            if (WaitForMapTravel(player, !travel.inPackCell, kMapReturnWaitSec)) {
                break;
            }
            const auto arrival = ObserveHSafeEscapeArrival(player);
            const bool atMarker = !travel.kind && !travel.inPackCell && IsPlayerReadyInWorld() && arrival.AtMarker();
            Check("H9_UnauthorizedEntryAndSafeEscape", g_hUnauthorizedEntryObserved && atMarker,
                std::format("unauthorizedEntry={}, outsidePack={}, inTamriel={}, markerDistance={:.2f}, pending={}",
                    g_hUnauthorizedEntryObserved, !travel.inPackCell, arrival.inTamriel, arrival.distance, travel.kind.has_value()));
            EnterPhase(Phase::ScenarioH_ReenterStart);
            break;
        }

        case Phase::ScenarioH_ReenterStart: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && player->IsInCombat() && PhaseElapsedSec() < kHCombatStopWaitSec) {
                StopPlayerCombatAndAlarms(player);
                break;
            }
            // H5: 재입장(5->4) 후 보스 처치 -> 아틀라스 T10 첫 클리어 +1P & 첫 T6+ 공물 확정
            const auto reenterRes = MapDeviceRuntime::ReenterActiveMap(player);
            if (!reenterRes.ok) {
                Check("H5_MapReenterBossKillAndAtlasPoint", false, std::format("ReenterActiveMap failed: {}", reenterRes.message));
                (void)MapDeviceRuntime::AbandonActiveMap(player);
                EnterPhase(Phase::ScenarioH_PostMapAbandonWait);
                break;
            }
            SaveLoad::UpdateState([](PersistentState& state) {
                state.atlas.completedTierMask = static_cast<std::uint16_t>(state.atlas.completedTierMask & ~kHAtlasTierBit);
                state.atlas.firstTributeGiven = false;
            });
            const auto stBefore = SaveLoad::GetStateCopy();
            g_hAvailPtsBeforeBoss = Core::PassiveTree::ComputeAvailableNormalPoints(
                *g_treeData, stBefore.tree, kHAtlasPointBaselineLevel, *g_config, stBefore.atlas.completedTierMask);
            g_hTributeCellId = 0;
            g_hTributeCountBeforeBoss.reset();
            EnterPhase(Phase::ScenarioH_ReenterWait);
            break;
        }

        case Phase::ScenarioH_ReenterWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const bool reentered = MapDeviceRuntime::IsPlayerInActiveMapCell();
            if (WaitForMapTravel(player, reentered, kMapEntryWaitSec)) {
                break;
            }
            if (!reentered || MapDeviceRuntime::GetTravelStatus(player).kind) {
                Check("H5_MapReenterBossKillAndAtlasPoint", false, "Map reentry was not confirmed.");
                (void)MapDeviceRuntime::AbandonActiveMap(player);
                EnterPhase(Phase::ScenarioH_PostMapAbandonWait);
                break;
            }
            auto* boss = (g_hMapBossFormId != 0)
                ? RE::TESForm::LookupByID<RE::Actor>(g_hMapBossFormId)
                : nullptr;
            const auto prepared = PrepareHBoss(player, boss);
            if (prepared == HBossReadiness::Waiting) {
                break;
            }
            if (prepared == HBossReadiness::Ready) {
                const auto* bossCell = boss->GetParentCell();
                g_hTributeCellId = bossCell ? bossCell->GetFormID() : 0;
                g_hTributeCountBeforeBoss = CountMapBossTributes(player, boss, bossCell);
            }
            g_hMapBossDamageKill = false;
            g_hNextBossHitSec = 0.0;
            EnterPhase(Phase::ScenarioH_BossKillWait);
            break;
        }

        case Phase::ScenarioH_BossKillWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                RestorePlayerVitals(player, true, true);
            }
            auto* boss = RE::TESForm::LookupByID<RE::Actor>(g_hMapBossFormId);
            const auto* bossCell = RE::TESForm::LookupByID<RE::TESObjectCELL>(g_hTributeCellId);
            if (!g_hMapBossDamageKill && boss && boss->Is3DLoaded() && !boss->IsDead() && g_hTributeCountBeforeBoss &&
                PhaseElapsedSec() >= g_hNextBossHitSec && PhaseElapsedSec() < kMapBossKillWaitSec) {
                g_hNextBossHitSec = PhaseElapsedSec() + kHBossHitIntervalSec;
                const bool applied = SkillExecution::DealDirectSkillDamage(
                    boss, kHMapBossHitDamage, Core::DamageType::Physical, true, true);
                g_hMapBossDamageKill = applied && (boss->IsDead() || BossHealthRatio(boss) <= 0.0);
            }
            const auto stNow = SaveLoad::GetStateCopy();
            const auto act = MapDeviceRuntime::GetActiveMapStateCopy();
            const bool t10Cleared = (stNow.atlas.completedTierMask & kHAtlasTierBit) != 0u;
            const auto tributeAfter = CountMapBossTributes(player, boss, bossCell);
            const auto& tributeBefore = g_hTributeCountBeforeBoss;
            const bool oneTributeGranted = tributeBefore && tributeAfter &&
                tributeAfter->Total() == tributeBefore->Total() + 1;
            if (!(g_hMapBossDamageKill && act.bossKilled && t10Cleared &&
                stNow.atlas.firstTributeGiven && oneTributeGranted) && PhaseElapsedSec() < kMapBossKillWaitSec) {
                break;
            }
            const auto availAfter = Core::PassiveTree::ComputeAvailableNormalPoints(
                *g_treeData, stNow.tree, kHAtlasPointBaselineLevel, *g_config, stNow.atlas.completedTierMask);
            // 보상이 회차 정리 때 지워지기 전에 실제 드랍을 주워 결투장 입장에 쓴다.
            if (oneTributeGranted && g_hMapBossDamageKill && act.bossKilled && player) {
                auto* tribute = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
                if (tribute && tributeAfter->corpse == tributeBefore->corpse + 1) {
                    boss->RemoveItem(tribute, 1, RE::ITEM_REMOVE_REASON::kStoreInContainer, nullptr, player);
                } else if (tributeAfter->floor == tributeBefore->floor + 1) {
                    for (const auto& [id, candidate] : tributeAfter->floorDrops) {
                        const auto previous = tributeBefore->floorDrops.find(id);
                        const auto countBefore = previous == tributeBefore->floorDrops.end() ? 0 : previous->second.count;
                        if (candidate.count == countBefore + 1) {
                            if (auto drop = candidate.handle.get()) {
                                player->PickUpObject(drop.get(), 1, false, false);
                            }
                            break;
                        }
                    }
                }
                g_hEarnedTributeBagCount = MapDeviceRuntime::CountPlayerTributes(player);
                g_hEarnedTributeAvailable = g_hEarnedTributeBagCount == tributeBefore->bag + 1;
            }
            // 결정 175: 보스 처치 후 귀환하면 남은 포털 수와 관계없이 회차가 끝난다.
            const auto resetBeforeReturn = MapDeviceRuntime::GetCellResetStatus(g_hMapCellId);
            // 결정 179 ⑦: 처치 뒤 사망 귀환이 먼저 접수됐으면 그 귀환이 보스 처치로 회차를 끝낸다(H6과 같음).
            const bool deathEscapeReturning = act.bossKilled && MapDeviceRuntime::IsDeathEscapeInFlight();
            const auto returnRes = !act.bossKilled
                ? MapActionResult{ false, "Boss did not die from the test attack in time." }
                : deathEscapeReturning ? MapActionResult{ true, "사망 귀환이 이미 진행 중(치명타 뒤 사망)" }
                : MapDeviceRuntime::ReturnPortal(player);
            const bool h5Ok = g_hMapBossDamageKill && t10Cleared && act.bossKilled &&
                act.portalsRemaining == kHReentryPortals && availAfter == g_hAvailPtsBeforeBoss + 1 &&
                stNow.atlas.firstTributeGiven && oneTributeGranted && returnRes.ok;
            g_hEarnedTributeAvailable = g_hEarnedTributeAvailable && h5Ok;
            Check("H5_MapReenterBossKillAndAtlasPoint", h5Ok,
                std::format("damageKill={}, t10Bit={}, bossKilled={}, portals={}, pts={}->{}, firstTrib={}, tributes={}->{}, collected={}, return={}",
                    g_hMapBossDamageKill, t10Cleared, act.bossKilled, act.portalsRemaining, g_hAvailPtsBeforeBoss, availAfter,
                    stNow.atlas.firstTributeGiven, tributeBefore ? std::to_string(tributeBefore->Total()) : "unavailable",
                    tributeAfter ? std::to_string(tributeAfter->Total()) : "unavailable",
                    g_hEarnedTributeAvailable, returnRes.message));

            g_hExpectMapReset = act.bossKilled && returnRes.ok && !resetBeforeReturn.pending &&
                resetBeforeReturn.dispatched == g_hResetCountBeforeMap;
            if (!returnRes.ok) {
                CloseHRun(player);
            }
            EnterPhase(Phase::ScenarioH_PostMapAbandonWait);
            break;
        }

        case Phase::ScenarioH_PostMapAbandonWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool outsideClosedRun = !travel.inPackCell && !MapDeviceRuntime::IsActiveMapRun();
            if (WaitForMapTravel(player, outsideClosedRun, kMapReturnWaitSec)) {
                break;
            }
            if (!player || !outsideClosedRun || travel.kind) {
                Check("H11_ClosedMapCellResetDispatch", false, "The run did not close after returning.");
                Check("H6_Arena4ThreePhaseBossAndSpirit220", false,
                    "The map run did not close outside the pack after the post-boss return (decision 175) before Arena4.");
                EnterPhase(Phase::ScenarioH_PostArenaReturnWait);
                break;
            }
            EnterPhase(Phase::ScenarioH_CellResetWait);
            break;
        }

        case Phase::ScenarioH_CellResetWait: {
            const auto reset = MapDeviceRuntime::GetCellResetStatus(g_hMapCellId);
            // 접수(dispatched)와 VM 호출 완료 콜백(completed)이 모두 한 번 이상 늘고 대기에서 빠져야 한다.
            const bool dispatched = !reset.pending && reset.dispatched > g_hResetCountBeforeMap &&
                reset.completed > g_hResetCompletedBeforeMap;
            if (g_hExpectMapReset && !dispatched && PhaseElapsedSec() < kMapResetWaitSec) {
                break;
            }
            Check("H11_ClosedMapCellResetDispatch", g_hExpectMapReset && dispatched,
                std::format("cell=0x{:08X}, pending={}, dispatched={}->{}, completed={}->{}, expectedAfterBossReturn={}",
                    g_hMapCellId, reset.pending, g_hResetCountBeforeMap, reset.dispatched,
                    g_hResetCompletedBeforeMap, reset.completed, g_hExpectMapReset));
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                Check("H6_Arena4ThreePhaseBossAndSpirit220", false, "Player unavailable after the map closed.");
                EnterPhase(Phase::ScenarioH_PostArenaReturnWait);
                break;
            }
            RestorePlayerVitals(player, true, true);
            if (player->IsInCombat()) {
                player->StopCombat();
            }
            auto* map = FormIds::Lookup<RE::TESObjectMISC>(FormIds::MapItemLocalFormId(kH14MapTier));
            if (!map) {
                Check("H14_MapBossWithoutContribution", false, "The test tier's map item was unavailable.");
                EnterPhase(Phase::ScenarioH_Arena4Prerequisites);
                break;
            }
            player->AddObjectToContainer(map, nullptr, 1, nullptr);
            const auto open = MapDeviceRuntime::OpenNormalMap(player, {}, map->GetFormID());
            if (!open.ok) {
                Check("H14_MapBossWithoutContribution", false, open.message);
                EnterPhase(Phase::ScenarioH_Arena4Prerequisites);
                break;
            }
            EnterPhase(Phase::ScenarioH_NoContributionMapEntryWait);
            break;
        }

        case Phase::ScenarioH_NoContributionMapEntryWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto run = MapDeviceRuntime::GetActiveMapStateCopy();
            const bool ready = player && run.active && !run.isArena4 && run.bossRefFormId != 0 &&
                MapDeviceRuntime::IsPlayerInActiveMapCell();
            if (WaitForMapTravel(player, ready, kMapEntryWaitSec)) {
                break;
            }
            auto* boss = ready ? RE::TESForm::LookupByID<RE::Actor>(run.bossRefFormId) : nullptr;
            if (!boss || boss->IsDead() || MapDeviceRuntime::GetTravelStatus(player).kind) {
                Check("H14_MapBossWithoutContribution", false, "The separate map boss was unavailable before the kill.");
                (void)MapDeviceRuntime::AbandonActiveMap(player);
                EnterPhase(Phase::ScenarioH_NoContributionMapReturnWait);
                break;
            }
            RestorePlayerVitals(player, true, true);
            const auto prepared = PrepareHBoss(player, boss);
            if (prepared == HBossReadiness::Waiting) {
                break;
            }
            if (prepared == HBossReadiness::Unavailable) {
                Check("H14_MapBossWithoutContribution", false, "The separate map boss did not become ready before the kill.");
                CloseHRun(player);
                EnterPhase(Phase::ScenarioH_NoContributionMapReturnWait);
                break;
            }
            // 16/16 아틀라스에서도 이 처치가 새 완료를 기록하는지 검사한다. 패시브 포인트는 다시 계산하지 않는다.
            SaveLoad::UpdateState([](PersistentState& st) {
                g_hAtlasBeforeNoContributionKill = st.atlas;
                st.atlas.completedTierMask = static_cast<std::uint16_t>(st.atlas.completedTierMask & ~kH14AtlasTierBit);
            });
            // 플레이어 피해를 주지 않고 엔진 사망 이벤트로 회차만 진행시키는지 확인한다.
            boss->KillImmediate();
            EnterPhase(Phase::ScenarioH_NoContributionMapKillWait);
            break;
        }

        case Phase::ScenarioH_NoContributionMapKillWait: {
            const auto run = MapDeviceRuntime::GetActiveMapStateCopy();
            if (!run.bossKilled && PhaseElapsedSec() < kMapBossKillWaitSec) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto atlas = SaveLoad::GetStateCopy().atlas;
            const auto before = *g_hAtlasBeforeNoContributionKill;
            const auto testMask = static_cast<std::uint16_t>(before.completedTierMask & ~kH14AtlasTierBit);
            RestoreH14AtlasTier();
            const auto rec = g_killTracker ? g_killTracker->GetActorRecordSnapshot(run.bossRefFormId)
                : Core::ActorKillRecordSnapshot{};
            const auto returned = MapDeviceRuntime::ReturnPortal(player);
            Check("H14_MapBossWithoutContribution",
                run.bossKilled && !rec.playerContributed && returned.ok &&
                atlas.completedTierMask == testMask && atlas.totalMapsCleared == before.totalMapsCleared,
                std::format("bossKilled={}, contribution={}, atlasMask={:04X}->{:04X}, mapsCleared={}->{}, return={}",
                    run.bossKilled, rec.playerContributed, testMask,
                    atlas.completedTierMask, before.totalMapsCleared, atlas.totalMapsCleared, returned.message));
            if (!returned.ok || !run.bossKilled) {
                (void)MapDeviceRuntime::AbandonActiveMap(player);
            }
            EnterPhase(Phase::ScenarioH_NoContributionMapReturnWait);
            break;
        }

        case Phase::ScenarioH_NoContributionMapReturnWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool closed = !travel.inPackCell && !MapDeviceRuntime::IsActiveMapRun();
            if (WaitForMapTravel(player, closed, kMapReturnWaitSec)) {
                break;
            }
            if (!closed || travel.kind) {
                Check("H6_Arena4ThreePhaseBossAndSpirit220", false, "The no-contribution map did not close.");
                EnterPhase(Phase::ScenarioH_PostArenaReturnWait);
                break;
            }
            EnterPhase(Phase::ScenarioH_Arena4Prerequisites);
            break;
        }

        case Phase::ScenarioH_Arena4Prerequisites: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool closed = !travel.kind && !travel.inPackCell && !MapDeviceRuntime::IsActiveMapRun();
            if (!closed && PhaseElapsedSec() < kMapReturnWaitSec) {
                CloseHRun(player);
                break;
            }
            auto* tribute = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
            if (!player || !closed || !tribute) {
                Check("H12_Arena4Prerequisites", false, "Player, tribute form, or closed prior run unavailable for the gate probes.");
                Check(kHArenaCheck, false, "Arena entry setup was unavailable.");
                EnterPhase(Phase::ScenarioH_PostArenaReturnWait);
                break;
            }
            RestorePlayerVitals(player, true, true);
            player->StopCombat();
            const auto savedMask = SaveLoad::GetStateCopy().tree.ascendancyMilestonesBitmask;
            if (!g_hMaskBeforeNegativeArena) {
                g_hMaskBeforeNegativeArena = savedMask;
            }
            const auto arenaReadyMask = (savedMask | Core::kArena4PrerequisiteMask) & ~Core::kArena4MilestoneBit;
            const auto setMask = [](std::uint32_t mask) {
                SaveLoad::UpdateState([mask](PersistentState& st) { st.tree.ascendancyMilestonesBitmask = mask; });
            };
            constexpr std::array<std::uint32_t, 3> kMissingMilestones{ 1u << 0u, 1u << 1u, 1u << 2u };
            constexpr std::size_t kGateProbeCount = kMissingMilestones.size() + 1;
            if (g_hArenaGateIndex < kGateProbeCount) {
                const bool fixtureForGate = MapDeviceRuntime::CountPlayerTributes(player) == 0;
                if (fixtureForGate) {
                    player->AddObjectToContainer(tribute, nullptr, 1, nullptr);
                }
                const auto tributeCount = MapDeviceRuntime::CountPlayerTributes(player);
                const bool alreadyCleared = g_hArenaGateIndex == kMissingMilestones.size();
                const auto testMask = alreadyCleared ? arenaReadyMask | Core::kArena4MilestoneBit
                    : arenaReadyMask & ~kMissingMilestones[g_hArenaGateIndex];
                const auto expected = alreadyCleared ? MapEntryDenial::ArenaAlreadyCleared : MapEntryDenial::ArenaMissingMilestones;
                setMask(testMask);
                const auto rejected = MapDeviceRuntime::OpenArena4(player);
                const auto tributeAfterProbe = MapDeviceRuntime::CountPlayerTributes(player);
                setMask(savedMask);
                const bool fixtureRemoved = !fixtureForGate || tributeAfterProbe == 0 ||
                    UniqueIdUtil::RemovePlainItem(player, tribute, 1);
                const bool gateOk = tributeCount > 0 && !rejected.ok && rejected.entryDenial == expected &&
                    tributeAfterProbe == tributeCount && fixtureRemoved;
                g_hArenaGateOk = g_hArenaGateOk && gateOk;
                g_hArenaGateDetail += std::format("mask=0x{:X}, accepted={}, reason={}/{}, tribute={}->{}, fixtureRemoved={}; ",
                    testMask, rejected.ok, static_cast<unsigned>(rejected.entryDenial), static_cast<unsigned>(expected),
                    tributeCount, tributeAfterProbe, fixtureRemoved);
                ++g_hArenaGateIndex;
                if (rejected.ok) {
                    CloseHRun(player);
                }
                break;
            }
            Check("H12_Arena4Prerequisites", g_hArenaGateOk, g_hArenaGateDetail);
            g_hArenaTrial = HArenaTrial::EarnedTribute;
            g_hArenaCosaveAttempted = false;
            if (!g_hEarnedTributeAvailable || MapDeviceRuntime::CountPlayerTributes(player) != g_hEarnedTributeBagCount) {
                Check(kHArenaCheck, false, "The tribute earned and collected by H5 is unavailable.");
                g_hArenaTrial = HArenaTrial::Bypass;
                EnterPhase(Phase::ScenarioH_Arena4NegativeStart);
                break;
            }
            setMask(arenaReadyMask);
            SaveLoad::SyncStateToStatEngine();
            if (g_dataPack && g_dataPack->IsLoaded()) {
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            }
            const auto st = SaveLoad::GetStateCopy();
            const auto snap = g_statEngine->GetSnapshot();
            g_hSpiritBeforeArena4 = snap ? snap->baseMaxSpirit : 0u;
            g_hAscPtsBeforeArena4 = Core::PassiveTree::ComputeEarnedAscendancyPoints(st.tree, *g_config);
            g_hArenaPhase1Clamped = false;
            g_hArenaInvulnBlocked = false;
            g_hArenaPhase2Clamped = false;
            g_hArenaLastLoggedTransition = Core::kArena4FirstPhase;
            g_hArenaDamageKill = false;
            g_hArenaResultBeforeReturn.reset();
            g_hTributesBeforeArena = g_hEarnedTributeBagCount;
            EnsureHArenaHealthBuffer(player);
            const auto opened = MapDeviceRuntime::OpenArena4(player);
            if (!opened.ok) {
                Check(kHArenaCheck, false, opened.message);
                g_hArenaTrial = HArenaTrial::Bypass;
                EnterPhase(Phase::ScenarioH_Arena4NegativeStart);
                break;
            }
            EnterPhase(Phase::ScenarioH_Arena4EntryWait);
            break;
        }

        case Phase::ScenarioH_Arena4NegativeStart: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool closed = !travel.kind && !travel.inPackCell && !MapDeviceRuntime::IsActiveMapRun();
            if (WaitForMapTravel(player, closed, kMapReturnWaitSec)) {
                if (!closed) {
                    CloseHRun(player);
                }
                break;
            }
            const auto* arena = MapPackRuntime::FindForms(&MapPackRuntime::Manifest().arena);
            if (arena && MapDeviceRuntime::GetCellResetStatus(arena->cellId).pending &&
                PhaseElapsedSec() < kMapResetWaitSec) {
                break;
            }
            auto* tribute = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
            if (!player || !closed || !IsPlayerReadyInWorld() || !tribute) {
                Check(NegativeArenaCheck(), false, "Cannot set up an independent arena run or its tribute fixture.");
                EnterPhase(Phase::ScenarioH_Arena4NegativeReturnWait);
                break;
            }
            const auto mask = SaveLoad::GetStateCopy().tree.ascendancyMilestonesBitmask;
            if (!g_hMaskBeforeNegativeArena) {
                g_hMaskBeforeNegativeArena = mask;
            }
            SaveLoad::UpdateState([mask](PersistentState& st) {
                st.tree.ascendancyMilestonesBitmask = (mask | Core::kArena4PrerequisiteMask) & ~Core::kArena4MilestoneBit;
            });
            player->StopCombat();
            EnsureHArenaHealthBuffer(player);
            g_hNegativeTributeBaseline = MapDeviceRuntime::CountPlayerTributes(player);
            g_hArenaResultBeforeReturn.reset();
            player->AddObjectToContainer(tribute, nullptr, 1, nullptr);
            g_hTributesBeforeArena = MapDeviceRuntime::CountPlayerTributes(player);
            g_hNegativeTributeOutstanding = g_hTributesBeforeArena == g_hNegativeTributeBaseline + 1;
            const auto opened = g_hNegativeTributeOutstanding ? MapDeviceRuntime::OpenArena4(player)
                : MapActionResult{ false, "A separate negative-test tribute was not added." };
            if (!opened.ok) {
                Check(NegativeArenaCheck(), false, opened.message);
                EnterPhase(Phase::ScenarioH_Arena4NegativeReturnWait);
                break;
            }
            g_hArenaDamageKill = false;
            g_hArenaBypassPrimed = false;
            g_hCompanionAttackPhase = 0;
            g_hCompanionAttackPowerSet = false;
            EnterPhase(Phase::ScenarioH_Arena4EntryWait);
            break;
        }

        case Phase::ScenarioH_Arena4EntryWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            EnsureHArenaHealthBuffer(player);
            const auto act = MapDeviceRuntime::GetActiveMapStateCopy();
            const bool ready = MapDeviceRuntime::IsPlayerInActiveMapCell() && act.active && act.isArena4 && act.bossRefFormId != 0;
            if (WaitForMapTravel(player, ready, kMapEntryWaitSec)) {
                break;
            }
            const bool earned = g_hArenaTrial == HArenaTrial::EarnedTribute;
            auto* boss = ready ? RE::TESForm::LookupByID<RE::Actor>(act.bossRefFormId) : nullptr;
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            if (ready && !travel.kind && !g_hArenaSpawnsOk.has_value()) {
                const auto* forms = MapPackRuntime::FindForms(&MapPackRuntime::Manifest().arena);
                const auto layout = ObserveHSpawnLayout(forms, act);
                g_hArenaSpawnsOk = layout.Meets(kHArenaInitialReinforcements);
                g_hArenaSpawnsDetail = std::format(
                    "Arena phase 1: bosses={}, reinforcements={}/{}, invalid={}, minDist={:.1f}, registry={}",
                    layout.bosses, layout.adds, kHArenaInitialReinforcements, layout.invalid, layout.nearest, layout.matchesRegistry);
                Check(kHDungeonCheck, !MapPackRuntime::Manifest().maps.empty() && g_hDungeonsOk && *g_hArenaSpawnsOk,
                    g_hDungeonsDetail + g_hArenaSpawnsDetail);
            }
            const auto prepared = ready && !travel.kind
                ? PrepareHBoss(player, boss, g_hArenaTrial == HArenaTrial::Companion) : HBossReadiness::Unavailable;
            if (prepared == HBossReadiness::Waiting) {
                break;
            }
            const auto count = MapDeviceRuntime::CountPlayerTributes(player);
            const bool consumed = count == g_hTributesBeforeArena - 1;
            if (prepared != HBossReadiness::Ready || !consumed || act.arenaPhase != Core::kArena4FirstPhase) {
                Check(earned ? kHArenaCheck : NegativeArenaCheck(), false,
                    std::format("Phase 1 boss/arrival unavailable or tribute not consumed ({} -> {}).", g_hTributesBeforeArena, count));
                CloseHRun(player);
                if (earned) {
                    g_hArenaTrial = HArenaTrial::Bypass;
                    EnterPhase(Phase::ScenarioH_Arena4NegativeStart);
                } else {
                    EnterPhase(Phase::ScenarioH_Arena4NegativeReturnWait);
                }
                break;
            }
            g_hArenaBossFormId = act.bossRefFormId;
            if (g_hArenaTrial == HArenaTrial::Bypass) {
                // 기여가 생긴 1페이즈에서 의도적으로 엔진 즉사를 사용한다.
                (void)SkillExecution::DealDirectSkillDamage(
                    boss, kHContributionHitDamage, Core::DamageType::Physical, true, true);
                const auto rec = g_killTracker ? g_killTracker->GetActorRecordSnapshot(g_hArenaBossFormId)
                    : Core::ActorKillRecordSnapshot{};
                const auto killContext = MapDeviceRuntime::GetMapKillContext(
                    g_hArenaBossFormId, player->GetParentCell()->GetFormID());
                g_hArenaBypassPrimed = killContext.runId == act.runId && killContext.isBoss && !boss->IsDead() &&
                    Core::MapSystem::IsMapKillRewardEligible(
                        rec.playerContributed && rec.wasHostileOnFirstHit, killContext.rewardBlocked, killContext.runFailed) &&
                    MapDeviceRuntime::GetActiveMapStateCopy().arenaPhase == Core::kArena4FirstPhase;
                BeginHArenaNegativeKill(boss);
                boss->KillImmediate();
                EnterPhase(Phase::ScenarioH_Arena4NegativeKillWait);
                break;
            }
            if (earned) {
                const auto entryPhase = MapDeviceRuntime::GetArena4BossPhaseStatusForAutoTest(g_hArenaBossFormId);
                LogInfo("H6 arena entry: " + DescribeHArenaBoss(ObserveHArenaBoss(boss, entryPhase)));
                const bool hit = SkillExecution::DealDirectSkillDamage(
                    boss, kHArenaHitDamage, Core::DamageType::Physical, true, true);
                // The phase promotion is queued. Sample the clamp before returning from this tick.
                const auto requested = MapDeviceRuntime::GetArena4BossPhaseStatusForAutoTest(g_hArenaBossFormId);
                const auto afterHit = ObserveHArenaBoss(boss, requested);
                g_hArenaPhase1Clamped = hit &&
                    ArenaBossAtFloor(afterHit, kHArenaFloorFirst, Core::kArena4FirstPhase, kHArenaSecondPhase);
                LogInfo(std::format("H6 phase 1 floor hit (same tick, damageApplied={}): {}", hit, DescribeHArenaBoss(afterHit)));
                const bool blocked = requested.invulnerable &&
                    !SkillExecution::DealDirectSkillDamage(boss, kHArenaHitDamage, Core::DamageType::Physical, true, true);
                g_hArenaInvulnBlocked = blocked && !boss->IsDead() &&
                    std::abs(BossHealthRatio(boss) - afterHit.HealthRatio()) <= kHArenaPhaseHealthUnchangedTolerance;
                EnterPhase(Phase::ScenarioH_Arena4Phase2Wait);
                break;
            }
            if (!CombatHooks::AreAllHooksInstalled()) {
                Check(kHCompanionCheck, false, "Combat hooks incomplete. No NPC attack sent.");
                CloseHRun(player);
                EnterPhase(Phase::ScenarioH_Arena4NegativeReturnWait);
                break;
            }
            // 공격력은 시험 NPC 인스턴스에만 부여한다. 실제 타격은 엔진으로 보낸다.
            g_hArenaCompanion = SpawnNpcInFrontOfPlayer(player, kNeutralNpcId, kHCompanionSpawnDistance, 0.0f, 0.0f, false);
            CaptureHCompanionOwnership(player->GetParentCell()->GetFormID());
            BeginHArenaNegativeKill(boss);
            g_hNextBossHitSec = 0.0;
            EnterPhase(Phase::ScenarioH_Arena4NegativeKillWait);
            break;
        }

        case Phase::ScenarioH_Arena4Phase2Wait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            EnsureHArenaHealthBuffer(player);
            auto* boss = RE::TESForm::LookupByID<RE::Actor>(g_hArenaBossFormId);
            const auto phase = MapDeviceRuntime::GetArena4BossPhaseStatusForAutoTest(g_hArenaBossFormId);
            LogHArenaTransition(boss, phase, kHArenaSecondPhase);
            if (!g_hArenaCosaveAttempted) {
                const auto run = MapDeviceRuntime::TakeSnapshotForSave();
                const bool ready = run.playerAuthorized && run.isArena4 && run.arenaPhase == kHArenaSecondPhase &&
                    boss && !boss->IsDead() && boss->Is3DLoaded() && IsPlayerReadyInWorld();
                if (ready && run.arenaPhaseInvulnSec >= kHArenaSaveTimerMin && run.arenaPhaseInvulnSec <= kHArenaSaveTimerMax) {
                    g_hArenaHealthBeforeSave = BossHealthRatio(boss);
                    g_hArenaCosaveAttempted = true;
                    if (!StartHCosave(run, true)) {
                        Check(kHArenaCosaveCheck, false, "Could not record the prior save-file state.");
                    }
                    break;
                }
                if (boss && !boss->IsDead() && PhaseElapsedSec() < kMapBossKillWaitSec && phase.invulnerable) {
                    break;
                }
                Check(kHArenaCosaveCheck, false, "Could not request a save with a living phase 2 boss and a partial immunity timer.");
                g_hArenaCosaveAttempted = true;
            }
            if (phase.invulnerable && PhaseElapsedSec() < kArenaPhase3InvulnerabilityWaitSec) {
                break;
            }
            if (boss && !boss->IsDead()) {
                LogInfo("H6 phase 2 before floor hit: " + DescribeHArenaBoss(ObserveHArenaBoss(boss, phase)));
                const bool hit = SkillExecution::DealDirectSkillDamage(
                    boss, kHArenaHitDamage, Core::DamageType::Physical, true, true);
                const auto requested = MapDeviceRuntime::GetArena4BossPhaseStatusForAutoTest(g_hArenaBossFormId);
                const auto afterHit = ObserveHArenaBoss(boss, requested);
                g_hArenaPhase2Clamped = hit &&
                    ArenaBossAtFloor(afterHit, kHArenaFloorSecond, kHArenaSecondPhase, Core::kArena4FinalPhase);
                LogInfo(std::format("H6 phase 2 floor hit (same tick, damageApplied={}): {}", hit, DescribeHArenaBoss(afterHit)));
            }
            EnterPhase(Phase::ScenarioH_Arena4Phase3Wait);
            break;
        }

        case Phase::ScenarioH_Arena4Phase3Wait: {
            EnsureHArenaHealthBuffer(RE::PlayerCharacter::GetSingleton());
            auto* boss = RE::TESForm::LookupByID<RE::Actor>(g_hArenaBossFormId);
            const auto phase = MapDeviceRuntime::GetArena4BossPhaseStatusForAutoTest(g_hArenaBossFormId);
            LogHArenaTransition(boss, phase, Core::kArena4FinalPhase);
            if (phase.invulnerable && PhaseElapsedSec() < kArenaPhase3InvulnerabilityWaitSec) {
                break;
            }
            g_hNextBossHitSec = 0.0;
            EnterPhase(Phase::ScenarioH_Arena4KillWait);
            break;
        }

        case Phase::ScenarioH_Arena4KillWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            EnsureHArenaHealthBuffer(player);
            const auto act = MapDeviceRuntime::GetActiveMapStateCopy();
            auto* boss = RE::TESForm::LookupByID<RE::Actor>(g_hArenaBossFormId);
            const auto phase = MapDeviceRuntime::GetArena4BossPhaseStatusForAutoTest(g_hArenaBossFormId);
            LogHArenaTransition(boss, phase, Core::kArena4FinalPhase);
            const auto elapsed = PhaseElapsedSec();
            if (!g_hArenaDamageKill && boss && !boss->IsDead() && elapsed >= g_hNextBossHitSec &&
                elapsed < kMapBossKillWaitSec && phase.completed == Core::kArena4FinalPhase && !phase.invulnerable) {
                g_hNextBossHitSec = elapsed + kHBossHitIntervalSec;
                const auto beforeHit = ObserveHArenaBoss(boss, phase);
                const bool hit = SkillExecution::DealDirectSkillDamage(
                    boss, kHArenaHitDamage, Core::DamageType::Physical, true, false);
                const auto afterHit = ObserveHArenaBoss(boss, MapDeviceRuntime::GetArena4BossPhaseStatusForAutoTest(g_hArenaBossFormId));
                g_hArenaDamageKill = hit && (!afterHit.alive || afterHit.health <= 0.0);
                LogInfo(std::format("H6 phase 3 kill attempt (t={:.2f}s, damageApplied={}): before ({}), after ({})",
                    elapsed, hit, DescribeHArenaBoss(beforeHit), DescribeHArenaBoss(afterHit)));
            }
            const auto stNow = SaveLoad::GetStateCopy();
            const bool asc4Done = (stNow.tree.ascendancyMilestonesBitmask & Core::kArena4MilestoneBit) != 0u;
            const bool killConfirmed = g_hArenaDamageKill && act.bossKilled && asc4Done;
            if (!killConfirmed && elapsed < kMapBossKillWaitSec) {
                break;
            }
            const auto timeoutDetail = killConfirmed ? std::string{} :
                std::format(", phase 3 kill wait timed out; final boss ({})", DescribeHArenaBoss(ObserveHArenaBoss(boss, phase)));
            if (g_dataPack && g_dataPack->IsLoaded()) {
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            }
            const auto snap = g_statEngine->GetSnapshot();
            const auto ascAfter = Core::PassiveTree::ComputeEarnedAscendancyPoints(stNow.tree, *g_config);
            const std::uint32_t spiritAfter = snap ? snap->baseMaxSpirit : 0u;
            // 결정 179 개정: 처치 타격 뒤 사망 연출 동안 증원이 플레이어를 쓰러뜨리면 사망 귀환이 이미 회차를 끝내러 가고 있다.
            // 그때도 클리어는 인정되므로 그 귀환을 이 시험의 귀환으로 받는다.
            const bool deathEscapeReturning = act.bossKilled && MapDeviceRuntime::IsDeathEscapeInFlight();
            const auto returned = !act.bossKilled ? MapActionResult{ false, "Boss death not confirmed." }
                : deathEscapeReturning ? MapActionResult{ true, "사망 귀환이 이미 진행 중(치명타 뒤 사망)" }
                : MapDeviceRuntime::ReturnPortal(player);
            g_hArenaReturnRequested = returned.ok;
            g_hArenaReturnState = act;
            g_hArenaResultBeforeReturn = g_hArenaPhase1Clamped && g_hArenaInvulnBlocked && g_hArenaPhase2Clamped &&
                g_hArenaDamageKill && act.bossKilled && asc4Done && ascAfter == kHArenaExpectedAscendancyPoints &&
                spiritAfter == kHArenaExpectedBaseSpirit;
            g_hArenaResultDetail = std::format(
                "p1Clamp={}, invulnBlocked={}, p2Clamp={}, damageKill={}, bossKilled={}, bit8={}, asc={}->{}, baseSpirit={}->{}, return={}",
                g_hArenaPhase1Clamped, g_hArenaInvulnBlocked, g_hArenaPhase2Clamped, g_hArenaDamageKill, act.bossKilled,
                asc4Done, g_hAscPtsBeforeArena4, ascAfter, g_hSpiritBeforeArena4, spiritAfter, returned.message) + timeoutDetail;
            if (!returned.ok) {
                CloseHRun(player);
            }
            EnterPhase(Phase::ScenarioH_Arena4ReturnWait);
            break;
        }

        case Phase::ScenarioH_Arena4ReturnWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool closed = !travel.kind && !travel.inPackCell && !MapDeviceRuntime::IsActiveMapRun();
            if (WaitForMapTravel(player, closed, kMapReturnWaitSec)) {
                break;
            }
            const bool atOrigin = IsPlayerReadyInWorld() && HAtSavedReturn(player, g_hArenaReturnState);
            Check(kHArenaCheck, g_hArenaResultBeforeReturn.value_or(false) && g_hArenaReturnRequested && closed && atOrigin,
                std::format("{}, closed={}, atOrigin={}", g_hArenaResultDetail, closed, atOrigin));
            g_hArenaResultBeforeReturn.reset();
            g_hArenaReturnRequested = false;
            g_hArenaTrial = HArenaTrial::Bypass;
            EnterPhase(Phase::ScenarioH_Arena4NegativeStart);
            break;
        }

        case Phase::ScenarioH_Arena4NegativeKillWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            EnsureHArenaHealthBuffer(player);
            auto* boss = RE::TESForm::LookupByID<RE::Actor>(g_hArenaBossFormId);
            const auto act = MapDeviceRuntime::GetActiveMapStateCopy();
            const bool bypass = g_hArenaTrial == HArenaTrial::Bypass;
            const auto timeout = bypass ? kMapBossKillWaitSec : kHCompanionKillWaitSec;
            if (!bypass && !g_hArenaDamageKill && player && boss && !boss->IsDead() && act.active && !act.failed &&
                MapDeviceRuntime::IsPlayerInActiveMapCell() && !MapDeviceRuntime::IsArena4BossInvulnerable(g_hArenaBossFormId) &&
                PhaseElapsedSec() >= g_hNextBossHitSec && PhaseElapsedSec() < timeout) {
                auto attacker = g_hArenaCompanion.get();
                const auto* base = attacker ? attacker->GetBaseObject() : nullptr;
                if (attacker && !attacker->IsDead() && attacker->Is3DLoaded() && attacker.get() != player &&
                    boss->Is3DLoaded() && attacker->GetParentCell() == boss->GetParentCell() &&
                    base && base->GetFormID() == kNeutralNpcId) {
                    if (!g_hCompanionAttackPowerSet) {
                        if (auto* values = attacker->AsActorValueOwner()) {
                            values->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kUnarmedDamage,
                                kHCompanionUnarmedDamage);
                            g_hCompanionAttackPowerSet = true;
                        }
                    }
                    g_hNextBossHitSec = PhaseElapsedSec() + kHBossHitIntervalSec;
                    g_hCompanionAttackPhase = act.arenaPhase;
                    CallDealDamage(attacker.get(), boss, nullptr, false);
                    g_hArenaDamageKill = boss->IsDead() || BossHealthRatio(boss) <= 0.0;
                }
            }
            if (!act.bossKilled && PhaseElapsedSec() < timeout) {
                break;
            }
            if (!g_hArenaResultBeforeReturn.has_value()) {
                const auto state = SaveLoad::GetStateCopy();
                const bool unlocked = (state.tree.ascendancyMilestonesBitmask & Core::kArena4MilestoneBit) != 0u;
                const auto rec = g_killTracker ? g_killTracker->GetActorRecordSnapshot(g_hArenaBossFormId)
                    : Core::ActorKillRecordSnapshot{};
                const auto drops = WorldDropTracker::TakeSnapshotForSave();
                const bool noGroundLoot = std::ranges::none_of(drops, [](const auto& drop) {
                    return drop.corpseRefFormId == g_hArenaBossFormId;
                });
                const bool noCorpseLoot = boss && CountArenaCorpseItems(boss) == g_hCorpseItemsBeforeNegativeKill;
                bool noCharmCharge = g_charmSystem != nullptr;
                if (g_charmSystem) {
                    const auto charms = g_charmSystem->Slots();
                    for (std::size_t i = 0; i < charms.size(); ++i) {
                        noCharmCharge = noCharmCharge && charms[i].charges <= g_hCharmsBeforeNegativeKill[i].charges;
                    }
                }
                const bool sameGemProgress = ObserveHGemProgress(state) == g_hGemsBeforeNegativeKill;
                const bool qualifiedTrial = bypass ? g_hArenaBypassPrimed : !rec.playerContributed &&
                    g_hArenaDamageKill && g_hCompanionAttackPhase == Core::kArena4FinalPhase;
                g_hArenaResultBeforeReturn = qualifiedTrial && act.bossKilled && !unlocked &&
                    state.exileXp == g_hXpBeforeNegativeKill && state.exileLevel == g_hLevelBeforeNegativeKill &&
                    sameGemProgress && noCorpseLoot && noGroundLoot && noCharmCharge;
                g_hArenaResultDetail = std::format(
                    "trialReady={}, bossKilled={}, npcDamageKill={}, npcPhase={}, contribution={}, bit8={}, xp={}->{}, level={}->{}, gemsUnchanged={}, noCorpseLoot={}, noGroundLoot={}, noCharmCharge={}",
                    qualifiedTrial, act.bossKilled, g_hArenaDamageKill, g_hCompanionAttackPhase, rec.playerContributed,
                    unlocked, g_hXpBeforeNegativeKill, state.exileXp, g_hLevelBeforeNegativeKill, state.exileLevel,
                    sameGemProgress, noCorpseLoot, noGroundLoot, noCharmCharge);
                BeginHCompanionCleanup();
            }
            if (IsHCompanionCleanupPending()) {
                break;
            }
            const auto returned = act.bossKilled ? MapDeviceRuntime::ReturnPortal(player)
                : MapActionResult{ false, "Boss death not confirmed within the trial window." };
            g_hArenaReturnRequested = returned.ok;
            g_hArenaReturnState = act;
            g_hArenaResultDetail += std::format(", return={}", returned.message);
            if (!returned.ok) {
                CloseHRun(player);
            }
            EnterPhase(Phase::ScenarioH_Arena4NegativeReturnWait);
            break;
        }

        case Phase::ScenarioH_Arena4NegativeReturnWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool closed = !travel.kind && !travel.inPackCell && !MapDeviceRuntime::IsActiveMapRun();
            if (WaitForMapTravel(player, closed, kMapReturnWaitSec)) {
                if (!g_hArenaReturnRequested) {
                    CloseHRun(player);
                }
                break;
            }
            if (g_hArenaResultBeforeReturn.has_value()) {
                const bool atOrigin = IsPlayerReadyInWorld() && HAtSavedReturn(player, g_hArenaReturnState);
                const bool companionCleaned = g_hArenaTrial == HArenaTrial::Bypass || WasHCompanionCleanupSuccessful();
                const auto tributeCount = MapDeviceRuntime::CountPlayerTributes(player);
                const auto state = SaveLoad::GetStateCopy();
                const bool noDelayedRewards = (state.tree.ascendancyMilestonesBitmask & Core::kArena4MilestoneBit) == 0 &&
                    state.exileXp == g_hXpBeforeNegativeKill && state.exileLevel == g_hLevelBeforeNegativeKill &&
                    ObserveHGemProgress(state) == g_hGemsBeforeNegativeKill;
                Check(NegativeArenaCheck(), *g_hArenaResultBeforeReturn && g_hArenaReturnRequested && closed && atOrigin &&
                        companionCleaned && noDelayedRewards && tributeCount == g_hNegativeTributeBaseline,
                    std::format("{}, closed={}, atOrigin={}, cleanup={}, noDelayedRewards={}, tribute={}/{}", g_hArenaResultDetail,
                        closed, atOrigin, companionCleaned, noDelayedRewards, tributeCount, g_hNegativeTributeBaseline));
                g_hArenaResultBeforeReturn.reset();
            }
            ClearHNegativeArenaFixture(player);
            if (IsHCompanionCleanupPending()) {
                break;
            }
            g_hArenaReturnRequested = false;
            if (g_hArenaTrial == HArenaTrial::Bypass) {
                g_hArenaTrial = HArenaTrial::Companion;
                EnterPhase(Phase::ScenarioH_Arena4NegativeStart);
                break;
            }
            if (const auto mask = std::exchange(g_hMaskBeforeNegativeArena, std::nullopt)) {
                SaveLoad::UpdateState([mask](PersistentState& st) { st.tree.ascendancyMilestonesBitmask = *mask; });
                SaveLoad::SyncStateToStatEngine();
            }
            EnterPhase(Phase::ScenarioH_PostArenaReturnWait);
            break;
        }

        case Phase::ScenarioH_PostArenaReturnWait: {
            if (!g_hArenaSpawnsOk.has_value()) {
                g_hArenaSpawnsOk = false;
                Check(kHDungeonCheck, false, g_hDungeonsDetail + g_hArenaSpawnsDetail);
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool outsideClosedRun = !travel.inPackCell && !MapDeviceRuntime::IsActiveMapRun();
            if (WaitForMapTravel(player, outsideClosedRun, kMapReturnWaitSec)) {
                break;
            }
            if (!player || !outsideClosedRun || travel.kind || !IsPlayerReadyInWorld()) {
                ContinueHAfterCosaveFailure("Could not confirm arena return and closure before the map round trip.");
                break;
            }
            ClearHArenaHealthBuffer(player);
            constexpr auto kCosaveMapTier = Core::kMinMapTier;
            const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForTier(kCosaveMapTier));
            if (forms && MapDeviceRuntime::GetCellResetStatus(forms->cellId).pending &&
                PhaseElapsedSec() < kMapResetWaitSec) {
                break;
            }
            const auto open = OpenHMap(player, kCosaveMapTier, true);
            if (!open.ok) {
                ContinueHAfterCosaveFailure(open.message);
                break;
            }
            EnterPhase(Phase::ScenarioH_CosaveMapEntryWait);
            break;
        }

        case Phase::ScenarioH_CosaveMapEntryWait: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto run = MapDeviceRuntime::TakeSnapshotForSave();
            const bool ready = run.active && run.playerAuthorized && run.bossRefFormId != 0 &&
                !run.bossKilled && !run.failed && MapDeviceRuntime::IsPlayerInActiveMapCell();
            if (WaitForMapTravel(player, ready, kMapEntryWaitSec)) {
                break;
            }
            if (!player || !ready || !IsPlayerReadyInWorld()) {
                ContinueHAfterCosaveFailure("The active run was unavailable for saving.");
                break;
            }
            RestorePlayerVitals(player, true, true);
            if (!StartHCosave(std::move(run), false)) {
                ContinueHAfterCosaveFailure("Could not record the prior save-file state.");
            }
            break;
        }

        case Phase::ScenarioH_SavePump: {
            const bool written = IsHSaveWritten(g_cfg.tempSave);
            std::optional<float> timerAtSave;
            {
                std::scoped_lock lock(g_msgMutex);
                timerAtSave = g_hArenaTimerAtSave;
                if (written) {
                    g_expectedSaveStem.clear();
                }
            }
            if (written && (!g_hArenaCosaveInProgress ||
                            (timerAtSave && *timerAtSave > 0.0f && *timerAtSave <= kHArenaSaveTimerMax))) {
                EnterPhase(Phase::ScenarioH_SaveWaitBeforeLoad);
                break;
            }
            if (g_hArenaCosaveInProgress) {
                // 무적 만료 후 같은 이름으로 다시 저장하면 다른 상태가 시험을 통과할 수 있다.
                if (!written && PhaseElapsedSec() <= kHSaveWaitSec) {
                    break;
                }
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                    g_hWatchArenaCoSave = false;
                }
                Check(kHArenaCosaveCheck, false,
                    std::format("No verified save with a live immunity timer (written={}, timer={:.3f}s).",
                        written, timerAtSave.value_or(0.0f)));
                g_hArenaCosaveInProgress = false;
                EnterPhase(Phase::ScenarioH_Arena4Phase2Wait);
                break;
            }
            if (Clock::now() >= g_nextSaveRetryAt) {
                ++g_saveAttempts;
                if (auto* mgr = RE::BGSSaveLoadManager::GetSingleton()) {
                    mgr->Save(g_cfg.tempSave.c_str());
                } else {
                    ContinueHAfterCosaveFailure("BGSSaveLoadManager unavailable.");
                    break;
                }
                g_nextSaveRetryAt = Clock::now() + kHSaveRetryDelay;
            }
            if (PhaseElapsedSec() > kHSaveWaitSec) {
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_expectedSaveStem.clear();
                }
                ContinueHAfterCosaveFailure("Timed out waiting for the Scenario H save files to update.");
            }
            break;
        }

        case Phase::ScenarioH_SaveWaitBeforeLoad: {
            if (PhaseElapsedSec() < kHSaveBeforeLoadSec) {
                break;
            }
            if (!RequestLoadSave(g_cfg.tempSave)) {
                ContinueHAfterCosaveFailure("Scenario H reload request failed.");
                break;
            }
            EnterPhase(Phase::ScenarioH_AwaitReload);
            break;
        }

        case Phase::ScenarioH_AwaitReload: {
            bool signaled = false;
            bool ok = false;
            {
                std::scoped_lock lock(g_msgMutex);
                signaled = g_postLoadSignaled;
                ok = g_postLoadSucceeded;
            }
            if (signaled && !ok) {
                ContinueHAfterCosaveFailure("kPostLoadGame failed on Scenario H reload.");
                return;
            }
            if (signaled && ok && IsPlayerReadyInWorld()) {
                if (g_hArenaCosaveInProgress) {
                    // 불러온 결투장에는 2페이즈 보스가 살아 있다. 다음 틱을 기다리지 않고 여유를 확인한다.
                    EnsureHArenaHealthBuffer(RE::PlayerCharacter::GetSingleton());
                }
                EnterPhase(Phase::ScenarioH_ReloadSettle);
            } else if (PhaseElapsedSec() > kHReloadWaitSec) {
                ContinueHAfterCosaveFailure("Timed out waiting for Scenario H reload.");
            }
            break;
        }

        case Phase::ScenarioH_ReloadSettle: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (g_hArenaCosaveInProgress) {
                EnsureHArenaHealthBuffer(player);
            } else if (player) {
                RestorePlayerVitals(player, true, true);
            }
            if (!g_hArenaCosaveInProgress && g_unpausedAccumSec < kHReloadSettleSec &&
                PhaseElapsedSec() < kHReloadSettleLimitSec) {
                break;
            }
            const auto st = SaveLoad::GetStateCopy();
            const bool milestonesRestored = st.tree.ascendancyMilestonesBitmask == g_hMilestonesBeforeCosave;
            const auto versions = SaveLoad::GetLoadedMapRecordVersions();
            const auto run = MapDeviceRuntime::TakeSnapshotForSave();
            const auto& expected = g_hRunBeforeCosave;
            const bool atlasRestored = st.atlas.completedTierMask == g_hAtlasBeforeCosave.completedTierMask &&
                st.atlas.totalMapsCleared == g_hAtlasBeforeCosave.totalMapsCleared &&
                st.atlas.highestTierCleared == g_hAtlasBeforeCosave.highestTierCleared &&
                st.atlas.firstTributeGiven == g_hAtlasBeforeCosave.firstTributeGiven;
            const bool runRestored = run.active && run.playerAuthorized && MapDeviceRuntime::IsPlayerInActiveMapCell() &&
                run.runId == expected.runId && run.tier == expected.tier && run.isArena4 == expected.isArena4 &&
                run.dungeonKey == expected.dungeonKey && run.packVersion == expected.packVersion &&
                run.portalsRemaining == expected.portalsRemaining && run.arenaPhase == expected.arenaPhase &&
                run.bossKilled == expected.bossKilled && run.failed == expected.failed && run.endRequested == expected.endRequested &&
                run.spawnsDisabled == expected.spawnsDisabled &&
                run.bossRefFormId == expected.bossRefFormId && run.spawnedRefFormIds == expected.spawnedRefFormIds &&
                run.mods == expected.mods && run.returnCellFormId == expected.returnCellFormId &&
                run.returnWorldspaceFormId == expected.returnWorldspaceFormId &&
                run.returnPos == expected.returnPos && run.returnAngle == expected.returnAngle;
            const bool versionsOk = versions.atlas == SaveLoad::kAtlasVersion && versions.maps == SaveLoad::kMapsVersion;
            if (g_hArenaCosaveInProgress) {
                auto* boss = RE::TESForm::LookupByID<RE::Actor>(run.bossRefFormId);
                const auto profile = MonsterRuntime::GetProfileCopy(run.bossRefFormId);
                const bool phaseModsRestored = profile && profile->HasMod(Core::MonsterModKind::Hasted) &&
                    profile->HasMod(Core::MonsterModKind::ExtraFire);
                if ((!runRestored || !boss || !boss->Is3DLoaded() || !phaseModsRestored) &&
                    PhaseElapsedSec() < kHReloadSettleLimitSec) {
                    break;
                }
                std::optional<float> savedTimer;
                std::optional<float> loadedTimer;
                {
                    std::scoped_lock lock(g_msgMutex);
                    g_hWatchArenaCoSave = false;
                    savedTimer = g_hArenaTimerAtSave;
                    loadedTimer = g_hArenaTimerAtLoad;
                }
                const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(run));
                const double ratio = BossHealthRatio(boss);
                const bool livingPhase2 = forms && boss && boss->Is3DLoaded() && !boss->IsDead() && !boss->IsDisabled() &&
                    boss->GetParentCell() == forms->cell && run.arenaPhase == kHArenaSecondPhase && !run.bossKilled &&
                    std::abs(ratio - g_hArenaHealthBeforeSave) <= kHArenaHealthRatioTolerance;
                // SKSE의 저장 메시지는 엔진 저장 직전, 로드 완료 메시지는 로드 반환 직후에 온다.
                // 무적 타이머가 메인 틱에서 다시 감소하기 전 두 시점의 값을 대조한다.
                const bool timerRestored = savedTimer && loadedTimer && *loadedTimer > 0.0f &&
                    std::abs(*loadedTimer - *savedTimer) <= kHArenaSaveTimerTolerance;
                Check(kHArenaCosaveCheck, versionsOk && atlasRestored && milestonesRestored && runRestored &&
                        livingPhase2 && timerRestored && phaseModsRestored,
                    std::format("ATLS v{}, MAPS v{}, atlas={}, run={}, runId={}/{}, phase={}, boss=0x{:08X}, alivePhase2={}, health={:.4f}->{:.4f}, immunity request/save/load={:.3f}/{:.3f}/{:.3f}s, phaseMods={}, milestones={}",
                        versions.atlas, versions.maps, atlasRestored, runRestored, run.runId, expected.runId, run.arenaPhase,
                        run.bossRefFormId, livingPhase2, g_hArenaHealthBeforeSave, ratio, expected.arenaPhaseInvulnSec,
                        savedTimer.value_or(0.0f), loadedTimer.value_or(0.0f), phaseModsRestored, milestonesRestored));
                g_hArenaCosaveInProgress = false;
                EnterPhase(Phase::ScenarioH_Arena4Phase2Wait);
                break;
            }
            const bool cosaveOk = versionsOk && atlasRestored && milestonesRestored && runRestored && !expected.mods.empty();
            const auto detail = std::format(
                "ATLS v{}, MAPS v{}, atlasMask=0x{:04X}, cleared={}, trib={}, msMask=0x{:X}, atlasRestored={}, milestonesRestored={}, runRestored={}, mods={}/{}",
                versions.atlas, versions.maps,
                st.atlas.completedTierMask, st.atlas.totalMapsCleared, st.atlas.firstTributeGiven,
                st.tree.ascendancyMilestonesBitmask, atlasRestored, milestonesRestored, runRestored,
                run.mods.size(), expected.mods.size());
            Check("H7_MapsAndTreeCosaveRoundTrip", cosaveOk, detail);
            StartHContractAfterCosave(player);
            break;
        }

        case Phase::ScenarioH_Contract:
            TickHContract(paused, deltaSec);
            break;

        case Phase::ScenarioH_UninstallPrepare: {
            // H8 (반드시 최후반 — 이후 저장 금지!): 안전 제거 정리 + 다음 프레임 미재오염 확인
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto travel = MapDeviceRuntime::GetTravelStatus(player);
            const bool noTravelOrRun = !travel.kind && !travel.inPackCell && !MapDeviceRuntime::IsActiveMapRun();
            if (WaitForMapTravel(player, noTravelOrRun, kMapReturnWaitSec)) {
                break;
            }
            if (!player || !noTravelOrRun || !IsPlayerReadyInWorld()) {
                Check("H8_SafeUninstallCleanupAndNoReapply", false, "Expected to be outside the pack with no travel before cleanup.");
                FinishAndScheduleQuit("Scenario H cleanup setup failed.");
                break;
            }
            if (!ExerciseUninstallWithMissingForm(player)) {
                Check("H8_SafeUninstallCleanupAndNoReapply", false, "Missing gem with supports, jewel, charm, or inventory form.");
                FinishAndScheduleQuit("Scenario H cleanup inventory setup failed.");
                break;
            }
            EnterPhase(Phase::ScenarioH_UninstallVerifyWait);
            break;
        }

        case Phase::EnemyEsOnly: {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                break;
            }
            const double now = PhaseElapsedSec();
            const double waited = now - g_esTest.stepStarted;
            const auto next = [&](int step) {
                g_esTest.step = step;
                g_esTest.stepStarted = now;
            };
            // 플레이어 직접 피해(작은 값)로 "마지막 교전 적"을 그 대상으로 바꾼다(DealDirectSkillDamage가 기록).
            const auto engage = [](RE::Actor* actor) {
                const bool ok = actor && SkillExecution::DealDirectSkillDamage(actor, 10.0, Core::DamageType::Physical, true, false);
                g_esTest.lastEngageOk = ok;
                return ok;
            };
            const auto view = UiBridge::GetLastEnemyEsViewForTest();
            const auto engagedId = MonsterRuntime::GetLastEngagedEnemy().actorId;
            const auto describe = [&](std::string_view label) {
                return std::format("{}: view visible={}, formId=0x{:08X}, rarity={}, ES {}/{} fill={}, engaged=0x{:08X}, "
                                   "last engage ok={}, after {:.2f}s",
                    label, view.visible, view.formId, static_cast<int>(view.rarity), view.current, view.max, view.fillPermille,
                    engagedId, g_esTest.lastEngageOk, waited);
            };
            const auto failRemaining = [&](std::string_view reason) {
                for (const auto* name : kEnemyEsCheckNames) {
                    if (!g_esTest.reported.contains(name)) {
                        Check(name, false, std::string(reason));
                    }
                }
            };
            const auto report = [&](const char* name, bool ok, const std::string& detail) {
                g_esTest.reported.insert(name);
                Check(name, ok, detail);
            };
            auto a = g_esTest.a.get();
            auto bActor = g_esTest.b.get();
            auto c = g_esTest.c.get();
            const auto shows = [&](const RE::NiPointer<RE::Actor>& actor) {
                return view.visible && actor && view.formId == actor->GetFormID();
            };
            switch (g_esTest.step) {
            case 0:
                // 결정 186: TrueHUD 특수 바를 쓰는 중이면 자체 표시가 숨으므로 이 시험은 의미가 없다.
                if (TrueHudBridge::OwnsSpecialBar()) {
                    failRemaining("TrueHUD owns the special bars, so decision 186 hides the own display; run -Only es with TrueHudSpecialBar off.");
                    FinishAndScheduleQuit("Enemy-ES run skipped because TrueHUD owns the special bars.");
                    break;
                }
                // 조준점(화면 가운데)에 걸리지 않게 좌우와 뒤에 세운다.
                g_esTest.a = SpawnNpcInFrontOfPlayer(player, kEncSkeeverId, 0.0f, 260.0f, 2500.0f, false);
                g_esTest.b = SpawnNpcInFrontOfPlayer(player, kEncSkeeverId, 0.0f, -260.0f, 2500.0f, false);
                g_esTest.c = SpawnNpcInFrontOfPlayer(player, kEncSkeeverId, -260.0f, 0.0f, 2500.0f, false);
                next(1);
                break;
            case 1: {
                const bool loaded = a && bActor && c && a->Is3DLoaded() && bActor->Is3DLoaded() && c->Is3DLoaded();
                if (!loaded) {
                    if (waited >= 8.0) {
                        failRemaining("The three test actors did not load within 8 s.");
                        FinishAndScheduleQuit("Enemy-ES run could not load its test actors.");
                    }
                    break;
                }
                // 스스로 움직이거나 공격해 "마지막 교전 적"을 바꾸지 않게 AI를 끈다(ES6은 시험이 직접 공격을 넣음).
                for (auto* actor : { a.get(), bActor.get(), c.get() }) {
                    actor->EnableAI(false);
                }
                // A와 B는 희귀도·최대 ES가 같고 같은 피해를 받아, 화면 값에서 다른 것은 대상(FormID)뿐이다(ES2).
                const auto esMask = static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::EnergyShielded));
                MonsterRuntime::ForcePromoteForTest(a.get(), Core::MonsterRarity::Rare, esMask, kEnemyEsTestMaxEs);
                MonsterRuntime::ForcePromoteForTest(bActor.get(), Core::MonsterRarity::Rare, esMask, kEnemyEsTestMaxEs);
                MonsterRuntime::ForcePromoteForTest(c.get(), Core::MonsterRarity::Normal, 0, -1.0);  // 프로필 없음(ES 0)
                (void)engage(a.get());
                next(2);
                break;
            }
            case 2: {
                const bool ok = shows(a) && view.rarity == Core::MonsterRarity::Rare &&
                    view.max == static_cast<int>(kEnemyEsTestMaxEs) && view.current > 0 && view.current < view.max &&
                    view.fillPermille > 0 && view.fillPermille < 1000;
                if (!ok && waited < kEnemyEsWaitSec) {
                    break;
                }
                g_esTest.viewAfterA = view;
                report("ES1_EngagedRareTarget", ok, describe("rare A after a player hit"));
                (void)engage(bActor.get());
                next(3);
                break;
            }
            case 3: {
                // B는 A와 같은 값이라 FormID가 바뀌었는지만으로 전환을 판정한다.
                const bool sameValues = view.rarity == g_esTest.viewAfterA.rarity && view.current == g_esTest.viewAfterA.current &&
                    view.max == g_esTest.viewAfterA.max && view.fillPermille == g_esTest.viewAfterA.fillPermille;
                const bool ok = shows(bActor) && sameValues;
                if (!ok && waited < kEnemyEsWaitSec) {
                    break;
                }
                report("ES2_SwitchToSameEsTarget", ok,
                    std::format("{}; same values as A={}", describe("B with A's values after hitting B"), sameValues));
                (void)engage(c.get());
                next(4);
                break;
            }
            case 4: {
                const bool engagedC = c && engagedId == c->GetFormID();
                const bool ok = !view.visible && engagedC;
                if (!ok && waited < kEnemyEsWaitSec) {
                    break;
                }
                report("ES3_NoEsEngagedHides", ok, describe("no-ES C engaged last"));
                (void)engage(a.get());
                g_esTest.incomingTries = 0;
                next(5);
                break;
            }
            case 5: {
                // ES6: A를 보이게 한 뒤 B가 플레이어를 치면(회피되면 다시 침) 마지막 교전 적이 B로 바뀌어야 한다.
                if (!g_esTest.incomingStarted) {
                    if (!shows(a) && waited < kEnemyEsWaitSec) {
                        break;
                    }
                    g_esTest.incomingStarted = true;
                    g_esTest.shownBeforeIncoming = shows(a);
                }
                const bool switched = bActor && engagedId == bActor->GetFormID() && shows(bActor);
                if (!switched && g_esTest.incomingTries < 10 && waited < 6.0) {
                    if (bActor && waited >= 0.3 * (g_esTest.incomingTries + 1)) {
                        ++g_esTest.incomingTries;
                        CallDealDamage(bActor.get(), player, nullptr, false);
                    }
                    break;
                }
                report("ES6_IncomingHitTarget", g_esTest.shownBeforeIncoming && switched,
                    std::format("{}; A shown before={}, incoming hits sent={}",
                        describe("B after hitting the player"), g_esTest.shownBeforeIncoming, g_esTest.incomingTries));
                // ES7 준비: A에 점화를 걸고 B를 마지막 교전 적으로 둔다.
                if (a) {
                    Core::DamagePacket firePkt{};
                    firePkt.byType[static_cast<std::size_t>(Core::DamageType::Fire)] = { 120.0, 120.0 };
                    (void)AilmentRuntime::ApplyPlayerHitAilments(
                        a.get(), firePkt, true, AilmentRuntime::HitKind::Attack, 100.0, false, 100.0, 100.0);
                }
                (void)engage(bActor.get());
                g_esTest.igniteSeen = false;
                next(6);
                break;
            }
            case 6: {
                // ES7: A의 점화 틱(지속 피해)이 이어지는 동안에도 마지막 교전 적과 표시는 B로 남아야 한다.
                if (a) {
                    if (const auto ail = AilmentRuntime::GetStateCopy(a->GetFormID()); ail && ail->igniteRemaining > 0.0) {
                        g_esTest.igniteSeen = true;
                    }
                }
                if (waited < 1.5) {
                    if (!(bActor && engagedId == bActor->GetFormID())) {
                        g_esTest.dotStoleEngagement = true;
                    }
                    break;
                }
                const bool ok = g_esTest.igniteSeen && !g_esTest.dotStoleEngagement && shows(bActor) &&
                    bActor && engagedId == bActor->GetFormID();
                report("ES7_DamageOverTimeKeepsTarget", ok,
                    std::format("{}; ignite on A seen={}, engagement moved during ticks={}",
                        describe("B while A burns"), g_esTest.igniteSeen, g_esTest.dotStoleEngagement));
                (void)engage(a.get());
                next(7);
                break;
            }
            case 7: {
                const bool shownA = shows(a);
                if (!shownA && waited < kEnemyEsWaitSec) {
                    break;
                }
                if (!shownA) {
                    report("ES4_DeadTargetHides", false, describe("A was not shown before the kill"));
                    (void)engage(bActor.get());
                    next(9);
                    break;
                }
                a->KillImmediate();
                next(8);
                break;
            }
            case 8: {
                const bool ok = !view.visible;
                if (!ok && waited < kEnemyEsWaitSec) {
                    break;
                }
                report("ES4_DeadTargetHides", ok, describe("after killing the shown A"));
                (void)engage(bActor.get());
                next(9);
                break;
            }
            case 9:
                // 마지막 교전 뒤 3초에는 아직 보여야 한다. 시각은 교전 때부터 계속 잰다(단계 시작 시각을 바꾸지 않음).
                if (waited < 3.0) {
                    break;
                }
                g_esTest.visibleAt3s = shows(bActor);
                g_esTest.step = 10;
                break;
            case 10: {
                // 결정 185의 유지 시간 5초가 지나면 숨어야 한다(제품 상수가 아니라 결정값으로 판정).
                if (view.visible && waited < 8.0) {
                    break;
                }
                const bool hiddenInWindow = !view.visible && waited >= kEnemyEsExpectedHoldSec - 0.2 &&
                    waited <= kEnemyEsExpectedHoldSec + 1.0;
                report("ES5_EngagedHoldExpires", g_esTest.visibleAt3s && hiddenInWindow,
                    std::format("{}; visible at 3 s={}", describe("B after the hold"), g_esTest.visibleAt3s));
                failRemaining("Not reached.");
                FinishAndScheduleQuit("Enemy-ES investigation run finished.");
                break;
            }
            default:
                break;
            }
            break;
        }

        case Phase::ScenarioH_UninstallVerifyWait: {
            if (g_unpausedAccumSec < 1.0 && PhaseElapsedSec() < 4.0) {
                break;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            const auto st = SaveLoad::GetStateCopy();
            bool anySlotOccupied = false;
            for (const auto& s : st.slots) {
                if (s.occupied) anySlotOccupied = true;
            }
            const bool charmsEmpty = std::ranges::none_of(st.charmSlots, [](const auto& slot) { return slot.occupied; });
            const bool bagCountsMatch = VerifyUninstallBagCounts(player);
            bool anyProxySpellLeft = false;
            if (player) {
                for (const auto fid : FormIds::kProxySpells) {
                    if (auto* sp = FormIds::Lookup<RE::SpellItem>(fid); sp && player->HasSpell(sp)) {
                        anyProxySpellLeft = true;
                    }
                }
                for (const auto fid : FormIds::kProxyPowers) {
                    if (auto* pw = FormIds::Lookup<RE::SpellItem>(fid); pw && player->HasSpell(pw)) {
                        anyProxySpellLeft = true;
                    }
                }
            }
            const float permHpAfter = (player && player->AsActorValueOwner())
                ? player->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth) : 0.0f;
            const float permMpAfter = (player && player->AsActorValueOwner())
                ? player->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kMagicka) : 0.0f;
            const float var09After = (player && player->AsActorValueOwner())
                ? player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kVariable09) : -1.0f;
            const float expectedHp = g_hPermHpBeforeUninstall - static_cast<float>(g_hHpDeltaBeforeUninstall);
            const float expectedMp = g_hPermMpBeforeUninstall - static_cast<float>(g_hMpDeltaBeforeUninstall);
            const bool hpRevertedAndHeld = std::abs(permHpAfter - expectedHp) < 1.5f;
            const bool mpRevertedAndHeld = std::abs(permMpAfter - expectedMp) < 1.5f;

            Check("H8_SafeUninstallCleanupAndNoReapply",
                g_hUninstallResultOk && bagCountsMatch &&
                MapDeviceRuntime::IsUninstallCleanupArmed() && !MapDeviceRuntime::GetUninstallCleanupRemaining(st) &&
                !anySlotOccupied && st.socketedJewels.empty() && charmsEmpty && (st.activeAuraMask == 0u) &&
                !anyProxySpellLeft && (std::abs(st.appliedHealthDelta) < 0.01) &&
                (std::abs(st.appliedMagickaDelta) < 0.01) && hpRevertedAndHeld && mpRevertedAndHeld &&
                (std::abs(var09After) < 1e-3f),
                std::format("resultOk={}, bagCounts={}, armed={}, slotsEmpty={}, jewelsEmpty={}, charmsEmpty={}, proxyLeft={}, hp={:.1f}(exp {:.1f}), mp={:.1f}(exp {:.1f}), hpDelta={:.2f}, mpDelta={:.2f}, var09={:.2f}",
                    g_hUninstallResultOk, bagCountsMatch, MapDeviceRuntime::IsUninstallCleanupArmed(),
                    !anySlotOccupied, st.socketedJewels.empty(), charmsEmpty, anyProxySpellLeft,
                    permHpAfter, expectedHp, permMpAfter, expectedMp,
                    st.appliedHealthDelta, st.appliedMagickaDelta, var09After));

            FinishAndScheduleQuit(std::format("All Stage 2-6 AutoTest scenarios (A-H, {} checks) completed.", kExpectedAutoTestChecks));
            break;
        }

        default:
            break;
        }
    }
}
