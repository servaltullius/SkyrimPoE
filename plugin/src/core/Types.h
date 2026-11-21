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
    inline constexpr std::uint8_t kClassCount = 7;
    inline constexpr std::uint32_t kMaxAscendancyPoints = 8;

    // 3단계 패시브 트리 영구 상태(코세이브 'TREE' 레코드)
    struct PassiveTreeState
    {
        bool classSelected = false;
        std::uint8_t classId = 0;  // 0..6 (Scion..Shadow)
        std::string ascendancyId;  // "" 또는 기본 전직 19종 id (예: "Raider", "Juggernaut")
        std::vector<std::uint32_t> allocatedNodes;  // 정렬·중복 제거된 소비 노드 ID (시작점/허브/마스터리 제외)
        std::unordered_map<std::uint32_t, std::uint32_t> masterySelections;  // masteryNodeId -> effectId
        std::uint32_t bossKillCount = 0;
        std::uint32_t dragonKillCount = 0;
        std::uint32_t ascendancyMilestonesBitmask = 0;  // bit 0..3 = 1~4차 보스·결투장 이정표(각 전직 +2, 패시브 +2, 정신력 +30)

        [[nodiscard]] bool IsAllocated(std::uint32_t nodeId) const noexcept
        {
            for (const auto id : allocatedNodes) {
                if (id == nodeId) {
                    return true;
                }
            }
            return false;
        }
    };

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
        Support = 4,
        Aura = 5,
        Herald = 6,
        Guard = 7
    };

    // 5단계(결정 162): 벨트 참 6종(자동 발동, 최대 3칸)
    enum class CharmKind : std::uint8_t
    {
        Ruby = 0,
        Sapphire = 1,
        Topaz = 2,
        Amethyst = 3,
        Purifying = 4,
        Bastion = 5,
        Count = 6
    };

    inline constexpr std::size_t kCharmKindCount = static_cast<std::size_t>(CharmKind::Count);
    inline constexpr std::size_t kMaxCharmSlots = 3;

    struct CharmSlotState
    {
        bool occupied = false;
        CharmKind kind = CharmKind::Ruby;
        double charges = 40.0;
    };

    // 5단계(결정 163): 핵심 상태이상 5종(출혈·중독 분리 포함 6상태)
    enum class AilmentKind : std::uint8_t
    {
        Ignite = 0,
        Chill = 1,
        Freeze = 2,
        Shock = 3,
        Bleed = 4,
        Poison = 5,
        Count = 6
    };

    // 5단계(결정 164): 매직·레어 몬스터 런타임 승격
    enum class MonsterRarity : std::uint8_t
    {
        Normal = 0,
        Magic = 1,
        Rare = 2
    };

    enum class MonsterModKind : std::uint8_t
    {
        Hasted = 0,
        Armoured = 1,
        FireResistant = 2,
        ColdResistant = 3,
        LightningResistant = 4,
        ChaosResistant = 5,
        EnergyShielded = 6,
        ExtraPhysical = 7,
        ExtraFire = 8,
        ExtraCold = 9,
        ExtraLightning = 10,
        Deadly = 11,
        Count = 12
    };

    inline constexpr std::size_t kMonsterModCount = static_cast<std::size_t>(MonsterModKind::Count);

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

        // [PassiveTree] (3단계: 결정 151~155, U1 실게임 충돌 확인 후 비충돌 L=0x26 적용)
        std::uint32_t treePanelKey = 0x26;  // L (0x19 P는 바닐라 Quick Magic과 충돌 실측)
        bool freePassiveRefund = true;      // C++ 기본값은 단위 테스트 호환을 위해 true 유지; 배포 INI 기본값은 false(결정 152·158)
        std::string simpleImeMode = "auto";  // auto | on | off
        std::int32_t bonusPassivePoints = 0;
        std::int32_t bonusAscendancyPoints = 0;

        // [Items] (4단계: 결정 156~160)
        std::uint32_t gearPanelKey = 0x18;  // O
        bool giveStarterCurrency = true;
        bool enforceGemRequirements = true;  // U2 확정(2026-11-20): 기본 true (AutoTest는 실행 중 임시 우회 후 원복)
        bool includeModdedBasesInTokenPool = false;
        std::uint32_t ascendancyRefundCost = 5;  // 전직 노드 1개 회수 시 후회의 오브 비용(PoE1 표준 5)

        // [Loot] (4단계: 결정 16, 157, 159)
        bool enableLootDrops = true;
        double tokenDropChance = 0.25;
        double rareTokenRatio = 0.25;
        double currencyDropChance = 0.30;
        double bossDropMultiplier = 2.5;
        bool worldDropEnabled = true;
        std::uint8_t worldDropMinTier = 2;  // 0=전체, 1=매직+, 2=희귀+고가치오브
        std::uint32_t worldDropMaxCount = 16;
        double worldDropReturnSeconds = 120.0;
        bool lootBeamEnabled = true;
        bool lootSoundEnabled = true;

        // [Spirit] (5단계: 결정 161)
        std::uint32_t spiritBase = 100;
        std::uint32_t spiritPerMilestone = 30;

        // [Charms] (5단계: 결정 162)
        bool enableCharms = true;

        // [Monsters] (5단계: 결정 164)
        bool enableMonsterPromotion = true;
        double magicMonsterChance = 0.12;
        double rareMonsterChance = 0.04;
        double uniqueDropChance = 0.04;
        double monsterEsRatio = 0.35;

        // [Maps] (6단계: 결정 166)
        std::uint32_t mapPanelKey = 0x16;        // U (controlmap.txt 비충돌)
        bool mapDeathInterceptEnabled = true;    // 지도 인스턴스 내 치명타 클램프 귀환 (타 사망 대체 모드 병존 토글)
        std::uint32_t mapBasePackCount = 3;      // 무리 앵커당 기본 스폰 수 (PackSize로 스케일)
    };
}
