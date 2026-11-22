#pragma once

#include "core/Types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SkyrimPoE::Core
{
    struct PoEConstants
    {
        double lifeBase = 38.0;
        double lifePerLevel = 12.0;
        double manaBase = 34.0;
        double manaPerLevel = 6.0;
        double resistCap = 75.0;
        double esRechargeDelay = 2.0;
        double esRechargeRate = 0.33;
        double monsterDamageMult = 1.5;
        double armourMitigationCap = 0.90;
        double minHitChance = 0.05;
    };

    struct ExpectedDefenseRow
    {
        std::uint32_t level = 1;
        double bestArmour = 0.0;
        double bestEvasion = 0.0;
        double bestEnergyShield = 0.0;
        double bestResistTier = 8.5;
    };

    struct ClassBase
    {
        std::string name;
        std::uint32_t baseStr = 20;
        std::uint32_t baseDex = 20;
        std::uint32_t baseInt = 20;
    };

    struct GemLevelData
    {
        std::uint8_t level = 1;
        std::uint8_t levelRequirement = 1;
        std::uint32_t reqStr = 0;
        std::uint32_t reqDex = 0;
        std::uint32_t reqInt = 0;
        double manaCost = 0.0;
        double manaMultiplier = 0.0;          // +20 -> ×1.20
        double attackSpeedMultiplier = 0.0;   // +200 -> ×3.00
        double baseMultiplier = 1.0;
        double damageEffectiveness = 1.0;
        double critChance = 0.0;
        double cooldown = 0.0;
        std::uint32_t spiritCost = 0;
        std::unordered_map<std::string, double> stats;

        [[nodiscard]] double GetStat(const std::string& key, double fallback = 0.0) const;
    };

    struct SkillEffectDefinition
    {
        std::string id;
        std::string name;
        std::uint8_t color = 0;  // 1=Str, 2=Dex, 3=Int
        bool isSupport = false;
        GemKind kind = GemKind::Unknown;
        double castTime = 0.0;
        std::uint32_t spiritCost = 0;
        std::unordered_set<std::string> skillTypes;
        std::vector<std::string> requireSkillTypes;  // RPN 표현식 (AND, OR, NOT 포함)
        std::vector<std::string> excludeSkillTypes;  // RPN 표현식
        std::vector<std::string> addSkillTypes;
        std::unordered_set<std::string> weaponTypes;
        std::unordered_map<std::string, double> constantStats;
        std::unordered_map<std::uint8_t, GemLevelData> levels;

        // 발동 젬 전용(예: SupportCastOnCritTriggered)
        std::optional<std::vector<std::string>> triggeredRequireSkillTypes;
        std::optional<std::vector<std::string>> triggeredExcludeSkillTypes;
        std::vector<std::string> triggeredAddSkillTypes;
        std::unordered_map<std::uint8_t, GemLevelData> triggeredLevels;

        [[nodiscard]] const GemLevelData* GetLevel(std::uint8_t lv) const;
        [[nodiscard]] const GemLevelData* GetTriggeredLevel(std::uint8_t lv) const;
    };

    // PoB Modules/CalcTools.lua:doesTypeExpressionMatch와 동일한 RPN 스택 논리식 평가기.
    // checkTypes가 비어 있으면 조건이 없으므로 true를 반환한다.
    [[nodiscard]] bool DoesTypeExpressionMatch(
        const std::vector<std::string>& checkTypes,
        const std::unordered_set<std::string>& skillTypes);

    class DataPack
    {
    public:
        bool LoadFromFile(const std::filesystem::path& path, std::string* errorOut = nullptr);
        bool LoadFromJsonString(std::string_view jsonText, std::string* errorOut = nullptr);

        [[nodiscard]] bool IsLoaded() const noexcept { return loaded_; }
        [[nodiscard]] const PoEConstants& Constants() const noexcept { return constants_; }
        [[nodiscard]] double MonsterLife(std::uint32_t level) const noexcept;
        [[nodiscard]] double MonsterDamage(std::uint32_t level) const noexcept;
        [[nodiscard]] double MonsterAccuracy(std::uint32_t level) const noexcept;
        [[nodiscard]] double MapLevelLifeMult(std::uint32_t level) const noexcept;
        [[nodiscard]] const ExpectedDefenseRow& ExpectedDefense(std::uint32_t level) const noexcept;

        [[nodiscard]] const ClassBase* FindClass(const std::string& name) const;
        [[nodiscard]] const SkillEffectDefinition* FindGem(const std::string& gemId) const;
        [[nodiscard]] const std::unordered_map<std::string, SkillEffectDefinition>& AllGems() const noexcept
        {
            return gems_;
        }

        // 보조 젬이 대상 스킬 젬(보조가 추가한 유형 포함)에 호환되는지 판정한다(6.1).
        [[nodiscard]] bool CanSupportApply(
            const std::string& supportGemId,
            const std::string& activeGemId,
            const std::unordered_set<std::string>* extraSkillTypes = nullptr) const;

        // 발동 젬(CoC 등)이 공격 스킬 칸과 연결될 수 있는지 판정한다(결정 100).
        [[nodiscard]] bool CanTriggerLinkAttack(
            const std::string& triggerGemId,
            const std::string& attackGemId) const;

        // 발동 젬 소켓에 주문 젬을 담을 수 있는지 판정한다(결정 132, PoB Triggerable+Spell RPN).
        [[nodiscard]] bool CanTriggerSocketSpell(
            const std::string& triggerGemId,
            const std::string& spellGemId) const;

    private:
        bool loaded_ = false;
        PoEConstants constants_;
        std::vector<double> monsterLife_;
        std::vector<double> monsterDamage_;
        std::vector<double> monsterAccuracy_;
        std::vector<double> mapLevelLifeMult_;
        std::vector<ExpectedDefenseRow> expectedDefense_;
        std::unordered_map<std::string, ClassBase> classes_;
        std::unordered_map<std::string, SkillEffectDefinition> gems_;
    };
}
