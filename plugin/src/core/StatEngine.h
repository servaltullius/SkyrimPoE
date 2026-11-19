#pragma once

#include "core/DataPack.h"
#include "core/PassiveStatParser.h"
#include "core/Types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace SkyrimPoE::Core
{
    // 층 ①: 장비·패시브·젬·레벨 등 정적 입력(결정 117)
    struct Layer1Input
    {
        std::uint32_t exileLevel = 1;
        std::string className = "Scion";
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
        double spellSuppressionChance = 0.0;
        double spellSuppressionPrevent = 50.0;
        double baseWeaponPhysicalMin = 10.0;
        double baseWeaponPhysicalMax = 18.0;
        double baseWeaponCritChancePct = 5.0;
        double incAttackSpeedPct = 0.0;
        double incCritChancePct = 0.0;
        double critMultiplierPct = 150.0;
        bool bloodMagic = false;

        // 3단계: 패시브 트리 전체에서 집계된 정적 스탯·키스톤 합산치
        PassiveStatTotals passives{};
    };

    // 층 ②: 충전·버프·이동·생명력 비율 등 동적 상태 입력(결정 117)
    struct Layer2Input
    {
        bool moving = false;
        bool lowLife = false;
        bool elementalOverloadActive = false;  // 최근 8초 이내 치명타 적중 시 활성(원소 피해 40% 증폭)
        std::uint8_t enduranceCharges = 0;
        std::uint8_t frenzyCharges = 0;
        std::uint8_t powerCharges = 0;
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
        double attackSpeedMult = 1.0;        // 사이클론 기본 3.0 × 보조/충전 배율
        double castSpeedMult = 1.0;          // 주문 시전 속도 배율(패시브 시전 속도 + 격분 충전)
        double baseCritChancePct = 5.0;      // 무기 또는 주문 기본 치명타 + 추가 기본 치명타
        double critChanceIncPct = 0.0;       // 치명타 확률 증가 합
        double critMultiplierPct = 150.0;    // 치명타 피해 배율(%)
        double areaMult = 1.0;               // 반경 배율: sqrt((1 + area_inc/100) * more)
        double poeManaCost = 0.0;            // PoE 단위 스킬 비용
        double cooldownSec = 0.0;            // 쿨다운(초)
        bool dealNoElemental = false;
        bool dealNoChaos = false;

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

        double poeMaxLife = 50.0;
        double poeMaxMana = 40.0;
        double poeMaxEnergyShield = 0.0;
        double armour = 0.0;
        double evasion = 0.0;
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
        double spellSuppressionChance = 0.0;
        double spellSuppressionPrevent = 50.0;
        double spellDodgeChance = 0.0;
        double esRechargeRateMult = 1.0;
        double esRechargeDelayMult = 1.0;
        double momRatio = 0.0;

        bool bloodMagic = false;
        bool chaosInoculation = false;
        bool chaosImmune = false;
        bool resoluteTechnique = false;
        bool unwaveringStance = false;

        std::uint8_t maxEnduranceCharges = 3;
        std::uint8_t maxFrenzyCharges = 3;
        std::uint8_t maxPowerCharges = 3;

        // 9칸 슬롯별 스탯 및 발동 젬 안 주문 젬(instanceId -> ComputedSkillStats)
        std::array<ComputedSkillStats, kSkillSlotCount> slotStats{};
        std::unordered_map<std::uint64_t, ComputedSkillStats> triggeredSpellStats;
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
        [[nodiscard]] bool IsDirty() const noexcept { return dirty_.load(std::memory_order_acquire); }

    private:
        [[nodiscard]] StatSnapshot ComputeSnapshotLocked(
            const Layer1Input& l1,
            const Layer2Input& l2,
            const DataPack& pack,
            const Config& config,
            std::uint64_t nextVersion) const;

        mutable std::mutex inputMutex_;
        Layer1Input layer1_;
        Layer2Input layer2_;

        std::atomic_bool dirty_{ true };
        std::uint64_t nextVersion_{ 1 };

        mutable std::mutex snapshotMutex_;
        std::shared_ptr<const StatSnapshot> snapshot_;
    };
}
