#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace SkyrimPoE::Core
{
    // 결정 156, 162: PoE 어픽스가 적용되는 논리 10칸(8 물리 + 가상 반지2·벨트)
    enum class EquipSlot : std::uint8_t
    {
        MainHand = 0,
        OffHand = 1,
        Helmet = 2,
        Body = 3,
        Gloves = 4,
        Boots = 5,
        Amulet = 6,
        Ring1 = 7,
        Ring2 = 8,
        Belt = 9,
        Count = 10
    };

    inline constexpr std::size_t kEquipSlotCount = static_cast<std::size_t>(EquipSlot::Count);

    enum class ItemRarity : std::uint8_t
    {
        Normal = 0,
        Magic = 1,
        Rare = 2,
        Unique = 3
    };

    // 결정 158: 핵심 화폐 오브 12종
    enum class CurrencyKind : std::uint8_t
    {
        ScrollOfWisdom = 0,
        Transmutation = 1,
        Augmentation = 2,
        Alteration = 3,
        Regal = 4,
        Alchemy = 5,
        Chaos = 6,
        Exalted = 7,
        Scouring = 8,
        Divine = 9,
        Regret = 10,
        Jeweller = 11,
        Count = 12
    };

    inline constexpr std::size_t kCurrencyKindCount = static_cast<std::size_t>(CurrencyKind::Count);

    // 결정 157, 162, 164, 166: 5티어 무상태 봉인 토큰 부위군(0..7 4단계 + 8 벨트 + 9 주얼 + 10 지도)
    enum class TokenSlotGroup : std::uint8_t
    {
        OneHandWeapon = 0,
        TwoHandOrRangedWeapon = 1,
        Shield = 2,
        Helmet = 3,
        Body = 4,
        Gloves = 5,
        Boots = 6,
        Jewelry = 7,
        Belt = 8,
        Jewel = 9,
        Map = 10,
        Count = 11
    };

    inline constexpr std::size_t kStandardTokenSlotGroupCount = 8;  // 기존 80폼 0x82B..0x87A 공식 불변 유지
    inline constexpr std::size_t kDroppableTokenSlotGroupCount = 9; // 봉인 토큰 드랍 대상(0..8, Belt 포함)
    inline constexpr std::size_t kTokenSlotGroupCount = static_cast<std::size_t>(TokenSlotGroup::Count);
    inline constexpr std::uint8_t kItemTierCount = 5;

    // 방어구 종류 매핑(통합 설계서 8.1: 중갑=방어도, 경갑=회피, 의복/로브=에너지 실드)
    enum class DefenseSubtype : std::uint8_t
    {
        None = 0,
        Armour = 1,
        Evasion = 2,
        EnergyShield = 3
    };

    enum class AffixType : std::uint8_t
    {
        Prefix = 0,
        Suffix = 1
    };

    struct RolledAffix
    {
        std::string affixId;
        std::vector<double> values;  // lines의 모든 ranges를 순서대로 평탄화한 값(검수 C1: double 정밀도 유지)

        [[nodiscard]] bool operator==(const RolledAffix&) const noexcept = default;
    };

    // 감정 또는 오브 제작을 거친 개별 PoE 아이템 데이터(코세이브 'ITEM' v1/v2 직렬화 단위)
    struct PoEItemRecord
    {
        std::uint32_t baseFormId = 0;
        ItemRarity rarity = ItemRarity::Normal;
        std::uint32_t itemLevel = 1;
        TokenSlotGroup slotGroup = TokenSlotGroup::OneHandWeapon;
        DefenseSubtype defenseSubtype = DefenseSubtype::None;
        bool isAmulet = false;  // Jewelry일 때 목걸이(true) vs 반지(false) 구분(검수 m6)
        std::string weaponTag;  // "OneHandSword", "TwoHandAxe", "Bow", "Crossbow", "Dagger", "Staff", "Warstaff" 등
        std::string uniqueId;   // ItemRarity::Unique일 때 고유 아이템 ID(예: "Headhunter", "Starforge")
        std::uint8_t jewelColor = 0;  // Jewel 전용: 0=Crimson, 1=Viridian, 2=Cobalt
        std::vector<RolledAffix> implicitAffixes;  // 벨트·고유 아이템 임플리싯(Chaos/Scouring/Alteration 불변 보존)
        std::vector<RolledAffix> affixes;
    };

    // 단일 장비의 파싱된 어픽스 스탯 합산치(로컬/글로벌 엄격 분리, 검수 m1)
    struct ItemStatTotals
    {
        // --- 로컬 모디파이어(해당 장비 베이스에만 적용) ---
        double localFlatArmour = 0.0;
        double localFlatEvasion = 0.0;
        double localFlatEnergyShield = 0.0;
        double localIncArmourPct = 0.0;
        double localIncEvasionPct = 0.0;
        double localIncEnergyShieldPct = 0.0;
        double localIncBlockPct = 0.0;

        double localFlatPhysMin = 0.0;
        double localFlatPhysMax = 0.0;
        double localFlatFireMin = 0.0;
        double localFlatFireMax = 0.0;
        double localFlatColdMin = 0.0;
        double localFlatColdMax = 0.0;
        double localFlatLightningMin = 0.0;
        double localFlatLightningMax = 0.0;
        double localFlatChaosMin = 0.0;
        double localFlatChaosMax = 0.0;
        double localIncPhysPct = 0.0;
        double localIncAttackSpeedPct = 0.0;
        double localIncCritChancePct = 0.0;

        // --- 글로벌 모디파이어(캐릭터 전체 합산) ---
        double flatStr = 0.0;
        double flatDex = 0.0;
        double flatInt = 0.0;

        double flatLife = 0.0;
        double incLifePct = 0.0;
        double flatMana = 0.0;
        double incManaPct = 0.0;
        double flatEnergyShield = 0.0;
        double incEnergyShieldPct = 0.0;

        double flatArmour = 0.0;
        double incArmourPct = 0.0;
        double flatEvasion = 0.0;
        double incEvasionPct = 0.0;
        double incArmourAndEvasionPct = 0.0;

        double fireResist = 0.0;
        double coldResist = 0.0;
        double lightningResist = 0.0;
        double chaosResist = 0.0;
        double physDamageReduction = 0.0;

        double attackBlockChance = 0.0;
        double spellBlockChance = 0.0;
        double attackBlockMaxBonusPct = 0.0;
        double spellBlockMaxBonusPct = 0.0;
        double spellSuppressionChance = 0.0;
        double incEsRechargeRatePct = 0.0;
        double fasterEsRechargeStartPct = 0.0;

        double flatAttackPhysMin = 0.0;
        double flatAttackPhysMax = 0.0;
        double flatAttackFireMin = 0.0;
        double flatAttackFireMax = 0.0;
        double flatAttackColdMin = 0.0;
        double flatAttackColdMax = 0.0;
        double flatAttackLightningMin = 0.0;
        double flatAttackLightningMax = 0.0;
        double flatAttackChaosMin = 0.0;
        double flatAttackChaosMax = 0.0;

        double flatSpellFireMin = 0.0;
        double flatSpellFireMax = 0.0;
        double flatSpellColdMin = 0.0;
        double flatSpellColdMax = 0.0;
        double flatSpellLightningMin = 0.0;
        double flatSpellLightningMax = 0.0;
        double flatSpellChaosMin = 0.0;
        double flatSpellChaosMax = 0.0;

        double incPhysicalDamagePct = 0.0;
        double incSpellDamagePct = 0.0;
        double incFireDamagePct = 0.0;
        double incColdDamagePct = 0.0;
        double incLightningDamagePct = 0.0;
        double incElementalAttackDamagePct = 0.0;

        double incAttackSpeedPct = 0.0;
        double incCastSpeedPct = 0.0;
        double incGlobalCritChancePct = 0.0;
        double incSpellCritChancePct = 0.0;
        double globalCritMultiplierPct = 0.0;

        double lifeRegenPerSec = 0.0;
        double incManaRegenPct = 0.0;
        double manaRegenPerSec = 0.0;
        double incMovementSpeedPct = 0.0;

        // --- 5단계 추가(정신력·주얼·고유 아이템 전용 스탯 및 플래그) ---
        double flatSpirit = 0.0;
        double flatManaCostDelta = 0.0;  // 음수면 스킬 마나 비용 감소(Praxis: -4..-8)
        double incGenericDamagePct = 0.0;
        double incMeleeDamagePct = 0.0;
        double incMeleePhysicalDamagePct = 0.0;
        double incElementalDamagePct = 0.0;
        double incAreaDamagePct = 0.0;
        double incAreaOfEffectPct = 0.0;
        double incAttackAreaOfEffectPct = 0.0;
        double incProjectileDamagePct = 0.0;
        double incChaosDamagePct = 0.0;
        double spellCritMultiPct = 0.0;
        double meleeCritMultiplierPct = 0.0;
        double elementalCritMultiPct = 0.0;
        double incDamageVsRarePct = 0.0;
        double esRecoupPct = 0.0;
        double itemRarityPct = 0.0;
        // U5 확정(2026-11-20): Starkonja 저체력 단점·회피 및 Astramentis 물리 공격 고정 피해 감소
        double reducedDamageOnLowLifePct = 0.0;
        double incEvasionOnLowLifePct = 0.0;
        double flatPhysTakenFromAttacksDelta = 0.0;  // 음수면 감소(예: Astramentis -4)

        bool cannotBeFrozen = false;
        bool shavronneChaosEs = false;
        bool starforgeShock = false;
        bool dealNoElemental = false;
        bool berekProlif = false;
        bool headhunter = false;
    };

    // StatEngine Layer1Input에 전달되는 논리 1칸 장비 스냅샷(엔진 포인터 없음)
    struct EquippedSlotInput
    {
        bool occupied = false;
        bool isCrafted = false;  // false = 미제작 바닐라/모드 장비(요구치 0, 항상 활성: 검수 C3)
        std::uint32_t baseFormId = 0;
        ItemRarity rarity = ItemRarity::Normal;
        std::uint32_t itemLevel = 1;

        std::uint32_t reqLevel = 0;
        std::uint32_t reqStr = 0;
        std::uint32_t reqDex = 0;
        std::uint32_t reqInt = 0;

        // 베이스 방어/무기 수치(로컬 모디파이어 적용 전)
        double baseArmour = 0.0;
        double baseEvasion = 0.0;
        double baseEnergyShield = 0.0;
        double baseBlockChance = 0.0;

        bool isWeapon = false;
        bool isTwoHanded = false;
        double baseWeaponPhysMin = 0.0;
        double baseWeaponPhysMax = 0.0;
        double baseWeaponCritChancePct = 5.0;
        double baseWeaponAttackRate = 1.4;

        ItemStatTotals stats{};
    };

    [[nodiscard]] inline std::uint8_t ItemLevelToTier(std::uint32_t itemLevel) noexcept
    {
        if (itemLevel <= 20) return 1;
        if (itemLevel <= 40) return 2;
        if (itemLevel <= 60) return 3;
        if (itemLevel <= 80) return 4;
        return 5;
    }

    [[nodiscard]] inline std::pair<std::uint32_t, std::uint32_t> TierLevelBounds(std::uint8_t tier) noexcept
    {
        if (tier <= 1) return { 1, 20 };
        switch (tier) {
            case 2: return { 21, 40 };
            case 3: return { 41, 60 };
            case 4: return { 61, 80 };
            default: return { 81, 100 };
        }
    }

    [[nodiscard]] inline const char* EquipSlotKoreanName(EquipSlot slot) noexcept
    {
        switch (slot) {
            case EquipSlot::MainHand: return "주무기";
            case EquipSlot::OffHand:  return "보조무기/방패";
            case EquipSlot::Helmet:   return "투구";
            case EquipSlot::Body:     return "갑옷";
            case EquipSlot::Gloves:   return "장갑";
            case EquipSlot::Boots:    return "장화";
            case EquipSlot::Amulet:   return "목걸이";
            case EquipSlot::Ring1:    return "반지 1";
            case EquipSlot::Ring2:    return "반지 2";
            case EquipSlot::Belt:     return "허리띠";
            default:                  return "알 수 없음";
        }
    }

    [[nodiscard]] inline const char* RarityKoreanLabel(ItemRarity rarity) noexcept
    {
        switch (rarity) {
            case ItemRarity::Normal: return "일반";
            case ItemRarity::Magic:  return "매직";
            case ItemRarity::Rare:   return "희귀";
            case ItemRarity::Unique: return "고유";
            default:                 return "일반";
        }
    }
}
