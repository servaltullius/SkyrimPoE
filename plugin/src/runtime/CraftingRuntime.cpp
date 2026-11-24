#include "PCH.h"
#include "runtime/CraftingRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/ItemTracker.h"
#include "runtime/SaveLoad.h"
#include "runtime/UniqueIdUtil.h"

#include "core/Config.h"
#include "core/ItemCrafting.h"
#include "core/MapSystem.h"
#include "core/RulesPack.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <random>
#include <unordered_set>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        const Core::ItemData* g_itemData = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::Config* g_config = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;
        Core::StatEngine* g_statEngine = nullptr;

        std::mutex g_rngMutex;
        std::mt19937_64 g_rng{ std::random_device{}() };

        // houseCARL 대조 완료 Skyrim.esm 안전 플레이 가능 베이스 장비 풀 (5티어 × 8부위군)
        // 모든 FormID는 Skyrim.esm에서 직접 검증된 비퀘스트 일반 장비임.
        struct TierBasePool
        {
            std::array<std::vector<RE::FormID>, 5> byTier;
        };

        const TierBasePool& GetVanillaPool(Core::TokenSlotGroup group)
        {
            static const TierBasePool kOneHand{{{
                { 0x00012EB7, 0x00013790, 0x00013982, 0x0001397E },  // T1: Iron Sword/WarAxe/Mace/Dagger
                { 0x00013989, 0x00013983, 0x00013988, 0x00013986 },  // T2: Steel
                { 0x00013999, 0x00013993, 0x00013998, 0x00013996 },  // T3: Dwarven
                { 0x000139A9, 0x000139A3, 0x000139A8, 0x000139A6 },  // T4: Glass
                { 0x000139B9, 0x000139B3, 0x000139B8, 0x000139B6 },  // T5: Daedric
            }}};
            static const TierBasePool kTwoHand{{{
                { 0x0001359D, 0x00013985 },  // T1: Iron Greatsword, Hunting Bow
                { 0x00013987, 0x00013841 },  // T2: Steel Greatsword, Imperial Bow
                { 0x00013997, 0x00013995 },  // T3: Dwarven Greatsword, Dwarven Bow
                { 0x000139A7, 0x000139A5 },  // T4: Glass Greatsword, Glass Bow
                { 0x000139B7, 0x000139B5 },  // T5: Daedric Greatsword, Daedric Bow
            }}};
            static const TierBasePool kShield{{{
                { 0x00012EB6, 0x00013914 },  // T1: Iron / Hide Shield
                { 0x00013955, 0x0001391E },  // T2: Steel / Elven Shield
                { 0x00013950, 0x0001391E },  // T3: Dwarven / Elven Shield
                { 0x00013946, 0x0001393C },  // T4: Orcish / Glass Shield
                { 0x00013964, 0x0001393C },  // T5: Ebony / Glass Shield
            }}};
            static const TierBasePool kHelmet{{{
                { 0x00012E4D, 0x00013913, 0x000D3DE8 },  // T1: Iron / Hide / College Hood
                { 0x00013922, 0x0010D6A6, 0x00012E4D },  // T2: Leather / College Hood V1 / Iron
                { 0x0001394F, 0x0001391D, 0x0010D6A7 },  // T3: Dwarven / Elven / College Hood V2
                { 0x00013959, 0x0001B3A1, 0x0010D6A7 },  // T4: Orcish / Scaled / Hood
                { 0x00013963, 0x0001393B, 0x0010D6A7 },  // T5: Ebony / Glass / Hood
            }}};
            static const TierBasePool kBody{{{
                { 0x00012E49, 0x00013911, 0x000D3DEA },  // T1: Iron / Hide / College Robes Common
                { 0x00013952, 0x0003619E, 0x000D3DE9 },  // T2: Steel / Leather / Apprentice Robes
                { 0x0001394D, 0x000896A3, 0x0010D691 },  // T3: Dwarven / Elven / Apprentice V1
                { 0x00013957, 0x0001B3A3, 0x0010D6A4 },  // T4: Orcish / Scaled / Apprentice V2
                { 0x00013961, 0x00013939, 0x0010D6A5 },  // T5: Ebony / Glass / Common V1
            }}};
            static const TierBasePool kGloves{{{
                { 0x00012E46, 0x00013912, 0x000261C1 },  // T1: Iron / Hide / Farm Gloves
                { 0x00013921, 0x00065BB3, 0x00012E46 },  // T2: Leather / Thalmor Gloves / Iron
                { 0x0001394E, 0x0001391C, 0x00065BB3 },  // T3: Dwarven / Elven / Thalmor
                { 0x00013958, 0x0001B3A0, 0x00065B9D },  // T4: Orcish / Scaled / Psijic Gloves
                { 0x00013962, 0x0001393A, 0x00065B9D },  // T5: Ebony / Glass / Psijic Gloves
            }}};
            static const TierBasePool kBoots{{{
                { 0x00012E4B, 0x00013910, 0x0010E2CE },  // T1: Iron / Hide / College Boots Common
                { 0x00013920, 0x0010E2DC, 0x00012E4B },  // T2: Leather / College Boots Apprentice
                { 0x0001394C, 0x0001391A, 0x0010E2DC },  // T3: Dwarven / Elven / College Boots
                { 0x00013956, 0x0001B39F, 0x0010E2DC },  // T4: Orcish / Scaled / College Boots
                { 0x00013960, 0x00013938, 0x0010E2DC },  // T5: Ebony / Glass / College Boots
            }}};
            static const TierBasePool kJewelry{{{
                { 0x0003B97C, 0x0009171B },              // T1: Silver Ring / Silver Necklace
                { 0x0001CF2B, 0x000877D5 },              // T2: Gold Ring / Gold Necklace
                { 0x000877CA, 0x000877DC },              // T3: Gold Emerald Ring / Gold Ruby Necklace
                { 0x000877C9, 0x000877F1 },              // T4: Gold Diamond Ring / Gold Diamond Necklace
                { 0x000877C9, 0x000877F1 },              // T5: Gold Diamond Ring / Gold Diamond Necklace
            }}};

            switch (group) {
                case Core::TokenSlotGroup::OneHandWeapon:         return kOneHand;
                case Core::TokenSlotGroup::TwoHandOrRangedWeapon: return kTwoHand;
                case Core::TokenSlotGroup::Shield:                return kShield;
                case Core::TokenSlotGroup::Helmet:                return kHelmet;
                case Core::TokenSlotGroup::Body:                  return kBody;
                case Core::TokenSlotGroup::Gloves:                return kGloves;
                case Core::TokenSlotGroup::Boots:                 return kBoots;
                case Core::TokenSlotGroup::Jewelry:               return kJewelry;
                default:                                          return kOneHand;
            }
        }

        // 주의: TESObjectARMO는 TESForm::GetPlayable()을 오버라이드하지 않으며,
        // TESForm::GetPlayable()은 ((formFlags >> 2) & 1) == kNonPlayable을 그대로 반환한다(반전됨).
        // 따라서 방어구/장신구 플레이 가능 여부는 반드시 formFlags & kNonPlayable == 0 으로 직접 판정한다.
        [[nodiscard]] bool IsArmorPlayable(const RE::TESObjectARMO* armor) noexcept
        {
            return armor && ((armor->formFlags & RE::TESObjectARMO::RecordFlags::kNonPlayable) == 0);
        }

        [[nodiscard]] std::int32_t CountBoundObjectInInventory(RE::TESObjectREFR* owner, RE::TESBoundObject* obj)
        {
            if (!owner || !obj) {
                return 0;
            }
            const auto counts = owner->GetInventoryCounts([obj](RE::TESBoundObject& candidate) {
                return &candidate == obj;
            });
            const auto it = counts.find(obj);
            return (it != counts.end()) ? it->second : 0;
        }

        // 검수 C1: ExtraUniqueID가 없는 미추적(일반) 수량만 정확히 집계해 추적 희귀 장비 오삭제를 차단한다.
        struct UntrackedSearchResult
        {
            std::int32_t untrackedTotal = 0;
            RE::ExtraDataList* wornUntracked = nullptr;
            RE::ExtraDataList* bagUntracked = nullptr;
            bool hasPlainStack = false;
        };

        [[nodiscard]] UntrackedSearchResult InspectUntrackedItems(RE::TESObjectREFR* owner, RE::TESBoundObject* obj)
        {
            UntrackedSearchResult out;
            if (!owner || !obj) {
                return out;
            }
            const std::int32_t totalOwned = CountBoundObjectInInventory(owner, obj);
            if (totalOwned <= 0) {
                return out;
            }
            std::int32_t trackedCount = 0;
            std::int32_t extraListTotal = 0;
            if (auto* changes = owner->GetInventoryChanges(true); changes && changes->entryList) {
                for (auto* entry : *changes->entryList) {
                    if (!entry || entry->object != obj || !entry->extraLists) continue;
                    for (auto* extra : *entry->extraLists) {
                        if (!extra) continue;
                        const std::int32_t cnt = std::max<std::int32_t>(1, extra->GetCount());
                        extraListTotal += cnt;
                        if (extra->HasType<RE::ExtraUniqueID>()) {
                            trackedCount += cnt;
                        } else {
                            const bool worn = extra->HasType<RE::ExtraWorn>() || extra->HasType<RE::ExtraWornLeft>();
                            if (worn && !out.wornUntracked) {
                                out.wornUntracked = extra;
                            } else if (!worn && !out.bagUntracked) {
                                out.bagUntracked = extra;
                            }
                        }
                    }
                }
            }
            out.untrackedTotal = std::max<std::int32_t>(0, totalOwned - trackedCount);
            out.hasPlainStack = (totalOwned > extraListTotal);
            return out;
        }

        // 검수 M1, m3: 엔진 폼 조회는 g_rngMutex 밖에서 수행하고, IncludeModdedBasesInTokenPool=true 시
        // TESDataHandler의 플레이 가능·비퀘스트 모드 장비도 티어별 풀에 1회 캐싱해 포함한다.
        [[nodiscard]] std::vector<RE::TESBoundObject*> CollectBaseCandidatesForToken(
            std::uint8_t tier,
            Core::TokenSlotGroup slotGroup)
        {
            std::vector<RE::TESBoundObject*> valid;
            if (slotGroup == Core::TokenSlotGroup::Belt) {
                // T1: Leather/Heavy, T2: +Chain, T3: +Rustic, T4..T5: +Stygian Vise
                const std::size_t maxBelts = (tier >= 4) ? 5 : (tier == 3 ? 4 : (tier == 2 ? 3 : 2));
                for (std::size_t i = 0; i < std::min<std::size_t>(maxBelts, FormIds::kBeltBaseForms.size()); ++i) {
                    if (auto* misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kBeltBaseForms[i])) {
                        valid.push_back(misc);
                    }
                }
                return valid;
            }

            const std::size_t tIdx = std::clamp<std::size_t>(tier >= 1 ? tier - 1 : 0, 0, 4);
            const auto& vanillaIds = GetVanillaPool(slotGroup).byTier[tIdx];
            for (const auto fid : vanillaIds) {
                if (auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(fid)) {
                    valid.push_back(obj);
                }
            }

            if (g_config && g_config->includeModdedBasesInTokenPool) {
                static std::array<std::vector<RE::TESBoundObject*>, Core::kTokenSlotGroupCount> s_moddedBySlot;
                static bool s_moddedBuilt = false;
                if (!s_moddedBuilt) {
                    s_moddedBuilt = true;
                    if (auto* handler = RE::TESDataHandler::GetSingleton()) {
                        for (auto* weap : handler->GetFormArray<RE::TESObjectWEAP>()) {
                            if (!weap || !weap->GetPlayable() || weap->GetGoldValue() <= 0) continue;
                            const char* nm = weap->GetName();
                            if (!nm || nm[0] == '\0') continue;
                            const auto sg = EquipmentSync::IsTwoHandedWeapon(weap)
                                ? Core::TokenSlotGroup::TwoHandOrRangedWeapon
                                : Core::TokenSlotGroup::OneHandWeapon;
                            auto& vec = s_moddedBySlot[static_cast<std::size_t>(sg)];
                            if (vec.size() < 256) vec.push_back(weap);
                        }
                        for (auto* armor : handler->GetFormArray<RE::TESObjectARMO>()) {
                            if (!IsArmorPlayable(armor) || armor->GetGoldValue() <= 0) continue;
                            const char* nm = armor->GetName();
                            if (!nm || nm[0] == '\0') continue;
                            const auto eqOpt = EquipmentSync::ClassifyArmorEquipSlot(armor);
                            if (!eqOpt.has_value()) continue;
                            const auto sg = EquipmentSync::EquipSlotToTokenGroup(*eqOpt, false);
                            auto& vec = s_moddedBySlot[static_cast<std::size_t>(sg)];
                            if (vec.size() < 256) vec.push_back(armor);
                        }
                    }
                }
                const auto& extra = s_moddedBySlot[static_cast<std::size_t>(slotGroup)];
                valid.insert(valid.end(), extra.begin(), extra.end());
            }
            return valid;
        }
        // 검수 C4: 패시브 소켓 주얼을 인벤토리로 원자 반환하는 공용 헬퍼(실패 시 false 반환해 영구 소실 차단)
        [[nodiscard]] bool TryReturnJewelToInventory(
            RE::PlayerCharacter* player,
            const Core::PoEItemRecord& rec,
            Core::InventoryItemKey* outKey = nullptr,
            RE::FormID* outBaseFormId = nullptr)
        {
            if (!player) return false;
            auto* baseObj = RE::TESForm::LookupByID<RE::TESBoundObject>(rec.baseFormId);
            if (!baseObj) {
                baseObj = FormIds::Lookup<RE::TESObjectMISC>(FormIds::JewelLocalFormId(rec.jewelColor));
            }
            if (!baseObj) return false;
            auto* freshExtra = UniqueIdUtil::CreateGameExtraDataList();
            if (!freshExtra) return false;
            bool untrackedLeft = false;
            const auto newKey = ItemTracker::AddObjectWithTrackedUniqueId(
                player, baseObj, freshExtra, nullptr, ItemTracker::UntrackedAddPolicy::RemoveAgain, &untrackedLeft);
            if (!newKey.IsValid()) {
                if (untrackedLeft) {
                    // 재검수 R-15: 주얼 실물은 가방에 들어갔지만 어픽스를 묶지 못했다. 실패로 돌리면 재시도마다 복제되므로
                    // 반환된 것으로 처리하고(소켓에서 뺌) 어픽스 유실을 기록한다.
                    SKSE::log::error("CraftingRuntime: jewel returned without tracking; its affixes could not be attached.");
                    // 노드 회수·전체 회수·교체·직접 회수 어느 경로든 플레이어가 알 수 있게 HUD로 알린다(Codex 지적 7).
                    RE::SendHUDMessage::ShowHUDMessage("주얼을 가방으로 돌려받았지만 추적에 실패해 어픽스가 사라졌습니다.");
                    if (outBaseFormId) *outBaseFormId = baseObj->GetFormID();
                    return true;
                }
                return false;
            }
            ItemTracker::Upsert(newKey, rec, false);
            if (outKey) *outKey = newKey;
            if (outBaseFormId) *outBaseFormId = baseObj->GetFormID();
            return true;
        }
    }

    void CraftingRuntime::Initialize(
        const Core::ItemData* itemData,
        const Core::DataPack* dataPack,
        const Core::PassiveTreeData* treeData,
        const Core::Config* config,
        Core::CharmSystem* charmSystem,
        Core::StatEngine* statEngine)
    {
        g_itemData = itemData;
        g_dataPack = dataPack;
        g_treeData = treeData;
        g_config = config;
        g_charmSystem = charmSystem;
        g_statEngine = statEngine;
    }

    std::int32_t CraftingRuntime::CountCurrency(RE::TESObjectREFR* owner, Core::CurrencyKind kind)
    {
        auto* misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CurrencyLocalFormId(kind));
        return CountBoundObjectInInventory(owner, misc);
    }

    std::array<std::int32_t, Core::kCurrencyKindCount> CraftingRuntime::CountAllCurrencies(RE::TESObjectREFR* owner)
    {
        std::array<std::int32_t, Core::kCurrencyKindCount> out{};
        if (!owner) {
            return out;
        }
        for (std::size_t i = 0; i < Core::kCurrencyKindCount; ++i) {
            out[i] = CountCurrency(owner, static_cast<Core::CurrencyKind>(i));
        }
        return out;
    }

    std::int32_t CraftingRuntime::CountUniqueTokens(RE::TESObjectREFR* owner)
    {
        auto* misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kUniqueSealedToken);
        return CountBoundObjectInInventory(owner, misc);
    }

    std::array<std::int32_t, Core::kCharmKindCount> CraftingRuntime::CountAllCharms(RE::TESObjectREFR* owner)
    {
        std::array<std::int32_t, Core::kCharmKindCount> out{};
        if (!owner) {
            return out;
        }
        for (std::size_t i = 0; i < Core::kCharmKindCount; ++i) {
            auto* misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CharmLocalFormId(static_cast<Core::CharmKind>(i)));
            out[i] = CountBoundObjectInInventory(owner, misc);
        }
        return out;
    }

    bool CraftingRuntime::GrantCurrency(RE::TESObjectREFR* owner, Core::CurrencyKind kind, std::int32_t count)
    {
        if (!owner || count <= 0) {
            return false;
        }
        auto* misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CurrencyLocalFormId(kind));
        if (!misc) {
            return false;
        }
        owner->AddObjectToContainer(misc, nullptr, count, nullptr);
        return true;
    }

    bool CraftingRuntime::ConsumeCurrency(RE::TESObjectREFR* owner, Core::CurrencyKind kind, std::int32_t count)
    {
        if (!owner || count <= 0) {
            return false;
        }
        auto* misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CurrencyLocalFormId(kind));
        if (!misc || CountBoundObjectInInventory(owner, misc) < count) {
            return false;
        }
        owner->RemoveItem(misc, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        return true;
    }

    bool CraftingRuntime::GrantToken(
        RE::TESObjectREFR* owner,
        std::uint8_t tier,
        Core::ItemRarity rarity,
        Core::TokenSlotGroup slotGroup,
        std::int32_t count)
    {
        if (!owner || count <= 0) {
            return false;
        }
        auto* misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::SealedTokenLocalFormId(tier, rarity, slotGroup));
        if (!misc) {
            return false;
        }
        owner->AddObjectToContainer(misc, nullptr, count, nullptr);
        return true;
    }

    bool CraftingRuntime::GrantUniqueToken(RE::TESObjectREFR* owner, std::int32_t count)
    {
        if (!owner || count <= 0) return false;
        auto* misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kUniqueSealedToken);
        if (!misc) return false;
        owner->AddObjectToContainer(misc, nullptr, count, nullptr);
        return true;
    }

    bool CraftingRuntime::GrantJewel(RE::TESObjectREFR* owner, std::uint8_t jewelColor, std::int32_t count)
    {
        if (!owner || count <= 0) return false;
        auto* misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::JewelLocalFormId(jewelColor));
        if (!misc) return false;
        owner->AddObjectToContainer(misc, nullptr, count, nullptr);
        return true;
    }

    bool CraftingRuntime::GrantCharm(RE::TESObjectREFR* owner, Core::CharmKind kind, std::int32_t count)
    {
        if (!owner || count <= 0) return false;
        auto* misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CharmLocalFormId(kind));
        if (!misc) return false;
        owner->AddObjectToContainer(misc, nullptr, count, nullptr);
        return true;
    }

    bool CraftingRuntime::GrantBeltBase(RE::TESObjectREFR* owner, RE::FormID localBeltFormId, std::int32_t count)
    {
        if (!owner || count <= 0 || !FormIds::IsBeltBaseLocalFormId(localBeltFormId)) return false;
        auto* misc = FormIds::Lookup<RE::TESObjectMISC>(localBeltFormId);
        if (!misc) return false;
        owner->AddObjectToContainer(misc, nullptr, count, nullptr);
        return true;
    }

    std::size_t CraftingRuntime::GiveStarterCurrency(RE::PlayerCharacter* player)
    {
        if (!player) {
            return 0;
        }
        struct Grant { Core::CurrencyKind kind; std::int32_t count; };
        constexpr std::array<Grant, 10> kStarterGrants = {{
            { Core::CurrencyKind::ScrollOfWisdom, 10 },
            { Core::CurrencyKind::Transmutation,   4 },
            { Core::CurrencyKind::Augmentation,    2 },
            { Core::CurrencyKind::Alteration,      4 },
            { Core::CurrencyKind::Regal,           1 },
            { Core::CurrencyKind::Alchemy,         2 },
            { Core::CurrencyKind::Chaos,           2 },
            { Core::CurrencyKind::Scouring,        1 },
            { Core::CurrencyKind::Regret,          2 },
            { Core::CurrencyKind::Jeweller,        2 },
        }};
        std::size_t grantedKinds = 0;
        for (const auto& g : kStarterGrants) {
            if (GrantCurrency(player, g.kind, g.count)) {
                ++grantedKinds;
            }
        }
        SKSE::log::info("CraftingRuntime: granted starter currency ({}/{} kinds).", grantedKinds, kStarterGrants.size());
        return grantedKinds;
    }

    std::vector<PlayerBagItemEntry> CraftingRuntime::ListPlayerCraftableItems(RE::PlayerCharacter* player)
    {
        std::vector<PlayerBagItemEntry> out;
        if (!player) {
            return out;
        }
        const RE::FormID playerId = player->GetFormID();
        const auto savedState = SaveLoad::GetStateCopy();

        // 검수 m4 + 5~6단계: WEAP/ARMO + SkyrimPoE.esp 벨트/주얼/지도 MISC만 제작 대상으로 열거
        const auto inv = player->GetInventory([](RE::TESBoundObject& obj) {
            if (obj.Is(RE::FormType::Weapon) || obj.Is(RE::FormType::Armor)) {
                return true;
            }
            if (obj.Is(RE::FormType::Misc)) {
                const auto localId = FormIds::ResolveLocalFormIdIfOurs(obj.GetFormID());
                return localId != 0 && (FormIds::IsBeltBaseLocalFormId(localId) ||
                    FormIds::JewelColorFromLocalFormId(localId).has_value() ||
                    FormIds::IsMapItemLocalFormId(localId));
            }
            return false;
        });

        for (const auto& [obj, pair] : inv) {
            const std::int32_t totalCount = pair.first;
            const auto& entryData = pair.second;
            if (!obj || totalCount <= 0) {
                continue;
            }
            auto* weap = obj->As<RE::TESObjectWEAP>();
            auto* armor = obj->As<RE::TESObjectARMO>();
            auto* misc = obj->As<RE::TESObjectMISC>();
            if (!weap && !armor && !misc) {
                continue;
            }

            Core::EquipSlot eqSlot = Core::EquipSlot::MainHand;
            Core::TokenSlotGroup slotGroup = Core::TokenSlotGroup::OneHandWeapon;
            Core::DefenseSubtype defSub = Core::DefenseSubtype::None;
            bool isAmulet = false;
            std::uint8_t jewelColor = 0;
            std::string wTag;

            if (weap) {
                if (!weap->GetPlayable()) continue;
                const bool twoH = EquipmentSync::IsTwoHandedWeapon(weap);
                eqSlot = Core::EquipSlot::MainHand;
                slotGroup = twoH ? Core::TokenSlotGroup::TwoHandOrRangedWeapon : Core::TokenSlotGroup::OneHandWeapon;
                wTag = EquipmentSync::ClassifyWeaponTag(weap);
            } else if (armor) {
                if (!IsArmorPlayable(armor)) continue;
                const auto slotOpt = EquipmentSync::ClassifyArmorEquipSlot(armor);
                if (!slotOpt.has_value()) {
                    continue;  // 결정 156: 외형 전용 바디슬롯은 제작 대상 제외
                }
                eqSlot = *slotOpt;
                slotGroup = EquipmentSync::EquipSlotToTokenGroup(eqSlot, false);
                defSub = EquipmentSync::ClassifyDefenseSubtype(armor);
                isAmulet = (eqSlot == Core::EquipSlot::Amulet);
            } else {
                const auto localId = FormIds::ResolveLocalFormIdIfOurs(misc->GetFormID());
                if (FormIds::IsBeltBaseLocalFormId(localId)) {
                    eqSlot = Core::EquipSlot::Belt;
                    slotGroup = Core::TokenSlotGroup::Belt;
                } else if (const auto jColor = FormIds::JewelColorFromLocalFormId(localId); jColor.has_value()) {
                    eqSlot = Core::EquipSlot::Belt;  // 주얼은 장비 슬롯 비귀속(slotGroup=Jewel로 판별)
                    slotGroup = Core::TokenSlotGroup::Jewel;
                    jewelColor = *jColor;
                } else if (FormIds::IsMapItemLocalFormId(localId)) {
                    eqSlot = Core::EquipSlot::Belt;  // 지도는 장비 슬롯 비귀속(slotGroup=Map으로 판별)
                    slotGroup = Core::TokenSlotGroup::Map;
                } else {
                    continue;
                }
            }

            const std::string name = obj->GetName() ? obj->GetName() : "";
            std::int32_t accountedCount = 0;

            if (entryData && entryData->extraLists) {
                for (auto* extra : *entryData->extraLists) {
                    if (!extra) continue;
                    const std::int32_t cnt = std::max<std::int32_t>(1, extra->GetCount());
                    accountedCount += cnt;
                    const bool worn = extra->HasType<RE::ExtraWorn>() || extra->HasType<RE::ExtraWornLeft>();
                    std::uint16_t uid = 0;
                    std::optional<Core::PoEItemRecord> poeRec;
                    if (const auto* unique = extra->GetByType<RE::ExtraUniqueID>()) {
                        if (unique->baseID == playerId && unique->uniqueID != 0) {
                            uid = unique->uniqueID;
                            poeRec = ItemTracker::FindByKey(Core::InventoryItemKey{ playerId, uid });
                        }
                    }

                    const Core::InventoryItemKey thisKey{ playerId, uid };
                    const bool isVirtualRing2 = (uid != 0) &&
                        savedState.virtualRing2BaseFormId == obj->GetFormID() &&
                        savedState.virtualRing2Key == thisKey;
                    const bool isVirtualBelt = (uid != 0) &&
                        savedState.virtualBeltBaseFormId == obj->GetFormID() &&
                        savedState.virtualBeltKey == thisKey;

                    PlayerBagItemEntry item;
                    item.baseFormId = obj->GetFormID();
                    item.uniqueId = uid;
                    item.count = cnt;
                    item.isVirtualEquipped = (isVirtualRing2 || isVirtualBelt);
                    item.equipped = worn || item.isVirtualEquipped;
                    item.displayName = name;
                    item.equipSlot = isVirtualRing2 ? Core::EquipSlot::Ring2 : eqSlot;
                    item.slotGroup = slotGroup;
                    item.defenseSubtype = defSub;
                    item.isAmulet = isAmulet;
                    item.jewelColor = jewelColor;
                    item.weaponTag = wTag;
                    if (poeRec) {
                        item.hasPoERecord = true;
                        item.record = *poeRec;
                    }
                    out.push_back(std::move(item));
                }
            }

            const std::int32_t plainRem = totalCount - accountedCount;
            if (plainRem > 0) {
                const bool plainBeltEquipped = (slotGroup == Core::TokenSlotGroup::Belt) &&
                    savedState.virtualBeltBaseFormId == obj->GetFormID() &&
                    !savedState.virtualBeltKey.IsValid();

                PlayerBagItemEntry plain;
                plain.baseFormId = obj->GetFormID();
                plain.uniqueId = 0;
                plain.count = plainRem;
                plain.isVirtualEquipped = plainBeltEquipped;
                plain.equipped = plainBeltEquipped;
                plain.displayName = name;
                plain.equipSlot = eqSlot;
                plain.slotGroup = slotGroup;
                plain.defenseSubtype = defSub;
                plain.isAmulet = isAmulet;
                plain.jewelColor = jewelColor;
                plain.weaponTag = wTag;
                out.push_back(std::move(plain));
            }
        }

        // 정렬: 장착 중 먼저 → 희귀도 높은 순 → 이름 순
        std::sort(out.begin(), out.end(), [](const PlayerBagItemEntry& a, const PlayerBagItemEntry& b) {
            if (a.equipped != b.equipped) return a.equipped > b.equipped;
            const auto ra = a.hasPoERecord ? static_cast<int>(a.record.rarity) : -1;
            const auto rb = b.hasPoERecord ? static_cast<int>(b.record.rarity) : -1;
            if (ra != rb) return ra > rb;
            return a.displayName < b.displayName;
        });
        return out;
    }

    std::vector<PlayerTokenStackEntry> CraftingRuntime::ListPlayerTokens(RE::PlayerCharacter* player)
    {
        std::vector<PlayerTokenStackEntry> out;
        if (!player) {
            return out;
        }
        auto scanRange = [&](RE::FormID startId, std::size_t count) {
            for (std::size_t offset = 0; offset < count; ++offset) {
                const RE::FormID localId = startId + static_cast<RE::FormID>(offset);
                auto* misc = FormIds::Lookup<RE::TESObjectMISC>(localId);
                if (!misc) continue;
                const std::int32_t cnt = CountBoundObjectInInventory(player, misc);
                if (cnt <= 0) continue;
                const auto decoded = FormIds::DecodeSealedTokenLocalFormId(localId);
                if (!decoded) continue;

                PlayerTokenStackEntry st;
                st.localFormId = localId;
                st.tier = decoded->tier;
                st.rarity = decoded->rarity;
                st.slotGroup = decoded->slotGroup;
                st.count = cnt;
                st.displayName = misc->GetName() ? misc->GetName() : "";
                out.push_back(std::move(st));
            }
        };
        scanRange(FormIds::kSealedTokenBase, FormIds::kSealedTokenCount);
        scanRange(FormIds::kBeltTokenBase, FormIds::kBeltTokenCount);

        if (auto* uniqMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kUniqueSealedToken)) {
            const std::int32_t uCnt = CountBoundObjectInInventory(player, uniqMisc);
            if (uCnt > 0) {
                PlayerTokenStackEntry u;
                u.localFormId = FormIds::kUniqueSealedToken;
                u.tier = 5;
                u.rarity = Core::ItemRarity::Unique;
                u.slotGroup = Core::TokenSlotGroup::Body;
                u.isUniqueToken = true;
                u.count = uCnt;
                u.displayName = uniqMisc->GetName() ? uniqMisc->GetName() : "고유 봉인 토큰";
                out.push_back(std::move(u));
            }
        }

        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.isUniqueToken != b.isUniqueToken) return a.isUniqueToken > b.isUniqueToken;
            if (a.tier != b.tier) return a.tier > b.tier;
            if (a.rarity != b.rarity) return a.rarity > b.rarity;
            return a.slotGroup < b.slotGroup;
        });
        return out;
    }

    CraftActionResult CraftingRuntime::IdentifyToken(
        std::uint8_t tier,
        Core::ItemRarity rarity,
        Core::TokenSlotGroup slotGroup)
    {
        CraftActionResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_itemData || !g_itemData->IsLoaded()) {
            res.message = "아이템 데이터가 아직 준비되지 않았습니다.";
            return res;
        }

        const RE::FormID tokenLocalId = FormIds::SealedTokenLocalFormId(tier, rarity, slotGroup);
        auto* tokenMisc = FormIds::Lookup<RE::TESObjectMISC>(tokenLocalId);
        auto* wisdomMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CurrencyLocalFormId(Core::CurrencyKind::ScrollOfWisdom));
        if (!tokenMisc || !wisdomMisc) {
            res.message = "토큰 또는 감정 주문서 폼을 찾을 수 없습니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            if (CountBoundObjectInInventory(player, tokenMisc) < 1) {
                res.message = "해당 봉인 토큰을 보유하고 있지 않습니다.";
                return;
            }
            if (CountBoundObjectInInventory(player, wisdomMisc) < 1) {
                res.message = "감정 주문서가 부족합니다.";
                return;
            }

            // 검수 M1, m3: 엔진 폼 조회는 g_rngMutex 밖에서 수행
            const auto candidates = CollectBaseCandidatesForToken(tier, slotGroup);
            if (candidates.empty()) {
                res.message = "감정에 사용할 베이스 장비를 찾지 못했습니다.";
                return;
            }
            RE::TESBoundObject* baseObj = nullptr;
            {
                std::scoped_lock lock(g_rngMutex);
                std::uniform_int_distribution<std::size_t> dist(0, candidates.size() - 1);
                baseObj = candidates[dist(g_rng)];
            }

            Core::DefenseSubtype defSub = Core::DefenseSubtype::None;
            std::string wTag;
            bool isAmulet = false;
            std::string implicitId;
            if (auto* weap = baseObj->As<RE::TESObjectWEAP>()) {
                wTag = EquipmentSync::ClassifyWeaponTag(weap);
            } else if (auto* armor = baseObj->As<RE::TESObjectARMO>()) {
                defSub = EquipmentSync::ClassifyDefenseSubtype(armor);
                const auto slotOpt = EquipmentSync::ClassifyArmorEquipSlot(armor);
                isAmulet = (slotOpt == Core::EquipSlot::Amulet);
            } else if (auto* misc = baseObj->As<RE::TESObjectMISC>(); misc && slotGroup == Core::TokenSlotGroup::Belt) {
                const auto localId = FormIds::ResolveLocalFormIdIfOurs(misc->GetFormID());
                if (const auto* beltDef = g_itemData->FindBeltByLocalFormId(localId)) {
                    implicitId = beltDef->implicitAffixId;
                }
            }

            const auto st = SaveLoad::GetStateCopy();
            Core::PoEItemRecord rolled;
            {
                std::scoped_lock lock(g_rngMutex);
                rolled = Core::ItemCrafting::IdentifyToken(
                    tier, rarity, slotGroup, defSub, wTag, isAmulet,
                    baseObj->GetFormID(), st.exileLevel, *g_itemData, g_rng, implicitId);
            }
            if (rolled.affixes.empty()) {
                res.message = "감정 중 유효한 어픽스를 생성하지 못했습니다.";
                return;
            }

            // 검수 M5: 롤링 성공 확정 후에만 게임 ExtraDataList 할당
            auto* freshExtra = UniqueIdUtil::CreateGameExtraDataList();
            if (!freshExtra) {
                res.message = "아이템 고유 데이터 할당에 실패했습니다.";
                return;
            }

            // 원자 반영: 토큰 1 + 감정 주문서 1 차감 → AddObjectWithTrackedUniqueId로 엔진 최종 UID 확정 → ItemTracker 등록
            player->RemoveItem(tokenMisc, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            player->RemoveItem(wisdomMisc, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            bool untrackedLeft = false;
            const Core::InventoryItemKey key = ItemTracker::AddObjectWithTrackedUniqueId(
                player, baseObj, freshExtra, nullptr, ItemTracker::UntrackedAddPolicy::RemoveAgain, &untrackedLeft);
            if (!key.IsValid()) {
                if (untrackedLeft) {
                    // 재검수 R-15: 장비가 추적 없이 이미 들어갔고 되돌리지도 못했다. 환불하면 장비가 복제되므로 소모로 처리한다.
                    res.message = "감정 장비의 고유 ID 추적에 실패했습니다. 장비는 어픽스 없이 가방에 들어갔고 토큰은 소모되었습니다.";
                    return;
                }
                // C-5: 추적 ID 확정 실패 시 차감한 토큰·감정 주문서를 환불한다(장비는 들어가지 않았거나 되돌려짐).
                player->AddObjectToContainer(tokenMisc, nullptr, 1, nullptr);
                player->AddObjectToContainer(wisdomMisc, nullptr, 1, nullptr);
                res.message = "감정 장비의 고유 ID 추적에 실패해 토큰과 감정 주문서를 되돌렸습니다.";
                return;
            }
            ItemTracker::Upsert(key, rolled, false);

            res.ok = true;
            res.resultBaseFormId = baseObj->GetFormID();
            res.resultKey = key;
            res.resultRecord = rolled;
            res.message = std::string(baseObj->GetName() ? baseObj->GetName() : "장비") + " 감정 완료!";
        });

        return res;
    }

    CraftActionResult CraftingRuntime::IdentifyUniqueToken(std::string_view forcedUniqueId)
    {
        CraftActionResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_itemData || !g_itemData->IsLoaded() || g_itemData->AllUniques().empty()) {
            res.message = "고유 아이템 데이터가 준비되지 않았습니다.";
            return res;
        }
        auto* tokenMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kUniqueSealedToken);
        auto* wisdomMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CurrencyLocalFormId(Core::CurrencyKind::ScrollOfWisdom));
        if (!tokenMisc || !wisdomMisc) {
            res.message = "고유 토큰 또는 감정 주문서 폼을 찾을 수 없습니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            if (CountBoundObjectInInventory(player, tokenMisc) < 1) {
                res.message = "고유 봉인 토큰이 없습니다.";
                return;
            }
            if (CountBoundObjectInInventory(player, wisdomMisc) < 1) {
                res.message = "감정 주문서가 부족합니다.";
                return;
            }

            const Core::UniqueDefinition* chosen = nullptr;
            if (!forcedUniqueId.empty()) {
                chosen = g_itemData->FindUnique(forcedUniqueId);
            } else {
                const auto& all = g_itemData->AllUniques();
                std::scoped_lock lock(g_rngMutex);
                std::uniform_int_distribution<std::size_t> dist(0, all.size() - 1);
                chosen = &all[dist(g_rng)];
            }
            if (!chosen) {
                res.message = "유효한 고유 아이템 정의를 찾지 못했습니다.";
                return;
            }

            RE::TESBoundObject* baseObj = nullptr;
            if (chosen->slotGroup == Core::TokenSlotGroup::Belt && FormIds::IsBeltBaseLocalFormId(chosen->baseFormId)) {
                baseObj = FormIds::Lookup<RE::TESBoundObject>(chosen->baseFormId);
            } else {
                baseObj = RE::TESForm::LookupByID<RE::TESBoundObject>(chosen->baseFormId);
            }
            if (!baseObj) {
                res.message = "고유 아이템 베이스 폼(0x" + std::to_string(chosen->baseFormId) + ")을 찾지 못했습니다.";
                return;
            }

            const auto st = SaveLoad::GetStateCopy();
            const std::uint32_t iLvl = std::max(chosen->reqLevel, std::clamp<std::uint32_t>(st.exileLevel, 1, Core::kMaxExileLevel));
            Core::PoEItemRecord rolled;
            {
                std::scoped_lock lock(g_rngMutex);
                rolled = Core::ItemCrafting::RollUniqueItem(*chosen, baseObj->GetFormID(), iLvl, *g_itemData, g_rng);
            }

            auto* freshExtra = UniqueIdUtil::CreateGameExtraDataList();
            if (!freshExtra) {
                res.message = "고유 아이템 데이터 할당에 실패했습니다.";
                return;
            }

            player->RemoveItem(tokenMisc, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            player->RemoveItem(wisdomMisc, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            bool untrackedLeft = false;
            const Core::InventoryItemKey key = ItemTracker::AddObjectWithTrackedUniqueId(
                player, baseObj, freshExtra, nullptr, ItemTracker::UntrackedAddPolicy::RemoveAgain, &untrackedLeft);
            if (!key.IsValid()) {
                if (untrackedLeft) {
                    // 재검수 R-15: 베이스 아이템이 추적 없이 이미 들어갔다. 환불하면 복제되므로 소모로 처리한다.
                    res.message = "고유 아이템 추적 ID 등록에 실패했습니다. 베이스 아이템만 가방에 들어갔고 토큰은 소모되었습니다.";
                    return;
                }
                // C-5: 추적 ID 확정 실패 시 차감한 고유 토큰·감정 주문서를 환불한다.
                player->AddObjectToContainer(tokenMisc, nullptr, 1, nullptr);
                player->AddObjectToContainer(wisdomMisc, nullptr, 1, nullptr);
                res.message = "고유 아이템 추적 ID 등록에 실패해 토큰과 감정 주문서를 되돌렸습니다.";
                return;
            }
            ItemTracker::Upsert(key, rolled, false);

            res.ok = true;
            res.resultBaseFormId = baseObj->GetFormID();
            res.resultKey = key;
            res.resultRecord = rolled;
            res.message = "[고유] " + chosen->koName + " 감정 완료!";
        });

        return res;
    }

    CraftActionResult CraftingRuntime::ApplyOrbToItem(
        Core::CurrencyKind orb,
        RE::FormID baseFormId,
        std::uint16_t uniqueId,
        bool preferEquipped)
    {
        CraftActionResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_itemData || !g_itemData->IsLoaded()) {
            res.message = "아이템 데이터가 아직 준비되지 않았습니다.";
            return res;
        }
        auto* baseObj = RE::TESForm::LookupByID<RE::TESBoundObject>(baseFormId);
        if (!baseObj) {
            res.message = "대상 장비를 찾을 수 없습니다.";
            return res;
        }

        auto* weap = baseObj->As<RE::TESObjectWEAP>();
        auto* armor = baseObj->As<RE::TESObjectARMO>();
        auto* misc = baseObj->As<RE::TESObjectMISC>();
        if (!weap && !armor && !misc) {
            res.message = "무기, 방어구, 벨트 또는 주얼에만 오브를 사용할 수 있습니다.";
            return res;
        }

        Core::TokenSlotGroup slotGroup = Core::TokenSlotGroup::OneHandWeapon;
        Core::DefenseSubtype defSub = Core::DefenseSubtype::None;
        bool isAmulet = false;
        std::uint8_t jewelColor = 0;
        std::uint8_t mapTier = 0;
        const Core::BeltBaseDef* beltDef = nullptr;
        std::string wTag;
        if (weap) {
            slotGroup = EquipmentSync::IsTwoHandedWeapon(weap)
                ? Core::TokenSlotGroup::TwoHandOrRangedWeapon
                : Core::TokenSlotGroup::OneHandWeapon;
            wTag = EquipmentSync::ClassifyWeaponTag(weap);
        } else if (armor) {
            const auto slotOpt = EquipmentSync::ClassifyArmorEquipSlot(armor);
            if (!slotOpt.has_value()) {
                res.message = "PoE 어픽스 적용 대상이 아닌 외형 장비입니다.";
                return res;
            }
            slotGroup = EquipmentSync::EquipSlotToTokenGroup(*slotOpt, false);
            defSub = EquipmentSync::ClassifyDefenseSubtype(armor);
            isAmulet = (*slotOpt == Core::EquipSlot::Amulet);
        } else {
            const auto localId = FormIds::ResolveLocalFormIdIfOurs(misc->GetFormID());
            if (FormIds::IsBeltBaseLocalFormId(localId)) {
                slotGroup = Core::TokenSlotGroup::Belt;
                beltDef = g_itemData->FindBeltByLocalFormId(localId);
            } else if (const auto jColor = FormIds::JewelColorFromLocalFormId(localId); jColor.has_value()) {
                slotGroup = Core::TokenSlotGroup::Jewel;
                jewelColor = *jColor;
            } else if (const auto mTier = FormIds::MapTierFromLocalFormId(localId); mTier.has_value()) {
                slotGroup = Core::TokenSlotGroup::Map;
                mapTier = *mTier;
            } else {
                res.message = "PoE 어픽스를 부여할 수 없는 일반 소지품입니다.";
                return res;
            }
        }

        SaveLoad::RunSerializedTransaction([&]() {
            if (CountCurrency(player, orb) < 1) {
                res.message = "선택한 화폐 오브가 부족합니다.";
                return;
            }

            const RE::FormID playerId = player->GetFormID();
            RE::ExtraDataList* targetExtra = nullptr;
            Core::InventoryItemKey existingKey{};

            if (uniqueId != 0) {
                existingKey = Core::InventoryItemKey{ playerId, uniqueId };
                targetExtra = ItemTracker::FindExtraListByKey(player, baseObj, existingKey);
                if (!targetExtra) {
                    res.message = "인벤토리에서 대상 아이템 개체를 찾지 못했습니다.";
                    return;
                }
            } else {
                // 검수 C1: 미추적(ExtraUniqueID 없음) 수량만 검증해 추적 중인 희귀 장비 오삭제를 원천 차단
                const auto untracked = InspectUntrackedItems(player, baseObj);
                if (untracked.untrackedTotal < 1) {
                    res.message = "인벤토리에서 미제작 일반 대상 아이템을 찾지 못했습니다.";
                    return;
                }
                if (preferEquipped && untracked.wornUntracked) {
                    targetExtra = untracked.wornUntracked;
                } else if (!preferEquipped) {
                    // 가방 미추적 개체 우선(plain 스택이면 nullptr로 1개 분리), 없을 때만 착용 미추적 개체 사용
                    if (untracked.hasPlainStack) {
                        targetExtra = nullptr;
                    } else if (untracked.bagUntracked) {
                        targetExtra = untracked.bagUntracked;
                    } else {
                        targetExtra = untracked.wornUntracked;
                    }
                } else {
                    targetExtra = untracked.hasPlainStack ? nullptr : untracked.bagUntracked;
                }
            }

            const auto st = SaveLoad::GetStateCopy();
            const std::uint32_t defaultItemLevel = (slotGroup == Core::TokenSlotGroup::Map && mapTier > 0)
                ? Core::MapSystem::MonsterLevelForTier(mapTier)
                : std::clamp<std::uint32_t>(st.exileLevel, 1, Core::kMaxExileLevel);
            Core::PoEItemRecord working;
            if (existingKey.IsValid()) {
                if (const auto found = ItemTracker::FindByKey(existingKey)) {
                    working = *found;
                    if (slotGroup == Core::TokenSlotGroup::Map && mapTier > 0) {
                        working.slotGroup = Core::TokenSlotGroup::Map;
                        working.itemLevel = defaultItemLevel;
                    }
                } else {
                    working.baseFormId = baseFormId;
                    working.rarity = Core::ItemRarity::Normal;
                    working.itemLevel = defaultItemLevel;
                    working.slotGroup = slotGroup;
                    working.defenseSubtype = defSub;
                    working.isAmulet = isAmulet;
                    working.jewelColor = jewelColor;
                    working.weaponTag = wTag;
                }
            } else {
                working.baseFormId = baseFormId;
                working.rarity = Core::ItemRarity::Normal;
                working.itemLevel = defaultItemLevel;
                working.slotGroup = slotGroup;
                working.defenseSubtype = defSub;
                working.isAmulet = isAmulet;
                working.jewelColor = jewelColor;
                working.weaponTag = wTag;
            }

            if (beltDef && working.uniqueId.empty() && working.implicitAffixes.empty()) {
                std::scoped_lock lock(g_rngMutex);
                working.implicitAffixes = Core::ItemCrafting::RollBeltImplicits(*beltDef, *g_itemData, g_rng);
            }

            const auto itemTags = Core::ItemData::BuildItemTags(
                working.slotGroup, working.defenseSubtype, working.weaponTag, working.isAmulet, working.jewelColor);

            Core::CraftOutcome outcome;
            {
                std::scoped_lock lock(g_rngMutex);
                outcome = Core::ItemCrafting::ApplyOrb(orb, working, itemTags, *g_itemData, g_rng);
            }
            if (!outcome.IsOk()) {
                res.message = outcome.message;
                return;  // 실패 시 오브 차감 0!
            }

            // 성공 시에만 UID 보장(필요 시 스택 1개 분리) + 오브 1 차감 + ItemTracker 갱신
            const Core::InventoryItemKey finalKey = existingKey.IsValid()
                ? existingKey
                : ItemTracker::EnsureUniqueId(player, baseObj, targetExtra);
            if (!finalKey.IsValid()) {
                res.message = "아이템 고유 ID 발급에 실패해 제작을 취소했습니다.";
                return;
            }

            if (!ConsumeCurrency(player, orb, 1)) {
                res.message = "화폐 오브 차감에 실패했습니다.";
                return;
            }

            // 검수 M1: Scouring 후에도 itemLevel 보존을 위해 Normal 상태로 ItemTracker에 유지
            ItemTracker::Upsert(finalKey, working, false);

            res.ok = true;
            res.resultBaseFormId = baseFormId;
            res.resultKey = finalKey;
            res.resultRecord = working;
            res.message = outcome.message;
        });

        if (res.ok) {
            EquipmentSync::RefreshFromPlayerNow();
        }
        return res;
    }

    CraftActionResult CraftingRuntime::ApplyJewellerToGem(
        std::uint8_t slotIndex,
        std::uint64_t triggeredSpellInstanceId)
    {
        CraftActionResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_dataPack || !g_dataPack->IsLoaded()) {
            res.message = "젬 데이터가 준비되지 않았습니다.";
            return res;
        }
        if (slotIndex >= Core::kSkillSlotCount) {
            res.message = "잘못된 스킬 슬롯입니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            if (CountCurrency(player, Core::CurrencyKind::Jeweller) < 1) {
                res.message = "쥬얼러 오브가 부족합니다.";
                return;
            }

            // 검수 C2: 사본에서 먼저 검증·적용 → 오브 차감 성공 확인 → 성공 시에만 g_state 커밋
            PersistentState working = SaveLoad::GetStateCopy();
            auto& slot = working.slots[slotIndex];
            if (!slot.occupied) {
                res.message = "해당 슬롯에 장착된 젬이 없습니다.";
                return;
            }
            Core::GemInstance* targetGem = &slot.gem;
            if (triggeredSpellInstanceId != 0) {
                targetGem = nullptr;
                for (auto& child : slot.gem.socketedGems) {
                    if (child.instanceId == triggeredSpellInstanceId) {
                        targetGem = &child;
                        break;
                    }
                }
                if (!targetGem) {
                    res.message = "발동 젬 안에서 대상 주문 젬을 찾지 못했습니다.";
                    return;
                }
            }
            const auto* def = g_dataPack->FindGem(targetGem->gemId);
            const auto kind = def ? def->kind : Core::GemKind::Unknown;
            const auto out = Core::ItemCrafting::ApplyJewellerToGem(*targetGem, kind);
            if (!out.IsOk()) {
                res.message = out.message;
                return;
            }

            if (!ConsumeCurrency(player, Core::CurrencyKind::Jeweller, 1)) {
                res.message = "쥬얼러 오브 차감에 실패했습니다.";
                return;
            }
            // C-30: 변경한 스킬 슬롯만 필드 단위로 병합
            SaveLoad::UpdateState([&](PersistentState& st) {
                st.slots[slotIndex] = working.slots[slotIndex];
            });
            res.ok = true;
            res.message = out.message;
        });

        if (res.ok) {
            SaveLoad::SyncStateToStatEngine();
        }
        return res;
    }

    Core::PassiveTreeOpResult CraftingRuntime::RefundPassiveNodeWithRegret(std::uint32_t nodeId)
    {
        Core::PassiveTreeOpResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_treeData || !g_treeData->IsLoaded() || !g_config) {
            res.message = "패시브 트리 데이터가 준비되지 않았습니다.";
            return res;
        }

        // 검수 C2: 사본 검증 → 오브 차감 성공 확인 → 성공 시에만 g_state 커밋 + unallocatedPassivePoints 동기화
        SaveLoad::RunSerializedTransaction([&]() {
            const auto availOrbs = static_cast<std::uint32_t>(std::max<std::int32_t>(
                0, CountCurrency(player, Core::CurrencyKind::Regret)));
            PersistentState working = SaveLoad::GetStateCopy();
            res = Core::PassiveTree::RefundNode(
                *g_treeData, working.tree, nodeId, working.exileLevel, *g_config, availOrbs, working.atlas.completedTierMask);
            if (!res.ok) {
                return;
            }
            if (res.regretOrbsConsumed > 0 &&
                !ConsumeCurrency(player, Core::CurrencyKind::Regret, static_cast<std::int32_t>(res.regretOrbsConsumed))) {
                res = { false, "후회의 오브 차감에 실패해 회수를 취소했습니다.", 0, 0 };
                return;
            }
            // 5단계(결정 164) + 검수 C4: 회수된 소켓 노드에 주얼이 꽂혀 있으면 반환 성공 시에만 제거(실패 시 PostLoadInit 재시도용 보존)
            if (auto jIt = working.socketedJewels.find(nodeId); jIt != working.socketedJewels.end()) {
                if (TryReturnJewelToInventory(player, jIt->second)) {
                    working.socketedJewels.erase(jIt);
                } else {
                    SKSE::log::warn("CraftingRuntime: failed to return jewel from refunded socket node {}; kept in map for retry.", nodeId);
                }
            }
            const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                *g_treeData, working.tree, working.exileLevel, *g_config, working.atlas.completedTierMask);
            const auto newUnalloc = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
            // C-30: 트리·주얼·미배분 포인트만 필드 단위로 병합
            SaveLoad::UpdateState([&](PersistentState& st) {
                st.tree = working.tree;
                st.socketedJewels = working.socketedJewels;
                st.unallocatedPassivePoints = newUnalloc;
            });
        });

        if (res.ok) {
            SaveLoad::SyncStateToStatEngine();
        }
        return res;
    }

    Core::PassiveTreeOpResult CraftingRuntime::RefundAllPassivesWithRegret(bool ascendancyOnly)
    {
        Core::PassiveTreeOpResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_treeData || !g_treeData->IsLoaded() || !g_config) {
            res.message = "패시브 트리 데이터가 준비되지 않았습니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            const auto availOrbs = static_cast<std::uint32_t>(std::max<std::int32_t>(
                0, CountCurrency(player, Core::CurrencyKind::Regret)));
            PersistentState working = SaveLoad::GetStateCopy();
            res = Core::PassiveTree::RefundAll(
                *g_treeData, working.tree, ascendancyOnly, working.exileLevel, *g_config, availOrbs, working.atlas.completedTierMask);
            if (!res.ok) {
                return;
            }
            if (res.regretOrbsConsumed > 0 &&
                !ConsumeCurrency(player, Core::CurrencyKind::Regret, static_cast<std::int32_t>(res.regretOrbsConsumed))) {
                res = { false, "후회의 오브 차감에 실패해 회수를 취소했습니다.", 0, 0 };
                return;
            }
            // 5단계(결정 164) + 검수 C4: 전체 회수로 해제된 소켓 주얼은 반환 성공 시에만 제거
            for (auto it = working.socketedJewels.begin(); it != working.socketedJewels.end(); ) {
                if (!working.tree.IsAllocated(it->first)) {
                    if (TryReturnJewelToInventory(player, it->second)) {
                        it = working.socketedJewels.erase(it);
                    } else {
                        SKSE::log::warn("CraftingRuntime: failed to return jewel from refunded socket node {}; kept for retry.", it->first);
                        ++it;
                    }
                } else {
                    ++it;
                }
            }
            const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                *g_treeData, working.tree, working.exileLevel, *g_config, working.atlas.completedTierMask);
            const auto newUnalloc = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
            // C-30: 트리·주얼·미배분 포인트만 필드 단위로 병합
            SaveLoad::UpdateState([&](PersistentState& st) {
                st.tree = working.tree;
                st.socketedJewels = working.socketedJewels;
                st.unallocatedPassivePoints = newUnalloc;
            });
        });

        if (res.ok) {
            SaveLoad::SyncStateToStatEngine();
        }
        return res;
    }

    Core::PassiveTreeOpResult CraftingRuntime::SelectMasteryWithRegret(
        std::uint32_t masteryNodeId,
        std::uint32_t effectId)
    {
        Core::PassiveTreeOpResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_treeData || !g_treeData->IsLoaded() || !g_config) {
            res.message = "패시브 트리 데이터가 준비되지 않았습니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            const auto availOrbs = static_cast<std::uint32_t>(std::max<std::int32_t>(
                0, CountCurrency(player, Core::CurrencyKind::Regret)));
            PersistentState working = SaveLoad::GetStateCopy();
            res = Core::PassiveTree::SelectMastery(
                *g_treeData, working.tree, masteryNodeId, effectId, working.exileLevel, *g_config, availOrbs, working.atlas.completedTierMask);
            if (!res.ok) {
                return;
            }
            if (res.regretOrbsConsumed > 0 &&
                !ConsumeCurrency(player, Core::CurrencyKind::Regret, static_cast<std::int32_t>(res.regretOrbsConsumed))) {
                res = { false, "후회의 오브 차감에 실패해 마스터리 교체를 취소했습니다.", 0, 0 };
                return;
            }
            const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                *g_treeData, working.tree, working.exileLevel, *g_config, working.atlas.completedTierMask);
            const auto newUnalloc = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
            // C-30: 트리·주얼·미배분 포인트만 필드 단위로 병합
            SaveLoad::UpdateState([&](PersistentState& st) {
                st.tree = working.tree;
                st.socketedJewels = working.socketedJewels;
                st.unallocatedPassivePoints = newUnalloc;
            });
        });

        if (res.ok) {
            SaveLoad::SyncStateToStatEngine();
        }
        return res;
    }

    CraftActionResult CraftingRuntime::EquipVirtualRing2(RE::FormID baseFormId, std::uint16_t uniqueId)
    {
        CraftActionResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(baseFormId);
        if (!player || !IsArmorPlayable(armor) ||
            EquipmentSync::ClassifyArmorEquipSlot(armor) != Core::EquipSlot::Ring1) {
            res.message = "반지2 칸에는 착용 가능한 반지 장비만 지정할 수 있습니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            const RE::FormID playerId = player->GetFormID();
            const auto gearMeta = EquipmentSync::GetCurrentGearMeta();
            const auto ring1Key = gearMeta[static_cast<std::size_t>(Core::EquipSlot::Ring1)].key;

            Core::InventoryItemKey finalKey{};
            if (uniqueId != 0) {
                finalKey = Core::InventoryItemKey{ playerId, uniqueId };
                auto* extra = ItemTracker::FindExtraListByKey(player, armor, finalKey);
                if (!extra) {
                    res.message = "가방에서 해당 반지 개체를 찾지 못했습니다.";
                    return;
                }
                if (extra->HasType<RE::ExtraWorn>() || extra->HasType<RE::ExtraWornLeft>() ||
                    (ring1Key.IsValid() && ring1Key == finalKey)) {
                    res.message = "반지1에 이미 착용 중인 반지는 반지2에 중복 장착할 수 없습니다.";
                    return;
                }
            } else {
                const auto untracked = InspectUntrackedItems(player, armor);
                RE::ExtraDataList* targetExtra = nullptr;
                if (untracked.hasPlainStack) {
                    targetExtra = nullptr;
                } else if (untracked.bagUntracked) {
                    targetExtra = untracked.bagUntracked;
                } else {
                    // 바닐라 엔진이 부여한 ExtraUniqueID만 있고 ItemTracker 미등록·미착용인 여분 반지 허용
                    if (auto* changes = player->GetInventoryChanges(true); changes && changes->entryList) {
                        for (auto* entry : *changes->entryList) {
                            if (!entry || entry->object != armor || !entry->extraLists) continue;
                            for (auto* ex : *entry->extraLists) {
                                if (!ex || ex->HasType<RE::ExtraWorn>() || ex->HasType<RE::ExtraWornLeft>()) continue;
                                if (const auto* u = ex->GetByType<RE::ExtraUniqueID>(); u && u->baseID == playerId && u->uniqueID != 0) {
                                    const Core::InventoryItemKey cand{ playerId, u->uniqueID };
                                    if ((!ring1Key.IsValid() || ring1Key != cand) && !ItemTracker::FindByKey(cand).has_value()) {
                                        finalKey = cand;
                                        break;
                                    }
                                }
                            }
                            if (finalKey.IsValid()) break;
                        }
                    }
                    if (!finalKey.IsValid()) {
                        res.message = "반지1에 착용 중이지 않은 여분의 반지가 가방에 없습니다.";
                        return;
                    }
                }
                if (!finalKey.IsValid()) {
                    finalKey = ItemTracker::EnsureUniqueId(player, armor, targetExtra);
                }
                if (!finalKey.IsValid()) {
                    res.message = "반지2 고유 ID 부여에 실패했습니다.";
                    return;
                }
            }

            SaveLoad::UpdateState([&](PersistentState& st) {
                st.virtualRing2Key = finalKey;
                st.virtualRing2BaseFormId = baseFormId;
            });
            res.ok = true;
            res.resultBaseFormId = baseFormId;
            res.resultKey = finalKey;
            res.message = std::string(armor->GetName() ? armor->GetName() : "반지") + "을(를) 반지2 칸에 장착했습니다.";
        });

        if (res.ok) {
            EquipmentSync::RefreshFromPlayerNow();
        }
        return res;
    }

    CraftActionResult CraftingRuntime::UnequipVirtualRing2()
    {
        CraftActionResult res;
        SaveLoad::RunSerializedTransaction([&]() {
            SaveLoad::UpdateState([&](PersistentState& st) {
                st.virtualRing2Key = {};
                st.virtualRing2BaseFormId = 0;
            });
            res.ok = true;
            res.message = "반지2 칸 장착을 해제했습니다.";
        });
        EquipmentSync::RefreshFromPlayerNow();
        return res;
    }

    CraftActionResult CraftingRuntime::EquipVirtualBelt(RE::FormID baseFormId, std::uint16_t uniqueId)
    {
        CraftActionResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* misc = RE::TESForm::LookupByID<RE::TESObjectMISC>(baseFormId);
        const auto localId = misc ? FormIds::ResolveLocalFormIdIfOurs(misc->GetFormID()) : 0;
        if (!player || !misc || !FormIds::IsBeltBaseLocalFormId(localId) || !g_itemData || !g_itemData->IsLoaded()) {
            res.message = "벨트 칸에는 유효한 허리띠 장비만 장착할 수 있습니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            const RE::FormID playerId = player->GetFormID();
            Core::InventoryItemKey finalKey{};
            if (uniqueId != 0) {
                finalKey = Core::InventoryItemKey{ playerId, uniqueId };
                if (!ItemTracker::FindExtraListByKey(player, misc, finalKey)) {
                    res.message = "가방에서 해당 벨트 개체를 찾지 못했습니다.";
                    return;
                }
            } else {
                const auto untracked = InspectUntrackedItems(player, misc);
                if (untracked.untrackedTotal < 1) {
                    res.message = "가방에 장착할 일반 벨트가 없습니다.";
                    return;
                }
                RE::ExtraDataList* targetExtra = untracked.hasPlainStack ? nullptr : untracked.bagUntracked;
                finalKey = ItemTracker::EnsureUniqueId(player, misc, targetExtra);
                if (!finalKey.IsValid()) {
                    res.message = "벨트 고유 ID 부여에 실패했습니다.";
                    return;
                }
            }

            // 일반 벨트에 아직 PoE 레코드가 없으면 고정 임플리싯을 1회 부여
            if (!ItemTracker::FindByKey(finalKey).has_value()) {
                if (const auto* beltDef = g_itemData->FindBeltByLocalFormId(localId)) {
                    const auto st = SaveLoad::GetStateCopy();
                    Core::PoEItemRecord rec;
                    rec.baseFormId = baseFormId;
                    rec.rarity = Core::ItemRarity::Normal;
                    rec.itemLevel = std::clamp<std::uint32_t>(st.exileLevel, 1, Core::kMaxExileLevel);
                    rec.slotGroup = Core::TokenSlotGroup::Belt;
                    {
                        std::scoped_lock lk(g_rngMutex);
                        rec.implicitAffixes = Core::ItemCrafting::RollBeltImplicits(*beltDef, *g_itemData, g_rng);
                    }
                    ItemTracker::Upsert(finalKey, rec, false);
                }
            }

            SaveLoad::UpdateState([&](PersistentState& st) {
                st.virtualBeltKey = finalKey;
                st.virtualBeltBaseFormId = baseFormId;
            });
            res.ok = true;
            res.resultBaseFormId = baseFormId;
            res.resultKey = finalKey;
            res.message = std::string(misc->GetName() ? misc->GetName() : "벨트") + "을(를) 장착했습니다.";
        });

        if (res.ok) {
            EquipmentSync::RefreshFromPlayerNow();
        }
        return res;
    }

    CraftActionResult CraftingRuntime::UnequipVirtualBelt()
    {
        CraftActionResult res;
        SaveLoad::RunSerializedTransaction([&]() {
            SaveLoad::UpdateState([&](PersistentState& st) {
                st.virtualBeltKey = {};
                st.virtualBeltBaseFormId = 0;
            });
            res.ok = true;
            res.message = "벨트 장착을 해제했습니다.";
        });
        EquipmentSync::RefreshFromPlayerNow();
        return res;
    }

    CraftActionResult CraftingRuntime::EquipCharm(std::uint8_t slotIndex, Core::CharmKind kind)
    {
        CraftActionResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_charmSystem || slotIndex >= Core::kMaxCharmSlots) {
            res.message = "참 슬롯을 사용할 수 없습니다.";
            return res;
        }
        const auto localId = FormIds::CharmLocalFormId(kind);
        auto* charmMisc = localId ? FormIds::Lookup<RE::TESObjectMISC>(localId) : nullptr;
        if (!charmMisc) {
            res.message = "해당 참 폼을 찾지 못했습니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            const std::uint8_t unlocked = g_charmSystem->UnlockedSlots();
            if (slotIndex >= unlocked) {
                res.message = "벨트 등급이 낮아 해당 참 칸이 잠겨 있습니다.";
                return;
            }
            if (CountBoundObjectInInventory(player, charmMisc) < 1) {
                res.message = "가방에 선택한 참이 없습니다.";
                return;
            }

            // 검수 M6: 단일 슬롯 원자 갱신(SetSlot)으로 다른 칸의 전투 스레드 충전 차감 덮어쓰기(TOCTOU) 방지
            const auto curSlot = g_charmSystem->Slots()[slotIndex];
            if (curSlot.occupied) {
                if (auto* oldMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CharmLocalFormId(curSlot.kind))) {
                    player->AddObjectToContainer(oldMisc, nullptr, 1, nullptr);
                }
            }
            player->RemoveItem(charmMisc, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);

            Core::CharmSlotState nextSlot{};
            nextSlot.occupied = true;
            nextSlot.kind = kind;
            // U4 확정(2026-11-20): 탈착/재장착 무한 충전 악용 차단을 위해 신규 장착 시 항상 0 충전으로 시작(처치로만 충전)
            nextSlot.charges = 0.0;
            g_charmSystem->SetSlot(slotIndex, nextSlot);
            SaveLoad::PersistCharmSlotsFromSystem();

            res.ok = true;
            res.resultBaseFormId = charmMisc->GetFormID();
            res.message = std::string(charmMisc->GetName() ? charmMisc->GetName() : "참") + "을(를) 장착했습니다.";
        });
        return res;
    }

    CraftActionResult CraftingRuntime::UnequipCharm(std::uint8_t slotIndex)
    {
        CraftActionResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_charmSystem || slotIndex >= Core::kMaxCharmSlots) {
            res.message = "잘못된 참 슬롯입니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            const auto curSlot = g_charmSystem->Slots()[slotIndex];
            if (!curSlot.occupied) {
                res.message = "비어 있는 참 슬롯입니다.";
                return;
            }
            if (auto* oldMisc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CharmLocalFormId(curSlot.kind))) {
                player->AddObjectToContainer(oldMisc, nullptr, 1, nullptr);
            }
            g_charmSystem->SetSlot(slotIndex, Core::CharmSlotState{});
            SaveLoad::PersistCharmSlotsFromSystem();
            res.ok = true;
            res.message = "참을 가방으로 회수했습니다.";
        });
        return res;
    }

    CraftActionResult CraftingRuntime::SocketPassiveJewel(
        std::uint32_t nodeId,
        RE::FormID baseFormId,
        std::uint16_t uniqueId)
    {
        CraftActionResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* jewelMisc = RE::TESForm::LookupByID<RE::TESObjectMISC>(baseFormId);
        const auto localId = jewelMisc ? FormIds::ResolveLocalFormIdIfOurs(jewelMisc->GetFormID()) : 0;
        const auto colorOpt = FormIds::JewelColorFromLocalFormId(localId);
        if (!player || !jewelMisc || !colorOpt.has_value() || !g_treeData || !g_treeData->IsLoaded()) {
            res.message = "유효한 패시브 주얼 또는 트리 데이터가 아닙니다.";
            return res;
        }
        const auto* node = g_treeData->FindNode(nodeId);
        if (!node || node->kind != Core::PassiveNodeKind::Socket) {
            res.message = "선택한 패시브 노드는 주얼 소켓이 아닙니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            PersistentState working = SaveLoad::GetStateCopy();
            if (!working.tree.IsAllocated(nodeId)) {
                res.message = "먼저 해당 주얼 소켓 패시브 노드를 할당해야 합니다.";
                return;
            }

            const RE::FormID playerId = player->GetFormID();
            Core::PoEItemRecord jewelRec;
            RE::ExtraDataList* removeExtra = nullptr;
            Core::InventoryItemKey trackedKey{};

            if (uniqueId != 0) {
                trackedKey = Core::InventoryItemKey{ playerId, uniqueId };
                removeExtra = ItemTracker::FindExtraListByKey(player, jewelMisc, trackedKey);
                if (!removeExtra) {
                    res.message = "가방에서 해당 주얼 개체를 찾지 못했습니다.";
                    return;
                }
                if (const auto found = ItemTracker::FindByKey(trackedKey)) {
                    jewelRec = *found;
                } else {
                    jewelRec.baseFormId = baseFormId;
                    jewelRec.rarity = Core::ItemRarity::Normal;
                    jewelRec.itemLevel = std::clamp<std::uint32_t>(working.exileLevel, 1, Core::kMaxExileLevel);
                    jewelRec.slotGroup = Core::TokenSlotGroup::Jewel;
                    jewelRec.jewelColor = *colorOpt;
                }
            } else {
                const auto untracked = InspectUntrackedItems(player, jewelMisc);
                if (untracked.untrackedTotal < 1) {
                    res.message = "가방에 일반 주얼이 없습니다.";
                    return;
                }
                removeExtra = untracked.hasPlainStack ? nullptr : untracked.bagUntracked;
                jewelRec.baseFormId = baseFormId;
                jewelRec.rarity = Core::ItemRarity::Normal;
                jewelRec.itemLevel = std::clamp<std::uint32_t>(working.exileLevel, 1, Core::kMaxExileLevel);
                jewelRec.slotGroup = Core::TokenSlotGroup::Jewel;
                jewelRec.jewelColor = *colorOpt;
            }

            // 검수 C4: 기존 소켓 주얼이 있으면 반환 성공을 먼저 확인한 뒤에만 교체(실패 시 롤백해 영구 소실 차단)
            bool returnedOld = false;
            if (auto oldIt = working.socketedJewels.find(nodeId); oldIt != working.socketedJewels.end()) {
                if (!TryReturnJewelToInventory(player, oldIt->second)) {
                    res.message = "기존 주얼을 가방으로 반환하는 데 실패해 교체를 취소했습니다.";
                    return;
                }
                returnedOld = true;
            }

            // 가방에서 1개 차감 후 트리 소켓에 직접 소유 저장(Q-6: 일반 주얼은 RemovePlainItem으로 추적 주얼 보호)
            if (removeExtra) {
                player->RemoveItem(jewelMisc, 1, RE::ITEM_REMOVE_REASON::kRemove, removeExtra, nullptr);
            } else {
                if (!UniqueIdUtil::RemovePlainItem(player, jewelMisc, 1)) {
                    if (returnedOld) {
                        working.socketedJewels.erase(nodeId);
                        SaveLoad::UpdateState([&](PersistentState& st) {
                            st.socketedJewels = working.socketedJewels;
                        });
                    }
                    res.message = "가방에서 일반 주얼을 안전하게 차감하지 못했습니다.";
                    return;
                }
            }
            if (trackedKey.IsValid()) {
                ItemTracker::RemoveByKey(trackedKey);
            }

            working.socketedJewels[nodeId] = jewelRec;
            SaveLoad::UpdateState([&](PersistentState& st) {
                st.socketedJewels = working.socketedJewels;
            });
            res.ok = true;
            res.resultBaseFormId = baseFormId;
            res.resultRecord = jewelRec;
            res.message = "패시브 주얼을 소켓에 장착했습니다.";
        });

        if (res.ok) {
            SaveLoad::SyncStateToStatEngine();
        }
        return res;
    }

    CraftActionResult CraftingRuntime::UnsocketPassiveJewel(std::uint32_t nodeId)
    {
        CraftActionResult res;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            res.message = "플레이어를 찾지 못했습니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            PersistentState working = SaveLoad::GetStateCopy();
            auto it = working.socketedJewels.find(nodeId);
            if (it == working.socketedJewels.end()) {
                res.message = "해당 소켓에 장착된 주얼이 없습니다.";
                return;
            }
            const auto rec = it->second;
            Core::InventoryItemKey newKey{};
            RE::FormID returnedBaseId = 0;
            if (!TryReturnJewelToInventory(player, rec, &newKey, &returnedBaseId)) {
                res.message = "주얼 가방 반환 추적에 실패했습니다.";
                return;
            }
            working.socketedJewels.erase(it);
            SaveLoad::UpdateState([&](PersistentState& st) {
                st.socketedJewels = working.socketedJewels;
            });

            res.ok = true;
            res.resultBaseFormId = returnedBaseId;
            res.resultKey = newKey;
            res.resultRecord = rec;
            // 재검수 R-15: 실물은 돌아왔지만 추적에 실패해 어픽스를 묶지 못한 경우를 그대로 알린다.
            res.message = newKey.IsValid()
                ? "패시브 주얼을 가방으로 회수했습니다."
                : "패시브 주얼을 가방으로 회수했지만 추적에 실패해 어픽스가 사라졌습니다.";
        });

        if (res.ok) {
            SaveLoad::SyncStateToStatEngine();
        }
        return res;
    }

    CraftActionResult CraftingRuntime::ToggleAuraSlot(std::uint8_t slotIndex)
    {
        CraftActionResult res;
        if (slotIndex >= Core::kSkillSlotCount || !g_dataPack || !g_dataPack->IsLoaded() || !g_config || !g_statEngine) {
            res.message = "잘못된 스킬 슬롯입니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            PersistentState working = SaveLoad::GetStateCopy();
            const auto& slot = working.slots[slotIndex];
            if (!slot.occupied) {
                res.message = "해당 슬롯에 장착된 젬이 없습니다.";
                return;
            }
            const auto* def = g_dataPack->FindGem(slot.gem.gemId);
            if (!def) {
                res.message = "정신력 예약 스킬을 찾지 못했습니다.";
                return;
            }

            const std::uint16_t bit = static_cast<std::uint16_t>(1u << slotIndex);
            const bool turningOn = (working.activeAuraMask & bit) == 0;
            const auto proposedMask = static_cast<std::uint16_t>(working.activeAuraMask ^ bit);
            if (turningOn) {
                // 저장 상태와 현재 장비를 합친 뒤 스냅숏과 같은 입력으로 미리 계산한다.
                SaveLoad::SyncStateToStatEngine();
                const auto preview = g_statEngine->PreviewSpiritToggle(slotIndex, proposedMask, *g_dataPack, *g_config);
                if (!preview.canReserve) {
                    res.message = "정신력 예약이 가능한 스킬에만 사용할 수 있습니다.";
                    return;
                }
                if (g_config->enforceGemRequirements && !preview.meetsRequirements) {
                    res.message = def->name + " 요구치(레벨/능력치)를 충족하지 못했습니다.";
                    return;
                }
                if (preview.requestedSpirit > preview.maxSpirit) {
                    res.message = std::format("정신력이 부족합니다 (예약 필요 {} / 최대 {}).", preview.requestedSpirit, preview.maxSpirit);
                    return;
                }
                res.message = std::format("{} 활성화 (정신력 예약 {}/{}).", def->name, preview.reservedSpirit, preview.maxSpirit);
            } else {
                res.message = def->name + " 비활성화.";
            }

            SaveLoad::UpdateState([&](PersistentState& st) {
                st.activeAuraMask = proposedMask;
            });
            res.ok = true;
        });

        if (res.ok) {
            SaveLoad::SyncStateToStatEngine();
        }
        return res;
    }
}
