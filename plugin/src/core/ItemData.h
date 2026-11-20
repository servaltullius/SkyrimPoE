#pragma once

#include "core/ItemTypes.h"
#include "core/Types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace SkyrimPoE::Core
{
    struct AffixRange
    {
        double minVal = 0.0;
        double maxVal = 0.0;
        bool isFloat = false;
    };

    struct AffixLineDef
    {
        std::string templateEn;
        std::string templateKo;
        std::vector<AffixRange> ranges;
    };

    struct AffixDefinition
    {
        std::string id;
        AffixType type = AffixType::Prefix;
        std::string affixName;
        std::string group;
        std::uint32_t level = 1;
        std::uint32_t tier = 1;  // 1 = 최고 티어
        bool isLocal = false;
        bool rollable = false;
        std::vector<AffixLineDef> lines;
        std::vector<std::pair<std::string, std::uint32_t>> weights;  // 원본 순서 보존(first-match-wins)
        std::vector<std::string> tags;

        [[nodiscard]] std::size_t TotalRangeCount() const noexcept
        {
            std::size_t n = 0;
            for (const auto& ln : lines) {
                n += ln.ranges.size();
            }
            return n;
        }
    };

    struct CurrencyMetadata
    {
        CurrencyKind kind = CurrencyKind::ScrollOfWisdom;
        std::string id;
        std::string enName;
        std::string koName;
        std::string koDesc;
        std::uint32_t dropWeight = 0;
        std::uint8_t worldDropTier = 0;
    };

    struct BaseArmorCurveRow
    {
        std::uint32_t reqLevel = 1;
        std::uint32_t reqStr = 0;
        std::uint32_t reqDex = 0;
        std::uint32_t reqInt = 0;
        double baseArmour = 0.0;
        double baseEvasion = 0.0;
        double baseEnergyShield = 0.0;
        double baseBlockChance = 0.0;
    };

    struct BaseWeaponCurveRow
    {
        std::uint32_t reqLevel = 1;
        std::uint32_t reqStr = 0;
        std::uint32_t reqDex = 0;
        std::uint32_t reqInt = 0;
        double physMin = 10.0;
        double physMax = 18.0;
        double critChance = 5.0;
        double attackRate = 1.4;
    };

    // 5단계(결정 162, 164): 벨트 베이스, 참, 몬스터 속성, PoE1 고유 아이템 정의
    struct BeltBaseDef
    {
        std::string id;
        std::string enName;
        std::string koName;
        std::uint32_t localFormId = 0;
        std::uint32_t reqLevel = 1;
        bool bonusCharmSlot = false;  // Stygian Vise 등 추가 참 슬롯 보유 여부
        std::string implicitAffixId;
    };

    struct CharmMetadata
    {
        std::string id;
        CharmKind kind = CharmKind::Ruby;
        std::uint32_t localFormId = 0;
        std::string enName;
        std::string koName;
        std::string koDesc;
        double maxCharges = 40.0;
        double chargesPerUse = 20.0;
        double durationSec = 4.0;
    };

    struct MonsterModMetadata
    {
        std::string id;
        MonsterModKind kind = MonsterModKind::Hasted;
        std::string koName;
        std::string koDesc;
    };

    struct UniqueDefinition
    {
        std::string id;
        std::string enName;
        std::string koName;
        TokenSlotGroup slotGroup = TokenSlotGroup::Helmet;
        DefenseSubtype defenseSubtype = DefenseSubtype::None;
        bool isAmulet = false;
        std::string weaponTag;
        std::uint32_t baseFormId = 0;  // 바닐라 Skyrim.esm FormID 또는 0x000890..0x000894 로컬 FormID(벨트)
        std::uint32_t reqLevel = 1;
        std::vector<std::string> implicitAffixIds;
        std::vector<std::string> explicitAffixIds;
        std::vector<std::string> specialFlags;
    };

    class ItemData
    {
    public:
        bool LoadFromFile(const std::filesystem::path& path, std::string* errorOut = nullptr);
        bool LoadFromJsonString(std::string_view jsonText, std::string* errorOut = nullptr);

        [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }
        [[nodiscard]] const std::vector<AffixDefinition>& AllAffixes() const noexcept { return affixes_; }
        [[nodiscard]] const AffixDefinition* FindAffix(std::string_view id) const;
        [[nodiscard]] const std::vector<CurrencyMetadata>& AllCurrencies() const noexcept { return currencies_; }
        [[nodiscard]] const CurrencyMetadata* FindCurrency(CurrencyKind kind) const;

        [[nodiscard]] const std::vector<BeltBaseDef>& AllBelts() const noexcept { return beltBases_; }
        [[nodiscard]] const BeltBaseDef* FindBelt(std::string_view id) const;
        [[nodiscard]] const BeltBaseDef* FindBeltByLocalFormId(std::uint32_t localFormId) const;

        [[nodiscard]] const std::vector<CharmMetadata>& AllCharms() const noexcept { return charms_; }
        [[nodiscard]] const CharmMetadata* FindCharm(CharmKind kind) const;
        [[nodiscard]] const CharmMetadata* FindCharmByLocalFormId(std::uint32_t localFormId) const;

        [[nodiscard]] const std::vector<MonsterModMetadata>& AllMonsterMods() const noexcept { return monsterMods_; }
        [[nodiscard]] const MonsterModMetadata* FindMonsterMod(MonsterModKind kind) const;

        [[nodiscard]] const std::vector<UniqueDefinition>& AllUniques() const noexcept { return uniques_; }
        [[nodiscard]] const UniqueDefinition* FindUnique(std::string_view id) const;

        // PoE 본가 first-match-wins 가중치 평가기(검수 C1):
        // weights를 앞에서부터 순회해 itemTags에 포함된 첫 키의 weightVal을 즉시 반환한다(0이면 차단).
        [[nodiscard]] static std::uint32_t ResolveWeight(
            const AffixDefinition& affix,
            const std::unordered_set<std::string>& itemTags) noexcept;

        // 장비의 부위군·방어유형·무기태그로부터 PoB Data/Bases/*.lua 기준 태그 집합을 생성한다.
        [[nodiscard]] static std::unordered_set<std::string> BuildItemTags(
            TokenSlotGroup slotGroup,
            DefenseSubtype defenseSubtype,
            std::string_view weaponTag,
            bool isAmulet = false,
            std::uint8_t jewelColor = 0);

        // 주어진 장비 조건에서 추첨 가능한 (AffixDefinition*, weight) 후보 목록을 수집한다.
        [[nodiscard]] std::vector<std::pair<const AffixDefinition*, std::uint32_t>> CollectCandidates(
            AffixType type,
            std::uint32_t itemLevel,
            const std::unordered_set<std::string>& itemTags,
            const std::unordered_set<std::string>& excludedGroups) const;

        // PoB Data/Bases 커브에서 itemLevel 이하 최고 기본 방어구/무기 행을 조회한다(검수 C3).
        [[nodiscard]] BaseArmorCurveRow LookupArmorBase(
            TokenSlotGroup slotGroup,
            DefenseSubtype subtype,
            std::uint32_t itemLevel) const;

        [[nodiscard]] BaseWeaponCurveRow LookupWeaponBase(
            std::string_view weaponTag,
            std::uint32_t itemLevel) const;

    private:
        bool loaded_ = false;
        std::vector<AffixDefinition> affixes_;
        std::unordered_map<std::string, std::size_t> byId_;
        std::vector<CurrencyMetadata> currencies_;
        std::vector<BeltBaseDef> beltBases_;
        std::vector<CharmMetadata> charms_;
        std::vector<MonsterModMetadata> monsterMods_;
        std::vector<UniqueDefinition> uniques_;
        std::unordered_map<std::string, std::size_t> uniqueById_;
        std::unordered_map<std::string, std::vector<BaseArmorCurveRow>> armorCurves_;
        std::unordered_map<std::string, std::vector<BaseWeaponCurveRow>> weaponCurves_;
    };
}
