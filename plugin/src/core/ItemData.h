#pragma once

#include "core/ItemTypes.h"

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
            bool isAmulet = false);

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
        std::unordered_map<std::string, std::vector<BaseArmorCurveRow>> armorCurves_;
        std::unordered_map<std::string, std::vector<BaseWeaponCurveRow>> weaponCurves_;
    };
}
