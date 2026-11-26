#include "PCH.h"
#include "runtime/UiBridge.h"
#include "runtime/CraftingRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/ItemTracker.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/ModState.h"
#include "runtime/MapPackRuntime.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/PrismaUIApi.h"
#include "runtime/ProxySpellRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SimpleImeBridge.h"
#include "runtime/SkillExecution.h"
#include "runtime/TrueHudBridge.h"

#include "core/ContentFeatures.h"
#include "core/EnemyEsHud.h"
#include "core/GemTransaction.h"
#include "core/ItemCrafting.h"
#include "core/ItemFormatter.h"
#include "core/KillTracker.h"
#include "core/MapSystem.h"
#include "core/PassiveTree.h"
#include "core/Config.h"
#include "core/RulesPack.h"
#include "core/UiBuildCheck.h"

#include <algorithm>
#include <bitset>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        using json = nlohmann::json;
        using Clock = std::chrono::steady_clock;
        namespace UiBuildCheck = Core::UiBuildCheck;

        constexpr auto kViewPath = "SkyrimPoE/index.html";

        // 지도 팩 목록 파일의 던전 유형 키(tools/dungeon-clone/dungeons.json의 type)를 화면 문구로 바꾼다.
        // 모르는 유형은 키를 그대로 보인다.
        [[nodiscard]] std::string MapDungeonTypeLabel(std::string_view type)
        {
            static constexpr std::pair<std::string_view, std::string_view> kLabels[] = {
                { "mine", "광산" },
                { "cave", "동굴" },
                { "nordic_barrow", "노르드 고분" },
                { "dwemer_ruin", "드웨머 유적" },
                { "grove", "숲" },
            };
            for (const auto& [key, label] : kLabels) {
                if (key == type) {
                    return std::string(label);
                }
            }
            return std::string(type);
        }
        constexpr auto kKoLangPath = "Data/SKSE/Plugins/SkyrimPoE/lang/ko-KR.json";
        constexpr auto kEnLangPath = "Data/SKSE/Plugins/SkyrimPoE/lang/en-US.json";
        constexpr auto kJsCommandListener = "poeCommand";
        constexpr auto kJsUpdateHud = "poeUpdateHud";
        constexpr auto kJsUpdateEnemyEs = "poeUpdateEnemyEs";
        constexpr auto kJsUpdatePanel = "poeUpdatePanel";
        constexpr auto kJsOnPanelOpen = "poeOnPanelOpen";
        constexpr auto kJsOnPanelClose = "poeOnPanelClose";
        constexpr auto kJsToast = "poeToast";
        constexpr auto kJsImeText = "poeImeText";
        constexpr auto kJsImeKey = "poeImeKey";
        constexpr auto kJsBlurSearch = "poeBlurSearch";
        constexpr std::uint32_t kEscapeScanCode = 0x01;
        constexpr auto kViewRetryCooldown = std::chrono::seconds(5);
        constexpr std::uint8_t kTabSkills = 0;
        constexpr std::uint8_t kTabTree = 1;
        constexpr std::uint8_t kTabGear = 2;
        constexpr std::uint8_t kTabMaps = 3;

        std::string_view TabNameFromId(std::uint8_t tab) noexcept
        {
            switch (tab) {
                case kTabTree: return "tree";
                case kTabGear: return "gear";
                case kTabMaps: return "maps";
                default:       return "skills";
            }
        }

        std::uint8_t TabIdFromName(std::string_view name) noexcept
        {
            if (name == "tree") return kTabTree;
            if (name == "gear") return kTabGear;
            if (name == "maps") return kTabMaps;
            return kTabSkills;
        }

        const char* EquipSlotLabelKo(Core::EquipSlot slot) noexcept
        {
            switch (slot) {
                case Core::EquipSlot::MainHand: return "주무기";
                case Core::EquipSlot::OffHand:  return "보조장비";
                case Core::EquipSlot::Helmet:   return "투구";
                case Core::EquipSlot::Body:     return "갑옷";
                case Core::EquipSlot::Gloves:   return "장갑";
                case Core::EquipSlot::Boots:    return "신발";
                case Core::EquipSlot::Amulet:   return "목걸이";
                case Core::EquipSlot::Ring1:    return "반지 1";
                case Core::EquipSlot::Ring2:    return "반지 2";
                case Core::EquipSlot::Belt:     return "벨트";
                default:                        return "장비";
            }
        }

        const char* TokenSlotGroupLabelKo(Core::TokenSlotGroup g) noexcept
        {
            switch (g) {
                case Core::TokenSlotGroup::OneHandWeapon:         return "한손 무기";
                case Core::TokenSlotGroup::TwoHandOrRangedWeapon: return "양손·원거리";
                case Core::TokenSlotGroup::Shield:                return "방패";
                case Core::TokenSlotGroup::Helmet:                return "투구";
                case Core::TokenSlotGroup::Body:                  return "갑옷";
                case Core::TokenSlotGroup::Gloves:                return "장갑";
                case Core::TokenSlotGroup::Boots:                 return "신발";
                case Core::TokenSlotGroup::Jewelry:               return "장신구";
                case Core::TokenSlotGroup::Belt:                  return "벨트";
                case Core::TokenSlotGroup::Jewel:                 return "주얼";
                case Core::TokenSlotGroup::Map:                   return "지도";
                default:                                          return "장비";
            }
        }

        const char* RarityStr(Core::ItemRarity r) noexcept
        {
            switch (r) {
                case Core::ItemRarity::Magic:  return "magic";
                case Core::ItemRarity::Rare:   return "rare";
                case Core::ItemRarity::Unique: return "unique";
                default:                       return "normal";
            }
        }

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        Core::EnergyShieldPool* g_esPool = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        const Core::Config* g_config = nullptr;
        Core::SkillSystem* g_skillSystem = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;
        bool g_installed = false;

        PRISMA_UI_API::IVPrismaUI1* g_api = nullptr;
        PRISMA_UI_API::PrismaView g_view = 0;
        std::atomic_bool g_domReady{ false };
        Clock::time_point g_lastCreateAttempt{};
        bool g_hasTriedCreate = false;

        // 주 갱신/메인 스레드에서만 읽고 쓰는 패널·포커스·탭 상태
        std::atomic_bool g_panelOpen{ false };
        std::atomic<std::uint8_t> g_activeTab{ kTabSkills };
        std::atomic_bool g_searchFocused{ false };
        bool g_acquiredFocus = false;
        std::atomic_bool g_hudShown{ false };
        bool g_inputHooked = false;
        std::bitset<256> g_hiddenKeys;
        std::atomic_bool g_needCloseOnMain{ false };
        std::atomic_bool g_needResetOnMain{ false };
        std::atomic<std::uint64_t> g_lastProcessedReqId{ 0 };
        bool g_uiBuildMismatch = false;
        std::atomic<std::uint64_t> g_gearGateRejections{ 0 };
        std::atomic<std::uint64_t> g_gearGatePasses{ 0 };
        // 받은 buildId, 받은 uiProtocol, 기대 buildId. 기대 프로토콜은 실행 중 고정이다.
        std::set<std::tuple<std::string, std::string, std::string>> g_loggedUiBuildMismatches;
        constexpr std::size_t kMaxLoggedUiBuildMismatches = 32;
        bool g_uiBuildMismatchOverflowLogged = false;

        // 직전 HUD 스냅샷(값이 실제로 바뀔 때만 JS로 푸시해 유휴 프레임 비용 0 유지)
        struct CachedHud
        {
            bool valid = false;
            int esCurrent = -1;
            int esMax = -1;
            std::uint32_t exileLevel = 0;
            std::uint64_t exileXp = 0;
            std::uint64_t exileReqXp = 0;
            std::int32_t unallocatedPts = -1;
            std::uint32_t spiritReserved = 0;
            std::uint32_t spiritMax = 0;
            int guardShield = 0;
            int guardMax = 0;
            std::size_t hhStacks = 0;
            std::uint8_t charmUnlocked = 0;
            std::uint8_t charmActiveMask = 0;
            std::array<int, Core::kMaxCharmSlots> charmCharges{ -1, -1, -1 };
            std::array<int, Core::kMaxCharmSlots> charmKinds{ -1, -1, -1 };
            bool mapActive = false;
            bool mapIsArena4 = false;
            bool mapInCell = false;
            bool mapBossKilled = false;
            bool mapArena4Unlocked = false;
            std::uint8_t mapTier = 0;
            std::uint8_t mapPortalsRem = 0;
            std::uint8_t mapArenaPhase = 1;
            std::uint32_t mapAreaLevel = 0;
        };
        CachedHud g_lastHud;

        // 결정 185: 직전에 보낸 적 ES 표시 값과 이름(바뀔 때만 JS로 보냄)
        struct CachedEnemyEs
        {
            bool valid = false;
            Core::EnemyEsView view{};
            std::string name;
        };
        CachedEnemyEs g_lastEnemyEs;

        // 표시 후보로 쓸 수 있는 액터인지 보고, 적인지와 몬스터 프로필의 최대 ES를 붙인다(주 갱신 스레드).
        Core::EnemyEsCandidate MakeEnemyEsCandidate(RE::Actor* actor, RE::PlayerCharacter* player)
        {
            Core::EnemyEsCandidate candidate;
            if (!actor || !player || actor == player || actor->IsDead() || actor->IsDisabled() || actor->IsDeleted() ||
                !actor->Is3DLoaded()) {
                return candidate;
            }
            candidate.formId = actor->GetFormID();
            candidate.alive = true;
            // 적: 아군(동료·소환·지배)이 아니고, 처치 보상과 같은 기준으로 적대(지도 몬스터는 교전 전에도 적, 결정 181)
            candidate.enemy = !SkillExecution::IsAllyOfPlayer(player, actor) &&
                SkillExecution::IsHostileForFirstHitReward(actor, player);
            if (const auto profile = MonsterRuntime::GetProfileCopy(candidate.formId)) {
                candidate.maxEs = profile->maxEsSkyrim;
            }
            return candidate;
        }

        // 결정 185: 조준점의 살아 있는 적(ES가 없으면 숨김), 없으면 마지막 교전 적(5초)의 ES를 화면 위 가운데에 보여 준다.
        // 자체 패널이 열려 있거나 TrueHUD 특수 바를 쓰는 동안(결정 186)에는 숨긴다. 값이나 이름이 바뀔 때만 보낸다.
        void PushEnemyEsIfChanged(bool panelOpen)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            RE::NiPointer<RE::Actor> target;
            Core::EnemyEsView view{};
            if (!panelOpen && player && !TrueHudBridge::OwnsSpecialBar()) {
                RE::NiPointer<RE::Actor> crosshairActor;
                if (const auto* pick = RE::CrosshairPickData::GetSingleton()) {
                    if (const auto ref = pick->GetActiveTarget().get()) {
                        crosshairActor = RE::NiPointer<RE::Actor>(ref->As<RE::Actor>());
                    }
                }
                const auto crosshairCandidate = MakeEnemyEsCandidate(crosshairActor.get(), player);
                // 교전 기록이 아직 유효할 때만 폼을 찾는다(만료 뒤 매 프레임 조회하지 않음).
                const auto engaged = MonsterRuntime::GetLastEngagedEnemy();
                RE::NiPointer<RE::Actor> engagedActor;
                Core::EnemyEsCandidate engagedCandidate;
                if (engaged.actorId != 0 && Core::IsEnemyEsEngagementRecent(engaged.ageSec) &&
                    !Core::IsEnemyEsTargetable(crosshairCandidate)) {
                    engagedActor = RE::NiPointer<RE::Actor>(RE::TESForm::LookupByID<RE::Actor>(engaged.actorId));
                    engagedCandidate = MakeEnemyEsCandidate(engagedActor.get(), player);
                }
                const auto chosen = Core::SelectEnemyEsTarget(crosshairCandidate, engagedCandidate, engaged.ageSec);
                if (chosen != 0) {
                    target = chosen == crosshairCandidate.formId ? crosshairActor : engagedActor;
                    if (const auto profile = MonsterRuntime::GetProfileCopy(chosen)) {
                        view = Core::MakeEnemyEsView(chosen, profile->rarity, profile->currentEsSkyrim, profile->maxEsSkyrim);
                    }
                }
            }
            std::string name;
            if (view.visible && target) {
                const char* displayName = target->GetDisplayFullName();
                name = displayName ? displayName : "";
            }
            if (g_lastEnemyEs.valid && g_lastEnemyEs.view == view && g_lastEnemyEs.name == name) {
                return;
            }
            g_lastEnemyEs.valid = true;
            g_lastEnemyEs.view = view;
            g_lastEnemyEs.name = name;

            json payload{ { "visible", view.visible } };
            if (view.visible) {
                payload["name"] = name;
                payload["rarity"] = static_cast<int>(view.rarity);
                payload["current"] = view.current;
                payload["max"] = view.max;
                payload["fillPermille"] = view.fillPermille;
            }
            const std::string text = payload.dump(-1, ' ', false, json::error_handler_t::replace);
            g_api->InteropCall(g_view, kJsUpdateEnemyEs, text.c_str());
        }
        std::optional<MapTravelStatus> g_panelMapTravel;

        std::string_view MapTravelLabel(Core::MapTravelKind kind)
        {
            using Core::MapTravelKind;
            switch (kind) {
            case MapTravelKind::OpenMap: return "지도 입장";
            case MapTravelKind::OpenArena: return "결투장 입장";
            case MapTravelKind::Reenter: return "지도 재입장";
            case MapTravelKind::Return: return "포털 귀환";
            case MapTravelKind::Abandon: return "회차 포기";
            case MapTravelKind::DeathEscape: return "사망 귀환";
            case MapTravelKind::CancelledRun: return "취소 후 귀환";
            case MapTravelKind::SafeEscape: return "안전 탈출";
            }
            return {};
        }

        // 언어 팩 이름 매핑(ko-KR 우선, en-US 폴백)
        std::unordered_map<std::string, std::string> g_gemDisplayNames;
        std::unordered_map<std::string, std::string> g_classDisplayNames;

        void LoadLangNames()
        {
            g_gemDisplayNames.clear();
            g_classDisplayNames.clear();

            auto tryLoad = [](const char* path) -> bool {
                std::ifstream ifs(path, std::ios::binary);
                if (!ifs) {
                    return false;
                }
                try {
                    const auto root = json::parse(ifs);
                    if (root.contains("gems") && root["gems"].is_object()) {
                        for (const auto& [k, v] : root["gems"].items()) {
                            if (v.is_string()) {
                                g_gemDisplayNames[k] = v.get<std::string>();
                            } else if (v.is_object() && v.contains("name") && v["name"].is_string()) {
                                g_gemDisplayNames[k] = v["name"].get<std::string>();
                            }
                        }
                    }
                    if (root.contains("classes") && root["classes"].is_object()) {
                        for (const auto& [k, v] : root["classes"].items()) {
                            if (v.is_string()) {
                                g_classDisplayNames[k] = v.get<std::string>();
                            }
                        }
                    }
                    return true;
                } catch (...) {
                    return false;
                }
            };

            if (!tryLoad(kKoLangPath)) {
                (void)tryLoad(kEnLangPath);
            }
        }

        std::string DisplayGemName(const std::string& gemId)
        {
            if (const auto it = g_gemDisplayNames.find(gemId); it != g_gemDisplayNames.end()) {
                return it->second;
            }
            if (g_dataPack) {
                if (const auto* def = g_dataPack->FindGem(gemId, SaveLoad::WorldGemGeneration())) {
                    return def->name;
                }
            }
            return gemId;
        }

        std::string DisplayClassName(const std::string& className)
        {
            if (const auto it = g_classDisplayNames.find(className); it != g_classDisplayNames.end()) {
                return it->second;
            }
            return className;
        }

        const char* GemKindStr(Core::GemKind kind) noexcept
        {
            switch (kind) {
                case Core::GemKind::AttackChannel: return "attack";
                case Core::GemKind::Trigger:       return "trigger";
                case Core::GemKind::Spell:         return "spell";
                case Core::GemKind::Support:       return "support";
                case Core::GemKind::Aura:          return "aura";
                case Core::GemKind::Herald:        return "herald";
                case Core::GemKind::Guard:         return "guard";
                default:                           return "unknown";
            }
        }

        bool ViewUsable() noexcept
        {
            return g_api && g_view != 0 && g_api->IsValid(g_view);
        }

        bool IsInGameplay()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded() || player->IsDead()) {
                return false;
            }
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return false;
            }
            if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::FaderMenu::MENU_NAME)) {
                return false;
            }
            return true;
        }

        // 결정 120: 제작·캐릭터 생성·대화·책·일시정지 메뉴·텍스트 입력(검색창) 등 다른 UI가 활성이면 패널 열기 및 HUD 노출을 막는다.
        bool IsOtherBlockingMenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return false;
            }
            if (ui->GameIsPaused()) {
                return true;
            }
            // 검수 C1: 자체 SimpleImeBridge가 연 AllowTextInput 1건은 차단 메뉴로 오인하지 않는다.
            const auto allowedSelfEntries = SimpleImeBridge::TextEntryActive() ? 1 : 0;
            if (const auto* controls = RE::ControlMap::GetSingleton();
                controls && controls->GetRuntimeData().textEntryCount > allowedSelfEntries) {
                return true;
            }
            static constexpr std::array<std::string_view, 22> kBlockingMenus = {
                RE::MainMenu::MENU_NAME,
                RE::LoadingMenu::MENU_NAME,
                RE::Console::MENU_NAME,
                RE::InventoryMenu::MENU_NAME,
                RE::MagicMenu::MENU_NAME,
                RE::MapMenu::MENU_NAME,
                RE::JournalMenu::MENU_NAME,
                RE::DialogueMenu::MENU_NAME,
                RE::ContainerMenu::MENU_NAME,
                RE::BarterMenu::MENU_NAME,
                RE::FavoritesMenu::MENU_NAME,
                RE::TweenMenu::MENU_NAME,
                RE::StatsMenu::MENU_NAME,
                RE::SleepWaitMenu::MENU_NAME,
                RE::CraftingMenu::MENU_NAME,
                RE::RaceSexMenu::MENU_NAME,
                RE::BookMenu::MENU_NAME,
                RE::LockpickingMenu::MENU_NAME,
                RE::GiftMenu::MENU_NAME,
                RE::TrainingMenu::MENU_NAME,
                RE::MessageBoxMenu::MENU_NAME,
                RE::LevelUpMenu::MENU_NAME,
            };
            for (const auto name : kBlockingMenus) {
                if (ui->IsMenuOpen(name)) {
                    return true;
                }
            }
            return false;
        }

        bool IsIgnoredMenuForAutoClose(std::string_view menuName) noexcept
        {
            return menuName.empty() ||
                menuName == RE::HUDMenu::MENU_NAME ||
                menuName == RE::CursorMenu::MENU_NAME ||
                menuName == RE::FaderMenu::MENU_NAME ||
                menuName == RE::MistMenu::MENU_NAME ||
                menuName == SimpleImeBridge::kMenuName ||
                menuName == SimpleImeBridge::kSimpleImeMenuName ||
                menuName.find("Prisma") != std::string_view::npos;
        }

        void OnImeText(const char* utf8Text)
        {
            if (!utf8Text || !g_queue || !g_panelOpen.load(std::memory_order_acquire)) {
                return;
            }
            const auto generation = g_queue->CurrentGeneration();
            std::string txt(utf8Text);
            g_queue->EnqueueForGeneration(generation, [txt = std::move(txt)]() {
                if (!ViewUsable() || !g_domReady.load(std::memory_order_acquire) ||
                    !g_panelOpen.load(std::memory_order_acquire)) {
                    return;
                }
                const std::string payload = json{ { "text", txt } }.dump(-1, ' ', false, json::error_handler_t::replace);
                g_api->InteropCall(g_view, kJsImeText, payload.c_str());
            });
        }

        void OnImeKey(const char* keyName)
        {
            if (!keyName || !g_queue || !g_panelOpen.load(std::memory_order_acquire)) {
                return;
            }
            const auto generation = g_queue->CurrentGeneration();
            std::string key(keyName);
            g_queue->EnqueueForGeneration(generation, [key = std::move(key)]() {
                if (!ViewUsable() || !g_domReady.load(std::memory_order_acquire) ||
                    !g_panelOpen.load(std::memory_order_acquire)) {
                    return;
                }
                const std::string payload = json{ { "key", key } }.dump();
                g_api->InteropCall(g_view, kJsImeKey, payload.c_str());
            });
        }

        class PlayerGemAdapter final : public Core::IGemInventoryAdapter
        {
        public:
            explicit PlayerGemAdapter(RE::PlayerCharacter* player) noexcept
                : player_(player)
            {
            }

            std::optional<Core::GemInstance> TakeGem(
                const Core::InventoryItemKey& key,
                std::string_view fallbackGemId,
                std::uint8_t defaultSockets,
                std::uint64_t& nextInstanceId) override
            {
                return GemInventory::TakeGemFromPlayerInventory(
                    player_, key, fallbackGemId, defaultSockets, nextInstanceId);
            }

            std::optional<Core::InventoryItemKey> ReturnGem(Core::GemInstance gem) override
            {
                return GemInventory::ReturnGemToPlayerInventory(player_, std::move(gem));
            }

        private:
            RE::PlayerCharacter* player_ = nullptr;
        };

        json SerializeComputedStats(const Core::ComputedSkillStats& cs, const Core::StatSnapshot& snap,
            bool triggered = false)
        {
            const auto lifeCost = snap.bloodMagic && !snap.poe2Passives && cs.poeLifeCost == 0.0
                ? cs.poeManaCost : cs.poeLifeCost;
            const auto cooldown = cs.kind == Core::GemKind::Guard ? std::max(4.0, cs.cooldownSec)
                : triggered ? std::max(Core::SkillSystem::kMinTriggerCooldownSec, cs.cooldownSec) : cs.cooldownSec;
            return json{
                { "avgDamage", std::round(cs.baseHitDamage.TotalAverage() * 10.0) / 10.0 },
                { "critPct", std::round(cs.EffectiveCritChancePct() * 10.0) / 10.0 },
                { "manaCost", snap.bloodMagic ? 0.0 : std::round(cs.poeManaCost * 10.0) / 10.0 },
                { "lifeCost", std::round(lifeCost * 10.0) / 10.0 },
                { "areaMult", std::round(cs.areaMult * 100.0) / 100.0 },
                { "cooldownSec", cooldown },
                { "spiritCost", cs.spiritCost },
                { "auraActive", cs.auraActive },
                { "requirementMet", cs.requirementMet },
                { "projectileCount", cs.projectileCount },
                { "chainCount", cs.chainCount },
                { "pierceCount", cs.pierceCount },
                { "forkEnabled", cs.forkEnabled },
                { "spellRepeatCount", cs.spellRepeatCount }
            };
        }

        json SerializeGemInstance(const Core::GemInstance& gem, const Core::StatSnapshot* snap, std::uint32_t depth = 0)
        {
            json j;
            j["instanceId"] = gem.instanceId;
            j["gemId"] = gem.gemId;
            j["name"] = DisplayGemName(gem.gemId);
            j["level"] = gem.level;
            j["xp"] = gem.xp;
            const auto gemGeneration = SaveLoad::WorldGemGeneration();
            j["reqXp"] = gemGeneration == 0 ? Core::KillTracker::RequiredXpForNextGemLevel(gem.level) : 0;
            j["reason"] = Core::GemForms::UseReasonText(Core::GemForms::ClassifyUse(gemGeneration, gem.gemId).reason);
            j["socketCount"] = gem.socketCount;
            Core::GemKind kind = Core::GemKind::Unknown;
            if (g_dataPack) {
                if (const auto* def = g_dataPack->FindGem(gem.gemId, gemGeneration)) {
                    kind = def->kind;
                }
            }
            j["kind"] = GemKindStr(kind);

            // 발동 젬 안 주문(IceNova/ShockNova)의 사전 계산 스탯도 함께 직렬화해 UI에 표시한다.
            if (snap) {
                if (const auto it = snap->triggeredSpellStats.find(gem.instanceId); it != snap->triggeredSpellStats.end()) {
                    j["computed"] = SerializeComputedStats(it->second, *snap, true);
                }
            }

            json children = json::array();
            if (depth < 3) {
                for (const auto& sub : gem.socketedGems) {
                    children.push_back(SerializeGemInstance(sub, snap, depth + 1));
                }
            }
            j["socketedGems"] = std::move(children);
            return j;
        }

        json SerializeFormattedCard(const Core::FormattedItemCard& card)
        {
            auto serializeLines = [](const std::vector<Core::FormattedAffixLine>& lines) {
                json arr = json::array();
                for (const auto& a : lines) {
                    arr.push_back(json{
                        { "type", a.type == Core::AffixType::Prefix ? "prefix" : "suffix" },
                        { "tier", a.tier },
                        { "tierBadge", a.tierBadge },
                        { "textKo", a.textKo }
                    });
                }
                return arr;
            };
            return json{
                { "title", card.title },
                { "uniqueId", card.uniqueId },
                { "rarity", RarityStr(card.rarity) },
                { "rarityBadge", card.rarityBadge },
                { "itemLevel", card.itemLevel },
                { "slotLabel", card.slotLabel },
                { "requirementMet", card.requirementMet },
                { "requirementText", card.requirementText },
                { "baseStatLines", card.baseStatLines },
                { "implicitLines", serializeLines(card.implicitLines) },
                { "affixLines", serializeLines(card.affixLines) }
            };
        }

        void BuildCanApplyOrbData(
            const Core::PoEItemRecord* existingRecord,
            Core::TokenSlotGroup slotGroup,
            Core::DefenseSubtype defenseSubtype,
            const std::string& weaponTag,
            bool isAmulet,
            std::uint32_t baseFormId,
            std::uint32_t exileLevel,
            json& outFlags,
            json& outReasons,
            std::uint8_t jewelColor = 0)
        {
            outFlags = json::array();
            outReasons = json::array();
            if (!g_itemData || !g_itemData->IsLoaded()) {
                for (std::size_t i = 0; i < Core::kCurrencyKindCount; ++i) {
                    outFlags.push_back(false);
                    outReasons.push_back("아이템 데이터가 로드되지 않았습니다.");
                }
                return;
            }
            Core::PoEItemRecord tmp;
            const Core::PoEItemRecord* rec = existingRecord;
            if (!rec) {
                tmp.rarity = Core::ItemRarity::Normal;
                std::uint32_t fallbackIlvl = std::clamp<std::uint32_t>(exileLevel, 1u, 100u);
                if (slotGroup == Core::TokenSlotGroup::Map) {
                    const auto localId = FormIds::ResolveLocalFormIdIfOurs(baseFormId);
                    if (const auto tier = FormIds::MapTierFromLocalFormId(localId); tier.has_value()) {
                        fallbackIlvl = Core::MapSystem::MonsterLevelForTier(*tier);
                    }
                }
                tmp.itemLevel = fallbackIlvl;
                tmp.baseFormId = baseFormId;
                tmp.slotGroup = slotGroup;
                tmp.defenseSubtype = defenseSubtype;
                tmp.isAmulet = isAmulet;
                tmp.jewelColor = jewelColor;
                tmp.weaponTag = weaponTag;
                rec = &tmp;
            }
            const auto tags = Core::ItemData::BuildItemTags(
                rec->slotGroup, rec->defenseSubtype, rec->weaponTag, rec->isAmulet, rec->jewelColor);
            for (std::size_t i = 0; i < Core::kCurrencyKindCount; ++i) {
                const auto kind = static_cast<Core::CurrencyKind>(i);
                const auto chk = Core::ItemCrafting::CanApplyOrb(kind, *rec, tags, *g_itemData);
                outFlags.push_back(chk.IsOk());
                outReasons.push_back(chk.IsOk() ? std::string() : chk.message);
            }
        }

        std::string BuildPanelPayloadJson()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !g_statEngine || !g_config) {
                return "{}";
            }

            // 판이 다른 UI의 상태 응답을 만들 때는 기존 스냅샷을 읽는다.
            if (!g_uiBuildMismatch) {
                // 계약(C-1·C-34): 주 갱신 스레드, g_txMutex 미보유 상태에서만 호출한다.
                // 줍기 예약이 실행되지 못한 바닥 스탬프 젬·아이템을 목록/장비 구성 직전에 보완 연결한다.
                (void)GemInventory::RelinkPickedUpGems(player);
                (void)ItemTracker::RelinkPickedUpItems(player);

                // 장비 탭·능력치에 최신 착용 장비 및 제작 변경이 즉시 반영되도록 더티 상태를 플러시하고 스탯을 재계산한다(검수 C3).
                EquipmentSync::FlushIfDirty();
                if (!MapDeviceRuntime::IsUninstallCleanupArmed() && g_dataPack && g_dataPack->IsLoaded()) {
                    (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                }
            }

            const auto state = SaveLoad::GetStateCopy();
            const auto snap = g_statEngine->GetSnapshot();
            const auto inv = GemInventory::ListPlayerGems(player);
            const auto es = g_esPool ? g_esPool->Snapshot() : Core::EnergyShieldState{};
            const auto currencyCounts = CraftingRuntime::CountAllCurrencies(player);

            const bool treeLoaded = g_treeData && g_treeData->IsLoaded();
            const std::int32_t availNormal = treeLoaded
                ? Core::PassiveTree::ComputeAvailableNormalPoints(
                      *g_treeData, state.tree, state.exileLevel, *g_config, state.atlas.completedTierMask)
                : static_cast<std::int32_t>(state.unallocatedPassivePoints);
            const std::int32_t spentNormal = treeLoaded
                ? Core::PassiveTree::ComputeSpentNormalPoints(*g_treeData, state.tree)
                : 0;
            const std::int32_t grantedPts = treeLoaded
                ? Core::PassiveTree::ComputeGrantedPassivePoints(*g_treeData, state.tree)
                : 0;
            const std::int32_t earnedAsc = Core::PassiveTree::ComputeEarnedAscendancyPoints(state.tree, *g_config);
            const std::int32_t spentAsc = treeLoaded
                ? Core::PassiveTree::ComputeSpentAscendancyPoints(*g_treeData, state.tree)
                : 0;
            const std::int32_t availAsc = earnedAsc - spentAsc;

            json root;
            root["activeTab"] = std::string(TabNameFromId(g_activeTab.load(std::memory_order_acquire)));
            root["uiBuildMismatch"] = g_uiBuildMismatch;
            root["simpleImeAvailable"] = SimpleImeBridge::Available();
            root["freePassiveRefund"] = g_config->freePassiveRefund;
            // 7단계 G1(결정 239·248·249): 설치 패키지 세대로 꺼진 기능. UI는 이 값으로 빈 상태와 버튼을 정한다.
            {
                const auto features = Core::ActiveContentFeatures();
                root["contentGeneration"] = Core::ActiveContentGeneration();
                root["contentFeatures"] = {
                    { "crafting", features.crafting }, { "mapEntry", features.mapEntry },
                    { "jewelSocketing", features.jewelSocketing }, { "charms", features.charms },
                    { "itemDrops", features.itemDrops },
                };
            }
            root["exileLevel"] = state.exileLevel;
            root["exileXp"] = state.exileXp;
            root["exileReqXp"] = Core::KillTracker::RequiredXpForNextExileLevel(state.exileLevel, *g_config);
            root["unallocatedPoints"] = availNormal;
            root["className"] = state.tree.classSelected
                ? DisplayClassName(state.className)
                : std::string("미선택");

            json treeJ;
            treeJ["loaded"] = treeLoaded;
            treeJ["generation"] = state.tree.generation;
            treeJ["classSelected"] = state.tree.classSelected;
            treeJ["classId"] = state.tree.classId;
            treeJ["classContentId"] = state.tree.classContentId;
            treeJ["ascendancyId"] = state.tree.ascendancyId;
            treeJ["allocatedNodes"] = state.tree.allocatedNodes;
            json attributeSelections = json::object();
            for (const auto& [nodeId, optionId] : state.tree.attributeSelections) {
                attributeSelections[std::to_string(nodeId)] = optionId;
            }
            treeJ["attributeSelections"] = std::move(attributeSelections);
            json mSel = json::object();
            for (const auto& [mNode, mEff] : state.tree.masterySelections) {
                mSel[std::to_string(mNode)] = mEff;
            }
            treeJ["masterySelections"] = std::move(mSel);
            treeJ["availableNormalPoints"] = availNormal;
            treeJ["spentNormalPoints"] = spentNormal;
            treeJ["grantedPassivePoints"] = grantedPts;
            treeJ["earnedAscendancyPoints"] = earnedAsc;
            treeJ["spentAscendancyPoints"] = spentAsc;
            treeJ["availableAscendancyPoints"] = availAsc;
            treeJ["bossKillCount"] = state.tree.bossKillCount;
            treeJ["dragonKillCount"] = state.tree.dragonKillCount;
            treeJ["ascendancyMilestonesBitmask"] = state.tree.ascendancyMilestonesBitmask;
            const auto* rulesPack = Core::GetRulesPack();
            const auto* rules = rulesPack ? rulesPack->Values() : nullptr;
            if (rules) {
                treeJ["spiritMilestoneRewards"] = Core::SpiritMilestoneRewards(*rules, g_config);
            }
            treeJ["atlasTierPoints"] = Core::MapSystem::CountAtlasTierPoints(state.atlas.completedTierMask);
            treeJ["milestonePassivePoints"] = Core::MapSystem::CountMilestonePassivePoints(state.tree.ascendancyMilestonesBitmask);
            treeJ["regretOrbCount"] = currencyCounts[static_cast<std::size_t>(Core::CurrencyKind::Regret)];
            treeJ["ascendancyRefundCost"] = g_config->freePassiveRefund ? 0u : g_config->ascendancyRefundCost;
            treeJ["refundAllCost"] = treeLoaded
                ? Core::PassiveTree::GetRefundAllCost(*g_treeData, state.tree, false, *g_config)
                : 0u;
            treeJ["refundAscCost"] = treeLoaded
                ? Core::PassiveTree::GetRefundAllCost(*g_treeData, state.tree, true, *g_config)
                : 0u;
            // 재검수 R-7: 노드 하나를 회수할 때의 실제 비용(자동 회수되는 마스터리 포함)을 C++ 계산 그대로 보낸다.
            // JS가 따로 근사하면 마스터리가 딸린 주요 패시브(1 + 자동 회수 수)와 AscendancyRefundCost=0일 때 어긋났다.
            json refundNodeCosts = json::object();
            if (treeLoaded) {
                for (const auto nid : state.tree.allocatedNodes) {
                    auto dependents = Core::PassiveTree::RefundDependents(*g_treeData, state.tree, nid);
                    if (dependents.empty()) {
                        refundNodeCosts[std::to_string(nid)] =
                            Core::PassiveTree::GetRefundNodeCost(*g_treeData, state.tree, nid, *g_config);
                    } else {
                        refundNodeCosts[std::to_string(nid)] = { { "blockedBy", std::move(dependents) } };
                    }
                }
                for (const auto& [mNode, mEff] : state.tree.masterySelections) {
                    refundNodeCosts[std::to_string(mNode)] =
                        Core::PassiveTree::GetRefundNodeCost(*g_treeData, state.tree, mNode, *g_config);
                }
            }
            treeJ["refundNodeCosts"] = std::move(refundNodeCosts);
            // 이미 효과를 고른 마스터리에서 다른 효과로 바꿀 때의 비용(SelectMastery가 실제로 쓰는 함수로 계산)
            json masterySwapCosts = json::object();
            for (const auto& [mNode, mEff] : state.tree.masterySelections) {
                const std::uint32_t otherEffect = (mEff == 0u) ? 1u : 0u;  // 현재와 다른 아무 효과
                masterySwapCosts[std::to_string(mNode)] =
                    Core::PassiveTree::GetSelectMasterySwapCost(state.tree, mNode, otherEffect, *g_config);
            }
            treeJ["masterySwapCosts"] = std::move(masterySwapCosts);

            // 5단계(결정 164): 패시브 주얼 소켓 장착 현황 직렬화
            json sockJewelsJ = json::object();
            if (g_itemData && g_itemData->IsLoaded()) {
                for (const auto& [nid, jRec] : state.socketedJewels) {
                    const auto* baseForm = RE::TESForm::LookupByID<RE::TESBoundObject>(jRec.baseFormId);
                    const std::string baseName = (baseForm && baseForm->GetName()) ? baseForm->GetName() : "주얼";
                    const Core::EquippedSlotInput dummy{};
                    const auto card = Core::ItemFormatter::FormatItem(
                        &jRec, baseName, Core::EquipSlot::Belt, dummy, true, *g_itemData);
                    sockJewelsJ[std::to_string(nid)] = json{
                        { "nodeId", nid },
                        { "baseFormId", jRec.baseFormId },
                        { "jewelColor", jRec.jewelColor },
                        { "rarity", RarityStr(jRec.rarity) },
                        { "title", card.title },
                        { "card", SerializeFormattedCard(card) }
                    };
                }
            }
            treeJ["socketedJewels"] = std::move(sockJewelsJ);
            root["tree"] = std::move(treeJ);

            const auto guardSnap = g_skillSystem ? g_skillSystem->GuardSnapshot() : Core::GuardShieldSnapshot{};
            const auto hhStacks = MonsterRuntime::ActiveHeadhunterStackCount();

            if (snap) {
                json st;
                st["str"] = snap->str;
                st["dex"] = snap->dex;
                st["intel"] = snap->intel;
                st["poeMaxLife"] = std::round(snap->poeMaxLife);
                st["poeMaxMana"] = std::round(snap->poeMaxMana);
                st["poeMaxEs"] = std::round(snap->poeMaxEnergyShield);
                st["esCurrent"] = std::round(es.current);
                st["esMax"] = std::round(es.max);
                st["armour"] = std::round(snap->armour);
                st["evasion"] = std::round(snap->evasion);
                st["deflectionRating"] = std::round(snap->deflectionRating);
                st["penetrationPct"] = {
                    { "fire", snap->penetrationPct[static_cast<std::size_t>(Core::DamageType::Fire)] },
                    { "cold", snap->penetrationPct[static_cast<std::size_t>(Core::DamageType::Cold)] },
                    { "lightning", snap->penetrationPct[static_cast<std::size_t>(Core::DamageType::Lightning)] },
                    { "chaos", snap->penetrationPct[static_cast<std::size_t>(Core::DamageType::Chaos)] },
                };
                st["attackElementalPenetrationPct"] = snap->attackElementalPenetrationPct;
                st["fireResist"] = std::round(snap->fireResist);
                st["coldResist"] = std::round(snap->coldResist);
                st["lightningResist"] = std::round(snap->lightningResist);
                st["chaosResist"] = std::round(snap->chaosResist);
                // 계산 전 스냅숏의 최대 저항은 NaN이다(StatEngine.h). 화면에는 규칙의 기본 최대값을 보인다.
                const double baseMaxResist = Core::ActiveRuleValues().resistance.baseMaxPercent;
                const auto maxResistOrBase = [baseMaxResist](double v) {
                    return std::round(std::isfinite(v) ? v : baseMaxResist);
                };
                st["maxFireResist"] = maxResistOrBase(snap->maxFireResist);
                st["maxColdResist"] = maxResistOrBase(snap->maxColdResist);
                st["maxLightningResist"] = maxResistOrBase(snap->maxLightningResist);
                st["maxChaosResist"] = maxResistOrBase(snap->maxChaosResist);
                st["physDR"] = std::round(snap->physDamageReduction);
                st["elemDR"] = std::round(snap->elementalDamageReduction);
                st["attackBlock"] = std::round(snap->attackBlockChance);
                st["spellBlock"] = std::round(snap->spellBlockChance);
                st["spellSuppression"] = std::round(snap->spellSuppressionChance);
                st["spellDodge"] = std::round(snap->spellDodgeChance);
                st["momRatio"] = std::round(snap->momRatio * 100.0);
                st["lifeRegenPerSec"] = std::round(snap->lifeRegenPerSec * 10.0) / 10.0;
                st["incManaRegenPct"] = std::round(snap->incManaRegenPct);
                st["incMovementSpeedPct"] = std::round(snap->incMovementSpeedPct);
                st["movementSpeedApplied"] = snap->poe2Passives;
                st["bloodMagic"] = snap->bloodMagic;
                st["chaosInoculation"] = snap->chaosInoculation;
                st["resoluteTechnique"] = snap->resoluteTechnique;
                st["unwaveringStance"] = snap->unwaveringStance;
                st["spiritReserved"] = snap->reservedSpirit;
                st["spiritMax"] = snap->maxSpirit;
                st["guardShield"] = guardSnap.active ? std::round(guardSnap.currentShieldSkyrim) : 0.0;
                st["guardMax"] = guardSnap.active ? std::round(guardSnap.maxShieldSkyrim) : 0.0;
                st["hhStacks"] = hhStacks;
                root["stats"] = std::move(st);
            }

            json slotsArr = json::array();
            for (std::size_t i = 0; i < state.slots.size(); ++i) {
                const auto& s = state.slots[i];
                const std::uint16_t bit = static_cast<std::uint16_t>(1u << i);
                json sj;
                sj["slotIndex"] = i;
                sj["occupied"] = s.occupied;
                sj["linkedAttackSlot"] = s.linkedAttackSlot;
                sj["auraRequested"] = (state.activeAuraMask & bit) != 0;
                sj["auraActive"] = snap && ((snap->effectiveActiveAuraMask & bit) != 0);
                if (s.occupied) {
                    sj["gem"] = SerializeGemInstance(s.gem, snap.get());
                    if (snap && snap->slotStats[i].gemId == s.gem.gemId) {
                        sj["computed"] = SerializeComputedStats(snap->slotStats[i], *snap);
                    }
                }
                slotsArr.push_back(std::move(sj));
            }
            root["slots"] = std::move(slotsArr);

            const auto gemGeneration = SaveLoad::WorldGemGeneration();
            json invArr = json::array();
            for (const auto& t : inv.trackedGems) {
                json tj;
                tj["tracked"] = true;
                tj["ownerFormId"] = t.key.ownerFormId;
                tj["uniqueId"] = t.key.uniqueId;
                tj["gemId"] = t.gem.gemId;
                tj["name"] = DisplayGemName(t.gem.gemId);
                tj["reason"] = Core::GemForms::UseReasonText(
                    Core::GemForms::ClassifyUse(gemGeneration, t.gem.gemId).reason);
                tj["level"] = t.gem.level;
                tj["socketCount"] = t.gem.socketCount;
                tj["socketedCount"] = t.gem.socketedGems.size();
                tj["count"] = 1;
                Core::GemKind kind = Core::GemKind::Unknown;
                if (g_dataPack) {
                    if (const auto* def = g_dataPack->FindGem(t.gem.gemId, gemGeneration)) {
                        kind = def->kind;
                    }
                }
                tj["kind"] = GemKindStr(kind);
                invArr.push_back(std::move(tj));
            }
            for (const auto& p : inv.plainStacks) {
                Core::GemKind kind = Core::GemKind::Unknown;
                if (g_dataPack) {
                    if (const auto* def = g_dataPack->FindGem(p.gemId, gemGeneration)) {
                        kind = def->kind;
                    }
                }
                json pj;
                pj["tracked"] = false;
                pj["ownerFormId"] = 0;
                pj["uniqueId"] = 0;
                pj["gemId"] = p.gemId;
                pj["name"] = DisplayGemName(p.gemId);
                pj["reason"] = Core::GemForms::UseReasonText(
                    Core::GemForms::ClassifyUse(gemGeneration, p.gemId).reason);
                pj["level"] = 1;
                const bool hasNoSockets =
                    (kind == Core::GemKind::Support || kind == Core::GemKind::Aura || kind == Core::GemKind::Herald);
                pj["socketCount"] = hasNoSockets ? 0 : g_config->starterSockets;
                pj["socketedCount"] = 0;
                pj["count"] = p.count;
                pj["kind"] = GemKindStr(kind);
                invArr.push_back(std::move(pj));
            }
            root["inventory"] = std::move(invArr);

            // 4단계 3번째 탭(장비·제작) 페이로드(결정 156, 157, 158, 160)
            // 세대 1(빈 items, 제작 꺼짐)은 장비 탭이 안내만 보이므로 가방 전체 직렬화를 하지 않는다.
            const bool itemsLoaded = g_itemData && g_itemData->HasItemContent() && Core::ActiveContentFeatures().crafting;
            json gearJ;
            gearJ["loaded"] = itemsLoaded;
            if (itemsLoaded) {
                json curArr = json::array();
                for (std::size_t i = 0; i < Core::kCurrencyKindCount; ++i) {
                    const auto kind = static_cast<Core::CurrencyKind>(i);
                    const auto* def = g_itemData->FindCurrency(kind);
                    curArr.push_back(json{
                        { "kind", i },
                        { "id", def ? def->id : std::string("Currency") },
                        { "koName", def ? def->koName : std::string("오브") },
                        { "koDesc", def ? def->koDesc : std::string() },
                        { "count", currencyCounts[i] }
                    });
                }
                gearJ["currencies"] = std::move(curArr);

                const auto tokens = CraftingRuntime::ListPlayerTokens(player);
                json tokArr = json::array();
                for (const auto& tk : tokens) {
                    tokArr.push_back(json{
                        { "tier", tk.tier },
                        { "rarity", RarityStr(tk.rarity) },
                        { "rarityCode", static_cast<std::uint8_t>(tk.rarity) },
                        { "slotGroup", static_cast<std::uint8_t>(tk.slotGroup) },
                        { "slotLabel", tk.isUniqueToken ? "고유" : TokenSlotGroupLabelKo(tk.slotGroup) },
                        { "isUniqueToken", tk.isUniqueToken },
                        { "count", tk.count },
                        { "displayName", tk.displayName }
                    });
                }
                gearJ["tokens"] = std::move(tokArr);

                const auto gearInputs = EquipmentSync::GetCurrentGearSnapshot();
                const auto gearMetas = EquipmentSync::GetCurrentGearMeta();
                json eqArr = json::array();
                for (std::size_t i = 0; i < Core::kEquipSlotCount; ++i) {
                    const auto slot = static_cast<Core::EquipSlot>(i);
                    const auto& meta = gearMetas[i];
                    const auto& inp = gearInputs[i];
                    const bool reqMet = !snap || !snap->gearSlotReqFailed[i];
                    json ej;
                    ej["slotIndex"] = i;
                    ej["slotLabel"] = EquipSlotLabelKo(slot);
                    ej["isVirtual"] = (slot == Core::EquipSlot::Ring2 || slot == Core::EquipSlot::Belt);
                    ej["occupied"] = meta.occupied;
                    ej["baseFormId"] = meta.baseFormId;
                    ej["uniqueId"] = meta.key.uniqueId;
                    ej["displayName"] = meta.displayName;
                    ej["hasPoERecord"] = meta.hasPoERecord;
                    ej["rarity"] = meta.hasPoERecord ? RarityStr(meta.record.rarity) : "normal";
                    ej["requirementMet"] = reqMet;
                    if (meta.occupied) {
                        const Core::PoEItemRecord* recPtr = meta.hasPoERecord ? &meta.record : nullptr;
                        const auto card = Core::ItemFormatter::FormatItem(
                            recPtr, meta.displayName, slot, inp, reqMet, *g_itemData);
                        ej["card"] = SerializeFormattedCard(card);
                        json canFlags;
                        json canReasons;
                        BuildCanApplyOrbData(
                            recPtr, meta.slotGroup, meta.defenseSubtype, meta.weaponTag, meta.isAmulet,
                            meta.baseFormId, state.exileLevel, canFlags, canReasons);
                        ej["canApplyOrb"] = std::move(canFlags);
                        ej["canApplyOrbReason"] = std::move(canReasons);
                    }
                    eqArr.push_back(std::move(ej));
                }
                gearJ["equippedSlots"] = std::move(eqArr);

                const auto bagItems = CraftingRuntime::ListPlayerCraftableItems(player);
                json bagArr = json::array();
                for (const auto& b : bagItems) {
                    auto* baseForm = RE::TESForm::LookupByID<RE::TESBoundObject>(b.baseFormId);
                    const Core::PoEItemRecord* recPtr = b.hasPoERecord ? &b.record : nullptr;
                    auto slotInp = baseForm
                        ? EquipmentSync::BuildSlotInput(baseForm, recPtr, state.exileLevel, *g_itemData)
                        : Core::EquippedSlotInput{};
                    if (b.slotGroup == Core::TokenSlotGroup::Map && !recPtr) {
                        const auto localId = FormIds::ResolveLocalFormIdIfOurs(b.baseFormId);
                        if (const auto tier = FormIds::MapTierFromLocalFormId(localId); tier.has_value()) {
                            slotInp.itemLevel = Core::MapSystem::MonsterLevelForTier(*tier);
                        }
                    }
                    // 검수 Major #4: StatEngine과 동일하게 exileLevel >= reqLevel 포함 4조건 모두 판정
                    const bool reqMet = !snap ||
                        (snap->exileLevel >= slotInp.reqLevel &&
                         snap->str >= slotInp.reqStr &&
                         snap->dex >= slotInp.reqDex &&
                         snap->intel >= slotInp.reqInt);
                    auto card = Core::ItemFormatter::FormatItem(
                        recPtr, b.displayName, b.equipSlot, slotInp, reqMet, *g_itemData);
                    const bool isJewel = (b.slotGroup == Core::TokenSlotGroup::Jewel);
                    const bool isMap = (b.slotGroup == Core::TokenSlotGroup::Map);
                    if (isJewel) {
                        card.slotLabel = "주얼";
                    } else if (isMap) {
                        card.slotLabel = "지도";
                    }
                    json canFlags;
                    json canReasons;
                    BuildCanApplyOrbData(
                        recPtr, b.slotGroup, b.defenseSubtype, b.weaponTag, b.isAmulet,
                        b.baseFormId, state.exileLevel, canFlags, canReasons, b.jewelColor);
                    const bool isBelt = (b.slotGroup == Core::TokenSlotGroup::Belt);
                    const bool isRing = (!isJewel && !isBelt && !isMap && (b.equipSlot == Core::EquipSlot::Ring1 || b.equipSlot == Core::EquipSlot::Ring2));
                    bagArr.push_back(json{
                        { "baseFormId", b.baseFormId },
                        { "uniqueId", b.uniqueId },
                        { "count", b.count },
                        { "equipped", b.equipped },
                        { "isVirtualEquipped", b.isVirtualEquipped },
                        { "isRing", isRing },
                        { "isBelt", isBelt },
                        { "isJewel", isJewel },
                        { "isMap", isMap },
                        { "jewelColor", b.jewelColor },
                        { "displayName", b.displayName },
                        { "slotIndex", static_cast<std::size_t>(b.equipSlot) },
                        { "slotGroup", static_cast<std::uint8_t>(b.slotGroup) },
                        { "slotLabel", isJewel ? "주얼" : (isMap ? "지도" : EquipSlotLabelKo(b.equipSlot)) },
                        { "hasPoERecord", b.hasPoERecord },
                        { "rarity", b.hasPoERecord ? RarityStr(b.record.rarity) : "normal" },
                        { "card", SerializeFormattedCard(card) },
                        { "canApplyOrb", std::move(canFlags) },
                        { "canApplyOrbReason", std::move(canReasons) }
                    });
                }
                gearJ["bagItems"] = std::move(bagArr);

                // 5단계(결정 162): 벨트 참(Charms) 슬롯 + 가방 보유 참 직렬화 (잠긴 칸 보유 참 회수 포함)
                json charmsJ;
                const std::uint8_t unlockedCharms = g_charmSystem ? g_charmSystem->UnlockedSlots() : 0;
                const auto liveCharmSlots = g_charmSystem ? g_charmSystem->Slots() : state.charmSlots;
                const auto activeBonuses = g_charmSystem ? g_charmSystem->ComputeActiveBonuses() : Core::CharmActiveBonuses{};
                charmsJ["unlockedSlots"] = unlockedCharms;
                json cSlots = json::array();
                for (std::size_t ci = 0; ci < Core::kMaxCharmSlots; ++ci) {
                    const auto& cs = liveCharmSlots[ci];
                    const auto* cMeta = g_itemData->FindCharm(cs.kind);
                    const bool isAct = cs.occupied &&
                        ((activeBonuses.activeMask & static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(cs.kind))) != 0);
                    cSlots.push_back(json{
                        { "slotIndex", ci },
                        { "occupied", cs.occupied },
                        { "locked", ci >= unlockedCharms },
                        { "kind", static_cast<std::uint8_t>(cs.kind) },
                        { "koName", cMeta ? cMeta->koName : std::string("참") },
                        { "koDesc", cMeta ? cMeta->koDesc : std::string() },
                        { "charges", std::round(cs.charges) },
                        { "maxCharges", std::round(Core::CharmSystem::kMaxCharges) },
                        { "active", isAct }
                    });
                }
                charmsJ["slots"] = std::move(cSlots);
                const auto charmBagCounts = CraftingRuntime::CountAllCharms(player);
                json cBag = json::array();
                for (std::size_t ck = 0; ck < Core::kCharmKindCount; ++ck) {
                    const auto kind = static_cast<Core::CharmKind>(ck);
                    const auto* cMeta = g_itemData->FindCharm(kind);
                    cBag.push_back(json{
                        { "kind", ck },
                        { "koName", cMeta ? cMeta->koName : std::string("참") },
                        { "koDesc", cMeta ? cMeta->koDesc : std::string() },
                        { "count", charmBagCounts[ck] }
                    });
                }
                charmsJ["bagCounts"] = std::move(cBag);
                gearJ["charms"] = std::move(charmsJ);

                // 쥬얼러 오브 대상 젬 목록(장착 스킬/발동 젬 + 발동 젬 내부 주문 젬)
                json jewArr = json::array();
                for (std::size_t i = 0; i < state.slots.size(); ++i) {
                    const auto& s = state.slots[i];
                    if (!s.occupied) {
                        continue;
                    }
                    // 세계 세대에서 쓸 수 없는 젬(구판·미준비)은 쥬얼러 대상이 아니다.
                    if (!Core::GemForms::AllowsExecution(gemGeneration, s.gem.gemId)) {
                        continue;
                    }
                    Core::GemKind rootKind = Core::GemKind::Unknown;
                    if (g_dataPack) {
                        if (const auto* def = g_dataPack->FindGem(s.gem.gemId, gemGeneration)) {
                            rootKind = def->kind;
                        }
                    }
                    if (rootKind != Core::GemKind::Support && rootKind != Core::GemKind::Aura &&
                        rootKind != Core::GemKind::Herald && rootKind != Core::GemKind::Unknown) {
                        const auto rootName = DisplayGemName(s.gem.gemId);
                        jewArr.push_back(json{
                            { "slotIndex", i },
                            { "triggeredSpellInstanceId", 0 },
                            { "label", std::format("칸 {} · {} Lv.{}", i + 1, rootName, s.gem.level) },
                            { "gemId", s.gem.gemId },
                            { "name", rootName },
                            { "level", s.gem.level },
                            { "socketCount", s.gem.socketCount },
                            { "maxSockets", Core::kMaxGemSockets },
                            { "canExpand", s.gem.socketCount < Core::kMaxGemSockets }
                        });
                    }
                    if (rootKind == Core::GemKind::Trigger) {
                        for (const auto& sp : s.gem.socketedGems) {
                            if (!Core::GemForms::AllowsExecution(gemGeneration, sp.gemId)) {
                                continue;
                            }
                            const auto spName = DisplayGemName(sp.gemId);
                            jewArr.push_back(json{
                                { "slotIndex", i },
                                { "triggeredSpellInstanceId", sp.instanceId },
                                { "label", std::format("칸 {} > {} Lv.{}", i + 1, spName, sp.level) },
                                { "gemId", sp.gemId },
                                { "name", spName },
                                { "level", sp.level },
                                { "socketCount", sp.socketCount },
                                { "maxSockets", Core::kMaxGemSockets },
                                { "canExpand", sp.socketCount < Core::kMaxGemSockets }
                            });
                        }
                    }
                }
                gearJ["jewellerTargets"] = std::move(jewArr);
            }
            root["gear"] = std::move(gearJ);

            // 6단계(결정 166): 지도 장치·아틀라스·4차 전직 결투장·안전 제거 탭 페이로드
            {
                json mapsJ;
                const auto& mapPack = MapPackRuntime::Manifest();
                const auto& packError = MapPackRuntime::UnavailableReason();
                mapsJ["packAvailable"] = packError.empty();
                mapsJ["packUnavailableReason"] = packError;
                // 결정 239·248: 세대 1은 새 입장·결투장·재입장만 막고 귀환·포기·탈출·안전 제거 버튼은 남긴다.
                const bool entryDisabled = !Core::ActiveContentFeatures().mapEntry;
                mapsJ["entryDisabled"] = entryDisabled;
                mapsJ["entryDisabledReason"] = entryDisabled
                    ? "세대 1(PoE2)에서는 아직 지도에 들어갈 수 없습니다. 지도 기능은 P6b에서 다시 엽니다. 진행 중이던 회차의 귀환과 안전 제거는 쓸 수 있습니다."
                    : "";
                mapsJ["arenaDungeonName"] = mapPack.arena.name;
                const auto travel = MapDeviceRuntime::GetTravelStatus(player);
                g_panelMapTravel = travel;
                mapsJ["travelInProgress"] = travel.kind.has_value();
                mapsJ["travelKind"] = travel.kind ? MapTravelLabel(*travel.kind) : std::string_view{};
                mapsJ["inPackCell"] = travel.inPackCell;
                mapsJ["unauthorizedInPackCell"] = travel.unauthorized;
                mapsJ["canRetryReturn"] = travel.canRetryReturn;
                const auto atlasPts = Core::MapSystem::CountAtlasTierPoints(state.atlas.completedTierMask);
                const auto msPts = Core::MapSystem::CountMilestonePassivePoints(state.tree.ascendancyMilestonesBitmask);
                json tierDefs = json::array();
                for (std::uint8_t t = Core::kMinMapTier; t <= Core::kMaxMapTier; ++t) {
                    const auto* dungeon = mapPack.ForTier(t);
                    const bool done = (state.atlas.completedTierMask & static_cast<std::uint16_t>(1u << (t - 1u))) != 0u;
                    tierDefs.push_back(json{
                        { "tier", t },
                        { "areaLevel", Core::MapSystem::MonsterLevelForTier(t) },
                        { "dungeonName", dungeon ? dungeon->name : "" },
                        { "theme", dungeon ? MapDungeonTypeLabel(dungeon->type) : "" },
                        { "completed", done }
                    });
                }
                mapsJ["atlas"] = json{
                    { "completedTierMask", state.atlas.completedTierMask },
                    { "atlasTierPoints", atlasPts },
                    { "milestonePassivePoints", msPts },
                    { "totalBonusPoints", atlasPts + msPts },
                    { "totalMapsCleared", state.atlas.totalMapsCleared },
                    { "highestTierCleared", state.atlas.highestTierCleared },
                    { "bonusIiqPct", Core::MapSystem::AtlasBonusIiqPct(state.atlas.completedTierMask) },
                    { "bonusIirPct", Core::MapSystem::AtlasBonusIirPct(state.atlas.completedTierMask) },
                    { "firstTributeGiven", state.atlas.firstTributeGiven },
                    { "tiers", std::move(tierDefs) }
                };

                const auto activeMap = MapDeviceRuntime::GetActiveMapStateCopy();
                const auto* activeDungeon = mapPack.ForRun(activeMap);
                json activeModLines = json::array();
                for (const auto& af : activeMap.mods) {
                    activeModLines.push_back(Core::MapSystem::FormatMapModKoreanLine(af));
                }
                mapsJ["active"] = json{
                    { "active", activeMap.active },
                    { "isArena4", activeMap.isArena4 },
                    { "inMapCell", MapDeviceRuntime::IsPlayerInActiveMapCell() },
                    { "bossKilled", activeMap.bossKilled },
                    { "failed", activeMap.failed },
                    { "endRequested", activeMap.endRequested },
                    { "tier", activeMap.tier },
                    { "areaLevel", activeMap.active ? (activeMap.isArena4 ? Core::kArena4MonsterLevel : Core::MapSystem::MonsterLevelForTier(activeMap.tier)) : 0u },
                    { "dungeonName", activeDungeon ? activeDungeon->name : "" },
                    { "portalsRemaining", activeMap.portalsRemaining },
                    { "portalsTotal", activeMap.isArena4 ? Core::kArena4MaxPortals : Core::kNormalMapMaxPortals },
                    { "arenaPhase", activeMap.arenaPhase },
                    { "modLines", std::move(activeModLines) },
                    { "iiqPct", activeMap.summary.bonusIiqPct },
                    { "iirPct", activeMap.summary.bonusIirPct },
                    { "packSizePct", activeMap.summary.packSizePct }
                };

                mapsJ["tributeCount"] = MapDeviceRuntime::CountPlayerTributes(player);
                const auto arenaEntry = Core::MapSystem::GetArena4EntryStatus(state.tree.ascendancyMilestonesBitmask);
                mapsJ["arena4CanEnter"] = arenaEntry.CanEnter();
                mapsJ["arena4MilestonesCompleted"] = arenaEntry.milestonesCompleted;
                mapsJ["ascendancy4Unlocked"] = arenaEntry.denial == Core::Arena4EntryDenial::AlreadyCleared;
                if (rules) {
                    mapsJ["arena4BaseMaxSpirit"] = Core::ComputeSpirit(
                        Core::kArena4PrerequisiteMask | Core::kArena4MilestoneBit, *rules);
                }
                mapsJ["uninstallCleanupArmed"] = MapDeviceRuntime::IsUninstallCleanupArmed();
                if (const auto remaining = MapDeviceRuntime::GetUninstallCleanupRemaining(state)) {
                    mapsJ["uninstallCleanupRemaining"] = {
                        { "gems", remaining->gems },
                        { "jewels", remaining->jewels },
                        { "charms", remaining->charms }
                    };
                }

                json bagMapsArr = json::array();
                for (const auto& bm : MapDeviceRuntime::ListPlayerMaps(player)) {
                    bagMapsArr.push_back(json{
                        { "ownerFormId", bm.key.ownerFormId },
                        { "uniqueId", bm.key.uniqueId },
                        { "baseFormId", bm.baseFormId },
                        { "tier", bm.tier },
                        { "count", bm.count },
                        { "tracked", bm.tracked },
                        { "rarity", RarityStr(bm.rarity) },
                        { "displayName", bm.displayName },
                        { "modLines", bm.modLines },
                        { "iiqPct", bm.summary.bonusIiqPct },
                        { "iirPct", bm.summary.bonusIirPct },
                        { "packSizePct", bm.summary.packSizePct }
                    });
                }
                mapsJ["bagMaps"] = std::move(bagMapsArr);
                root["maps"] = std::move(mapsJ);
            }

            // C-28: 비-UTF-8 모드 아이템 이름이 있어도 패널 JSON 전체가 예외로 소실되지 않도록 대체 문자로 직렬화한다.
            return root.dump(-1, ' ', false, json::error_handler_t::replace);
        }

        void PushPanelStateToJs()
        {
            if (!ViewUsable() || !g_domReady.load(std::memory_order_acquire)) {
                return;
            }
            const std::string payload = BuildPanelPayloadJson();
            if (payload == "{}") {
                return;
            }
            g_api->InteropCall(g_view, kJsUpdatePanel, payload.c_str());
        }

        void PushToastToJs(bool ok, const std::string& message)
        {
            if (!ViewUsable() || !g_domReady.load(std::memory_order_acquire)) {
                return;
            }
            json j{ { "ok", ok }, { "message", message } };
            const std::string payload = j.dump(-1, ' ', false, json::error_handler_t::replace);
            g_api->InteropCall(g_view, kJsToast, payload.c_str());
        }

        // ---- 지연 키 차단 훅(프로토타입 4 검증: 최초 패널 열기 시 설치해 타 ImGui 모드보다 선점) ----
        bool HideKeyFromOthers(const RE::InputEvent& event)
        {
            if (event.GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                return false;
            }
            const bool open = g_panelOpen.load(std::memory_order_acquire);
            if (const auto* btn = event.AsButtonEvent()) {
                const auto code = btn->GetIDCode();
                if (code >= g_hiddenKeys.size()) {
                    return false;
                }
                if (g_hiddenKeys.test(code)) {
                    if (!btn->IsPressed()) {
                        g_hiddenKeys.reset(code);
                    }
                    return true;
                }
                if (open && btn->IsDown()) {
                    g_hiddenKeys.set(code);
                    return true;
                }
                return false;
            }
            return open && event.GetEventType() == RE::INPUT_EVENT_TYPE::kChar;
        }

        void OpenPanelOnMainThread(std::uint8_t targetTab);

        void SwitchTabOnMainThread(std::uint8_t targetTab)
        {
            const std::uint8_t prev = g_activeTab.exchange(targetTab, std::memory_order_acq_rel);
            if (targetTab != kTabTree) {
                SimpleImeBridge::SetTextEntry(false);
                g_searchFocused.store(false, std::memory_order_release);
            }
            if (prev != targetTab || g_panelOpen.load(std::memory_order_acquire)) {
                PushPanelStateToJs();
            }
        }

        void HandleKeyboardEvents(RE::InputEvent* events)
        {
            if (!ModState::AllowsGameplay() || !g_config || !g_queue) {
                return;
            }
            const auto generation = g_queue->CurrentGeneration();
            const std::uint32_t panelKey = g_config->panelKey;
            const std::uint32_t treeKey = g_config->treePanelKey;
            const std::uint32_t gearKey = g_config->gearPanelKey;
            const std::uint32_t mapKey = g_config->mapPanelKey;
            const bool open = g_panelOpen.load(std::memory_order_acquire);
            const bool imeActive = open && SimpleImeBridge::TextEntryActive();
            const bool searchFocused = open && g_searchFocused.load(std::memory_order_acquire);

            for (auto* ev = events; ev; ev = ev->next) {
                const auto* btn = ev->AsButtonEvent();
                if (!btn || btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                    continue;
                }
                const auto code = btn->GetIDCode();

                // 검수 M4: SimpleIME 편집 키는 IsDown만이 아니라 홀드 반복·뗌(Up)까지 모두 전달해야 함
                if (imeActive) {
                    SimpleImeBridge::OnGameKey(code, btn->IsPressed(), btn->HeldDuration());
                }

                if (!btn->IsDown()) {
                    continue;
                }

                // 검수 M4: 검색칸 포커스 중에는 K/P 탭 전환·닫기 차단, Escape는 검색칸 포커스 해제만 수행
                if (searchFocused) {
                    if (code == kEscapeScanCode && !imeActive) {
                        g_queue->EnqueueForGeneration(generation, []() {
                            g_searchFocused.store(false, std::memory_order_release);
                            SimpleImeBridge::SetTextEntry(false);
                            if (ViewUsable() && g_domReady.load(std::memory_order_acquire)) {
                                g_api->InteropCall(g_view, kJsBlurSearch, "");
                            }
                        });
                    }
                    continue;
                }

                if (code == panelKey) {
                    g_queue->EnqueueForGeneration(generation, []() {
                        if (!g_panelOpen.load(std::memory_order_acquire)) {
                            OpenPanelOnMainThread(kTabSkills);
                        } else if (g_activeTab.load(std::memory_order_acquire) == kTabSkills) {
                            UiBridge::ClosePanelOnMainThread();
                        } else {
                            SwitchTabOnMainThread(kTabSkills);
                        }
                    });
                } else if (code == treeKey) {
                    g_queue->EnqueueForGeneration(generation, []() {
                        if (!g_panelOpen.load(std::memory_order_acquire)) {
                            OpenPanelOnMainThread(kTabTree);
                        } else if (g_activeTab.load(std::memory_order_acquire) == kTabTree) {
                            UiBridge::ClosePanelOnMainThread();
                        } else {
                            SwitchTabOnMainThread(kTabTree);
                        }
                    });
                } else if (code == gearKey) {
                    g_queue->EnqueueForGeneration(generation, []() {
                        if (!g_panelOpen.load(std::memory_order_acquire)) {
                            OpenPanelOnMainThread(kTabGear);
                        } else if (g_activeTab.load(std::memory_order_acquire) == kTabGear) {
                            UiBridge::ClosePanelOnMainThread();
                        } else {
                            SwitchTabOnMainThread(kTabGear);
                        }
                    });
                } else if (code == mapKey) {
                    g_queue->EnqueueForGeneration(generation, []() {
                        if (!g_panelOpen.load(std::memory_order_acquire)) {
                            OpenPanelOnMainThread(kTabMaps);
                        } else if (g_activeTab.load(std::memory_order_acquire) == kTabMaps) {
                            UiBridge::ClosePanelOnMainThread();
                        } else {
                            SwitchTabOnMainThread(kTabMaps);
                        }
                    });
                } else if (open && code == kEscapeScanCode) {
                    g_queue->EnqueueForGeneration(generation, []() {
                        UiBridge::ClosePanelOnMainThread();
                    });
                }
            }
        }

        struct InputDispatchHook
        {
            static void Thunk(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent* const* a_events)
            {
                const auto callOrig = [&](RE::InputEvent* const* evs) {
                    const auto orig = originalAddr.load(std::memory_order_acquire);
                    if (orig != 0) {
                        using Fn = void (*)(RE::BSTEventSource<RE::InputEvent*>*, RE::InputEvent* const*);
                        reinterpret_cast<Fn>(orig)(a_source, evs);
                    }
                };

                if (!ModState::AllowsGameplay() || !a_events || !*a_events) {
                    callOrig(a_events);
                    return;
                }

                HandleKeyboardEvents(*a_events);

                if (!g_panelOpen.load(std::memory_order_acquire) && g_hiddenKeys.none()) {
                    callOrig(a_events);
                    return;
                }

                RE::InputEvent* head = nullptr;
                RE::InputEvent* tail = nullptr;
                for (auto* ev = *a_events; ev;) {
                    auto* next = ev->next;
                    if (!HideKeyFromOthers(*ev)) {
                        if (tail) {
                            tail->next = ev;
                        } else {
                            head = ev;
                        }
                        tail = ev;
                    }
                    ev = next;
                }
                if (tail) {
                    tail->next = nullptr;
                }
                RE::InputEvent* const remaining = head;
                callOrig(&remaining);
            }

            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        bool InstallDeferredInputHook()
        {
            if (g_inputHooked) {
                return true;
            }
            const REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(67315, 68617), 0x7B };
            const auto site = target.address();
            const auto textSeg = REL::Module::get().segment(REL::Segment::textx);
            if (site < textSeg.address() || site + 5 > textSeg.address() + textSeg.size()) {
                return false;
            }
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(site);
            if (bytes[0] != 0xE8) {
                SKSE::log::warn("UiBridge: InputDispatch site 0x{:X} is not an E8 call; keyboard blocking disabled.", site);
                return false;
            }
            auto& trampoline = SKSE::GetTrampoline();
            if (trampoline.free_size() < 14) {
                SKSE::log::error("UiBridge: insufficient trampoline space for InputDispatchHook.");
                return false;
            }
            std::int32_t rel32 = 0;
            std::memcpy(&rel32, bytes + 1, sizeof(rel32));
            const auto preTarget = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(site + 5) + static_cast<std::intptr_t>(rel32));
            InputDispatchHook::originalAddr.store(preTarget, std::memory_order_release);
            const auto prev = trampoline.write_call<5>(site, InputDispatchHook::Thunk);
            InputDispatchHook::originalAddr.store(prev, std::memory_order_release);
            g_inputHooked = true;
            SKSE::log::info("UiBridge: deferred InputDispatchHook installed at 0x{:X}.", site);
            return true;
        }

        class PreHookInputSink final : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            static PreHookInputSink& Get()
            {
                static PreHookInputSink instance;
                return instance;
            }

            RE::BSEventNotifyControl ProcessEvent(
                RE::InputEvent* const* a_events,
                RE::BSTEventSource<RE::InputEvent*>*) override
            {
                if (a_events && *a_events && !g_inputHooked) {
                    HandleKeyboardEvents(*a_events);
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            PreHookInputSink() = default;
        };

        class OtherMenuCloseSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            static OtherMenuCloseSink& Get()
            {
                static OtherMenuCloseSink instance;
                return instance;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!event || !event->opening) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const std::string_view name = event->menuName.empty() ? std::string_view{} : std::string_view(event->menuName.c_str());
                if (g_panelOpen.load(std::memory_order_acquire) && !IsIgnoredMenuForAutoClose(name)) {
                    g_needCloseOnMain.store(true, std::memory_order_release);
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            OtherMenuCloseSink() = default;
        };

        bool ReadBoundedInt(const json& obj, const char* key, std::int64_t minVal, std::int64_t maxVal, std::int64_t& out)
        {
            if (!obj.contains(key) || !obj[key].is_number_integer()) {
                return false;
            }
            const auto v = obj[key].get<std::int64_t>();
            if (v < minVal || v > maxVal) {
                return false;
            }
            out = v;
            return true;
        }

        // ---- JS -> C++ 명령 처리(Prisma UI 스레드에서 호출될 수 있으므로 반드시 CommandQueue로 이연) ----
        void ExecuteCommandOnMainThread(const std::string& rawJson, std::uint64_t mapRunId,
            std::pair<bool, std::string>* gemResult = nullptr)
        {
            if (!g_dataPack || !g_config || !g_statEngine) {
                return;
            }
            try {
                const json cmd = json::parse(rawJson);
                if (!cmd.is_object()) {
                    SKSE::log::warn("UiBridge: ignoring non-object JS command.");
                    return;
                }

                const std::string op = cmd.contains("op") && cmd["op"].is_string() ? cmd["op"].get<std::string>() : "";
                const auto content = SaveLoad::GetContentGeneration();
                const std::string_view expectedBuildId = content ? std::string_view(content->buildId) : std::string_view{};
                const auto buildField = cmd.find("buildId");
                const std::string_view receivedBuildId = buildField != cmd.end() && buildField->is_string()
                    ? std::string_view(buildField->get_ref<const std::string&>()) : std::string_view{};
                const auto protocolField = cmd.find("uiProtocol");
                std::optional<std::uint64_t> receivedProtocol;
                if (protocolField != cmd.end() && protocolField->is_number_unsigned()) {
                    receivedProtocol = protocolField->get<std::uint64_t>();
                }

                const auto buildCheck = UiBuildCheck::Evaluate(
                    receivedBuildId, receivedProtocol, expectedBuildId, UiBuildCheck::kUiProtocolVersion);
                const bool mismatchChanged = g_uiBuildMismatch != !buildCheck.match;
                g_uiBuildMismatch = !buildCheck.match;
                if (g_uiBuildMismatch) {
                    if (!g_uiBuildMismatchOverflowLogged) {
                        auto key = std::tuple{
                            buildField != cmd.end() ? buildField->dump() : "<missing>",
                            protocolField != cmd.end() ? protocolField->dump() : "<missing>",
                            std::string(expectedBuildId) };
                        if (g_loggedUiBuildMismatches.size() < kMaxLoggedUiBuildMismatches) {
                            const auto [logged, inserted] = g_loggedUiBuildMismatches.insert(std::move(key));
                            if (inserted) {
                                const auto& [loggedBuildId, loggedProtocol, loggedExpectedBuildId] = *logged;
                                SKSE::log::warn(
                                    "UiBridge: {} (received buildId={}, uiProtocol={}; expected buildId=\"{}\", uiProtocol={}).",
                                    buildCheck.reason, loggedBuildId, loggedProtocol, loggedExpectedBuildId,
                                    UiBuildCheck::kUiProtocolVersion);
                            }
                        } else if (!g_loggedUiBuildMismatches.contains(key)) {
                            g_uiBuildMismatchOverflowLogged = true;
                            SKSE::log::warn("UiBridge: 추가 불일치는 생략합니다.");
                        }
                    }
                    if (UiBuildCheck::ClassifyOperation(op) == UiBuildCheck::OperationKind::StateChanging) {
                        PushToastToJs(false, "UI 판이 모드 DLL과 맞지 않습니다. 모드를 다시 설치하거나 게임을 다시 시작하세요.");
                        // 일반 명령과 같은 토스트·패널 갱신으로 거부 결과를 돌려준다.
                        PushPanelStateToJs();
                        return;
                    }
                }

                // 검수 C1/M4: close, refresh, treeSearchFocus, switchTab은 reqId·사망 검사보다 먼저 처리해 포커스 해제 누락을 차단한다.
                if (op == "close") {
                    UiBridge::ClosePanelOnMainThread();
                    return;
                }
                if (op == "refresh") {
                    PushPanelStateToJs();
                    return;
                }
                if (op == "treeSearchFocus") {
                    const bool focused = cmd.contains("focused") && cmd["focused"].is_boolean() && cmd["focused"].get<bool>();
                    const bool active = focused && g_panelOpen.load(std::memory_order_acquire) &&
                        (g_activeTab.load(std::memory_order_acquire) == kTabTree);
                    g_searchFocused.store(active, std::memory_order_release);
                    SimpleImeBridge::SetTextEntry(active);
                    if (mismatchChanged && g_panelOpen.load(std::memory_order_acquire)) {
                        PushPanelStateToJs();
                    }
                    return;
                }
                if (op == "switchTab") {
                    const std::string tab = cmd.contains("tab") && cmd["tab"].is_string() ? cmd["tab"].get<std::string>() : "skills";
                    SwitchTabOnMainThread(TabIdFromName(tab));
                    return;
                }

                if (!ModState::AllowsGameplay()) {
                    return;
                }

                // 결정 137(마): 중복 요청 번호 거절
                std::uint64_t reqId = 0;
                if (cmd.contains("reqId") && cmd["reqId"].is_number_unsigned()) {
                    reqId = cmd["reqId"].get<std::uint64_t>();
                }
                if (reqId > 0) {
                    const auto prevReq = g_lastProcessedReqId.load(std::memory_order_acquire);
                    if (reqId <= prevReq) {
                        return;
                    }
                    g_lastProcessedReqId.store(reqId, std::memory_order_release);
                }

                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player || !player->Is3DLoaded() || player->IsDead()) {
                    PushToastToJs(false, "지금은 변경할 수 없습니다.");
                    return;
                }
                if (MapDeviceRuntime::IsUninstallCleanupArmed() && op != "mapSafeEscape") {
                    PushToastToJs(false, "안전 제거 정리 완료 상태입니다. 저장 후 게임을 종료하세요.");
                    PushPanelStateToJs();
                    return;
                }

                // 5단계(결정 161): 정신력 오라/전령 ON/OFF 토글 명령
                if (op == "toggleAura") {
                    std::int64_t slotVal = -1;
                    CraftActionResult auraRes;
                    if (!ReadBoundedInt(cmd, "slot", 0, static_cast<std::int64_t>(Core::kSkillSlotCount - 1), slotVal)) {
                        auraRes = { false, "잘못된 스킬 칸 번호입니다." };
                    } else {
                        auraRes = CraftingRuntime::ToggleAuraSlot(static_cast<std::uint8_t>(slotVal));
                        if (auraRes.ok) {
                            (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                        }
                    }
                    PushToastToJs(auraRes.ok, auraRes.message);
                    PushPanelStateToJs();
                    return;
                }

                // 3~5단계 패시브 트리 변경 및 주얼 소켓 명령 분기
                if (op.starts_with("tree")) {
                    if (!g_treeData || !g_treeData->IsLoaded()) {
                        PushToastToJs(false, "패시브 트리 데이터가 로드되지 않았습니다.");
                        return;
                    }
                    // 5단계 주얼 소켓 장착·탈착 및 주얼 반환 포함 노드/전체 회수는 CraftingRuntime으로 위임(결정 105·164)
                    if (op == "treeSocketJewel") {
                        std::int64_t nidVal = 0, baseFormVal = 0, uidVal = 0;
                        CraftActionResult jRes;
                        if (!ReadBoundedInt(cmd, "nodeId", 1, 0xFFFFFFFFLL, nidVal) ||
                            !ReadBoundedInt(cmd, "baseFormId", 1, 0xFFFFFFFFLL, baseFormVal) ||
                            !ReadBoundedInt(cmd, "uniqueId", 0, 0xFFFFLL, uidVal)) {
                            jRes = { false, "잘못된 주얼 장착 요청입니다." };
                        } else {
                            jRes = CraftingRuntime::SocketPassiveJewel(
                                static_cast<std::uint32_t>(nidVal),
                                static_cast<RE::FormID>(baseFormVal),
                                static_cast<std::uint16_t>(uidVal));
                            if (jRes.ok) {
                                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                            }
                        }
                        PushToastToJs(jRes.ok, jRes.message);
                        PushPanelStateToJs();
                        return;
                    }
                    if (op == "treeUnsocketJewel") {
                        std::int64_t nidVal = 0;
                        CraftActionResult jRes;
                        if (!ReadBoundedInt(cmd, "nodeId", 1, 0xFFFFFFFFLL, nidVal)) {
                            jRes = { false, "잘못된 주얼 회수 요청입니다." };
                        } else {
                            jRes = CraftingRuntime::UnsocketPassiveJewel(static_cast<std::uint32_t>(nidVal));
                            if (jRes.ok) {
                                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                            }
                        }
                        PushToastToJs(jRes.ok, jRes.message);
                        PushPanelStateToJs();
                        return;
                    }
                    if (op == "treeRefundNode") {
                        std::int64_t nidVal = 0, expectedCost = 0;
                        Core::PassiveTreeOpResult treeRes;
                        if (!ReadBoundedInt(cmd, "nodeId", 1, 0xFFFFFFFFLL, nidVal)) {
                            treeRes = { false, "잘못된 노드 번호입니다.", 0, 0 };
                        } else if (!ReadBoundedInt(cmd, "expectedRegretCost", 0, 0xFFFFFFFFLL, expectedCost)) {
                            treeRes = { false, "노드 회수 비용을 다시 확인해 주세요." };
                        } else {
                            const auto nodeId = static_cast<std::uint32_t>(nidVal);
                            const auto state = SaveLoad::GetStateCopy();
                            const auto dependents = Core::PassiveTree::RefundDependents(*g_treeData, state.tree, nodeId);
                            if (!dependents.empty()) {
                                treeRes.message = "이 노드가 해금한 패시브를 먼저 회수해야 합니다.";
                                for (const auto id : dependents) {
                                    treeRes.message += " #" + std::to_string(id);
                                }
                            } else if (expectedCost != Core::PassiveTree::GetRefundNodeCost(
                                *g_treeData, state.tree, nodeId, *g_config)) {
                                treeRes.message = "노드 회수 비용이 달라졌습니다. 다시 확인해 주세요.";
                            } else {
                                treeRes = CraftingRuntime::RefundPassiveNodeWithRegret(nodeId);
                                if (treeRes.ok) {
                                    (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                                }
                            }
                        }
                        PushToastToJs(treeRes.ok, treeRes.message);
                        PushPanelStateToJs();
                        return;
                    }
                    if (op == "treeRefundAll") {
                        const auto scope = cmd.find("ascendancyOnly");
                        std::int64_t expectedCost = 0;
                        Core::PassiveTreeOpResult treeRes;
                        if (scope == cmd.end() || !scope->is_boolean() ||
                            !ReadBoundedInt(cmd, "expectedRegretCost", 0, 0xFFFFFFFFLL, expectedCost)) {
                            treeRes.message = "회수 범위와 비용을 다시 확인해 주세요.";
                        } else {
                            const bool ascOnly = scope->get<bool>();
                            const auto state = SaveLoad::GetStateCopy();
                            if (expectedCost != Core::PassiveTree::GetRefundAllCost(*g_treeData, state.tree, ascOnly, *g_config)) {
                                treeRes.message = "전체 회수 비용이 달라졌습니다. 다시 확인해 주세요.";
                            } else {
                                treeRes = CraftingRuntime::RefundAllPassivesWithRegret(ascOnly);
                                if (treeRes.ok) {
                                    (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                                }
                            }
                        }
                        PushToastToJs(treeRes.ok, treeRes.message);
                        PushPanelStateToJs();
                        return;
                    }

                    Core::PassiveTreeOpResult treeRes;
                    // 잠금 계층 + 원자성 준수(결정 118, 검수 C2/M4):
                    // 사본에서 먼저 검증·연산 → 필요 후회의 오브 차감 성공 확인 → 성공 시에만 g_state 커밋
                    SaveLoad::RunSerializedTransaction([&]() {
                        const auto availOrbs = static_cast<std::uint32_t>(std::max<std::int32_t>(
                            0, CraftingRuntime::CountCurrency(player, Core::CurrencyKind::Regret)));
                        PersistentState working = SaveLoad::GetStateCopy();
                        if (op == "treeSelectClass") {
                            if (const auto id = cmd.find("classContentId"); id != cmd.end()) {
                                if (!id->is_string()) {
                                    treeRes = { false, "잘못된 클래스 ID입니다." };
                                    return;
                                }
                                treeRes = Core::PassiveTree::SelectClass(
                                    *g_treeData, working.tree, id->get_ref<const std::string&>());
                            } else {
                                std::int64_t clsVal = -1;
                                if (!ReadBoundedInt(cmd, "classId", 0,
                                    static_cast<std::int64_t>(g_treeData->Classes().size()) - 1, clsVal)) {
                                    treeRes = { false, "잘못된 클래스 번호입니다." };
                                    return;
                                }
                                treeRes = Core::PassiveTree::SelectClass(
                                    *g_treeData, working.tree, static_cast<std::uint8_t>(clsVal));
                            }
                            if (treeRes.ok) {
                                if (const auto* cls = g_treeData->FindClass(working.tree.classId)) {
                                    working.className = cls->name;
                                }
                            }
                        } else if (op == "treeSelectAscendancy") {
                            const std::string ascId = cmd.contains("ascendancyId") && cmd["ascendancyId"].is_string()
                                ? cmd["ascendancyId"].get<std::string>() : "";
                            treeRes = Core::PassiveTree::SelectAscendancy(*g_treeData, working.tree, ascId, *g_config);
                        } else if (op == "treeClearAscendancy") {
                            treeRes = Core::PassiveTree::ClearAscendancy(*g_treeData, working.tree);
                        } else if (op == "treeAllocatePath") {
                            std::vector<std::uint32_t> nodes;
                            if (!cmd.contains("nodes") || !cmd["nodes"].is_array() || cmd["nodes"].size() > 256) {
                                treeRes = { false, "잘못된 패시브 경로입니다." };
                                return;
                            }
                            for (const auto& value : cmd["nodes"]) {
                                if (!value.is_number_unsigned() || value.get<std::uint64_t>() == 0 ||
                                    value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
                                    treeRes = { false, "잘못된 패시브 노드 번호입니다." };
                                    return;
                                }
                                nodes.push_back(value.get<std::uint32_t>());
                            }
                            std::unordered_map<std::uint32_t, std::uint32_t> attributeChoices;
                            if (const auto choices = cmd.find("attributeChoices"); choices != cmd.end()) {
                                if (!choices->is_array() || choices->size() > nodes.size()) {
                                    treeRes = { false, "잘못된 능력치 선택 요청입니다." };
                                    return;
                                }
                                for (const auto& choice : *choices) {
                                    std::int64_t nodeId = 0, optionId = 0;
                                    if (!ReadBoundedInt(choice, "nodeId", 1, 0xFFFFFFFFLL, nodeId) ||
                                        !ReadBoundedInt(choice, "optionId", 1, 0xFFFFFFFFLL, optionId) ||
                                        std::ranges::find(nodes, static_cast<std::uint32_t>(nodeId)) == nodes.end() ||
                                        !attributeChoices.emplace(static_cast<std::uint32_t>(nodeId),
                                            static_cast<std::uint32_t>(optionId)).second) {
                                        treeRes = { false, "잘못된 능력치 선택 요청입니다." };
                                        return;
                                    }
                                }
                            }
                            treeRes = Core::PassiveTree::AllocatePath(
                                *g_treeData, working.tree, nodes, working.exileLevel, *g_config,
                                working.atlas.completedTierMask, attributeChoices);
                        } else if (op == "treeSetAttribute") {
                            std::int64_t nodeId = 0, optionId = 0, expectedCost = 0;
                            if (!ReadBoundedInt(cmd, "nodeId", 1, 0xFFFFFFFFLL, nodeId) ||
                                !ReadBoundedInt(cmd, "optionId", 1, 0xFFFFFFFFLL, optionId) ||
                                !ReadBoundedInt(cmd, "expectedRegretCost", 0, 1, expectedCost)) {
                                treeRes = { false, "잘못된 능력치 변경 요청입니다." };
                                return;
                            }
                            treeRes = Core::PassiveTree::ChangeAttribute(*g_treeData, working.tree,
                                static_cast<std::uint32_t>(nodeId), static_cast<std::uint32_t>(optionId), *g_config, availOrbs);
                            if (treeRes.ok && expectedCost != treeRes.regretOrbsConsumed) {
                                treeRes = { false, "능력치 변경 비용이 달라졌습니다. 다시 확인해 주세요." };
                            }
                        } else if (op == "treeSelectMastery") {
                            std::int64_t mNodeVal = 0;
                            std::int64_t effVal = 0;
                            if (!ReadBoundedInt(cmd, "masteryNodeId", 1, 0xFFFFFFFFLL, mNodeVal) ||
                                !ReadBoundedInt(cmd, "effectId", 1, 0xFFFFFFFFLL, effVal)) {
                                treeRes = { false, "잘못된 마스터리 요청입니다.", 0, 0 };
                                return;
                            }
                            treeRes = Core::PassiveTree::SelectMastery(
                                *g_treeData, working.tree,
                                static_cast<std::uint32_t>(mNodeVal),
                                static_cast<std::uint32_t>(effVal),
                                working.exileLevel, *g_config, availOrbs, working.atlas.completedTierMask);
                        } else {
                            treeRes = { false, "지원하지 않는 패시브 트리 명령입니다.", 0, 0 };
                            return;
                        }

                        if (!treeRes.ok) {
                            return;
                        }
                        if (treeRes.regretOrbsConsumed > 0 &&
                            !CraftingRuntime::ConsumeCurrency(
                                player, Core::CurrencyKind::Regret, static_cast<std::int32_t>(treeRes.regretOrbsConsumed))) {
                            treeRes = { false, "후회의 오브 차감에 실패해 변경을 취소했습니다.", 0, 0 };
                            return;
                        }
                        const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                            *g_treeData, working.tree, working.exileLevel, *g_config, working.atlas.completedTierMask);
                        const auto nextUnalloc = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
                        // C-30: 상태 전체 덮어쓰기 대신 트리 관련 필드만 병합해 동시 변경 유실을 차단한다.
                        SaveLoad::UpdateState([&](PersistentState& cur) {
                            cur.tree = working.tree;
                            cur.className = working.className;
                            cur.unallocatedPassivePoints = nextUnalloc;
                        });
                    });

                    if (treeRes.ok) {
                        SaveLoad::SyncStateToStatEngine();
                        (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                    }
                    PushToastToJs(treeRes.ok, treeRes.message);
                    PushPanelStateToJs();
                    return;
                }

                // 4단계 장비·제작 탭 명령 분기(결정 157, 158, 160)
                if (op.starts_with("gear")) {
                    // 결정 239·248: 세대 1 패키지는 P5까지 감정·화폐·쥬얼러·고유·반지·벨트·참 장착을 받지 않는다(회수는 허용).
                    if (!Core::ActiveContentFeatures().crafting && !op.starts_with("gearUnequip")) {
                        g_gearGateRejections.fetch_add(1, std::memory_order_relaxed);
                        PushToastToJs(false, "세대 1(PoE2)에서는 아직 장비·제작 기능이 없습니다. P5에서 엽니다.");
                        PushPanelStateToJs();
                        return;
                    }
                    g_gearGatePasses.fetch_add(1, std::memory_order_relaxed);
                    if (!g_itemData || !g_itemData->IsLoaded()) {
                        PushToastToJs(false, "아이템 데이터가 로드되지 않았습니다.");
                        return;
                    }
                    CraftActionResult craftRes;
                    if (op == "gearIdentifyToken") {
                        std::int64_t tierVal = 0;
                        std::int64_t slotGroupVal = 0;
                        std::int64_t rarityVal = 0;
                        if (!ReadBoundedInt(cmd, "tier", 1, 5, tierVal) ||
                            !ReadBoundedInt(cmd, "slotGroup", 0, static_cast<std::int64_t>(Core::kDroppableTokenSlotGroupCount - 1), slotGroupVal)) {
                            craftRes = { false, "잘못된 봉인 토큰 정보입니다." };
                        } else {
                            Core::ItemRarity rar = Core::ItemRarity::Magic;
                            bool rarOk = false;
                            if (ReadBoundedInt(cmd, "rarityCode", 1, 2, rarityVal)) {
                                rar = static_cast<Core::ItemRarity>(rarityVal);
                                rarOk = true;
                            } else if (cmd.contains("rarity") && cmd["rarity"].is_string()) {
                                const auto rs = cmd["rarity"].get<std::string>();
                                if (rs == "magic") {
                                    rar = Core::ItemRarity::Magic;
                                    rarOk = true;
                                } else if (rs == "rare") {
                                    rar = Core::ItemRarity::Rare;
                                    rarOk = true;
                                }
                            }
                            if (!rarOk) {
                                craftRes = { false, "잘못된 봉인 토큰 희귀도입니다." };
                            } else {
                                craftRes = CraftingRuntime::IdentifyToken(
                                    static_cast<std::uint8_t>(tierVal),
                                    rar,
                                    static_cast<Core::TokenSlotGroup>(slotGroupVal));
                            }
                        }
                    } else if (op == "gearApplyOrb") {
                        std::int64_t orbVal = -1;
                        std::int64_t baseFormVal = 0;
                        std::int64_t uidVal = 0;
                        const bool preferEquipped = cmd.contains("preferEquipped") &&
                            cmd["preferEquipped"].is_boolean() && cmd["preferEquipped"].get<bool>();
                        if (!ReadBoundedInt(cmd, "orbKind", 0, static_cast<std::int64_t>(Core::kCurrencyKindCount - 1), orbVal) ||
                            !ReadBoundedInt(cmd, "baseFormId", 1, 0xFFFFFFFFLL, baseFormVal) ||
                            !ReadBoundedInt(cmd, "uniqueId", 0, 0xFFFFLL, uidVal)) {
                            craftRes = { false, "잘못된 오브 제작 요청입니다." };
                        } else {
                            craftRes = CraftingRuntime::ApplyOrbToItem(
                                static_cast<Core::CurrencyKind>(orbVal),
                                static_cast<RE::FormID>(baseFormVal),
                                static_cast<std::uint16_t>(uidVal),
                                preferEquipped);
                        }
                    } else if (op == "gearApplyJeweller") {
                        std::int64_t slotVal = -1;
                        std::uint64_t spellInstId = 0;
                        bool spellIdValid = true;
                        if (cmd.contains("triggeredSpellInstanceId")) {
                            if (cmd["triggeredSpellInstanceId"].is_number_unsigned()) {
                                spellInstId = cmd["triggeredSpellInstanceId"].get<std::uint64_t>();
                            } else {
                                spellIdValid = false;
                            }
                        }
                        if (!spellIdValid ||
                            !ReadBoundedInt(cmd, "slotIndex", 0, static_cast<std::int64_t>(Core::kSkillSlotCount - 1), slotVal)) {
                            craftRes = { false, "잘못된 젬 소켓 확장 요청입니다." };
                        } else {
                            craftRes = CraftingRuntime::ApplyJewellerToGem(
                                static_cast<std::uint8_t>(slotVal),
                                spellInstId);
                        }
                    } else if (op == "gearIdentifyUnique") {
                        // C-19: 사용자 JS 명령 경로에서는 시험용 forcedUniqueId 지정을 허용하지 않는다.
                        craftRes = CraftingRuntime::IdentifyUniqueToken("");
                    } else if (op == "gearEquipRing2") {
                        std::int64_t baseFormVal = 0, uidVal = 0;
                        if (!ReadBoundedInt(cmd, "baseFormId", 1, 0xFFFFFFFFLL, baseFormVal) ||
                            !ReadBoundedInt(cmd, "uniqueId", 0, 0xFFFFLL, uidVal)) {
                            craftRes = { false, "잘못된 반지2 장착 요청입니다." };
                        } else {
                            craftRes = CraftingRuntime::EquipVirtualRing2(
                                static_cast<RE::FormID>(baseFormVal), static_cast<std::uint16_t>(uidVal));
                        }
                    } else if (op == "gearUnequipRing2") {
                        craftRes = CraftingRuntime::UnequipVirtualRing2();
                    } else if (op == "gearEquipBelt") {
                        std::int64_t baseFormVal = 0, uidVal = 0;
                        if (!ReadBoundedInt(cmd, "baseFormId", 1, 0xFFFFFFFFLL, baseFormVal) ||
                            !ReadBoundedInt(cmd, "uniqueId", 0, 0xFFFFLL, uidVal)) {
                            craftRes = { false, "잘못된 벨트 장착 요청입니다." };
                        } else {
                            craftRes = CraftingRuntime::EquipVirtualBelt(
                                static_cast<RE::FormID>(baseFormVal), static_cast<std::uint16_t>(uidVal));
                        }
                    } else if (op == "gearUnequipBelt") {
                        craftRes = CraftingRuntime::UnequipVirtualBelt();
                    } else if (op == "gearEquipCharm") {
                        std::int64_t slotIdxVal = -1, kindVal = -1;
                        if (!ReadBoundedInt(cmd, "slotIndex", 0, static_cast<std::int64_t>(Core::kMaxCharmSlots - 1), slotIdxVal) ||
                            !ReadBoundedInt(cmd, "charmKind", 0, static_cast<std::int64_t>(Core::kCharmKindCount - 1), kindVal)) {
                            craftRes = { false, "잘못된 참 장착 요청입니다." };
                        } else {
                            craftRes = CraftingRuntime::EquipCharm(
                                static_cast<std::uint8_t>(slotIdxVal), static_cast<Core::CharmKind>(kindVal));
                        }
                    } else if (op == "gearUnequipCharm") {
                        std::int64_t slotIdxVal = -1;
                        if (!ReadBoundedInt(cmd, "slotIndex", 0, static_cast<std::int64_t>(Core::kMaxCharmSlots - 1), slotIdxVal)) {
                            craftRes = { false, "잘못된 참 회수 요청입니다." };
                        } else {
                            craftRes = CraftingRuntime::UnequipCharm(static_cast<std::uint8_t>(slotIdxVal));
                        }
                    } else {
                        craftRes = { false, "지원하지 않는 장비·제작 명령입니다." };
                    }

                    PushToastToJs(craftRes.ok, craftRes.message);
                    PushPanelStateToJs();
                    return;
                }

                // 6단계(결정 166): 지도 장치·4차 결투장·안전 제거 탭 명령 분기
                if (op.starts_with("map")) {
                    if (MapDeviceRuntime::GetActiveRunId() != mapRunId) {
                        PushToastToJs(false, "지도 회차가 바뀌었습니다. 다시 시도하세요.");
                        PushPanelStateToJs();
                        return;
                    }
                    MapActionResult mapRes{};
                    bool closeOnSuccess = false;
                    if (op == "mapOpen") {
                        std::int64_t baseFormVal = 0;
                        std::int64_t ownerVal = 0;
                        std::int64_t uidVal = 0;
                        if (!ReadBoundedInt(cmd, "baseFormId", 1, 0xFFFFFFFFLL, baseFormVal) ||
                            !ReadBoundedInt(cmd, "ownerFormId", 0, 0xFFFFFFFFLL, ownerVal) ||
                            !ReadBoundedInt(cmd, "uniqueId", 0, 0xFFFFLL, uidVal)) {
                            mapRes = { false, "잘못된 지도 개방 요청입니다." };
                        } else {
                            const Core::InventoryItemKey mKey{
                                static_cast<std::uint32_t>(ownerVal),
                                static_cast<std::uint16_t>(uidVal)
                            };
                            mapRes = MapDeviceRuntime::OpenNormalMap(
                                player, mKey, static_cast<RE::FormID>(baseFormVal));
                            closeOnSuccess = true;
                        }
                    } else if (op == "mapOpenArena4") {
                        mapRes = MapDeviceRuntime::OpenArena4(player);
                        closeOnSuccess = true;
                    } else if (op == "mapReenter") {
                        mapRes = MapDeviceRuntime::ReenterActiveMap(player);
                        closeOnSuccess = true;
                    } else if (op == "mapReturnPortal") {
                        mapRes = MapDeviceRuntime::ReturnPortal(player);
                        closeOnSuccess = true;
                    } else if (op == "mapAbandon") {
                        mapRes = MapDeviceRuntime::AbandonActiveMap(player);
                        closeOnSuccess = MapDeviceRuntime::GetTravelStatus(player).kind == Core::MapTravelKind::Abandon;
                    } else if (op == "mapSafeEscape") {
                        mapRes = MapDeviceRuntime::SafeEscape(player);
                        closeOnSuccess = true;
                    } else if (op == "mapSafeUninstall") {
                        mapRes = MapDeviceRuntime::RunSafeUninstallCleanup(player);
                    } else {
                        mapRes = { false, "지원하지 않는 지도 장치 명령입니다." };
                    }

                    PushToastToJs(mapRes.ok, mapRes.message);
                    if (mapRes.ok && closeOnSuccess) {
                        // 패널이 토스트와 함께 닫히므로 요청 접수도 HUD로 남긴다. 도착 확정은 별도 HUD 메시지다.
                        RE::SendHUDMessage::ShowHUDMessage(mapRes.message.c_str());
                        UiBridge::ClosePanelOnMainThread();
                    } else {
                        PushPanelStateToJs();
                    }
                    return;
                }

                Core::GemTransactionResult txRes;
                // 결정 137(마)(ㅁ): 주 갱신 한 틱 안에서 코세이브 저장과 직렬화해 원자적으로 젬 거래 수행
                SaveLoad::RunSerializedTransaction([&]() {
                    PersistentState st = SaveLoad::GetStateCopy();
                    PlayerGemAdapter adapter(player);

                    std::int64_t slotVal = -1;
                    if (!ReadBoundedInt(cmd, "slot", 0, static_cast<std::int64_t>(Core::kSkillSlotCount - 1), slotVal)) {
                        txRes = { false, "잘못된 스킬 칸 번호입니다.", std::nullopt };
                        return;
                    }
                    const auto slot = static_cast<std::uint8_t>(slotVal);

                    auto readItemKey = [&]() -> Core::InventoryItemKey {
                        std::int64_t ownerVal = 0;
                        std::int64_t uidVal = 0;
                        const bool okOwner = ReadBoundedInt(cmd, "ownerFormId", 0, 0xFFFFFFFFLL, ownerVal);
                        const bool okUid = ReadBoundedInt(cmd, "uniqueId", 0, 0xFFFFLL, uidVal);
                        if (!okOwner || !okUid) {
                            return Core::InventoryItemKey{};
                        }
                        return Core::InventoryItemKey{
                            static_cast<std::uint32_t>(ownerVal),
                            static_cast<std::uint16_t>(uidVal)
                        };
                    };

                    if (op == "equip") {
                        const std::string gemId = cmd.contains("gemId") && cmd["gemId"].is_string() ? cmd["gemId"].get<std::string>() : "";
                        txRes = Core::GemTransaction::EquipSkillGem(
                            st.slots, st.nextGemInstanceId, slot, readItemKey(), gemId, g_config->starterSockets,
                            *g_dataPack, adapter, SaveLoad::WorldGemGeneration());
                    } else if (op == "unequip") {
                        txRes = Core::GemTransaction::UnequipSkillGem(st.slots, slot, adapter);
                    } else if (op == "socket") {
                        std::int64_t spellIdx = -1;
                        (void)ReadBoundedInt(cmd, "spellIdx", -1, 16, spellIdx);
                        const std::string gemId = cmd.contains("gemId") && cmd["gemId"].is_string() ? cmd["gemId"].get<std::string>() : "";
                        txRes = Core::GemTransaction::SocketSubGem(
                            st.slots, st.nextGemInstanceId, slot, static_cast<std::int32_t>(spellIdx),
                            readItemKey(), gemId, g_config->starterSockets, *g_dataPack, adapter, SaveLoad::WorldGemGeneration());
                    } else if (op == "unsocket") {
                        std::int64_t spellIdx = -1;
                        std::int64_t socketIdx = 0;
                        (void)ReadBoundedInt(cmd, "spellIdx", -1, 16, spellIdx);
                        if (!ReadBoundedInt(cmd, "socketIdx", 0, 16, socketIdx)) {
                            txRes = { false, "잘못된 소켓 번호입니다.", std::nullopt };
                            return;
                        }
                        txRes = Core::GemTransaction::UnsocketSubGem(
                            st.slots, slot, static_cast<std::int32_t>(spellIdx), static_cast<std::size_t>(socketIdx), adapter);
                    } else if (op == "link") {
                        std::int64_t atkSlot = -1;
                        if (!ReadBoundedInt(cmd, "attackSlot", -1, static_cast<std::int64_t>(Core::kSkillSlotCount - 1), atkSlot)) {
                            txRes = { false, "잘못된 연결 공격 칸입니다.", std::nullopt };
                            return;
                        }
                        txRes = Core::GemTransaction::LinkTriggerAttack(
                            st.slots, slot, static_cast<std::int8_t>(atkSlot), *g_dataPack, SaveLoad::WorldGemGeneration());
                    } else {
                        txRes = { false, "지원하지 않는 UI 명령입니다.", std::nullopt };
                    }

                    if (txRes.ok) {
                        SaveLoad::UpdateState([&](PersistentState& cur) {
                            cur.slots = st.slots;
                            cur.nextGemInstanceId = std::max(cur.nextGemInstanceId, st.nextGemInstanceId);
                            // 검수 M8: 스킬 칸 교체·탈착 시 이전 오라 토글 비트를 정리해 새 오라 자동 ON을 방지
                            if (op == "equip" || op == "unequip") {
                                cur.activeAuraMask &= static_cast<std::uint16_t>(~(1u << slot));
                            }
                        });
                    } else if (st.nextGemInstanceId > 0) {
                        // C-6: 롤백으로 새 instanceId가 인벤토리에 반환된 경우에도 단조 증가값을 보존해 ID 중복을 막는다.
                        SaveLoad::UpdateState([&](PersistentState& cur) {
                            cur.nextGemInstanceId = std::max(cur.nextGemInstanceId, st.nextGemInstanceId);
                        });
                    }
                });

                if (txRes.ok) {
                    SaveLoad::SyncStateToStatEngine();
                    (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                    ProxySpellRuntime::SyncProxySpellsFromState();
                }
                if (gemResult) {
                    *gemResult = { txRes.ok, txRes.message };
                }
                PushToastToJs(txRes.ok, txRes.message);
                PushPanelStateToJs();
            } catch (const std::exception& e) {
                SKSE::log::warn("UiBridge: JS command handling failed: {}", e.what());
            }
        }

        void OnJsCommand(const char* argument)
        {
            if (!ModState::AllowsGameplay() || !argument || !g_queue) {
                return;
            }
            const auto generation = g_queue->CurrentGeneration();
            std::string raw(argument);
            const auto mapRunId = MapDeviceRuntime::GetActiveRunId();
            g_queue->EnqueueForGeneration(generation, [raw = std::move(raw), mapRunId]() {
                ExecuteCommandOnMainThread(raw, mapRunId);
            });
        }

        void EnsureViewCreated(bool forceNow)
        {
            if (ViewUsable()) {
                return;
            }
            const auto now = Clock::now();
            if (!forceNow && g_hasTriedCreate && (now - g_lastCreateAttempt < kViewRetryCooldown)) {
                return;
            }
            g_hasTriedCreate = true;
            g_lastCreateAttempt = now;

            if (!g_api) {
                g_api = PRISMA_UI_API::RequestPluginAPI();
            }
            if (!g_api) {
                return;
            }
            g_uiBuildMismatch = false;
            g_domReady.store(false, std::memory_order_release);
            g_lastProcessedReqId.store(0, std::memory_order_release);
            g_view = g_api->CreateView(kViewPath, [](PRISMA_UI_API::PrismaView) {
                g_lastProcessedReqId.store(0, std::memory_order_release);
                g_domReady.store(true, std::memory_order_release);
                SKSE::log::info("UiBridge: Prisma UI DOM ready ('{}').", kViewPath);
            });
            if (g_view == 0 || !g_api->IsValid(g_view)) {
                SKSE::log::error("UiBridge: CreateView('{}') failed.", kViewPath);
                g_view = 0;
                return;
            }
            g_api->RegisterJSListener(g_view, kJsCommandListener, OnJsCommand);
            g_api->Hide(g_view);
            g_hudShown.store(false, std::memory_order_release);
            SKSE::log::info("UiBridge: created single Prisma UI view (view={}, order={}).", g_view, g_api->GetOrder(g_view));
        }

        void OpenPanelOnMainThread(std::uint8_t targetTab)
        {
            if (g_panelOpen.load(std::memory_order_acquire)) {
                SwitchTabOnMainThread(targetTab);
                return;
            }
            if (!IsInGameplay() || IsOtherBlockingMenuOpen()) {
                return;
            }
            EnsureViewCreated(true);
            if (!ViewUsable() || !g_domReady.load(std::memory_order_acquire)) {
                RE::SendHUDMessage::ShowHUDMessage("[SkyrimPoE] UI 화면을 불러오는 중입니다. 잠시 후 다시 눌러 주세요.");
                return;
            }

            // 결정 137(바): 다른 Prisma UI 화면이 이미 포커스를 쥐고 있으면 열지 않는다.
            if (g_api->HasAnyActiveFocus()) {
                SKSE::log::info("UiBridge: another Prisma UI view holds focus; panel open skipped.");
                return;
            }

            (void)InstallDeferredInputHook();

            if (!g_hudShown.load(std::memory_order_acquire)) {
                g_api->Show(g_view);
                g_hudShown.store(true, std::memory_order_release);
            }

            // 결정 61, 137(바): 게임을 멈추고 포커스를 요청하되, 실패 시 상태를 열림으로 바꾸지 않고 원복한다.
            const bool focused = g_api->Focus(g_view, true, false);
            if (!focused) {
                SKSE::log::warn("UiBridge: Prisma Focus() returned false; rolling back panel open.");
                if (g_api->HasFocus(g_view)) {
                    g_api->Unfocus(g_view);
                }
                return;
            }

            g_acquiredFocus = true;
            g_activeTab.store(targetTab, std::memory_order_release);
            g_searchFocused.store(false, std::memory_order_release);
            g_panelOpen.store(true, std::memory_order_release);
            // 결정 263: 새 게임·일반 로드 뒤 미뤄 둔 시작 젬 보충을 패널을 처음 열 때 한다(패널 상태에 바로 보이게 먼저).
            (void)SaveLoad::GrantPendingStarterOnPanelOpen();
            const std::string openArg = json{ { "tab", std::string(TabNameFromId(targetTab)) } }.dump();
            g_api->InteropCall(g_view, kJsOnPanelOpen, openArg.c_str());
            PushPanelStateToJs();
        }
    }

    bool UiBridge::Install(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        Core::EnergyShieldPool* esPool,
        const Core::DataPack* dataPack,
        const Core::PassiveTreeData* treeData,
        const Core::ItemData* itemData,
        const Core::Config* config,
        Core::SkillSystem* skillSystem,
        Core::CharmSystem* charmSystem)
    {
        if (g_installed) {
            return true;
        }
        g_queue = queue;
        g_statEngine = statEngine;
        g_esPool = esPool;
        g_dataPack = dataPack;
        g_treeData = treeData;
        g_itemData = itemData;
        g_config = config;
        g_skillSystem = skillSystem;
        g_charmSystem = charmSystem;

        LoadLangNames();
        if (g_config) {
            SimpleImeBridge::Initialize(
                SimpleImeBridge::ParseMode(g_config->simpleImeMode),
                OnImeText,
                OnImeKey);
        }
        EnsureViewCreated(true);

        if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
            input->AddEventSink(&PreHookInputSink::Get());
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&OtherMenuCloseSink::Get());
        }

        // 코세이브 OnRevert 콜백에서는 원자 플래그만 세워 메인 스레드에서 안전하게 정리한다(결정 119).
        SaveLoad::AddRevertCallback([]() {
            g_needCloseOnMain.store(true, std::memory_order_release);
            g_needResetOnMain.store(true, std::memory_order_release);
        });

        g_installed = true;
        SKSE::log::info("UiBridge: initialized (prismaLoaded={}, langGems={}, simpleIme={}).",
            g_api != nullptr, g_gemDisplayNames.size(), SimpleImeBridge::Available());
        return true;
    }

    void UiBridge::ClosePanelOnMainThread()
    {
        // 검수 C1: g_panelOpen 확인 전에 반드시 SimpleImeBridge 텍스트 입력과 검색 포커스를 동기 해제한다.
        SimpleImeBridge::SetTextEntry(false);
        g_searchFocused.store(false, std::memory_order_release);
        g_needCloseOnMain.store(false, std::memory_order_release);
        if (!g_panelOpen.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        // 결정 137(바): 실제로 획득한 포커스만 해제한다.
        if (ViewUsable()) {
            if (g_acquiredFocus && g_api->HasFocus(g_view)) {
                g_api->Unfocus(g_view);
            }
            // 결정 120: 패널만 닫고 뷰는 Hide하지 않아 HUD를 유지한다.
            g_api->InteropCall(g_view, kJsOnPanelClose, "");
        }
        g_acquiredFocus = false;
    }

    namespace
    {
        void HideHudOnMainThread()
        {
            if (g_hudShown.load(std::memory_order_acquire)) {
                if (ViewUsable()) {
                    g_api->Hide(g_view);
                }
                g_hudShown.store(false, std::memory_order_release);
            }
        }
    }

    void UiBridge::SuspendHudOnMainThread()
    {
        ClosePanelOnMainThread();
        g_hiddenKeys.reset();
        HideHudOnMainThread();
    }

    void UiBridge::Tick(RE::Main* main)
    {
        if (!g_installed || !g_config) {
            return;
        }

        if (g_needResetOnMain.exchange(false, std::memory_order_acq_rel)) {
            SimpleImeBridge::SetTextEntry(false);
            g_searchFocused.store(false, std::memory_order_release);
            g_lastHud = CachedHud{};
            g_lastProcessedReqId.store(0, std::memory_order_release);
            g_hiddenKeys.reset();
        }

        // 결정 186: TrueHUD 특수 바를 쓰는 중이면 소유자 확인·안전 제거 해제·적 ES 표 갱신(소유하지 않으면 아무것도 안 함)
        TrueHudBridge::TickMainThread();

        // 창 전환(Alt+Tab 등 gameActive == false) 시 키 추적 테이블·텍스트 입력을 비우고 패널을 닫는다(결정 120).
        if (main && !main->gameActive) {
            SimpleImeBridge::SetTextEntry(false);
            g_searchFocused.store(false, std::memory_order_release);
            g_hiddenKeys.reset();
            if (g_panelOpen.load(std::memory_order_acquire)) {
                ClosePanelOnMainThread();
            }
            return;
        }

        const bool inGameplay = IsInGameplay();

        if (!inGameplay) {
            SimpleImeBridge::SetTextEntry(false);
            g_searchFocused.store(false, std::memory_order_release);
            g_hiddenKeys.reset();
            ClosePanelOnMainThread();
        } else if (g_needCloseOnMain.exchange(false, std::memory_order_acq_rel)) {
            // 다른 메뉴 열림으로 닫을 때는 g_hiddenKeys를 즉시 비우지 않아 누르고 있던 키의 뗌(Up) 누수를 방지한다.
            ClosePanelOnMainThread();
        }

        if (!ViewUsable()) {
            if (inGameplay) {
                EnsureViewCreated(false);
            }
            return;
        }
        if (!g_domReady.load(std::memory_order_acquire)) {
            return;
        }

        // 메인 메뉴·로딩·바닐라 전체화면 메뉴(지도·인벤토리·일지·제작 등) 중에는 HUD를 숨긴다.
        const bool panelOpen = g_panelOpen.load(std::memory_order_acquire);
        const bool showView = inGameplay && (panelOpen || !IsOtherBlockingMenuOpen());
        if (!showView) {
            HideHudOnMainThread();
            return;
        }
        if (!g_hudShown.load(std::memory_order_acquire)) {
            g_api->Show(g_view);
            g_hudShown.store(true, std::memory_order_release);
            g_lastHud.valid = false;
            g_lastEnemyEs.valid = false;
        }
        if (panelOpen && g_activeTab.load(std::memory_order_acquire) == kTabMaps &&
            g_panelMapTravel != MapDeviceRuntime::GetTravelStatus(RE::PlayerCharacter::GetSingleton())) {
            PushPanelStateToJs();
        }

        PushEnemyEsIfChanged(panelOpen);

        // HUD 값 변경 시에만 C++ -> JS 푸시(프레임 유휴 비용 0 유지)
        const auto es = g_esPool ? g_esPool->Snapshot() : Core::EnergyShieldState{};
        const auto state = SaveLoad::GetStateCopy();
        const auto snap = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
        const auto guardSnap = g_skillSystem ? g_skillSystem->GuardSnapshot() : Core::GuardShieldSnapshot{};
        const int esCur = static_cast<int>(std::llround(es.current));
        const int esMax = static_cast<int>(std::llround(es.max));
        const auto reqXp = Core::KillTracker::RequiredXpForNextExileLevel(state.exileLevel, *g_config);
        const std::int32_t availPts = (g_treeData && g_treeData->IsLoaded())
            ? std::max<std::int32_t>(0, Core::PassiveTree::ComputeAvailableNormalPoints(
                  *g_treeData, state.tree, state.exileLevel, *g_config, state.atlas.completedTierMask))
            : static_cast<std::int32_t>(state.unallocatedPassivePoints);
        const std::uint32_t spiritRes = snap ? snap->reservedSpirit : 0u;
        const std::uint32_t spiritMax = snap ? snap->maxSpirit : g_config->spiritBase;
        const int guardCur = guardSnap.active ? static_cast<int>(std::llround(guardSnap.currentShieldSkyrim)) : 0;
        const int guardMax = guardSnap.active ? static_cast<int>(std::llround(guardSnap.maxShieldSkyrim)) : 0;
        const std::size_t hhStacks = MonsterRuntime::ActiveHeadhunterStackCount();
        const std::uint8_t charmUnlocked = g_charmSystem ? g_charmSystem->UnlockedSlots() : 0u;
        const auto charmBonuses = g_charmSystem ? g_charmSystem->ComputeActiveBonuses() : Core::CharmActiveBonuses{};
        const auto charmSlots = g_charmSystem ? g_charmSystem->Slots() : state.charmSlots;
        std::array<int, Core::kMaxCharmSlots> charmCharges{ -1, -1, -1 };
        std::array<int, Core::kMaxCharmSlots> charmKinds{ -1, -1, -1 };
        for (std::size_t ci = 0; ci < Core::kMaxCharmSlots; ++ci) {
            if (charmSlots[ci].occupied) {
                charmCharges[ci] = static_cast<int>(std::llround(charmSlots[ci].charges));
                charmKinds[ci] = static_cast<int>(charmSlots[ci].kind);
            }
        }

        const auto activeMapHud = MapDeviceRuntime::GetActiveMapStateCopy();
        const bool mapActive = activeMapHud.active;
        const bool mapIsArena4 = activeMapHud.active && activeMapHud.isArena4;
        const bool mapInCell = MapDeviceRuntime::IsPlayerInActiveMapCell();
        const bool mapBossKilled = activeMapHud.active && activeMapHud.bossKilled;
        const bool mapArena4Unlocked = mapIsArena4 &&
            (state.tree.ascendancyMilestonesBitmask & Core::kArena4MilestoneBit) != 0u;
        const std::uint8_t mapTier = activeMapHud.active ? activeMapHud.tier : 0u;
        const std::uint8_t mapPortalsRem = activeMapHud.active ? activeMapHud.portalsRemaining : 0u;
        const std::uint8_t mapArenaPhase = activeMapHud.active ? activeMapHud.arenaPhase : 1u;
        const std::uint32_t mapAreaLevel = activeMapHud.active
            ? (activeMapHud.isArena4 ? Core::kArena4MonsterLevel : Core::MapSystem::MonsterLevelForTier(activeMapHud.tier))
            : 0u;

        if (!g_lastHud.valid ||
            g_lastHud.esCurrent != esCur ||
            g_lastHud.esMax != esMax ||
            g_lastHud.exileLevel != state.exileLevel ||
            g_lastHud.exileXp != state.exileXp ||
            g_lastHud.exileReqXp != reqXp ||
            g_lastHud.unallocatedPts != availPts ||
            g_lastHud.spiritReserved != spiritRes ||
            g_lastHud.spiritMax != spiritMax ||
            g_lastHud.guardShield != guardCur ||
            g_lastHud.guardMax != guardMax ||
            g_lastHud.hhStacks != hhStacks ||
            g_lastHud.charmUnlocked != charmUnlocked ||
            g_lastHud.charmActiveMask != charmBonuses.activeMask ||
            g_lastHud.charmCharges != charmCharges ||
            g_lastHud.charmKinds != charmKinds ||
            g_lastHud.mapActive != mapActive ||
            g_lastHud.mapIsArena4 != mapIsArena4 ||
            g_lastHud.mapInCell != mapInCell ||
            g_lastHud.mapBossKilled != mapBossKilled ||
            g_lastHud.mapArena4Unlocked != mapArena4Unlocked ||
            g_lastHud.mapTier != mapTier ||
            g_lastHud.mapPortalsRem != mapPortalsRem ||
            g_lastHud.mapArenaPhase != mapArenaPhase ||
            g_lastHud.mapAreaLevel != mapAreaLevel) {
            g_lastHud.valid = true;
            g_lastHud.esCurrent = esCur;
            g_lastHud.esMax = esMax;
            g_lastHud.exileLevel = state.exileLevel;
            g_lastHud.exileXp = state.exileXp;
            g_lastHud.exileReqXp = reqXp;
            g_lastHud.unallocatedPts = availPts;
            g_lastHud.spiritReserved = spiritRes;
            g_lastHud.spiritMax = spiritMax;
            g_lastHud.guardShield = guardCur;
            g_lastHud.guardMax = guardMax;
            g_lastHud.hhStacks = hhStacks;
            g_lastHud.charmUnlocked = charmUnlocked;
            g_lastHud.charmActiveMask = charmBonuses.activeMask;
            g_lastHud.charmCharges = charmCharges;
            g_lastHud.charmKinds = charmKinds;
            g_lastHud.mapActive = mapActive;
            g_lastHud.mapIsArena4 = mapIsArena4;
            g_lastHud.mapInCell = mapInCell;
            g_lastHud.mapBossKilled = mapBossKilled;
            g_lastHud.mapArena4Unlocked = mapArena4Unlocked;
            g_lastHud.mapTier = mapTier;
            g_lastHud.mapPortalsRem = mapPortalsRem;
            g_lastHud.mapArenaPhase = mapArenaPhase;
            g_lastHud.mapAreaLevel = mapAreaLevel;

            json charmArr = json::array();
            for (std::size_t ci = 0; ci < charmUnlocked && ci < Core::kMaxCharmSlots; ++ci) {
                const auto& cs = charmSlots[ci];
                const auto* cMeta = (g_itemData && g_itemData->IsLoaded()) ? g_itemData->FindCharm(cs.kind) : nullptr;
                const bool act = cs.occupied &&
                    ((charmBonuses.activeMask & static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(cs.kind))) != 0);
                charmArr.push_back(json{
                    { "occupied", cs.occupied },
                    { "kind", static_cast<std::uint8_t>(cs.kind) },
                    { "koName", cMeta ? cMeta->koName : std::string("참") },
                    { "charges", charmCharges[ci] < 0 ? 0 : charmCharges[ci] },
                    { "maxCharges", static_cast<int>(Core::CharmSystem::kMaxCharges) },
                    { "active", act }
                });
            }

            json hud{
                { "esCurrent", esCur },
                { "esMax", esMax },
                { "exileLevel", state.exileLevel },
                { "exileXp", state.exileXp },
                { "exileReqXp", reqXp },
                { "unallocatedPoints", availPts },
                { "spiritReserved", spiritRes },
                { "spiritMax", spiritMax },
                { "guardShield", guardCur },
                { "guardMax", guardMax },
                { "hhStacks", hhStacks },
                { "charms", std::move(charmArr) },
                { "mapActive", mapActive },
                { "mapIsArena4", mapIsArena4 },
                { "mapInCell", mapInCell },
                { "mapBossKilled", mapBossKilled },
                { "mapArena4Unlocked", mapArena4Unlocked },
                { "mapTier", mapTier },
                { "mapPortalsRemaining", mapPortalsRem },
                { "mapPortalsTotal", mapIsArena4 ? Core::kArena4MaxPortals : Core::kNormalMapMaxPortals },
                { "mapArenaPhase", mapArenaPhase },
                { "mapAreaLevel", mapAreaLevel }
            };
            const std::string payload = hud.dump(-1, ' ', false, json::error_handler_t::replace);
            g_api->InteropCall(g_view, kJsUpdateHud, payload.c_str());
        }
    }

    Core::EnemyEsView UiBridge::GetLastEnemyEsViewForTest()
    {
        return g_lastEnemyEs.valid ? g_lastEnemyEs.view : Core::EnemyEsView{};
    }

    bool UiBridge::IsPanelOpen() noexcept
    {
        return g_panelOpen.load(std::memory_order_acquire);
    }

    bool UiBridge::IsDomReady() noexcept
    {
        return g_domReady.load(std::memory_order_acquire);
    }

    bool UiBridge::IsHudShown() noexcept
    {
        return g_hudShown.load(std::memory_order_acquire);
    }

    bool UiBridge::IsSearchFocused() noexcept
    {
        return g_searchFocused.load(std::memory_order_acquire);
    }

    std::string_view UiBridge::ActiveTab() noexcept
    {
        return TabNameFromId(g_activeTab.load(std::memory_order_acquire));
    }

    std::string UiBridge::GetGemDisplayName(std::string_view gemId)
    {
        return DisplayGemName(std::string(gemId));
    }

    bool UiBridge::TryOpenPanelOnMainThread(std::string_view initialTab)
    {
        OpenPanelOnMainThread(TabIdFromName(initialTab));
        return IsPanelOpen();
    }

    void UiBridge::ExecuteCommandForTestOnMainThread(std::string_view rawJson)
    {
        ExecuteCommandOnMainThread(std::string(rawJson), MapDeviceRuntime::GetActiveRunId());
    }

    std::pair<bool, std::string> UiBridge::ExecuteGemCommandForTestOnMainThread(std::string_view rawJson)
    {
        std::pair<bool, std::string> result;
        ExecuteCommandOnMainThread(std::string(rawJson), MapDeviceRuntime::GetActiveRunId(), &result);
        return result;
    }

    std::string UiBridge::BuildPanelJsonForTestOnMainThread()
    {
        return BuildPanelPayloadJson();
    }

    UiBridge::GateStats UiBridge::GetGateStatsForTest() noexcept
    {
        return {
            g_gearGateRejections.load(std::memory_order_relaxed),
            g_gearGatePasses.load(std::memory_order_relaxed),
        };
    }
}
