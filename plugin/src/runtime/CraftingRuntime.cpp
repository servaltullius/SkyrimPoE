#include "PCH.h"
#include "runtime/CraftingRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/ItemTracker.h"
#include "runtime/SaveLoad.h"
#include "runtime/UniqueIdUtil.h"

#include "core/ItemCrafting.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <random>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        const Core::ItemData* g_itemData = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::Config* g_config = nullptr;

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
            const std::size_t tIdx = std::clamp<std::size_t>(tier >= 1 ? tier - 1 : 0, 0, 4);
            const auto& vanillaIds = GetVanillaPool(slotGroup).byTier[tIdx];
            std::vector<RE::TESBoundObject*> valid;
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
                            if (!armor || !armor->GetPlayable() || armor->GetGoldValue() <= 0) continue;
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
    }

    void CraftingRuntime::Initialize(
        const Core::ItemData* itemData,
        const Core::DataPack* dataPack,
        const Core::PassiveTreeData* treeData,
        const Core::Config* config)
    {
        g_itemData = itemData;
        g_dataPack = dataPack;
        g_treeData = treeData;
        g_config = config;
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
        // 검수 m4: GetInventory()를 사용해 TESContainer 기본 보유 아이템(countDelta == 0)도 누락 없이 열거
        const auto inv = player->GetInventory([](RE::TESBoundObject& obj) {
            return obj.Is(RE::FormType::Weapon) || obj.Is(RE::FormType::Armor);
        });

        for (const auto& [obj, pair] : inv) {
            const std::int32_t totalCount = pair.first;
            const auto& entryData = pair.second;
            if (!obj || totalCount <= 0) {
                continue;
            }
            auto* weap = obj->As<RE::TESObjectWEAP>();
            auto* armor = obj->As<RE::TESObjectARMO>();
            if (!weap && !armor) {
                continue;
            }

            Core::EquipSlot eqSlot = Core::EquipSlot::MainHand;
            Core::TokenSlotGroup slotGroup = Core::TokenSlotGroup::OneHandWeapon;
            Core::DefenseSubtype defSub = Core::DefenseSubtype::None;
            bool isAmulet = false;
            std::string wTag;

            if (weap) {
                if (!weap->GetPlayable()) continue;
                const bool twoH = EquipmentSync::IsTwoHandedWeapon(weap);
                eqSlot = Core::EquipSlot::MainHand;
                slotGroup = twoH ? Core::TokenSlotGroup::TwoHandOrRangedWeapon : Core::TokenSlotGroup::OneHandWeapon;
                wTag = EquipmentSync::ClassifyWeaponTag(weap);
            } else {
                if (!armor->GetPlayable()) continue;
                const auto slotOpt = EquipmentSync::ClassifyArmorEquipSlot(armor);
                if (!slotOpt.has_value()) {
                    continue;  // 결정 156: 외형 전용 바디슬롯은 제작 대상 제외
                }
                eqSlot = *slotOpt;
                slotGroup = EquipmentSync::EquipSlotToTokenGroup(eqSlot, false);
                defSub = EquipmentSync::ClassifyDefenseSubtype(armor);
                isAmulet = (eqSlot == Core::EquipSlot::Amulet);
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

                    PlayerBagItemEntry item;
                    item.baseFormId = obj->GetFormID();
                    item.uniqueId = uid;
                    item.count = cnt;
                    item.equipped = worn;
                    item.displayName = name;
                    item.equipSlot = eqSlot;
                    item.slotGroup = slotGroup;
                    item.defenseSubtype = defSub;
                    item.isAmulet = isAmulet;
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
                PlayerBagItemEntry plain;
                plain.baseFormId = obj->GetFormID();
                plain.uniqueId = 0;
                plain.count = plainRem;
                plain.equipped = false;
                plain.displayName = name;
                plain.equipSlot = eqSlot;
                plain.slotGroup = slotGroup;
                plain.defenseSubtype = defSub;
                plain.isAmulet = isAmulet;
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
        for (std::size_t offset = 0; offset < FormIds::kSealedTokenCount; ++offset) {
            const RE::FormID localId = FormIds::kSealedTokenBase + static_cast<RE::FormID>(offset);
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
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
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
            res.message = "토큰 또는 지혜의 두루마리 폼을 찾을 수 없습니다.";
            return res;
        }

        SaveLoad::RunSerializedTransaction([&]() {
            if (CountBoundObjectInInventory(player, tokenMisc) < 1) {
                res.message = "해당 봉인 토큰을 보유하고 있지 않습니다.";
                return;
            }
            if (CountBoundObjectInInventory(player, wisdomMisc) < 1) {
                res.message = "지혜의 두루마리가 부족합니다.";
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
            if (auto* weap = baseObj->As<RE::TESObjectWEAP>()) {
                wTag = EquipmentSync::ClassifyWeaponTag(weap);
            } else if (auto* armor = baseObj->As<RE::TESObjectARMO>()) {
                defSub = EquipmentSync::ClassifyDefenseSubtype(armor);
                const auto slotOpt = EquipmentSync::ClassifyArmorEquipSlot(armor);
                isAmulet = (slotOpt == Core::EquipSlot::Amulet);
            }

            const auto st = SaveLoad::GetStateCopy();
            Core::PoEItemRecord rolled;
            {
                std::scoped_lock lock(g_rngMutex);
                rolled = Core::ItemCrafting::IdentifyToken(
                    tier, rarity, slotGroup, defSub, wTag, isAmulet,
                    baseObj->GetFormID(), st.exileLevel, *g_itemData, g_rng);
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

            // 원자 반영: 토큰 1 + 지혜의 두루마리 1 차감 → AddObjectWithTrackedUniqueId로 엔진 최종 UID 확정 → ItemTracker 등록
            player->RemoveItem(tokenMisc, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            player->RemoveItem(wisdomMisc, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            const Core::InventoryItemKey key = ItemTracker::AddObjectWithTrackedUniqueId(player, baseObj, freshExtra);
            if (!key.IsValid()) {
                res.message = "감정 장비의 고유 ID 추적에 실패했습니다.";
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
        if (!weap && !armor) {
            res.message = "무기 또는 방어구에만 오브를 사용할 수 있습니다.";
            return res;
        }

        Core::TokenSlotGroup slotGroup = Core::TokenSlotGroup::OneHandWeapon;
        Core::DefenseSubtype defSub = Core::DefenseSubtype::None;
        bool isAmulet = false;
        std::string wTag;
        if (weap) {
            slotGroup = EquipmentSync::IsTwoHandedWeapon(weap)
                ? Core::TokenSlotGroup::TwoHandOrRangedWeapon
                : Core::TokenSlotGroup::OneHandWeapon;
            wTag = EquipmentSync::ClassifyWeaponTag(weap);
        } else {
            const auto slotOpt = EquipmentSync::ClassifyArmorEquipSlot(armor);
            if (!slotOpt.has_value()) {
                res.message = "PoE 어픽스 적용 대상이 아닌 외형 장비입니다.";
                return res;
            }
            slotGroup = EquipmentSync::EquipSlotToTokenGroup(*slotOpt, false);
            defSub = EquipmentSync::ClassifyDefenseSubtype(armor);
            isAmulet = (*slotOpt == Core::EquipSlot::Amulet);
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
            Core::PoEItemRecord working;
            if (existingKey.IsValid()) {
                if (const auto found = ItemTracker::FindByKey(existingKey)) {
                    working = *found;
                } else {
                    working.baseFormId = baseFormId;
                    working.rarity = Core::ItemRarity::Normal;
                    working.itemLevel = std::clamp<std::uint32_t>(st.exileLevel, 1, Core::kMaxExileLevel);
                    working.slotGroup = slotGroup;
                    working.defenseSubtype = defSub;
                    working.isAmulet = isAmulet;
                    working.weaponTag = wTag;
                }
            } else {
                working.baseFormId = baseFormId;
                working.rarity = Core::ItemRarity::Normal;
                working.itemLevel = std::clamp<std::uint32_t>(st.exileLevel, 1, Core::kMaxExileLevel);
                working.slotGroup = slotGroup;
                working.defenseSubtype = defSub;
                working.isAmulet = isAmulet;
                working.weaponTag = wTag;
            }

            const auto itemTags = Core::ItemData::BuildItemTags(
                working.slotGroup, working.defenseSubtype, working.weaponTag, working.isAmulet);

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
                res.message = "쥬얼러의 오브가 부족합니다.";
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
                res.message = "쥬얼러의 오브 차감에 실패했습니다.";
                return;
            }
            SaveLoad::SetState(working);
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
            res = Core::PassiveTree::RefundNode(*g_treeData, working.tree, nodeId, working.exileLevel, *g_config, availOrbs);
            if (!res.ok) {
                return;
            }
            if (res.regretOrbsConsumed > 0 &&
                !ConsumeCurrency(player, Core::CurrencyKind::Regret, static_cast<std::int32_t>(res.regretOrbsConsumed))) {
                res = { false, "후회의 오브 차감에 실패해 회수를 취소했습니다.", 0, 0 };
                return;
            }
            const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                *g_treeData, working.tree, working.exileLevel, *g_config);
            working.unallocatedPassivePoints = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
            SaveLoad::SetState(working);
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
            res = Core::PassiveTree::RefundAll(*g_treeData, working.tree, ascendancyOnly, working.exileLevel, *g_config, availOrbs);
            if (!res.ok) {
                return;
            }
            if (res.regretOrbsConsumed > 0 &&
                !ConsumeCurrency(player, Core::CurrencyKind::Regret, static_cast<std::int32_t>(res.regretOrbsConsumed))) {
                res = { false, "후회의 오브 차감에 실패해 회수를 취소했습니다.", 0, 0 };
                return;
            }
            const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                *g_treeData, working.tree, working.exileLevel, *g_config);
            working.unallocatedPassivePoints = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
            SaveLoad::SetState(working);
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
                *g_treeData, working.tree, masteryNodeId, effectId, working.exileLevel, *g_config, availOrbs);
            if (!res.ok) {
                return;
            }
            if (res.regretOrbsConsumed > 0 &&
                !ConsumeCurrency(player, Core::CurrencyKind::Regret, static_cast<std::int32_t>(res.regretOrbsConsumed))) {
                res = { false, "후회의 오브 차감에 실패해 마스터리 교체를 취소했습니다.", 0, 0 };
                return;
            }
            const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                *g_treeData, working.tree, working.exileLevel, *g_config);
            working.unallocatedPassivePoints = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
            SaveLoad::SetState(working);
        });

        if (res.ok) {
            SaveLoad::SyncStateToStatEngine();
        }
        return res;
    }
}
