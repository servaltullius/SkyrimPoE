#include "PCH.h"

#include "core/CommandQueue.h"
#include "core/Config.h"
#include "core/DataPack.h"
#include "core/EnergyShield.h"
#include "core/KillTracker.h"
#include "core/PassiveTreeData.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"
#include "runtime/Addresses.h"
#include "runtime/AutoTest.h"
#include "runtime/CombatHooks.h"
#include "runtime/CycloneRuntime.h"
#include "runtime/DeathListener.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/MainUpdateHook.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"
#include "runtime/UiBridge.h"

#include <spdlog/sinks/basic_file_sink.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace SkyrimPoE;

    constexpr auto kConfigPath = "Data/SKSE/Plugins/SkyrimPoE.ini";
    constexpr auto kDataPackPath = "Data/SKSE/Plugins/SkyrimPoE/packs/core_3_30.json";
    constexpr auto kTreePackPath = "Data/SKSE/Plugins/SkyrimPoE/packs/tree_3_30.json";

    Core::Config g_config;
    Core::DataPack g_dataPack;
    Core::PassiveTreeData g_treeData;
    Core::CommandQueue g_commandQueue;
    Core::StatEngine g_statEngine;
    Core::EnergyShieldPool g_energyShield;
    Core::SkillSystem g_skillSystem;
    Core::KillTracker g_killTracker;

    void SetupLogging()
    {
        auto logDir = SKSE::log::log_directory();
        if (!logDir) {
            return;
        }
        std::error_code ec;
        fs::create_directories(*logDir, ec);
        const auto logPath = *logDir / "SkyrimPoE.log";
        const auto prevPath = *logDir / "SkyrimPoE.previous.log";
        if (fs::exists(logPath, ec)) {
            fs::rename(logPath, prevPath, ec);
        }

        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
        auto logger = std::make_shared<spdlog::logger>("global log", std::move(sink));
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(std::move(logger));
    }

    void VerifyEspForms()
    {
        std::size_t found = 0;
        std::size_t missing = 0;

        auto check = [&](auto* ptr, const char* label, RE::FormID localId) {
            if (ptr) {
                ++found;
            } else {
                ++missing;
                SKSE::log::warn("ESP form missing: {} (0x{:06X} in {}).", label, localId, Runtime::FormIds::kPluginName);
            }
        };

        check(Runtime::FormIds::Lookup<RE::EffectSetting>(Runtime::FormIds::kProxySpellEffect),
            "SPOE_ProxySpellEffect", Runtime::FormIds::kProxySpellEffect);
        check(Runtime::FormIds::Lookup<RE::EffectSetting>(Runtime::FormIds::kProxyPowerEffect),
            "SPOE_ProxyPowerEffect", Runtime::FormIds::kProxyPowerEffect);

        for (std::size_t i = 0; i < Runtime::FormIds::kProxySpells.size(); ++i) {
            check(Runtime::FormIds::Lookup<RE::SpellItem>(Runtime::FormIds::kProxySpells[i]),
                "SPOE_ProxySpell", Runtime::FormIds::kProxySpells[i]);
        }
        for (std::size_t i = 0; i < Runtime::FormIds::kProxyPowers.size(); ++i) {
            check(Runtime::FormIds::Lookup<RE::SpellItem>(Runtime::FormIds::kProxyPowers[i]),
                "SPOE_ProxyPower", Runtime::FormIds::kProxyPowers[i]);
        }
        for (const auto& entry : Runtime::FormIds::kStage2GemForms) {
            check(Runtime::FormIds::Lookup<RE::TESObjectMISC>(entry.localFormId),
                entry.gemId.data(), entry.localFormId);
        }
        check(Runtime::FormIds::Lookup<RE::BGSExplosion>(Runtime::FormIds::kVfxIceNova),
            "SPOE_Vfx_IceNova", Runtime::FormIds::kVfxIceNova);
        check(Runtime::FormIds::Lookup<RE::BGSExplosion>(Runtime::FormIds::kVfxShockNova),
            "SPOE_Vfx_ShockNova", Runtime::FormIds::kVfxShockNova);

        SKSE::log::info("SkyrimPoE.esp form verification: {} found, {} missing.", found, missing);
    }

    void OnDataLoaded()
    {
        // 1. INI 설정 로드
        const auto cfgRes = Core::LoadConfigFromFile(kConfigPath);
        g_config = cfgRes.config;
        SKSE::log::info("Config loaded from '{}' (fileFound={}, budgetMs={:.2f}, giveStarterGems={}).",
            kConfigPath, cfgRes.fileFound, g_config.mainUpdateBudgetMs, g_config.giveStarterGems);
        for (const auto& w : cfgRes.warnings) {
            SKSE::log::warn("Config warning: {}", w);
        }

        // 2. 데이터 팩 로드
        std::string packError;
        if (!g_dataPack.LoadFromFile(kDataPackPath, &packError)) {
            SKSE::log::error("DataPack load failed ('{}'): {}. Runtime hooks disabled.", kDataPackPath, packError);
            return;
        }
        SKSE::log::info("DataPack loaded: gems={}, lifeBase={:.0f}, manaBase={:.0f}.",
            g_dataPack.AllGems().size(),
            g_dataPack.Constants().lifeBase,
            g_dataPack.Constants().manaBase);

        // 2b. 3단계 패시브 트리 데이터 로드
        std::string treeError;
        if (g_treeData.LoadFromFile(kTreePackPath, &treeError)) {
            SKSE::log::info("PassiveTreeData loaded: classes={}, nodes={}, masteryEffects={}.",
                g_treeData.Classes().size(), g_treeData.NodeCount(), g_treeData.MasteryEffectCount());
        } else {
            SKSE::log::error("PassiveTreeData load failed ('{}'): {}. Passive tree UI disabled.",
                kTreePackPath, treeError);
        }

        // 3. ESP 폼 검증
        VerifyEspForms();

        // 4. 주 갱신 훅을 먼저 설치해 CommandQueue 드레인을 보장한 뒤 전투·스킬·처치 훅을 건다.
        if (!Runtime::MainUpdateHook::Install(&g_commandQueue, &g_statEngine, &g_dataPack, &g_config)) {
            SKSE::log::error("MainUpdateHook installation failed. Combat/skill hooks skipped.");
            return;
        }

        Runtime::SkillExecution::Initialize(&g_killTracker);
        (void)Runtime::GemInventory::Install(&g_commandQueue);
        (void)Runtime::DeathListener::Install(&g_killTracker, &g_commandQueue, &g_config);
        (void)Runtime::CombatHooks::Install(
            &g_commandQueue, &g_statEngine, &g_energyShield, &g_killTracker, &g_dataPack, &g_config);
        (void)Runtime::CycloneRuntime::Install(
            &g_commandQueue, &g_statEngine, &g_skillSystem, &g_dataPack, &g_config);
        (void)Runtime::UiBridge::Install(
            &g_commandQueue, &g_statEngine, &g_energyShield, &g_dataPack, &g_treeData, &g_config);
        Runtime::AutoTest::Install(
            &g_commandQueue, &g_statEngine, &g_energyShield, &g_killTracker, &g_dataPack, &g_treeData, &g_config);

        // 주 갱신 스레드 프레임 콜백 등록: 플레이어 풀/ES 재충전 + 사이클론·CoC 틱 + HUD/UI 감시 + (마커 존재 시) 자동 시험
        Runtime::MainUpdateHook::AddFrameCallback([](RE::Main* main, double dt) {
            auto* ui = RE::UI::GetSingleton();
            const bool paused = ui && ui->GameIsPaused();
            Runtime::CombatHooks::TickPlayerPools(dt, paused);
            Runtime::CycloneRuntime::Tick(dt, paused);
            Runtime::UiBridge::Tick(main);
            Runtime::AutoTest::Tick(main, dt);
        });

        Runtime::SaveLoad::SyncStateToStatEngine();
        SKSE::log::info("SkyrimPoE Stage 3 runtime initialization complete.");
    }

    void OnSkseMessage(SKSE::MessagingInterface::Message* msg)
    {
        if (!msg) {
            return;
        }
        try {
            switch (msg->type) {
            case SKSE::MessagingInterface::kDataLoaded:
                OnDataLoaded();
                break;
            case SKSE::MessagingInterface::kPreLoadGame:
                Runtime::UiBridge::ClosePanelOnMainThread();
                (void)g_commandQueue.BumpGenerationAndClear();
                Runtime::AutoTest::OnPreLoadGame();
                break;
            case SKSE::MessagingInterface::kNewGame:
                Runtime::SaveLoad::SchedulePostLoadOrNewGameInit(true);
                break;
            case SKSE::MessagingInterface::kPostLoadGame:
                if (msg->data != nullptr) {
                    Runtime::SaveLoad::SchedulePostLoadOrNewGameInit(false);
                }
                Runtime::AutoTest::OnPostLoadGame(msg->data != nullptr);
                break;
            case SKSE::MessagingInterface::kSaveGame:
                Runtime::AutoTest::OnSaveGame(*msg);
                break;
            default:
                break;
            }
        } catch (const std::exception& e) {
            SKSE::log::error("SKSE message {} handling exception: {}", msg->type, e.what());
        } catch (...) {
            SKSE::log::error("SKSE message {} handling unknown exception.", msg->type);
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    try {
        SetupLogging();
        // M5~M9 후속 훅을 포함해 여유 있는 1024바이트 트램펄린을 SKSE::Init에서 통합 확보한다.
        SKSE::Init(skse, { .log = false, .trampoline = true, .trampolineSize = 1024 });

        const auto runtimeVer = skse->RuntimeVersion();
        SKSE::log::info("SkyrimPoE {} starting on Skyrim runtime {}.",
            SKSE::GetPluginVersion().string("."),
            runtimeVer.string("."));

        if (!Runtime::Addresses::IsVerifiedRuntime(runtimeVer)) {
            SKSE::log::warn("Unverified Skyrim runtime {} (Stage 2 first slice supports AE 1.6.1170 only; decision 141). No hooks installed.",
                runtimeVer.string("."));
            return true;
        }

        const auto addressLibrary = fs::path("Data/SKSE/Plugins") /
            std::format("versionlib-{}.bin", runtimeVer.string());
        if (!fs::is_regular_file(addressLibrary)) {
            SKSE::log::error("Address Library file '{}' not found. No hooks installed.", addressLibrary.string());
            return true;
        }

        const auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging || !messaging->RegisterListener(OnSkseMessage)) {
            SKSE::log::error("SKSE MessagingInterface registration failed.");
            return true;
        }

        // SKSE 직렬화 콜백은 SKSEPluginLoad 단계에서 등록한다.
        (void)Runtime::SaveLoad::Install(&g_commandQueue, &g_statEngine, &g_dataPack, &g_treeData, &g_config);
    } catch (const std::exception& e) {
        SKSE::log::error("SkyrimPoE startup exception: {}", e.what());
    } catch (...) {
        REX::W32::OutputDebugStringA("SkyrimPoE: unknown startup exception.\n");
    }
    return true;
}
