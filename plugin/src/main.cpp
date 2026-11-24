#include "PCH.h"

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/Config.h"
#include "core/ContentCheck.h"
#include "core/RulesPack.h"
#include "core/DataPack.h"
#include "core/EnergyShield.h"
#include "core/EspFormCheck.h"
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
#include "runtime/MapPackRuntime.h"
#include "runtime/ModState.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/ProjectileRuntime.h"
#include "runtime/ProxySpellRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"
#include "runtime/TrueHudBridge.h"
#include "runtime/UiBridge.h"
#include "runtime/WorldDropTracker.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <bcrypt.h>
#include <fstream>
#include <limits>

namespace
{
    namespace fs = std::filesystem;
    using namespace SkyrimPoE;

    constexpr auto kConfigPath = "Data/SKSE/Plugins/SkyrimPoE.ini";
    constexpr auto kManifestPath = "Data/SKSE/Plugins/SkyrimPoE/manifest.json";
    constexpr std::array<std::string_view, 5> kContentPaths{
        "SKSE/Plugins/SkyrimPoE/packs/core_3_30.json",
        "SKSE/Plugins/SkyrimPoE/packs/tree_3_30.json",
        "SKSE/Plugins/SkyrimPoE/packs/items_3_30.json",
        "SKSE/Plugins/SkyrimPoE/packs/rules.json",
        Runtime::FormIds::kPluginName
    };

    std::optional<Core::RulesPack> g_activeRules;
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
    bool g_serializationInstalled = false;
    bool g_autoTestLoadStarted = false;

    void PreserveStartup(std::string reason)
    {
        SKSE::log::error("SkyrimPoE startup: {}", reason);
        Runtime::SaveLoad::SetContentCheck(std::nullopt, std::move(reason));
    }

    std::string ReadContentFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto length = static_cast<std::streamoff>(file.tellg());
        if (length < 0 || length > std::numeric_limits<ULONG>::max()) {
            throw std::runtime_error("could not open file or determine a supported size");
        }
        std::string bytes(static_cast<std::size_t>(length), '\0');
        if (!file.seekg(0).read(bytes.data(), static_cast<std::streamsize>(length)) ||
            file.peek() != std::char_traits<char>::eof() || file.bad()) {
            throw std::runtime_error("could not read the entire file");
        }
        return bytes;
    }

    std::string Sha256(std::string& bytes)
    {
        BCRYPT_ALG_HANDLE provider = nullptr;
        if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            throw std::runtime_error("could not open SHA-256 provider");
        }
        const auto closeProvider = [](void* handle) { BCryptCloseAlgorithmProvider(handle, 0); };
        const std::unique_ptr<void, decltype(closeProvider)> algorithm(provider, closeProvider);
        BCRYPT_HASH_HANDLE handle = nullptr;
        if (BCryptCreateHash(algorithm.get(), &handle, nullptr, 0, nullptr, 0, 0) != 0) {
            throw std::runtime_error("could not initialize SHA-256");
        }
        const std::unique_ptr<void, decltype(&BCryptDestroyHash)> hash(handle, &BCryptDestroyHash);
        std::array<UCHAR, 32> digest{};
        if (BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(bytes.data()),
                static_cast<ULONG>(bytes.size()), 0) != 0 ||
            BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
            throw std::runtime_error("could not compute SHA-256");
        }
        std::string hex;
        hex.reserve(digest.size() * 2);
        for (const auto byte : digest) {
            std::format_to(std::back_inserter(hex), "{:02x}", byte);
        }
        return hex;
    }

    std::optional<std::uint32_t> ReadEspGeneration(std::vector<std::string>& reasons)
    {
        constexpr RE::FormID localId = 0xC00;
        constexpr std::string_view editorId = "SPOE_ContentGeneration";
        auto* data = RE::TESDataHandler::GetSingleton();
        const auto* plugin = data ? data->LookupModByName(Runtime::FormIds::kPluginName) : nullptr;
        if (!plugin || plugin->GetCompileIndex() == 0xFF) {
            reasons.emplace_back("SkyrimPoE.esp is not loaded.");
            return std::nullopt;
        }
        const auto* form = data->LookupForm(localId, Runtime::FormIds::kPluginName);
        for (const auto* global : data->GetFormArray<RE::TESGlobal>()) {
            if (global && global != form && global->GetFile(0) == plugin &&
                editorId == global->GetFormEditorID()) {
                reasons.emplace_back("SkyrimPoE.esp: SPOE_ContentGeneration GLOB must use local FormID 0xC00.");
                break;
            }
        }
        if (!form) {
            return std::nullopt;  // A missing marker is the generation-zero format.
        }
        const auto* marker = form->As<RE::TESGlobal>();
        if (!marker || form->GetFile(0) != plugin || form->IsDeleted() ||
            editorId != marker->GetFormEditorID() || marker->type.get() != RE::TESGlobal::Type::kShort ||
            !std::isfinite(marker->value) || marker->value < 0 ||
            marker->value > std::numeric_limits<std::int16_t>::max() || std::trunc(marker->value) != marker->value) {
            reasons.emplace_back("SkyrimPoE.esp: invalid SPOE_ContentGeneration GLOB at 0xC00.");
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(marker->value);
    }

    std::optional<Core::SaveGeneration> LoadAndCheckContent() try
    {
        std::vector<std::string> reasons;
        g_activeRules.reset();
        std::array<nlohmann::json, kContentPaths.size() - 1> packMeta;
        std::array<Core::ContentFile, kContentPaths.size()> files;
        const auto load = [&](std::size_t index, auto& pack) {
            const auto path = kContentPaths[index];
            files[index].path = path;
            try {
                auto bytes = ReadContentFile(fs::path("Data") / path);
                try {
                    files[index].sha256 = Sha256(bytes);
                } catch (const std::exception& e) {
                    reasons.push_back(std::format("{}: {}", path, e.what()));
                }
                std::string error;
                const bool loaded = pack.LoadFromJsonString(bytes, &error);
                if (!loaded) {
                    reasons.push_back(std::format("{}: pack load failed: {}", path, error));
                }
                const auto root = nlohmann::json::parse(bytes, nullptr, false);
                if (const auto meta = root.find("meta"); meta != root.end()) {
                    packMeta[index] = *meta;
                }
                return loaded;
            } catch (const std::exception& e) {
                reasons.push_back(std::format("{}: {}", path, e.what()));
                return false;
            }
        };
        if (load(0, g_dataPack)) {
            SKSE::log::info("DataPack loaded: gems={}.", g_dataPack.AllGems().size());
        }
        if (load(1, g_treeData)) {
            SKSE::log::info("PassiveTreeData loaded: classes={}, nodes={}, masteryEffects={}.",
                g_treeData.Classes().size(), g_treeData.NodeCount(), g_treeData.MasteryEffectCount());
        }
        if (load(2, g_itemData)) {
            SKSE::log::info("ItemData loaded: affixes={}, currencies={}, belts={}, charms={}, monsterMods={}, uniques={}.",
                g_itemData.AllAffixes().size(), g_itemData.AllCurrencies().size(), g_itemData.AllBelts().size(),
                g_itemData.AllCharms().size(), g_itemData.AllMonsterMods().size(), g_itemData.AllUniques().size());
        }
        Core::RulesPack rules;
        (void)load(3, rules);

        files.back().path = kContentPaths.back();
        try {
            auto bytes = ReadContentFile(fs::path("Data") / files.back().path);
            files.back().sha256 = Sha256(bytes);
        } catch (const std::exception& e) {
            reasons.push_back(std::format("{}: {}", files.back().path, e.what()));
        }
        nlohmann::json manifest;
        try {
            manifest = nlohmann::json::parse(ReadContentFile(kManifestPath));
        } catch (const std::exception& e) {
            reasons.push_back(std::format("{}: {}", kManifestPath, e.what()));
        }
        const auto espGeneration = ReadEspGeneration(reasons);
        const auto check = Core::CheckContent(packMeta, manifest, files, espGeneration);
        reasons.insert(reasons.end(), check.reasons.begin(), check.reasons.end());
        if (!reasons.empty()) {
            for (const auto& reason : reasons) {
                SKSE::log::error("Content check failed: {}", reason);
            }
            PreserveStartup(std::format("콘텐츠 검사 실패. {} 자세한 이유는 SkyrimPoE.log를 확인하세요.", reasons.front()));
            return std::nullopt;
        }
        const auto& identity = *check.identity;
        g_activeRules.emplace(std::move(rules));
        SKSE::log::info("Content check passed: {} {}, generation {}, build {}.",
            identity.game, identity.gameVersion, identity.contentGeneration, identity.buildId);
        return Core::SaveGeneration{
            identity.game, identity.contentGeneration, identity.gameVersion, identity.buildId, SKSE::GetPluginVersion().string(".")
        };
    } catch (const std::exception& e) {
        PreserveStartup(std::format("콘텐츠 검사 중 오류가 발생했습니다. {}", e.what()));
        return std::nullopt;
    } catch (...) {
        PreserveStartup("콘텐츠 검사 중 알 수 없는 오류가 발생했습니다. SkyrimPoE.log를 확인하세요.");
        return std::nullopt;
    }

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

    std::string VerifyEspForms()
    {
        using namespace Runtime::FormIds;
        auto* data = RE::TESDataHandler::GetSingleton();
        std::vector<Core::EspFormObservation> observations;
        const auto check = [&](RE::FormType expectedType, std::string_view name, RE::FormID localId) {
            // The typed lookup returns null for both an absent form and a type mismatch.
            const auto* form = data ? data->LookupForm(localId, kPluginName) : nullptr;
            const auto* owner = form ? form->GetFile(0) : nullptr;
            observations.push_back({ name, localId, form != nullptr, form && form->GetFormType() == expectedType,
                owner && _stricmp(std::string(owner->GetFilename()).c_str(), std::string(kPluginName).c_str()) == 0,
                form && form->IsDeleted() });
        };

        check(RE::EffectSetting::FORMTYPE, "SPOE_ProxySpellEffect", kProxySpellEffect);
        check(RE::EffectSetting::FORMTYPE, "SPOE_ProxyPowerEffect", kProxyPowerEffect);

        for (const auto fid : kProxySpells) {
            check(RE::SpellItem::FORMTYPE, "SPOE_ProxySpell", fid);
        }
        for (const auto fid : kProxyPowers) {
            check(RE::SpellItem::FORMTYPE, "SPOE_ProxyPower", fid);
        }
        for (const auto& entry : kStage2GemForms) {
            check(RE::TESObjectMISC::FORMTYPE, entry.gemId, entry.localFormId);
        }
        check(RE::BGSExplosion::FORMTYPE, "SPOE_Vfx_IceNova", kVfxIceNova);
        check(RE::BGSExplosion::FORMTYPE, "SPOE_Vfx_ShockNova", kVfxShockNova);

        // 4단계 폼(화폐 오브 12종 + 봉인 토큰 80종 + 빛기둥 마커 2종)
        for (const auto fid : kCurrencyOrbForms) {
            check(RE::TESObjectMISC::FORMTYPE, "SPOE_Orb", fid);
        }
        for (std::size_t off = 0; off < kSealedTokenCount; ++off) {
            const auto fid = kSealedTokenBase + static_cast<RE::FormID>(off);
            check(RE::TESObjectMISC::FORMTYPE, "SPOE_Tok", fid);
        }
        check(RE::TESObjectSTAT::FORMTYPE, "SPOE_LootBeam_Rare", kLootBeamRare);
        check(RE::TESObjectSTAT::FORMTYPE, "SPOE_LootBeam_Currency", kLootBeamCurrency);

        // 5단계 폼(벨트 5 + 벨트 토큰 10 + 주얼 3 + 참 6 + 투사체 3 + 폭발 VFX 1 + 고유 토큰 1 + 고유 빛기둥 1)
        for (const auto fid : kBeltBaseForms) {
            check(RE::TESObjectMISC::FORMTYPE, "SPOE_Belt", fid);
        }
        for (std::size_t off = 0; off < kBeltTokenCount; ++off) {
            const auto fid = kBeltTokenBase + static_cast<RE::FormID>(off);
            check(RE::TESObjectMISC::FORMTYPE, "SPOE_Tok_Belt", fid);
        }
        for (const auto fid : kJewelBaseForms) {
            check(RE::TESObjectMISC::FORMTYPE, "SPOE_Jewel", fid);
        }
        for (const auto fid : kCharmForms) {
            check(RE::TESObjectMISC::FORMTYPE, "SPOE_Charm", fid);
        }
        check(RE::BGSProjectile::FORMTYPE, "SPOE_Proj_Fireball", kProjFireball);
        check(RE::BGSProjectile::FORMTYPE, "SPOE_Proj_FreezingPulse", kProjFreezingPulse);
        check(RE::BGSProjectile::FORMTYPE, "SPOE_Proj_Spark", kProjSpark);
        check(RE::BGSExplosion::FORMTYPE, "SPOE_Vfx_FireBurst", kVfxFireBurst);
        check(RE::TESObjectMISC::FORMTYPE, "SPOE_Tok_Unique", kUniqueSealedToken);
        check(RE::TESObjectSTAT::FORMTYPE, "SPOE_LootBeam_Unique", kLootBeamUnique);

        // 6단계 폼(지도 T1..T16 16종 + 4차 전직 공물 1종 + 지도 빛기둥 1종 = 18종, 누적 192폼 ESL)
        for (const auto fid : kMapItemForms) {
            check(RE::TESObjectMISC::FORMTYPE, "SPOE_Map", fid);
        }
        check(RE::TESObjectMISC::FORMTYPE, "SPOE_Tribute_Ascendancy4", kAscendancy4Tribute);
        check(RE::TESObjectSTAT::FORMTYPE, "SPOE_LootBeam_Map", kLootBeamMap);

        auto result = Core::CheckEspForms(observations);
        SKSE::log::info("{} form verification: {} found, {} missing. {} failed (type={}, owner={}, deleted={}).",
            kPluginName, result.found, result.missing, result.failedForms, result.wrongType, result.wrongOwner, result.deleted);
        return std::move(result.reason);
    }

    void OnDataLoaded() try
    {
        // 규칙 팩을 검증한 뒤 팩 기본값에 INI 설정을 적용한다.
        auto generation = LoadAndCheckContent();
        const auto cfgRes = Core::LoadConfigFromFile(kConfigPath);
        g_config = cfgRes.config;
        SKSE::log::info("Config loaded from '{}' (fileFound={}, budgetMs={:.2f}, giveStarterGems={}).",
            kConfigPath, cfgRes.fileFound, g_config.mainUpdateBudgetMs, g_config.giveStarterGems);
        for (const auto& w : cfgRes.warnings) {
            SKSE::log::warn("Config warning: {}", w);
        }

        if (generation && g_serializationInstalled) {
            if (auto reason = VerifyEspForms(); !reason.empty()) {
                PreserveStartup(std::move(reason));
                generation.reset();
            } else {
                Runtime::MapPackRuntime::Initialize();
            }
        }

        // 4. 주 갱신 훅을 먼저 설치해 CommandQueue 드레인을 보장한 뒤 전투·스킬·처치·아이템 훅을 건다.
        const bool hookInstalled = Runtime::MainUpdateHook::Install(&g_commandQueue, &g_statEngine, &g_dataPack, &g_config);
        if (!hookInstalled) {
            SKSE::log::error("MainUpdateHook installation failed. Combat/skill hooks skipped.");
        }

        if (!generation) {
            return;
        }
        if (!g_serializationInstalled || !hookInstalled) {
            PreserveStartup(!g_serializationInstalled ?
                "SKSE 직렬화 콜백을 등록하지 못했습니다." : "주 갱신 훅을 설치하지 못했습니다.");
            return;
        }

        Runtime::SkillExecution::Initialize(&g_killTracker, &g_statEngine);
        Runtime::EquipmentSync::Initialize(&g_itemData, &g_charmSystem);
        Runtime::CraftingRuntime::Initialize(&g_itemData, &g_dataPack, &g_treeData, &g_config, &g_charmSystem, &g_statEngine);
        Runtime::WorldDropTracker::Initialize(&g_itemData, &g_config);
        Runtime::MapDeviceRuntime::Initialize(
            &g_commandQueue, &g_statEngine, &g_itemData, &g_treeData, &g_config, &g_energyShield, &g_charmSystem, cfgRes.mapTravel);
        Runtime::MapDeviceRuntime::InstallReturnDoorSink();
        Runtime::MonsterRuntime::Initialize(&g_statEngine, &g_itemData, &g_dataPack, &g_config);
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
        Runtime::TrueHudBridge::Initialize(&g_config);  // 결정 184 ④·186: 선택 연동(기본 끔)
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

        Runtime::SaveLoad::SetContentCheck(std::move(generation), "콘텐츠 검사와 런타임 초기화를 마쳤습니다.");
        Runtime::SaveLoad::SyncStateToStatEngine();
        SKSE::log::info("SkyrimPoE Stage 6 runtime initialization complete.");
    } catch (const std::exception& e) {
        PreserveStartup(std::format("런타임 초기화 중 오류가 발생했습니다. {}", e.what()));
    } catch (...) {
        PreserveStartup("런타임 초기화 중 알 수 없는 오류가 발생했습니다. SkyrimPoE.log를 확인하세요.");
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
            case SKSE::MessagingInterface::kPreLoadGame: {
                const bool notifyAutoTest = Runtime::ModState::AllowsGameplay() || Runtime::AutoTest::IsCleanupOnly();
                Runtime::SaveLoad::OnGameLoadStarted();
                Runtime::MapDeviceRuntime::DisarmUninstallCleanup();
                Runtime::UiBridge::ClosePanelOnMainThread();
                (void)g_commandQueue.BumpGenerationAndClear();
                g_autoTestLoadStarted = notifyAutoTest;
                if (g_autoTestLoadStarted) {
                    Runtime::AutoTest::OnPreLoadGame(*msg);
                }
                break;
            }
            case SKSE::MessagingInterface::kNewGame:
                Runtime::SaveLoad::SchedulePostLoadOrNewGameInit(true);
                break;
            case SKSE::MessagingInterface::kPostLoadGame: {
                const bool notifyAutoTest = std::exchange(g_autoTestLoadStarted, false) || Runtime::AutoTest::IsCleanupOnly();
                Runtime::SaveLoad::OnGameLoadFinished(msg->data != nullptr);
                if (notifyAutoTest) {
                    Runtime::AutoTest::OnPostLoadGame(msg->data != nullptr);
                }
                break;
            }
            case SKSE::MessagingInterface::kSaveGame:
                if (Runtime::ModState::AllowsGameplay() || Runtime::AutoTest::IsCleanupOnly()) {
                    Runtime::AutoTest::OnSaveGame(*msg);
                }
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

const SkyrimPoE::Core::RulesPack* SkyrimPoE::Core::GetRulesPack() noexcept
{
    return g_activeRules ? &*g_activeRules : nullptr;
}

SkyrimPoE::Core::EnergyShieldPool& SkyrimPoE::Core::GetPlayerEnergyShield() noexcept
{
    return g_energyShield;
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
        g_serializationInstalled = Runtime::SaveLoad::Install(
            &g_commandQueue, &g_statEngine, &g_dataPack, &g_treeData, &g_config, &g_itemData, &g_charmSystem);
    } catch (const std::exception& e) {
        SKSE::log::error("SkyrimPoE startup exception: {}", e.what());
    } catch (...) {
        REX::W32::OutputDebugStringA("SkyrimPoE: unknown startup exception.\n");
    }
    return true;
}
