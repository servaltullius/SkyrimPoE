#include "test_harness.h"

#include "core/CharmSystem.h"
#include "core/Config.h"
#include "core/ContentFeatures.h"
#include "core/DataPack.h"
#include "core/ItemData.h"
#include "core/ItemFormatter.h"
#include "core/StatEngine.h"

#include <filesystem>
#include <string>

using namespace SkyrimPoE::Core;

namespace
{
    // 시험이 바꾼 설치 세대를 끝나면 세대 0으로 되돌린다(다른 시험은 세대 0을 가정).
    struct ScopedGeneration
    {
        explicit ScopedGeneration(std::uint32_t generation) { SetActiveContentGeneration(generation); }
        ~ScopedGeneration() { SetActiveContentGeneration(0); }
    };

    constexpr auto kEmptyItems =
        R"({"meta":{"schema":"skyrimpoe.items","schemaVersion":3,"game":"poe2","gameVersion":"0.5","contentGeneration":1}})";
}

POE_TEST("ContentFeatures: generation 0 enables every legacy system and generation 1 disables them (G1)")
{
    const auto legacy = ContentFeaturesFor(0);
    POE_CHECK(legacy.itemDrops && legacy.crafting && legacy.equipmentStats && legacy.mapEntry && legacy.charms &&
              legacy.monsterPromotion && legacy.jewelSocketing && legacy.starterCurrency);
    const auto poe2 = ContentFeaturesFor(1);
    POE_CHECK(!poe2.itemDrops && !poe2.crafting && !poe2.equipmentStats && !poe2.mapEntry && !poe2.charms &&
              !poe2.monsterPromotion && !poe2.jewelSocketing && !poe2.starterCurrency);
    POE_CHECK(ActiveContentGeneration() == 0);
    {
        const ScopedGeneration generation(1);
        POE_CHECK(ActiveContentGeneration() == 1 && !ActiveContentFeatures().crafting);
    }
    POE_CHECK(ActiveContentFeatures().crafting);
}

POE_TEST("ItemData: the generation-1 empty items pack loads without item content and rejects extra content")
{
    ItemData items;
    std::string error;
    POE_CHECK(items.LoadFromJsonString(kEmptyItems, &error));
    POE_CHECK(items.IsLoaded() && !items.HasItemContent() && items.Generation() == 1);
    POE_CHECK(items.AllAffixes().empty() && items.AllCurrencies().empty() && items.AllUniques().empty());

    for (const auto* invalid : {
             R"({"meta":{"schema":"skyrimpoe.items","schemaVersion":3,"game":"poe2","contentGeneration":1},"affixes":[]})",
             R"({"meta":{"schema":"skyrimpoe.items","schemaVersion":2,"game":"poe2","contentGeneration":1}})",
             R"({"meta":{"schema":"skyrimpoe.items","schemaVersion":3.0,"game":"poe2","contentGeneration":1}})",
             R"({"meta":{"schema":"skyrimpoe.items","schemaVersion":3,"game":"poe1","contentGeneration":1}})",
         }) {
        ItemData other;
        error.clear();
        POE_CHECK(!other.LoadFromJsonString(invalid, &error));
        POE_CHECK(error.find("세대 1 items.json") != std::string::npos);
        POE_CHECK(!other.IsLoaded());
    }

    // 세대 0 팩은 그대로 아이템 내용이 있다.
    ItemData legacy;
    POE_CHECK(legacy.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "gen0" / "items.json", &error));
    POE_CHECK(legacy.IsLoaded() && legacy.HasItemContent() && legacy.Generation() == 0);
}

POE_TEST("CharmSystem: generation 1 neither triggers, recharges nor grants bonuses from charms (decision 248)")
{
    CharmSystem charms;
    std::array<CharmSlotState, kMaxCharmSlots> slots{};
    slots[0] = CharmSlotState{ true, CharmKind::Ruby, 20.0 };
    slots[1] = CharmSlotState{ true, CharmKind::Bastion, 40.0 };
    charms.SetSlots(slots, 2);
    POE_CHECK(charms.TryTriggerOnIncomingDamage(DamageType::Fire, true));  // 세대 0: 켜진 루비
    {
        const ScopedGeneration generation(1);
        POE_CHECK(!charms.TryTriggerOnLowLife(0.1, true));
        POE_CHECK(!charms.TryTriggerOnAilment(AilmentKind::Freeze, true));
        charms.OnKillRecharge(MonsterRarity::Rare, true, false);
        POE_CHECK_NEAR(charms.Slots()[0].charges, 0.0, 1e-9);  // 발동으로 쓴 20이 충전되지 않음
        POE_CHECK_NEAR(charms.Slots()[1].charges, 40.0, 1e-9);
        const auto bonuses = charms.ComputeActiveBonuses();  // 이미 켜진 루비도 효과 없음
        POE_CHECK_NEAR(bonuses.fireResist, 0.0, 1e-9);
        POE_CHECK_NEAR(bonuses.maxFireResist, 0.0, 1e-9);
    }
    POE_CHECK_NEAR(charms.ComputeActiveBonuses().fireResist, 25.0, 1e-9);
}

POE_TEST("StatEngine: generation 1 drops the PoE1 dual-wield block and speed bonuses but keeps the dual-wield fact")
{
    DataPack pack;
    POE_CHECK(pack.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "gen0" / "core.json"));
    const Config config;
    Layer1Input l1;
    l1.exileLevel = 10;
    l1.className = "Scion";
    for (const auto slot : { EquipSlot::MainHand, EquipSlot::OffHand }) {
        auto& weapon = l1.equippedGear[static_cast<std::size_t>(slot)];
        weapon.occupied = true;
        weapon.isWeapon = true;
    }

    const auto blockFor = [&](std::uint32_t generation) {
        const ScopedGeneration scoped(generation);
        StatEngine engine;
        engine.SetLayer1(l1);
        POE_CHECK(engine.RebuildIfDirty(pack, config));
        return engine.GetSnapshot()->attackBlockChance;
    };
    POE_CHECK_NEAR(blockFor(0), 20.0, 1e-9);
    POE_CHECK_NEAR(blockFor(1), 0.0, 1e-9);
}

POE_TEST("StatEngine: a generation-1 weapon attack uses the unarmed base until weapon data exists (decision 244)")
{
    DataPack pack;
    POE_CHECK(pack.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "gen0" / "core.json"));
    const Config config;
    Layer1Input l1;
    l1.exileLevel = 10;
    l1.className = "Scion";
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 1;
    l1.slots[0].gem.gemId = "Cyclone";
    l1.slots[0].gem.level = 1;
    const auto physicalFor = [&](bool withWeapon) {
        auto input = l1;
        auto& weapon = input.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)];
        weapon.occupied = withWeapon;
        weapon.isWeapon = withWeapon;  // 세대 1 장비 동기화처럼 수치는 0, 장착 사실만 있음
        StatEngine engine;
        engine.SetLayer1(input);
        POE_CHECK(engine.RebuildIfDirty(pack, config));
        return engine.GetSnapshot()->slotStats[0].baseHitDamage.byType[static_cast<std::size_t>(DamageType::Physical)].Average();
    };
    const ScopedGeneration generation(1);
    const auto unarmed = physicalFor(false);
    POE_CHECK(unarmed > 0.0);
    POE_CHECK_NEAR(physicalFor(true), unarmed, 1e-9);
}

POE_TEST("CharmSystem: gate stats count trigger entries, trigger rejections, and recharge rejections (decision 259)")
{
    CharmSystem charms;
    std::array<CharmSlotState, kMaxCharmSlots> slots{};
    slots[0] = CharmSlotState{ true, CharmKind::Ruby, 40.0 };
    slots[1] = CharmSlotState{ true, CharmKind::Purifying, 40.0 };
    slots[2] = CharmSlotState{ true, CharmKind::Bastion, 40.0 };
    charms.SetSlots(slots, 3);

    const auto s0 = charms.GetGateStatsForTest();
    POE_CHECK(s0.triggerEntries == 0 && s0.triggerGateRejections == 0 && s0.rechargeGateRejections == 0);

    // 세대 0: 입구 카운터 증가, 게이트 거절 0.
    POE_CHECK(charms.TryTriggerOnIncomingDamage(DamageType::Fire, true));
    charms.OnKillRecharge(MonsterRarity::Normal, false, false);
    const auto s1 = charms.GetGateStatsForTest();
    POE_CHECK(s1.triggerEntries == 1 && s1.triggerGateRejections == 0 && s1.rechargeGateRejections == 0);

    // 세대 1: 세 TryTriggerOn* 입구와 게이트 거절, 처치 충전 거절 모두 증가.
    {
        const ScopedGeneration generation(1);
        POE_CHECK(!charms.TryTriggerOnIncomingDamage(DamageType::Fire, true));
        POE_CHECK(!charms.TryTriggerOnAilment(AilmentKind::Freeze, true));
        POE_CHECK(!charms.TryTriggerOnLowLife(0.2, true));
        charms.OnKillRecharge(MonsterRarity::Rare, true, false);
        const auto s2 = charms.GetGateStatsForTest();
        POE_CHECK(s2.triggerEntries == 4 && s2.triggerGateRejections == 3 && s2.rechargeGateRejections == 1);
    }

    // 열린 칸이 0이면 입구 카운터도 늘지 않는다.
    charms.SetUnlockedSlots(0);
    POE_CHECK(!charms.TryTriggerOnLowLife(0.1, true));
    POE_CHECK(charms.GetGateStatsForTest().triggerEntries == 4);
}

POE_TEST("EquipmentSync/StatEngine/ItemFormatter: generation-1 zeroed weapon keeps 0.00 attack rate and no dual-wield speed bonus")
{
    DataPack pack;
    POE_CHECK(pack.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "gen0" / "core.json"));
    ItemData emptyItems;
    std::string err;
    POE_CHECK(emptyItems.LoadFromJsonString(kEmptyItems, &err));
    const Config config;

    Layer1Input base;
    base.exileLevel = 10;
    base.className = "Scion";
    base.slots[0].occupied = true;
    base.slots[0].gem.instanceId = 1;
    base.slots[0].gem.gemId = "Cyclone";
    base.slots[0].gem.level = 1;

    // 세대 1 장비 동기화(EquipmentSync::BuildSlotInput)가 만드는 0화 무기 입력.
    EquippedSlotInput gen1Weapon;
    gen1Weapon.occupied = true;
    gen1Weapon.isWeapon = true;
    gen1Weapon.baseWeaponPhysMin = 0.0;
    gen1Weapon.baseWeaponPhysMax = 0.0;
    gen1Weapon.baseWeaponCritChancePct = 0.0;
    gen1Weapon.baseWeaponAttackRate = 0.0;

    // ItemFormatter: baseWeaponAttackRate=0일 때 0.1로 올리지 않고 0으로 표시(FormatNumber는 소수 끝 0 생략).
    const auto card = ItemFormatter::FormatItem(nullptr, "Iron Sword", EquipSlot::MainHand, gen1Weapon, true, emptyItems);
    bool sawZeroAps = false;
    bool sawClampedAps = false;
    for (const auto& line : card.baseStatLines) {
        if (line.ends_with("초당 공격: 0")) sawZeroAps = true;
        if (line.find("초당 공격: 0.1") != std::string::npos) sawClampedAps = true;
    }
    POE_CHECK(sawZeroAps && !sawClampedAps);

    // 세대 1에서는 쌍수·한손·맨손 모두 공격 속도 배율과 기본 피해가 같다(쌍수 ×1.10 없음).
    const ScopedGeneration generation(1);
    const auto runSnap = [&](bool mainW, bool offW) {
        auto input = base;
        if (mainW) input.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)] = gen1Weapon;
        if (offW) input.equippedGear[static_cast<std::size_t>(EquipSlot::OffHand)] = gen1Weapon;
        StatEngine engine;
        engine.SetLayer1(input);
        POE_CHECK(engine.RebuildIfDirty(pack, config));
        return engine.GetSnapshot();
    };
    const auto unarmed = runSnap(false, false);
    const auto oneHand = runSnap(true, false);
    const auto dual = runSnap(true, true);
    POE_CHECK_NEAR(oneHand->slotStats[0].attackSpeedMult, unarmed->slotStats[0].attackSpeedMult, 1e-9);
    POE_CHECK_NEAR(dual->slotStats[0].attackSpeedMult, unarmed->slotStats[0].attackSpeedMult, 1e-9);
    POE_CHECK_NEAR(dual->attackBlockChance, 0.0, 1e-9);
}
