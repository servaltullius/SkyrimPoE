#include "PCH.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/ItemTracker.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/SaveLoad.h"

#include "core/ItemAffixParser.h"
#include "core/ItemCrafting.h"

#include <atomic>
#include <mutex>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        const Core::ItemData* g_itemData = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;
        std::atomic_bool g_dirty{ true };
        std::mutex g_gearMutex;
        std::array<Core::EquippedSlotInput, Core::kEquipSlotCount> g_gear{};
        std::array<EquippedSlotMeta, Core::kEquipSlotCount> g_meta{};
    }

    void EquipmentSync::Initialize(const Core::ItemData* itemData, Core::CharmSystem* charmSystem)
    {
        g_itemData = itemData;
        g_charmSystem = charmSystem;
        g_dirty.store(true, std::memory_order_release);
    }

    std::optional<Core::EquipSlot> EquipmentSync::ClassifyArmorEquipSlot(const RE::TESObjectARMO* armor) noexcept
    {
        if (!armor) {
            return std::nullopt;
        }
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        const auto mask = armor->GetSlotMask();
        auto has = [&](Slot s) noexcept { return (mask & s) != Slot::kNone; };

        // 결정 156, 검수 M3: 다중 바디슬롯 장비는 최고 우선순위 단일 논리 칸 1곳에만 귀속,
        // 투구는 kLongHair(슬롯 41) 포함, 망토·배낭·장신구 등 추가 바디슬롯은 외형 전용으로 제외.
        if (has(Slot::kBody)) return Core::EquipSlot::Body;
        if (has(Slot::kHead) || has(Slot::kHair) || has(Slot::kCirclet) || has(Slot::kLongHair)) {
            return Core::EquipSlot::Helmet;
        }
        if (has(Slot::kHands)) return Core::EquipSlot::Gloves;
        if (has(Slot::kFeet)) return Core::EquipSlot::Boots;
        if (has(Slot::kShield)) return Core::EquipSlot::OffHand;
        if (has(Slot::kAmulet)) return Core::EquipSlot::Amulet;
        if (has(Slot::kRing)) return Core::EquipSlot::Ring1;
        return std::nullopt;
    }

    Core::TokenSlotGroup EquipmentSync::EquipSlotToTokenGroup(Core::EquipSlot slot, bool isTwoHandedWeapon) noexcept
    {
        switch (slot) {
            case Core::EquipSlot::MainHand:
                return isTwoHandedWeapon ? Core::TokenSlotGroup::TwoHandOrRangedWeapon : Core::TokenSlotGroup::OneHandWeapon;
            case Core::EquipSlot::OffHand:
                return Core::TokenSlotGroup::Shield;
            case Core::EquipSlot::Helmet: return Core::TokenSlotGroup::Helmet;
            case Core::EquipSlot::Body:   return Core::TokenSlotGroup::Body;
            case Core::EquipSlot::Gloves: return Core::TokenSlotGroup::Gloves;
            case Core::EquipSlot::Boots:  return Core::TokenSlotGroup::Boots;
            case Core::EquipSlot::Amulet:
            case Core::EquipSlot::Ring1:
            case Core::EquipSlot::Ring2:
                return Core::TokenSlotGroup::Jewelry;
            case Core::EquipSlot::Belt:
                return Core::TokenSlotGroup::Belt;
            default:
                return Core::TokenSlotGroup::OneHandWeapon;
        }
    }

    Core::DefenseSubtype EquipmentSync::ClassifyDefenseSubtype(const RE::TESObjectARMO* armor) noexcept
    {
        if (!armor) {
            return Core::DefenseSubtype::None;
        }
        const auto slot = ClassifyArmorEquipSlot(armor);
        if (slot == Core::EquipSlot::Amulet || slot == Core::EquipSlot::Ring1) {
            return Core::DefenseSubtype::None;
        }
        switch (armor->GetArmorType()) {
            case RE::BGSBipedObjectForm::ArmorType::kHeavyArmor:
                return Core::DefenseSubtype::Armour;
            case RE::BGSBipedObjectForm::ArmorType::kLightArmor:
                return Core::DefenseSubtype::Evasion;
            default:
                return Core::DefenseSubtype::EnergyShield;
        }
    }

    std::string EquipmentSync::ClassifyWeaponTag(const RE::TESObjectWEAP* weapon)
    {
        if (!weapon) {
            return "OneHandSword";
        }
        switch (weapon->GetWeaponType()) {
            case RE::WEAPON_TYPE::kOneHandSword:  return "OneHandSword";
            case RE::WEAPON_TYPE::kOneHandDagger: return "Dagger";
            case RE::WEAPON_TYPE::kOneHandAxe:    return "OneHandAxe";
            case RE::WEAPON_TYPE::kOneHandMace:   return "OneHandMace";
            case RE::WEAPON_TYPE::kTwoHandSword:  return "TwoHandSword";
            case RE::WEAPON_TYPE::kTwoHandAxe:    return "TwoHandAxe";
            case RE::WEAPON_TYPE::kBow:           return "Bow";
            case RE::WEAPON_TYPE::kCrossbow:      return "Crossbow";
            case RE::WEAPON_TYPE::kStaff:         return "Staff";
            default:                              return "OneHandSword";
        }
    }

    bool EquipmentSync::IsTwoHandedWeapon(const RE::TESObjectWEAP* weapon) noexcept
    {
        if (!weapon) {
            return false;
        }
        switch (weapon->GetWeaponType()) {
            case RE::WEAPON_TYPE::kTwoHandSword:
            case RE::WEAPON_TYPE::kTwoHandAxe:
            case RE::WEAPON_TYPE::kBow:
            case RE::WEAPON_TYPE::kCrossbow:
                return true;
            default:
                return false;
        }
    }

    Core::EquippedSlotInput EquipmentSync::BuildSlotInput(
        RE::TESBoundObject* baseObj,
        const Core::PoEItemRecord* poeRecord,
        std::uint32_t exileLevel,
        const Core::ItemData& itemData)
    {
        Core::EquippedSlotInput out;
        if (!baseObj) {
            return out;
        }

        out.occupied = true;
        out.baseFormId = baseObj->GetFormID();
        out.isCrafted = (poeRecord != nullptr);
        out.rarity = poeRecord ? poeRecord->rarity : Core::ItemRarity::Normal;
        out.itemLevel = poeRecord ? poeRecord->itemLevel : std::clamp<std::uint32_t>(exileLevel, 1, Core::kMaxExileLevel);

        // C-17: 고유 아이템은 감정 시점 itemLevel이 아닌 고유 정의의 reqLevel 기준으로 요구치를 산정한다.
        const Core::UniqueDefinition* uniqueDef = (poeRecord && !poeRecord->uniqueId.empty())
            ? itemData.FindUnique(poeRecord->uniqueId)
            : nullptr;
        const std::uint32_t reqLookupLevel = uniqueDef ? std::max<std::uint32_t>(1, uniqueDef->reqLevel) : out.itemLevel;

        if (auto* weap = baseObj->As<RE::TESObjectWEAP>()) {
            out.isWeapon = true;
            out.isTwoHanded = IsTwoHandedWeapon(weap);
            const std::string wTag = (poeRecord && !poeRecord->weaponTag.empty())
                ? poeRecord->weaponTag
                : ClassifyWeaponTag(weap);
            const auto curve = itemData.LookupWeaponBase(wTag, reqLookupLevel);
            out.baseWeaponPhysMin = curve.physMin;
            out.baseWeaponPhysMax = curve.physMax;
            out.baseWeaponCritChancePct = curve.critChance;
            out.baseWeaponAttackRate = curve.attackRate;
            if (out.isCrafted) {
                out.reqLevel = uniqueDef ? uniqueDef->reqLevel : curve.reqLevel;
                out.reqStr = curve.reqStr;
                out.reqDex = curve.reqDex;
                out.reqInt = curve.reqInt;
            }
        } else if (auto* armor = baseObj->As<RE::TESObjectARMO>()) {
            out.isWeapon = false;
            const auto equipSlot = ClassifyArmorEquipSlot(armor);
            const auto sub = ClassifyDefenseSubtype(armor);
            if (equipSlot.has_value() && *equipSlot != Core::EquipSlot::Amulet && *equipSlot != Core::EquipSlot::Ring1) {
                const auto tokGroup = EquipSlotToTokenGroup(*equipSlot, false);
                const auto curve = itemData.LookupArmorBase(tokGroup, sub, reqLookupLevel);
                out.baseArmour = curve.baseArmour;
                out.baseEvasion = curve.baseEvasion;
                out.baseEnergyShield = curve.baseEnergyShield;
                out.baseBlockChance = curve.baseBlockChance;
                if (out.isCrafted) {
                    out.reqLevel = uniqueDef ? uniqueDef->reqLevel : curve.reqLevel;
                    out.reqStr = curve.reqStr;
                    out.reqDex = curve.reqDex;
                    out.reqInt = curve.reqInt;
                }
            } else if (uniqueDef && out.isCrafted) {
                out.reqLevel = uniqueDef->reqLevel;
            }
        } else if (auto* misc = baseObj->As<RE::TESObjectMISC>()) {
            out.isWeapon = false;
            const auto localId = FormIds::ResolveLocalFormIdIfOurs(misc->GetFormID());
            if (FormIds::IsBeltBaseLocalFormId(localId)) {
                if (const auto* beltDef = itemData.FindBeltByLocalFormId(localId)) {
                    if (out.isCrafted) {
                        out.reqLevel = uniqueDef ? uniqueDef->reqLevel : beltDef->reqLevel;
                    }
                }
            }
        }

        if (uniqueDef && out.isCrafted) {
            out.reqLevel = uniqueDef->reqLevel;
        }

        if (poeRecord && (!poeRecord->affixes.empty() || !poeRecord->implicitAffixes.empty() || !poeRecord->uniqueId.empty())) {
            out.stats = Core::ItemAffixParser::ParseItem(*poeRecord, itemData);
        }
        return out;
    }

    void EquipmentSync::MarkDirty() noexcept
    {
        g_dirty.store(true, std::memory_order_release);
    }

    void EquipmentSync::FlushIfDirty()
    {
        if (g_dirty.exchange(false, std::memory_order_acq_rel)) {
            RefreshFromPlayerNow();
        }
    }

    void EquipmentSync::RefreshFromPlayerNow()
    {
        g_dirty.store(false, std::memory_order_release);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_itemData || !g_itemData->IsLoaded()) {
            return;
        }

        const auto st = SaveLoad::GetStateCopy();
        const std::uint32_t exileLevel = std::clamp<std::uint32_t>(st.exileLevel, 1, Core::kMaxExileLevel);
        const RE::FormID playerId = player->GetFormID();

        std::array<Core::EquippedSlotInput, Core::kEquipSlotCount> nextGear{};
        std::array<EquippedSlotMeta, Core::kEquipSlotCount> nextMeta{};
        bool mainHandTwoHanded = false;
        bool hasShieldInOffHand = false;

        auto fillSlot = [&](Core::EquipSlot slot, RE::TESBoundObject* obj, RE::ExtraDataList* extra, bool isWeapon) {
            const auto idx = static_cast<std::size_t>(slot);
            if (nextMeta[idx].occupied) {
                return;  // 결정 156: 동일 논리 칸은 최초 일치 1개만 귀속
            }

            Core::InventoryItemKey key{};
            std::optional<Core::PoEItemRecord> poeRec;
            if (extra) {
                if (const auto* unique = extra->GetByType<RE::ExtraUniqueID>()) {
                    if (unique->baseID == playerId && unique->uniqueID != 0) {
                        key = Core::InventoryItemKey{ unique->baseID, unique->uniqueID };
                        poeRec = ItemTracker::FindByKey(key);
                    }
                }
            }

            const Core::PoEItemRecord* recPtr = poeRec ? &(*poeRec) : nullptr;
            nextGear[idx] = BuildSlotInput(obj, recPtr, exileLevel, *g_itemData);

            auto& m = nextMeta[idx];
            m.occupied = true;
            m.baseFormId = obj->GetFormID();
            m.key = key;
            m.displayName = obj->GetName() ? obj->GetName() : "";
            m.hasPoERecord = (recPtr != nullptr);
            if (recPtr) {
                m.record = *recPtr;
            }
            if (isWeapon) {
                auto* weap = obj->As<RE::TESObjectWEAP>();
                const bool twoH = IsTwoHandedWeapon(weap);
                m.slotGroup = twoH ? Core::TokenSlotGroup::TwoHandOrRangedWeapon : Core::TokenSlotGroup::OneHandWeapon;
                m.weaponTag = ClassifyWeaponTag(weap);
            } else if (auto* armor = obj->As<RE::TESObjectARMO>()) {
                m.slotGroup = EquipSlotToTokenGroup(slot, false);
                m.defenseSubtype = ClassifyDefenseSubtype(armor);
                m.isAmulet = (slot == Core::EquipSlot::Amulet);
            }
        };

        // 검수 M3: GetInventoryChanges(true)에서 ExtraWorn / ExtraWornLeft를 직접 스캔해
        // 동일 베이스 폼 2개 보유 시에도 정확한 장착 개체(ExtraUniqueID)를 식별한다.
        if (auto* changes = player->GetInventoryChanges(true); changes && changes->entryList) {
            // 1패스: 주무기(ExtraWorn WEAP) 및 방어구·방패(ExtraWorn ARMO)
            for (auto* entry : *changes->entryList) {
                if (!entry || !entry->object || !entry->extraLists) {
                    continue;
                }
                auto* weap = entry->object->As<RE::TESObjectWEAP>();
                auto* armor = entry->object->As<RE::TESObjectARMO>();
                if (!weap && !armor) {
                    continue;
                }
                for (auto* extra : *entry->extraLists) {
                    if (!extra) continue;
                    if (weap) {
                        if (extra->HasType<RE::ExtraWorn>()) {
                            fillSlot(Core::EquipSlot::MainHand, weap, extra, true);
                        }
                    } else if (armor) {
                        if (extra->HasType<RE::ExtraWorn>()) {
                            const auto slotOpt = ClassifyArmorEquipSlot(armor);
                            if (slotOpt.has_value()) {
                                fillSlot(*slotOpt, armor, extra, false);
                                if (*slotOpt == Core::EquipSlot::OffHand) {
                                    hasShieldInOffHand = true;
                                }
                            }
                        }
                    }
                }
            }

            // 검수 m5: 실제로 MainHand에 귀속된 장비의 isTwoHanded로 판정
            mainHandTwoHanded = nextGear[static_cast<std::size_t>(Core::EquipSlot::MainHand)].isTwoHanded;

            // 2패스: 왼손 무기(ExtraWornLeft WEAP) — 양손 무기/방패 미착용 시에만 OffHand에 귀속
            if (!mainHandTwoHanded && !hasShieldInOffHand) {
                for (auto* entry : *changes->entryList) {
                    if (!entry || !entry->object || !entry->extraLists) {
                        continue;
                    }
                    auto* weap = entry->object->As<RE::TESObjectWEAP>();
                    if (!weap || IsTwoHandedWeapon(weap)) {
                        continue;
                    }
                    for (auto* extra : *entry->extraLists) {
                        if (extra && extra->HasType<RE::ExtraWornLeft>()) {
                            fillSlot(Core::EquipSlot::OffHand, weap, extra, true);
                            break;
                        }
                    }
                }
            }
        }

        // 양손 무기 착용 시 OffHand 강제 비움(검수 M3)
        if (mainHandTwoHanded) {
            const auto offIdx = static_cast<std::size_t>(Core::EquipSlot::OffHand);
            nextGear[offIdx] = Core::EquippedSlotInput{};
            nextMeta[offIdx] = EquippedSlotMeta{};
        }

        // 5단계(결정 162): 가상 반지2(Ring2) — 플레이어 가방에 존재하고 Ring1과 동일 개체가 아닐 때만 귀속
        bool clearVirtualRing2 = false;
        if (st.virtualRing2BaseFormId != 0 && st.virtualRing2Key.IsValid()) {
            auto* ringForm = RE::TESForm::LookupByID<RE::TESObjectARMO>(st.virtualRing2BaseFormId);
            const auto ring1Key = nextMeta[static_cast<std::size_t>(Core::EquipSlot::Ring1)].key;
            auto* extra = ringForm
                ? ItemTracker::FindExtraListByKey(player, ringForm, st.virtualRing2Key)
                : nullptr;
            const bool isRingArmor = ringForm && ClassifyArmorEquipSlot(ringForm) == Core::EquipSlot::Ring1;
            const bool sameAsRing1 = ring1Key.IsValid() && ring1Key == st.virtualRing2Key;
            const bool physicallyWorn = extra && (extra->HasType<RE::ExtraWorn>() || extra->HasType<RE::ExtraWornLeft>());
            if (isRingArmor && extra && !sameAsRing1 && !physicallyWorn) {
                fillSlot(Core::EquipSlot::Ring2, ringForm, extra, false);
            } else {
                clearVirtualRing2 = true;
            }
        } else if (st.virtualRing2BaseFormId != 0 || st.virtualRing2Key.IsValid()) {
            clearVirtualRing2 = true;
        }

        // 5단계(결정 162): 가상 벨트(Belt) — 플레이어 가방에 존재하는 SkyrimPoE.esp 벨트 베이스만 귀속
        bool clearVirtualBelt = false;
        bool isStygianBelt = false;
        if (st.virtualBeltBaseFormId != 0) {
            auto* beltMisc = RE::TESForm::LookupByID<RE::TESObjectMISC>(st.virtualBeltBaseFormId);
            const auto beltLocalId = beltMisc ? FormIds::ResolveLocalFormIdIfOurs(beltMisc->GetFormID()) : 0;
            if (beltMisc && FormIds::IsBeltBaseLocalFormId(beltLocalId)) {
                RE::ExtraDataList* beltExtra = nullptr;
                bool presentInBag = false;
                if (st.virtualBeltKey.IsValid()) {
                    beltExtra = ItemTracker::FindExtraListByKey(player, beltMisc, st.virtualBeltKey);
                    presentInBag = (beltExtra != nullptr);
                } else {
                    const auto counts = player->GetInventoryCounts();
                    if (auto it = counts.find(beltMisc); it != counts.end() && it->second > 0) {
                        presentInBag = true;
                    }
                }
                if (presentInBag) {
                    const auto beltIdx = static_cast<std::size_t>(Core::EquipSlot::Belt);
                    std::optional<Core::PoEItemRecord> poeRec;
                    if (st.virtualBeltKey.IsValid()) {
                        poeRec = ItemTracker::FindByKey(st.virtualBeltKey);
                    }
                    const Core::PoEItemRecord* recPtr = poeRec ? &(*poeRec) : nullptr;
                    nextGear[beltIdx] = BuildSlotInput(beltMisc, recPtr, exileLevel, *g_itemData);
                    auto& m = nextMeta[beltIdx];
                    m.occupied = true;
                    m.baseFormId = beltMisc->GetFormID();
                    m.key = st.virtualBeltKey;
                    m.displayName = beltMisc->GetName() ? beltMisc->GetName() : "";
                    m.slotGroup = Core::TokenSlotGroup::Belt;
                    m.defenseSubtype = Core::DefenseSubtype::None;
                    m.hasPoERecord = (recPtr != nullptr);
                    if (recPtr) {
                        m.record = *recPtr;
                    }
                    if (const auto* beltDef = g_itemData->FindBeltByLocalFormId(beltLocalId)) {
                        isStygianBelt = beltDef->bonusCharmSlot;
                    }
                } else {
                    clearVirtualBelt = true;
                }
            } else {
                clearVirtualBelt = true;
            }
        } else if (st.virtualBeltKey.IsValid()) {
            clearVirtualBelt = true;
        }

        if (clearVirtualRing2 || clearVirtualBelt) {
            SaveLoad::UpdateState([&](PersistentState& s) {
                if (clearVirtualRing2) {
                    s.virtualRing2Key = {};
                    s.virtualRing2BaseFormId = 0;
                }
                if (clearVirtualBelt) {
                    s.virtualBeltKey = {};
                    s.virtualBeltBaseFormId = 0;
                }
            });
        }

        const bool applyGearEffects = !MapDeviceRuntime::IsUninstallCleanupActive();
        // 5단계(결정 162): 장착 벨트에 따라 개방 참 슬롯 수(0~3)를 CharmSystem에 동기화
        if (applyGearEffects && g_charmSystem) {
            const auto& beltMeta = nextMeta[static_cast<std::size_t>(Core::EquipSlot::Belt)];
            const Core::PoEItemRecord* beltRecPtr = beltMeta.hasPoERecord ? &beltMeta.record : nullptr;
            const std::uint8_t unlocked = Core::ItemCrafting::ComputeBeltCharmSlots(
                beltMeta.occupied, beltRecPtr, isStygianBelt);
            g_charmSystem->SetUnlockedSlots(unlocked);
        }

        {
            std::scoped_lock lock(g_gearMutex);
            g_gear = nextGear;
            g_meta = nextMeta;
        }

        // 검수 C5: 단일 조합 지점(SaveLoad::SyncStateToStatEngine)으로 전달
        if (applyGearEffects) {
            SaveLoad::SyncStateToStatEngine();
        }
    }

    std::array<Core::EquippedSlotInput, Core::kEquipSlotCount> EquipmentSync::GetCurrentGearSnapshot()
    {
        std::scoped_lock lock(g_gearMutex);
        return g_gear;
    }

    std::array<EquippedSlotMeta, Core::kEquipSlotCount> EquipmentSync::GetCurrentGearMeta()
    {
        std::scoped_lock lock(g_gearMutex);
        return g_meta;
    }

    void EquipmentSync::Clear()
    {
        {
            std::scoped_lock lock(g_gearMutex);
            g_gear = {};
            g_meta = {};
        }
        g_dirty.store(true, std::memory_order_release);
    }
}
