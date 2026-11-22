#include "PCH.h"

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/Config.h"
#include "core/DataPack.h"
#include "core/EnergyShield.h"
#include "core/ItemData.h"
#include "core/KillTracker.h"
#include "core/PassiveTreeData.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"
#include "runtime/Addresses.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/AutoTest.h"
#include "runtime/CombatHooks.h"
#include "runtime/CraftingRuntime.h"
#include "runtime/CycloneRuntime.h"
#include "runtime/DeathListener.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/ItemTracker.h"
#include "runtime/MainUpdateHook.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/ProjectileRuntime.h"
#include "runtime/ProxySpellRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"
#include "runtime/UiBridge.h"
#include "runtime/WorldDropTracker.h"

#include <spdlog/sinks/basic_file_sink.h>

namespace
{
    namespace fs = std::filesystem;
    using namespace SkyrimPoE;

    constexpr auto kConfigPath = "Data/SKSE/Plugins/SkyrimPoE.ini";
    constexpr auto kDataPackPath = "Data/SKSE/Plugins/SkyrimPoE/packs/core_3_30.json";
    constexpr auto kTreePackPath = "Data/SKSE/Plugins/SkyrimPoE/packs/tree_3_30.json";
    constexpr auto kItemsPackPath = "Data/SKSE/Plugins/SkyrimPoE/packs/items_3_30.json";

    Core::Config g_config;
    Core::DataPack g_dataPack;
    Core::PassiveTreeData g_treeData;
    Core::ItemData g_itemData;
    Core::CommandQueue g_commandQueue;
    Core::StatEngine g_statEngine;
    Core::EnergyShieldPool g_energyShield;
    Core::SkillSystem g_skillSystem;
    Core::CharmSystem g_charmSystem;
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

        // 4단계 폼(화폐 오브 12종 + 봉인 토큰 80종 + 빛기둥 마커 2종)
        for (const auto fid : Runtime::FormIds::kCurrencyOrbForms) {
            check(Runtime::FormIds::Lookup<RE::TESObjectMISC>(fid), "SPOE_Orb", fid);
        }
        for (std::size_t off = 0; off < Runtime::FormIds::kSealedTokenCount; ++off) {
            const auto fid = Runtime::FormIds::kSealedTokenBase + static_cast<RE::FormID>(off);
            check(Runtime::FormIds::Lookup<RE::TESObjectMISC>(fid), "SPOE_Tok", fid);
        }
        check(Runtime::FormIds::Lookup<RE::TESObjectSTAT>(Runtime::FormIds::kLootBeamRare),
            "SPOE_LootBeam_Rare", Runtime::FormIds::kLootBeamRare);
        check(Runtime::FormIds::Lookup<RE::TESObjectSTAT>(Runtime::FormIds::kLootBeamCurrency),
            "SPOE_LootBeam_Currency", Runtime::FormIds::kLootBeamCurrency);

        // 5단계 폼(벨트 5 + 벨트 토큰 10 + 주얼 3 + 참 6 + 투사체 3 + 폭발 VFX 1 + 고유 토큰 1 + 고유 빛기둥 1)
        for (const auto fid : Runtime::FormIds::kBeltBaseForms) {
            check(Runtime::FormIds::Lookup<RE::TESObjectMISC>(fid), "SPOE_Belt", fid);
        }
        for (std::size_t off = 0; off < Runtime::FormIds::kBeltTokenCount; ++off) {
            const auto fid = Runtime::FormIds::kBeltTokenBase + static_cast<RE::FormID>(off);
            check(Runtime::FormIds::Lookup<RE::TESObjectMISC>(fid), "SPOE_Tok_Belt", fid);
        }
        for (const auto fid : Runtime::FormIds::kJewelBaseForms) {
            check(Runtime::FormIds::Lookup<RE::TESObjectMISC>(fid), "SPOE_Jewel", fid);
        }
        for (const auto fid : Runtime::FormIds::kCharmForms) {
            check(Runtime::FormIds::Lookup<RE::TESObjectMISC>(fid), "SPOE_Charm", fid);
        }
        check(Runtime::FormIds::Lookup<RE::BGSProjectile>(Runtime::FormIds::kProjFireball),
            "SPOE_Proj_Fireball", Runtime::FormIds::kProjFireball);
        check(Runtime::FormIds::Lookup<RE::BGSProjectile>(Runtime::FormIds::kProjFreezingPulse),
            "SPOE_Proj_FreezingPulse", Runtime::FormIds::kProjFreezingPulse);
        check(Runtime::FormIds::Lookup<RE::BGSProjectile>(Runtime::FormIds::kProjSpark),
            "SPOE_Proj_Spark", Runtime::FormIds::kProjSpark);
        check(Runtime::FormIds::Lookup<RE::BGSExplosion>(Runtime::FormIds::kVfxFireBurst),
            "SPOE_Vfx_FireBurst", Runtime::FormIds::kVfxFireBurst);
        check(Runtime::FormIds::Lookup<RE::TESObjectMISC>(Runtime::FormIds::kUniqueSealedToken),
            "SPOE_Tok_Unique", Runtime::FormIds::kUniqueSealedToken);
        check(Runtime::FormIds::Lookup<RE::TESObjectSTAT>(Runtime::FormIds::kLootBeamUnique),
            "SPOE_LootBeam_Unique", Runtime::FormIds::kLootBeamUnique);

        // 6단계 폼(지도 T1..T16 16종 + 4차 전직 공물 1종 + 지도 빛기둥 1종 = 18종, 누적 192폼 ESL)
        for (const auto fid : Runtime::FormIds::kMapItemForms) {
            check(Runtime::FormIds::Lookup<RE::TESObjectMISC>(fid), "SPOE_Map", fid);
        }
        check(Runtime::FormIds::Lookup<RE::TESObjectMISC>(Runtime::FormIds::kAscendancy4Tribute),
            "SPOE_Tribute_Ascendancy4", Runtime::FormIds::kAscendancy4Tribute);
        check(Runtime::FormIds::Lookup<RE::TESObjectSTAT>(Runtime::FormIds::kLootBeamMap),
            "SPOE_LootBeam_Map", Runtime::FormIds::kLootBeamMap);

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

        // 2c. 4~5단계 아이템·어픽스·화폐·벨트·참·몬스터 속성·고유 데이터 로드
        std::string itemsError;
        if (g_itemData.LoadFromFile(kItemsPackPath, &itemsError)) {
            SKSE::log::info("ItemData loaded: affixes={}, currencies={}, belts={}, charms={}, monsterMods={}, uniques={}.",
                g_itemData.AllAffixes().size(), g_itemData.AllCurrencies().size(),
                g_itemData.AllBelts().size(), g_itemData.AllCharms().size(),
                g_itemData.AllMonsterMods().size(), g_itemData.AllUniques().size());
        } else {
            SKSE::log::error("ItemData load failed ('{}'): {}. Item crafting disabled.",
                kItemsPackPath, itemsError);
        }

        // 3. ESP 폼 검증
        VerifyEspForms();

        // 4. 주 갱신 훅을 먼저 설치해 CommandQueue 드레인을 보장한 뒤 전투·스킬·처치·아이템 훅을 건다.
        if (!Runtime::MainUpdateHook::Install(&g_commandQueue, &g_statEngine, &g_dataPack, &g_config)) {
            SKSE::log::error("MainUpdateHook installation failed. Combat/skill hooks skipped.");
            return;
        }

        Runtime::SkillExecution::Initialize(&g_killTracker, &g_statEngine);
        Runtime::EquipmentSync::Initialize(&g_itemData, &g_charmSystem);
        Runtime::CraftingRuntime::Initialize(&g_itemData, &g_dataPack, &g_treeData, &g_config, &g_charmSystem, &g_statEngine);
        Runtime::WorldDropTracker::Initialize(&g_itemData, &g_config);
        Runtime::MapDeviceRuntime::Initialize(
            &g_commandQueue, &g_statEngine, &g_itemData, &g_treeData, &g_config, &g_energyShield, &g_charmSystem);
        Runtime::MonsterRuntime::Initialize(&g_statEngine, &g_itemData, &g_config);
        Runtime::AilmentRuntime::Initialize(&g_statEngine, &g_charmSystem, &g_dataPack, &g_config);
        (void)Runtime::GemInventory::Install(&g_commandQueue);
        (void)Runtime::ItemTracker::Install(&g_commandQueue, []() {
            Runtime::EquipmentSync::MarkDirty();
        });
        (void)Runtime::DeathListener::Install(&g_killTracker, &g_commandQueue, &g_config, &g_statEngine, &g_charmSystem);
        (void)Runtime::CombatHooks::Install(
            &g_commandQueue, &g_statEngine, &g_energyShield, &g_killTracker, &g_dataPack, &g_config, &g_skillSystem, &g_charmSystem);
        (void)Runtime::CycloneRuntime::Install(
            &g_commandQueue, &g_statEngine, &g_skillSystem, &g_dataPack, &g_config);
        (void)Runtime::ProjectileRuntime::Install(
            &g_commandQueue, &g_statEngine, &g_skillSystem, &g_dataPack, &g_config);
        (void)Runtime::ProxySpellRuntime::Install(
            &g_commandQueue, &g_statEngine, &g_skillSystem, &g_dataPack, &g_config);
        (void)Runtime::UiBridge::Install(
            &g_commandQueue, &g_statEngine, &g_energyShield, &g_dataPack, &g_treeData, &g_itemData, &g_config,
            &g_skillSystem, &g_charmSystem);
        Runtime::AutoTest::Install(
            &g_commandQueue, &g_statEngine, &g_energyShield, &g_killTracker, &g_dataPack, &g_treeData, &g_itemData, &g_config,
            &g_skillSystem, &g_charmSystem);

        Runtime::SaveLoad::AddRevertCallback([]() {
            Runtime::MonsterRuntime::ResetOnRevert();
            Runtime::AilmentRuntime::ResetOnRevert();
            Runtime::ProjectileRuntime::ResetOnRevert();
            Runtime::ProxySpellRuntime::ResetOnRevert();
        });

        // 주 갱신 스레드 프레임 콜백 등록: 지도 장치 + 장비 동기화 + 바닥 드랍/빛기둥 + 몬스터/상태이상/투사체/대리주문 + 풀/ES/참 + 사이클론·CoC + HUD/UI + 자동 시험
        Runtime::MainUpdateHook::AddFrameCallback([](RE::Main* main, double dt) {
            auto* ui = RE::UI::GetSingleton();
            const bool paused = ui && ui->GameIsPaused();
            // 재검수 R-9: 맡겨 둔 SpeedMult 원복은 안전 제거 뒤에도 계속 다시 시도한다(우리가 건 수정치만 되돌리므로 재오염이 아님).
            Runtime::SaveLoad::RetryPendingSpeedReverts(dt, paused);
            if (!Runtime::MapDeviceRuntime::IsUninstallCleanupArmed()) {
                Runtime::MapDeviceRuntime::Tick(dt, paused);
                Runtime::EquipmentSync::FlushIfDirty();
                Runtime::WorldDropTracker::Tick(dt, paused);
                Runtime::MonsterRuntime::Tick(dt, paused);
                Runtime::AilmentRuntime::Tick(dt, paused);
                Runtime::ProjectileRuntime::Tick(dt, paused);
                Runtime::ProxySpellRuntime::Tick(dt, paused);
                Runtime::CombatHooks::TickPlayerPools(dt, paused);
                Runtime::CycloneRuntime::Tick(dt, paused);
            }
            Runtime::UiBridge::Tick(main);
            Runtime::AutoTest::Tick(main, dt);
        });

        Runtime::SaveLoad::SyncStateToStatEngine();
        SKSE::log::info("SkyrimPoE Stage 6 runtime initialization complete.");
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
                Runtime::MapDeviceRuntime::DisarmUninstallCleanup();
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
        (void)Runtime::SaveLoad::Install(
            &g_commandQueue, &g_statEngine, &g_dataPack, &g_treeData, &g_config, &g_itemData, &g_charmSystem);
    } catch (const std::exception& e) {
        SKSE::log::error("SkyrimPoE startup exception: {}", e.what());
    } catch (...) {
        REX::W32::OutputDebugStringA("SkyrimPoE: unknown startup exception.\n");
    }
    return true;
}
