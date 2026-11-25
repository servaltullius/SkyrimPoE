#pragma once

#include "core/CharmSystem.h"
#include "core/DataPack.h"
#include "core/ItemTypes.h"
#include "core/MapTypes.h"
#include "core/PassiveStatParser.h"
#include "core/Types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimPoE::Core
{
    class EnergyShieldPool;

    // 층 ①: 장비·패시브·젬·레벨 등 정적 입력(결정 117)
    struct Layer1Input
    {
        std::uint32_t exileLevel = 1;
        std::string className;  // Compatibility for callers supplying a standalone stat input.
        std::optional<ClassBase> treeClass;
        std::array<SkillSlot, kSkillSlotCount> slots{};

        // 향후 3~4단계에서 패시브·장비가 채워 넣는 정적 합산치(2단계 기본값 0)
        double flatLife = 0.0;
        double incLifePct = 0.0;
        double flatMana = 0.0;
        double incManaPct = 0.0;
        double flatEnergyShield = 0.0;
        double incEnergyShieldPct = 0.0;
        double armour = 0.0;
        double evasion = 0.0;
        double fireResist = 0.0;
        double coldResist = 0.0;
        double lightningResist = 0.0;
        double chaosResist = 0.0;
        double physDamageReduction = 0.0;
        double elementalDamageReduction = 0.0;  // PoE 3.30 인내 충전: 원소 피해 감소(+4%/충전, 저항 상한과 별도 곱연산)
        double attackBlockChance = 0.0;
        double spellBlockChance = 0.0;
        double attackBlockMaxBonusPct = 0.0;
        double spellBlockMaxBonusPct = 0.0;
        double deflectionRating = 0.0;
        double baseWeaponPhysicalMin = 10.0;
        double baseWeaponPhysicalMax = 18.0;
        double baseWeaponCritChancePct = 5.0;
        double incAttackSpeedPct = 0.0;
        double incCritChancePct = 0.0;
        bool bloodMagic = false;

        // 3단계: 패시브 트리 전체에서 집계된 정적 스탯·키스톤 합산치
        PassiveStatTotals passives{};

        // 4~5단계(결정 156, 162): 논리 10칸 장착 장비 입력(8 물리 + 가상 반지2·벨트)
        std::array<EquippedSlotInput, kEquipSlotCount> equippedGear{};
        std::string mainWeaponTag;

        // 5단계(결정 161, 164): 패시브 주얼 소켓(고정점 루프 진입 전 선합산), 오라 토글, 보스 이정표 달성 수
        std::vector<ItemStatTotals> socketedJewelStats;
        // The live player pool outlives the stat engine input. Only set while the Phylactery is allocated.
        const EnergyShieldPool* phylacteryEnergyShield = nullptr;
        std::uint16_t activeAuraMask = 0;         // bit i = 스킬 칸 i의 오라/전령 ON 요청
        std::uint32_t achievementMask = 0;
    };

    // 층 ②: 충전·버프·이동·생명력 비율·참·헤드헌터·가드·활성 지도 상태 입력(결정 117, 166)
    struct Layer2Input
    {
        bool moving = false;
        bool lowLife = false;
        bool fullLife = false;
        bool mainHandOneHandedMartial = false;
        bool offHandOccupied = false;
        bool elementalOverloadActive = false;  // 최근 8초 이내 치명타 적중 시 활성(원소 피해 40% 증폭)
        bool phylacteryHasEnergyShield = false;
        std::uint8_t enduranceCharges = 0;
        std::uint8_t frenzyCharges = 0;
        std::uint8_t powerCharges = 0;

        // 5단계 동적 버프
        CharmActiveBonuses charmBonuses{};
        std::uint16_t headhunterModMask = 0;   // 레어 몬스터로부터 훔친 속성 비트마스크(최대 60초)
        double moltenShellBonusArmour = 0.0;   // 용암 방패 활성 중 고정 방어도 보너스

        // 결정 169: 플레이어의 활성 회차 셀 소속과 플레이어 쪽 지도 불이익을 위한 어픽스 요약
        bool inActiveMap = false;
        MapModSummary activeMapMods{};
    };

    struct PoeSkillCosts
    {
        double mana = 0.0;
        double life = 0.0;
    };

    [[nodiscard]] PoeSkillCosts ComputePoe2SkillCosts(double baseManaCost, double flatManaCostDelta,
        const PassiveStatTotals& passives, bool isAttack, bool isSpell, bool bloodMagic,
        bool phylacteryWithoutEnergyShield = false) noexcept;

    struct TargetAilmentHitDamage
    {
        DamagePacket basis;  // Converted and gained damage after more multipliers, before increases.
        DamageTypeValues increasedPct{};
        double againstElementalAilmentsPct = 0.0;
        double againstAnyAilmentsPct = 0.0;
        double againstIgnitedPct = 0.0;
        double againstBurningPct = 0.0;
        double againstFrozenPct = 0.0;

        [[nodiscard]] bool HasModifiers() const noexcept
        {
            return againstElementalAilmentsPct != 0.0 || againstAnyAilmentsPct != 0.0 ||
                againstIgnitedPct != 0.0 || againstBurningPct != 0.0 || againstFrozenPct != 0.0;
        }
    };

    // 사전 계산된 스킬 1건의 집계 스탯(불변 사본 내부)
    struct ComputedSkillStats
    {
        std::uint64_t gemInstanceId = 0;
        std::string gemId;
        GemKind kind = GemKind::Unknown;
        std::uint8_t level = 1;
        bool isTriggered = false;

        DamagePacket baseHitDamage;
        TargetAilmentHitDamage targetAilmentHitDamage;
        double attackSpeedMult = 1.0;        // 사이클론 기본 3.0 × 보조/충전 배율
        double castSpeedMult = 1.0;          // 주문 시전 속도 배율(패시브 시전 속도 + 격분 충전 + 주문 메아리)
        double baseCritChancePct = 5.0;      // 무기 또는 주문 기본 치명타 + 추가 기본 치명타
        double critChanceIncPct = 0.0;       // 치명타 확률 증가 합
        double critMultiplierPct = 100.0;    // 치명타 피해 배율(%)
        double areaMult = 1.0;               // 반경 배율: sqrt((1 + area_inc/100) * more)
        double poeManaCost = 0.0;            // PoE 단위 스킬 비용
        double poeLifeCost = 0.0;
        double cooldownSec = 0.0;            // 쿨다운(초)
        bool dealNoElemental = false;
        bool dealNoChaos = false;
        bool requirementMet = true;  // 4단계: 젬 레벨·능력치 요구치 충족 여부(검수 C3)

        // 5단계: 정신력·투사체·연쇄·분기·관통·반복·상태이상·가드 필드
        std::uint32_t spiritCost = 0;
        bool auraActive = false;
        std::uint8_t projectileCount = 1;
        std::uint8_t chainCount = 0;
        bool forkEnabled = false;
        std::uint8_t pierceCount = 0;
        bool alwaysPierce = false;
        std::uint8_t spellRepeatCount = 0;
        double projectilePierceChancePct = 0.0;
        std::array<double, 16> additionalSpellProjectilesChancePct{};
        double additionalProjectileChancePct = 0.0;
        double additionalChainChancePct = 0.0;
        double additionalForkProjectileChancePct = 0.0;
        double projectileSpeedMult = 1.0;
        double projectileAilmentChanceIncPct = 0.0;
        double igniteChancePct = 0.0;
        double arcRemainingChainMorePct = 0.0;
        double moltenShellFlatArmour = 0.0;
        double moltenShellAbsorbPct = 75.0;
        double moltenShellAbsorbCapPoe = 0.0;
        double moltenShellDurationSec = 3.0;
        double moltenShellReflectPct = 100.0;

        [[nodiscard]] double EffectiveCritChancePct() const noexcept
        {
            const double pct = baseCritChancePct * (1.0 + critChanceIncPct / 100.0);
            return pct < 0.0 ? 0.0 : (pct > 100.0 ? 100.0 : pct);
        }
    };

    // 결정 118, 137(다): 엔진 포인터를 일절 담지 않는 완전 불변 읽기 전용 스탯 사본.
    struct StatSnapshot
    {
        std::uint64_t version = 0;
        std::uint32_t exileLevel = 1;
        std::uint32_t str = 20;
        std::uint32_t dex = 20;
        std::uint32_t intel = 20;

        double poeMaxLife = 0.0;
        double poeMaxMana = 0.0;
        double expectedPoeLife = 0.0;
        double expectedPoeMana = 0.0;
        double poeMaxEnergyShield = 0.0;
        double armour = 0.0;
        double evasion = 0.0;
        double accuracy = 0.0;
        double fireResist = 0.0;
        double coldResist = 0.0;
        double lightningResist = 0.0;
        double chaosResist = 0.0;
        double maxFireResist = 75.0;
        double maxColdResist = 75.0;
        double maxLightningResist = 75.0;
        double maxChaosResist = 75.0;
        double physDamageReduction = 0.0;
        double elementalDamageReduction = 0.0;  // PoE 3.30 인내 충전: 원소 피해 감소(+4%/충전, 저항 상한과 별도 곱연산)
        double attackBlockChance = 0.0;
        double spellBlockChance = 0.0;
        double deflectionRating = 0.0;
        double deflectReductionBonusPct = 0.0;
        std::array<double, static_cast<std::size_t>(DamageType::Count)> armourAppliesPct{};
        bool unluckyEvade = false;
        double maximumEvadePct = 100.0;
        bool luckyDeflect = false;
        double spellSuppressionChance = 0.0;
        double spellDodgeChance = 0.0;
        double esRechargeRateMult = 1.0;
        double esRechargeDelayMult = 1.0;
        double momRatio = 0.0;
        double manaRecoveryMult = 1.0;
        double nativeManaRegenMult = 1.0;
        bool poe2Passives = false;
        bool excessLifeRegenToEs = false;
        bool esCannotRecharge = false;
        bool chaosFireSpellIgniteMagnitude = false;
        bool fireSpellIgniteDealsChaos = false;
        PassiveStatTotals passiveAilments{};
        double lifeRegenPerSec = 0.0;      // 4단계 검수 C2: 장비 생명력 재생
        double incManaRegenPct = 0.0;      // 4단계 검수 C2: 장비 마나 재생 속도 증가(%)
        double incMovementSpeedPct = 0.0;  // 4단계 검수 C2: 장비 이동 속도 증가(%)
        double incSprintingMovementSpeedPct = 0.0;
        double incEsMovementSpeedPct = 0.0;
        DamageTypeValues penetrationPct{};
        double attackElementalPenetrationPct = 0.0;

        bool bloodMagic = false;
        bool chaosInoculation = false;
        bool chaosImmune = false;
        bool resoluteTechnique = false;
        bool unwaveringStance = false;

        std::uint8_t maxEnduranceCharges = 3;
        std::uint8_t maxFrenzyCharges = 3;
        std::uint8_t maxPowerCharges = 3;
        std::array<ChargeKind, static_cast<std::size_t>(ChargeKind::Count)> chargeGain{
            ChargeKind::Endurance, ChargeKind::Frenzy, ChargeKind::Power
        };

        [[nodiscard]] ChargeKind MapChargeGain(ChargeKind source) const noexcept
        {
            return source == ChargeKind::Count ? source : chargeGain[static_cast<std::size_t>(source)];
        }

        // 5단계(결정 161~164): 정신력·오라·전령·고유 특수 효과·참 면역
        std::uint32_t baseMaxSpirit = 0;  // 기본 이정표 진행분, INI와 장비 제외
        std::uint32_t maxSpirit = 0;
        std::uint32_t reservedSpirit = 0;
        std::uint64_t requestedSpirit = 0;
        std::uint16_t effectiveActiveAuraMask = 0;
        bool heraldOfIceActive = false;
        double heraldOfIceExplosionMin = 0.0;
        double heraldOfIceExplosionMax = 0.0;
        bool heraldOfAshActive = false;
        double heraldOfAshOverkillPct = 0.0;

        bool elementalAilmentImmune = false;  // Purity of Elements
        bool cannotBeFrozen = false;          // Wanderlust / 정화의 참 / Purity of Elements
        bool bleedImmune = false;             // 정화의 참
        std::uint8_t activeCharmMask = 0;     // 이 스냅샷에 이미 반영된 참 비트마스크(같은 프레임 연속 피격 보정용)
        bool shavronneChaosEs = false;        // Shavronne's Wrappings
        bool starforgeShock = false;          // Starforge
        bool dealNoElemental = false;         // Starforge 전역 패널티
        bool berekProlif = false;             // Berek's Respite
        bool headhunter = false;              // Headhunter
        double flatManaCostDelta = 0.0;       // Praxis 등
        double manaRegenPerSec = 0.0;
        double esRecoupPct = 0.0;
        double incDamageVsRarePct = 0.0;
        double itemRarityPct = 0.0;
        double flatPhysTakenFromAttacksDelta = 0.0;  // U5: Astramentis 등 공격 물리 피해 고정 증감(PoE 단위)

        // 5단계(N-1): 전역 상태이상 유발 확률(%)
        double igniteChancePct = 0.0;
        double freezeChancePct = 0.0;
        double shockChancePct = 0.0;
        double bleedChancePct = 0.0;
        double poisonChancePct = 0.0;
        // 재검수 R-1: 공격·주문 한정 확률(%)
        double attackIgniteChancePct = 0.0;
        double attackBleedChancePct = 0.0;
        double attackPoisonChancePct = 0.0;
        double spellPoisonChancePct = 0.0;

        [[nodiscard]] double BleedChanceForHitPct(bool isAttack) const noexcept
        {
            return bleedChancePct + (isAttack ? attackBleedChancePct : 0.0);
        }

        // 결정 169: 플레이어 쪽 지도 불이익은 활성 회차 셀 안에서만 적용한다.
        bool inActiveMap = false;
        MapModSummary activeMapMods{};

        // 4~5단계(결정 156, 162): 고정점 수렴 후 10칸 장비 활성 및 요구치 미달 상태
        std::array<bool, kEquipSlotCount> gearSlotActive{};
        std::array<bool, kEquipSlotCount> gearSlotReqFailed{};

        // 9칸 슬롯별 스탯 및 발동 젬 안 주문 젬(instanceId -> ComputedSkillStats)
        std::array<ComputedSkillStats, kSkillSlotCount> slotStats{};
        std::unordered_map<std::uint64_t, ComputedSkillStats> triggeredSpellStats;
    };

    struct SpiritTogglePreview
    {
        bool canReserve = false;
        bool meetsRequirements = false;
        std::uint64_t requestedSpirit = 0;
        std::uint32_t maxSpirit = 0;
        std::uint32_t reservedSpirit = 0;
    };

    // 결정 117, 118, 137(다):
    // - 층 ①·② 입력 갱신은 dirty 플래그만 세우고, 주 갱신 스레드에서만 RebuildIfDirty()로 단일 불변 사본을 새로 만들어 교체한다.
    // - 피해 훅 등 모든 독자는 GetSnapshot()으로 사본을 받아 한 타격 전체를 같은 사본으로 계산한다.
    class StatEngine
    {
    public:
        StatEngine();

        void SetLayer1(const Layer1Input& input);
        void SetLayer2(const Layer2Input& input);
        void UpdateLayer2(const std::function<void(Layer2Input&)>& mutator);
        void MarkDirty() noexcept;

        // 주 갱신 스레드에서만 호출. dirty 상태면 층 ①·②를 함께 재계산해 새 불변 사본을 게시한다.
        bool RebuildIfDirty(const DataPack& pack, const Config& config);

        // 임의 스레드에서 안전하게 최신 불변 사본을 얻는다.
        [[nodiscard]] std::shared_ptr<const StatSnapshot> GetSnapshot() const;
        [[nodiscard]] SpiritTogglePreview PreviewSpiritToggle(std::size_t slotIndex, std::uint16_t proposedMask,
            const DataPack& pack, const Config& config) const;
        [[nodiscard]] bool IsDirty() const noexcept { return dirty_.load(std::memory_order_acquire); }

    private:
        [[nodiscard]] static StatSnapshot ComputeSnapshot(
            const Layer1Input& l1,
            const Layer2Input& l2,
            const DataPack& pack,
            const Config& config,
            std::uint64_t nextVersion);

        mutable std::mutex inputMutex_;
        Layer1Input layer1_;
        Layer2Input layer2_;

        std::atomic_bool dirty_{ true };
        std::uint64_t nextVersion_{ 1 };

        mutable std::mutex snapshotMutex_;
        std::shared_ptr<const StatSnapshot> snapshot_;
    };
}
