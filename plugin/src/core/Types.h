#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimPoE::Core
{
    inline constexpr std::size_t kSkillSlotCount = 9;
    inline constexpr std::uint32_t kMaxExileLevel = 100;
    inline constexpr std::uint32_t kMaxGemLevel = 20;
    inline constexpr std::uint32_t kMaxGemSockets = 5;

    enum class DamageType : std::uint8_t
    {
        Physical = 0,
        Fire = 1,
        Cold = 2,
        Lightning = 3,
        Chaos = 4,
        Count = 5
    };

    enum class GemKind : std::uint8_t
    {
        Unknown = 0,
        AttackChannel = 1,
        Trigger = 2,
        Spell = 3,
        Support = 4
    };

    struct DamageRange
    {
        double min = 0.0;
        double max = 0.0;

        [[nodiscard]] double Average() const noexcept { return (min + max) * 0.5; }
    };

    struct DamagePacket
    {
        std::array<DamageRange, static_cast<std::size_t>(DamageType::Count)> byType{};

        [[nodiscard]] double TotalAverage() const noexcept
        {
            double sum = 0.0;
            for (const auto& r : byType) {
                sum += r.Average();
            }
            return sum;
        }
    };

    // 결정 105~108, 145: 처음 장착 시 고유 인스턴스 ID를 부여하고 탈착 시 보조·주문 젬을 함께 보관한다.
    struct GemInstance
    {
        std::uint64_t instanceId = 0;
        std::string gemId;          // PoB grantedEffectId (예: "Cyclone", "IceNova")
        std::uint8_t level = 1;
        std::uint8_t quality = 0;
        std::uint64_t xp = 0;
        std::uint8_t socketCount = 3;
        std::vector<GemInstance> socketedGems;  // 일반 스킬 젬이면 보조 젬, 발동 젬이면 주문 젬(각 주문 젬은 자체 보조 젬 보유)
    };

    struct SkillSlot
    {
        std::uint8_t slotIndex = 0;  // 0..8 (결정 88)
        bool occupied = false;
        GemInstance gem;
        std::int8_t linkedAttackSlot = -1;  // 발동 젬 전용: 연결된 공격 칸 인덱스 (결정 100, 131)
    };

    // 결정 59: 인벤토리 아이템을 (소유자 FormID, 고유 번호)로 가리키는 키
    struct InventoryItemKey
    {
        std::uint32_t ownerFormId = 0;
        std::uint16_t uniqueId = 0;

        [[nodiscard]] bool IsValid() const noexcept { return ownerFormId != 0 && uniqueId != 0; }
        [[nodiscard]] bool operator==(const InventoryItemKey&) const noexcept = default;
    };

    struct Config
    {
        // [General]
        std::uint32_t panelKey = 0x25;  // K
        bool yieldToFrenzy = true;
        bool giveStarterGems = true;
        std::uint8_t starterSockets = 3;
        double mainUpdateBudgetMs = 2.0;

        // [Conversion]
        double offenseK = 75.0;
        double offenseBaseHits = 3.0;
        double baseHitRate = 1.6;
        double offenseRollback = 0.5;
        double defenseRollback = 0.5;
        double defenseDref = 10.7;
        double expectedArmorIncrease = 1.0;
        double expectedLifeIncrease = 0.0;
        double expectedManaIncrease = 0.0;
        double expectedResistAffixes = 3.0;
        std::uint32_t act5Level = 45;
        std::uint32_t act10Level = 67;

        // [Experience]
        std::int32_t levelDiffClamp = 10;
        std::int32_t bossLevelBonus = 3;
        std::int32_t maxBossLevelBonus = 5;
        double higherEnemyBonusPerLevel = 0.10;
        double higherEnemyMaxMultiplier = 2.0;
        double lowerEnemyMinMultiplier = 0.01;
        double killsBase = 10.0;
        double killsSlope = 2.0;
        double baseKillXp = 100.0;
        double gemXpRatio = 0.10;

        // [Cyclone]
        std::string powerAttackInput = "rightHand";
        double minChannelSeconds = 0.6;
        double channelIgnoreStagger = 0.5;
    };
}
