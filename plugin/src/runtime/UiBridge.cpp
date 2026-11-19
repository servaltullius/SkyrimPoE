#include "PCH.h"
#include "runtime/UiBridge.h"
#include "runtime/CraftingRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/GemInventory.h"
#include "runtime/PrismaUIApi.h"
#include "runtime/SaveLoad.h"
#include "runtime/SimpleImeBridge.h"

#include "core/GemTransaction.h"
#include "core/ItemCrafting.h"
#include "core/ItemFormatter.h"
#include "core/KillTracker.h"
#include "core/PassiveTree.h"

#include <bitset>
#include <fstream>
#include <nlohmann/json.hpp>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        using json = nlohmann::json;
        using Clock = std::chrono::steady_clock;

        constexpr auto kViewPath = "SkyrimPoE/index.html";
        constexpr auto kKoLangPath = "Data/SKSE/Plugins/SkyrimPoE/lang/ko-KR.json";
        constexpr auto kEnLangPath = "Data/SKSE/Plugins/SkyrimPoE/lang/en-US.json";
        constexpr auto kJsCommandListener = "poeCommand";
        constexpr auto kJsUpdateHud = "poeUpdateHud";
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

        std::string_view TabNameFromId(std::uint8_t tab) noexcept
        {
            switch (tab) {
                case kTabTree: return "tree";
                case kTabGear: return "gear";
                default:       return "skills";
            }
        }

        std::uint8_t TabIdFromName(std::string_view name) noexcept
        {
            if (name == "tree") return kTabTree;
            if (name == "gear") return kTabGear;
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
                case Core::EquipSlot::Ring1:    return "반지";
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
                default:                                          return "장비";
            }
        }

        const char* RarityStr(Core::ItemRarity r) noexcept
        {
            switch (r) {
                case Core::ItemRarity::Magic: return "magic";
                case Core::ItemRarity::Rare:  return "rare";
                default:                      return "normal";
            }
        }

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        Core::EnergyShieldPool* g_esPool = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        const Core::Config* g_config = nullptr;
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
        };
        CachedHud g_lastHud;

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
                if (const auto* def = g_dataPack->FindGem(gemId)) {
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
            std::string txt(utf8Text);
            g_queue->Enqueue([txt = std::move(txt)]() {
                if (!ViewUsable() || !g_domReady.load(std::memory_order_acquire) ||
                    !g_panelOpen.load(std::memory_order_acquire)) {
                    return;
                }
                const std::string payload = json{ { "text", txt } }.dump();
                g_api->InteropCall(g_view, kJsImeText, payload.c_str());
            });
        }

        void OnImeKey(const char* keyName)
        {
            if (!keyName || !g_queue || !g_panelOpen.load(std::memory_order_acquire)) {
                return;
            }
            std::string key(keyName);
            g_queue->Enqueue([key = std::move(key)]() {
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

        json SerializeComputedStats(const Core::ComputedSkillStats& cs)
        {
            return json{
                { "avgDamage", std::round(cs.baseHitDamage.TotalAverage() * 10.0) / 10.0 },
                { "critPct", std::round(cs.EffectiveCritChancePct() * 10.0) / 10.0 },
                { "manaCost", std::round(cs.poeManaCost * 10.0) / 10.0 },
                { "areaMult", std::round(cs.areaMult * 100.0) / 100.0 },
                { "cooldownSec", std::round(cs.cooldownSec * 100.0) / 100.0 }
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
            j["reqXp"] = Core::KillTracker::RequiredXpForNextGemLevel(gem.level);
            j["socketCount"] = gem.socketCount;
            Core::GemKind kind = Core::GemKind::Unknown;
            if (g_dataPack) {
                if (const auto* def = g_dataPack->FindGem(gem.gemId)) {
                    kind = def->kind;
                }
            }
            j["kind"] = GemKindStr(kind);

            // 발동 젬 안 주문(IceNova/ShockNova)의 사전 계산 스탯도 함께 직렬화해 UI에 표시한다.
            if (snap) {
                if (const auto it = snap->triggeredSpellStats.find(gem.instanceId); it != snap->triggeredSpellStats.end()) {
                    j["computed"] = SerializeComputedStats(it->second);
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
            json affArr = json::array();
            for (const auto& a : card.affixLines) {
                affArr.push_back(json{
                    { "type", a.type == Core::AffixType::Prefix ? "prefix" : "suffix" },
                    { "tier", a.tier },
                    { "tierBadge", a.tierBadge },
                    { "textKo", a.textKo }
                });
            }
            return json{
                { "title", card.title },
                { "rarity", RarityStr(card.rarity) },
                { "rarityBadge", card.rarityBadge },
                { "itemLevel", card.itemLevel },
                { "slotLabel", card.slotLabel },
                { "requirementMet", card.requirementMet },
                { "requirementText", card.requirementText },
                { "baseStatLines", card.baseStatLines },
                { "affixLines", std::move(affArr) }
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
            json& outReasons)
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
                tmp.itemLevel = std::clamp<std::uint32_t>(exileLevel, 1u, 100u);
                tmp.baseFormId = baseFormId;
                tmp.slotGroup = slotGroup;
                tmp.defenseSubtype = defenseSubtype;
                tmp.isAmulet = isAmulet;
                tmp.weaponTag = weaponTag;
                rec = &tmp;
            }
            const auto tags = Core::ItemData::BuildItemTags(
                rec->slotGroup, rec->defenseSubtype, rec->weaponTag, rec->isAmulet);
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

            // 장비 탭·능력치에 최신 착용 장비 및 제작 변경이 즉시 반영되도록 더티 상태를 플러시하고 스탯을 재계산한다(검수 C3).
            EquipmentSync::FlushIfDirty();
            if (g_dataPack && g_dataPack->IsLoaded()) {
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            }

            const auto state = SaveLoad::GetStateCopy();
            const auto snap = g_statEngine->GetSnapshot();
            const auto inv = GemInventory::ListPlayerGems(player);
            const auto es = g_esPool ? g_esPool->Snapshot() : Core::EnergyShieldState{};
            const auto currencyCounts = CraftingRuntime::CountAllCurrencies(player);

            const bool treeLoaded = g_treeData && g_treeData->IsLoaded();
            const std::int32_t availNormal = treeLoaded
                ? Core::PassiveTree::ComputeAvailableNormalPoints(*g_treeData, state.tree, state.exileLevel, *g_config)
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
            root["simpleImeAvailable"] = SimpleImeBridge::Available();
            root["freePassiveRefund"] = g_config->freePassiveRefund;
            root["exileLevel"] = state.exileLevel;
            root["exileXp"] = state.exileXp;
            root["exileReqXp"] = Core::KillTracker::RequiredXpForNextExileLevel(state.exileLevel, *g_config);
            root["unallocatedPoints"] = availNormal;
            root["className"] = state.tree.classSelected
                ? DisplayClassName(state.className)
                : std::string("미선택");

            json treeJ;
            treeJ["loaded"] = treeLoaded;
            treeJ["classSelected"] = state.tree.classSelected;
            treeJ["classId"] = state.tree.classId;
            treeJ["ascendancyId"] = state.tree.ascendancyId;
            treeJ["allocatedNodes"] = state.tree.allocatedNodes;
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
            treeJ["regretOrbCount"] = currencyCounts[static_cast<std::size_t>(Core::CurrencyKind::Regret)];
            root["tree"] = std::move(treeJ);

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
                st["fireResist"] = std::round(snap->fireResist);
                st["coldResist"] = std::round(snap->coldResist);
                st["lightningResist"] = std::round(snap->lightningResist);
                st["chaosResist"] = std::round(snap->chaosResist);
                st["maxFireResist"] = std::round(snap->maxFireResist);
                st["maxColdResist"] = std::round(snap->maxColdResist);
                st["maxLightningResist"] = std::round(snap->maxLightningResist);
                st["maxChaosResist"] = std::round(snap->maxChaosResist);
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
                st["bloodMagic"] = snap->bloodMagic;
                st["chaosInoculation"] = snap->chaosInoculation;
                st["resoluteTechnique"] = snap->resoluteTechnique;
                st["unwaveringStance"] = snap->unwaveringStance;
                root["stats"] = std::move(st);
            }

            json slotsArr = json::array();
            for (std::size_t i = 0; i < state.slots.size(); ++i) {
                const auto& s = state.slots[i];
                json sj;
                sj["slotIndex"] = i;
                sj["occupied"] = s.occupied;
                sj["linkedAttackSlot"] = s.linkedAttackSlot;
                if (s.occupied) {
                    sj["gem"] = SerializeGemInstance(s.gem, snap.get());
                    if (snap && snap->slotStats[i].gemId == s.gem.gemId) {
                        sj["computed"] = SerializeComputedStats(snap->slotStats[i]);
                    }
                }
                slotsArr.push_back(std::move(sj));
            }
            root["slots"] = std::move(slotsArr);

            json invArr = json::array();
            for (const auto& t : inv.trackedGems) {
                json tj;
                tj["tracked"] = true;
                tj["ownerFormId"] = t.key.ownerFormId;
                tj["uniqueId"] = t.key.uniqueId;
                tj["gemId"] = t.gem.gemId;
                tj["name"] = DisplayGemName(t.gem.gemId);
                tj["level"] = t.gem.level;
                tj["socketCount"] = t.gem.socketCount;
                tj["socketedCount"] = t.gem.socketedGems.size();
                tj["count"] = 1;
                Core::GemKind kind = Core::GemKind::Unknown;
                if (g_dataPack) {
                    if (const auto* def = g_dataPack->FindGem(t.gem.gemId)) {
                        kind = def->kind;
                    }
                }
                tj["kind"] = GemKindStr(kind);
                invArr.push_back(std::move(tj));
            }
            for (const auto& p : inv.plainStacks) {
                Core::GemKind kind = Core::GemKind::Unknown;
                if (g_dataPack) {
                    if (const auto* def = g_dataPack->FindGem(p.gemId)) {
                        kind = def->kind;
                    }
                }
                json pj;
                pj["tracked"] = false;
                pj["ownerFormId"] = 0;
                pj["uniqueId"] = 0;
                pj["gemId"] = p.gemId;
                pj["name"] = DisplayGemName(p.gemId);
                pj["level"] = 1;
                pj["socketCount"] = (kind == Core::GemKind::Support) ? 0 : g_config->starterSockets;
                pj["socketedCount"] = 0;
                pj["count"] = p.count;
                pj["kind"] = GemKindStr(kind);
                invArr.push_back(std::move(pj));
            }
            root["inventory"] = std::move(invArr);

            // 4단계 3번째 탭(장비·제작) 페이로드(결정 156, 157, 158, 160)
            const bool itemsLoaded = g_itemData && g_itemData->IsLoaded();
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
                        { "slotLabel", TokenSlotGroupLabelKo(tk.slotGroup) },
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
                    const auto slotInp = baseForm
                        ? EquipmentSync::BuildSlotInput(baseForm, recPtr, state.exileLevel, *g_itemData)
                        : Core::EquippedSlotInput{};
                    // 검수 Major #4: StatEngine과 동일하게 exileLevel >= reqLevel 포함 4조건 모두 판정
                    const bool reqMet = !snap ||
                        (snap->exileLevel >= slotInp.reqLevel &&
                         snap->str >= slotInp.reqStr &&
                         snap->dex >= slotInp.reqDex &&
                         snap->intel >= slotInp.reqInt);
                    const auto card = Core::ItemFormatter::FormatItem(
                        recPtr, b.displayName, b.equipSlot, slotInp, reqMet, *g_itemData);
                    json canFlags;
                    json canReasons;
                    BuildCanApplyOrbData(
                        recPtr, b.slotGroup, b.defenseSubtype, b.weaponTag, b.isAmulet,
                        b.baseFormId, state.exileLevel, canFlags, canReasons);
                    bagArr.push_back(json{
                        { "baseFormId", b.baseFormId },
                        { "uniqueId", b.uniqueId },
                        { "count", b.count },
                        { "equipped", b.equipped },
                        { "displayName", b.displayName },
                        { "slotIndex", static_cast<std::size_t>(b.equipSlot) },
                        { "slotLabel", EquipSlotLabelKo(b.equipSlot) },
                        { "hasPoERecord", b.hasPoERecord },
                        { "rarity", b.hasPoERecord ? RarityStr(b.record.rarity) : "normal" },
                        { "card", SerializeFormattedCard(card) },
                        { "canApplyOrb", std::move(canFlags) },
                        { "canApplyOrbReason", std::move(canReasons) }
                    });
                }
                gearJ["bagItems"] = std::move(bagArr);

                // 쥬얼러의 오브 대상 젬 목록(장착 스킬/발동 젬 + 발동 젬 내부 주문 젬)
                json jewArr = json::array();
                for (std::size_t i = 0; i < state.slots.size(); ++i) {
                    const auto& s = state.slots[i];
                    if (!s.occupied) {
                        continue;
                    }
                    Core::GemKind rootKind = Core::GemKind::Unknown;
                    if (g_dataPack) {
                        if (const auto* def = g_dataPack->FindGem(s.gem.gemId)) {
                            rootKind = def->kind;
                        }
                    }
                    if (rootKind != Core::GemKind::Support && rootKind != Core::GemKind::Unknown) {
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

            return root.dump();
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
            const std::string payload = j.dump();
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
            if (!g_config || !g_queue) {
                return;
            }
            const std::uint32_t panelKey = g_config->panelKey;
            const std::uint32_t treeKey = g_config->treePanelKey;
            const std::uint32_t gearKey = g_config->gearPanelKey;
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
                        g_queue->Enqueue([]() {
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
                    g_queue->Enqueue([]() {
                        if (!g_panelOpen.load(std::memory_order_acquire)) {
                            OpenPanelOnMainThread(kTabSkills);
                        } else if (g_activeTab.load(std::memory_order_acquire) == kTabSkills) {
                            UiBridge::ClosePanelOnMainThread();
                        } else {
                            SwitchTabOnMainThread(kTabSkills);
                        }
                    });
                } else if (code == treeKey) {
                    g_queue->Enqueue([]() {
                        if (!g_panelOpen.load(std::memory_order_acquire)) {
                            OpenPanelOnMainThread(kTabTree);
                        } else if (g_activeTab.load(std::memory_order_acquire) == kTabTree) {
                            UiBridge::ClosePanelOnMainThread();
                        } else {
                            SwitchTabOnMainThread(kTabTree);
                        }
                    });
                } else if (code == gearKey) {
                    g_queue->Enqueue([]() {
                        if (!g_panelOpen.load(std::memory_order_acquire)) {
                            OpenPanelOnMainThread(kTabGear);
                        } else if (g_activeTab.load(std::memory_order_acquire) == kTabGear) {
                            UiBridge::ClosePanelOnMainThread();
                        } else {
                            SwitchTabOnMainThread(kTabGear);
                        }
                    });
                } else if (open && code == kEscapeScanCode) {
                    g_queue->Enqueue([]() {
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

                if (!a_events || !*a_events) {
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
        void ExecuteCommandOnMainThread(const std::string& rawJson)
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
                    return;
                }
                if (op == "switchTab") {
                    const std::string tab = cmd.contains("tab") && cmd["tab"].is_string() ? cmd["tab"].get<std::string>() : "skills";
                    SwitchTabOnMainThread(TabIdFromName(tab));
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

                // 3단계 패시브 트리 변경 명령 분기
                if (op.starts_with("tree")) {
                    if (!g_treeData || !g_treeData->IsLoaded()) {
                        PushToastToJs(false, "패시브 트리 데이터가 로드되지 않았습니다.");
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
                            std::int64_t clsVal = -1;
                            if (!ReadBoundedInt(cmd, "classId", 0, Core::kClassCount - 1, clsVal)) {
                                treeRes = { false, "잘못된 클래스 번호입니다.", 0, 0 };
                                return;
                            }
                            treeRes = Core::PassiveTree::SelectClass(*g_treeData, working.tree, static_cast<std::uint8_t>(clsVal));
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
                            if (cmd.contains("nodes") && cmd["nodes"].is_array() && cmd["nodes"].size() <= 256) {
                                for (const auto& nv : cmd["nodes"]) {
                                    if (nv.is_number_unsigned()) {
                                        nodes.push_back(nv.get<std::uint32_t>());
                                    }
                                }
                            }
                            treeRes = Core::PassiveTree::AllocatePath(*g_treeData, working.tree, nodes, working.exileLevel, *g_config);
                        } else if (op == "treeRefundNode") {
                            std::int64_t nidVal = 0;
                            if (!ReadBoundedInt(cmd, "nodeId", 1, 0xFFFFFFFFLL, nidVal)) {
                                treeRes = { false, "잘못된 노드 번호입니다.", 0, 0 };
                                return;
                            }
                            treeRes = Core::PassiveTree::RefundNode(
                                *g_treeData, working.tree, static_cast<std::uint32_t>(nidVal), working.exileLevel, *g_config, availOrbs);
                        } else if (op == "treeRefundAll") {
                            const bool ascOnly = cmd.contains("ascendancyOnly") && cmd["ascendancyOnly"].is_boolean() &&
                                cmd["ascendancyOnly"].get<bool>();
                            treeRes = Core::PassiveTree::RefundAll(*g_treeData, working.tree, ascOnly, working.exileLevel, *g_config, availOrbs);
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
                                working.exileLevel, *g_config, availOrbs);
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
                            *g_treeData, working.tree, working.exileLevel, *g_config);
                        working.unallocatedPassivePoints = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
                        SaveLoad::SetState(working);
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
                            !ReadBoundedInt(cmd, "slotGroup", 0, static_cast<std::int64_t>(Core::kTokenSlotGroupCount - 1), slotGroupVal)) {
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
                    } else {
                        craftRes = { false, "지원하지 않는 장비·제작 명령입니다." };
                    }

                    PushToastToJs(craftRes.ok, craftRes.message);
                    PushPanelStateToJs();
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
                            st.slots, st.nextGemInstanceId, slot, readItemKey(), gemId, g_config->starterSockets, *g_dataPack, adapter);
                    } else if (op == "unequip") {
                        txRes = Core::GemTransaction::UnequipSkillGem(st.slots, slot, adapter);
                    } else if (op == "socket") {
                        std::int64_t spellIdx = -1;
                        (void)ReadBoundedInt(cmd, "spellIdx", -1, 16, spellIdx);
                        const std::string gemId = cmd.contains("gemId") && cmd["gemId"].is_string() ? cmd["gemId"].get<std::string>() : "";
                        txRes = Core::GemTransaction::SocketSubGem(
                            st.slots, st.nextGemInstanceId, slot, static_cast<std::int32_t>(spellIdx),
                            readItemKey(), gemId, g_config->starterSockets, *g_dataPack, adapter);
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
                            st.slots, slot, static_cast<std::int8_t>(atkSlot), *g_dataPack);
                    } else {
                        txRes = { false, "지원하지 않는 UI 명령입니다.", std::nullopt };
                    }

                    if (txRes.ok) {
                        SaveLoad::UpdateState([&](PersistentState& cur) {
                            cur.slots = st.slots;
                            cur.nextGemInstanceId = st.nextGemInstanceId;
                        });
                    }
                });

                if (txRes.ok) {
                    SaveLoad::SyncStateToStatEngine();
                    (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
                }
                PushToastToJs(txRes.ok, txRes.message);
                PushPanelStateToJs();
            } catch (const std::exception& e) {
                SKSE::log::warn("UiBridge: JS command handling failed: {}", e.what());
            }
        }

        void OnJsCommand(const char* argument)
        {
            if (!argument || !g_queue) {
                return;
            }
            std::string raw(argument);
            g_queue->Enqueue([raw = std::move(raw)]() {
                ExecuteCommandOnMainThread(raw);
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
        const Core::Config* config)
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
            if (g_hudShown.load(std::memory_order_acquire)) {
                g_api->Hide(g_view);
                g_hudShown.store(false, std::memory_order_release);
            }
            return;
        }
        if (!g_hudShown.load(std::memory_order_acquire)) {
            g_api->Show(g_view);
            g_hudShown.store(true, std::memory_order_release);
            g_lastHud.valid = false;
        }

        // HUD 값 변경 시에만 C++ -> JS 푸시(프레임 유휴 비용 0 유지)
        const auto es = g_esPool ? g_esPool->Snapshot() : Core::EnergyShieldState{};
        const auto state = SaveLoad::GetStateCopy();
        const int esCur = static_cast<int>(std::llround(es.current));
        const int esMax = static_cast<int>(std::llround(es.max));
        const auto reqXp = Core::KillTracker::RequiredXpForNextExileLevel(state.exileLevel, *g_config);
        const std::int32_t availPts = (g_treeData && g_treeData->IsLoaded())
            ? std::max<std::int32_t>(0, Core::PassiveTree::ComputeAvailableNormalPoints(*g_treeData, state.tree, state.exileLevel, *g_config))
            : static_cast<std::int32_t>(state.unallocatedPassivePoints);

        if (!g_lastHud.valid ||
            g_lastHud.esCurrent != esCur ||
            g_lastHud.esMax != esMax ||
            g_lastHud.exileLevel != state.exileLevel ||
            g_lastHud.exileXp != state.exileXp ||
            g_lastHud.exileReqXp != reqXp ||
            g_lastHud.unallocatedPts != availPts) {
            g_lastHud.valid = true;
            g_lastHud.esCurrent = esCur;
            g_lastHud.esMax = esMax;
            g_lastHud.exileLevel = state.exileLevel;
            g_lastHud.exileXp = state.exileXp;
            g_lastHud.exileReqXp = reqXp;
            g_lastHud.unallocatedPts = availPts;

            json hud{
                { "esCurrent", esCur },
                { "esMax", esMax },
                { "exileLevel", state.exileLevel },
                { "exileXp", state.exileXp },
                { "exileReqXp", reqXp },
                { "unallocatedPoints", availPts }
            };
            const std::string payload = hud.dump();
            g_api->InteropCall(g_view, kJsUpdateHud, payload.c_str());
        }
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

    bool UiBridge::TryOpenPanelOnMainThread(std::string_view initialTab)
    {
        OpenPanelOnMainThread(TabIdFromName(initialTab));
        return IsPanelOpen();
    }

    void UiBridge::ExecuteCommandForTestOnMainThread(std::string_view rawJson)
    {
        ExecuteCommandOnMainThread(std::string(rawJson));
    }

    std::string UiBridge::BuildPanelJsonForTestOnMainThread()
    {
        return BuildPanelPayloadJson();
    }
}
