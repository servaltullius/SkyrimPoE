#pragma once

#include "core/AilmentSystem.h"
#include "core/CharmSystem.h"
#include "core/DataPack.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <cstdint>
#include <optional>

namespace SkyrimPoE::Core
{
    struct TriggeredSpellCast;
}

namespace SkyrimPoE::Runtime
{
    // 5단계(결정 163): 핵심 상태이상 5종 런타임 실행기
    // - 0.25s 주 갱신 DoT 틱(점화·출혈·중독)
    // - kTemporary SpeedMult + CarryWeight 0.01 미세 펄스로 안전한 냉각/빙결 적용 및 100% 원복
    // - Berek's Respite 처치 시 점화·감전 주변 전파
    class AilmentRuntime
    {
    public:
        static void Initialize(
            Core::StatEngine* statEngine,
            Core::CharmSystem* charmSystem,
            const Core::DataPack* dataPack,
            const Core::Config* config);

        // 주 갱신 스레드 틱: 0.25s 주기 DoT 적용 및 SpeedMult 동기화/원복
        static void Tick(double deltaSec, bool gamePaused);

        // 전역 확률에 더하는 공격·주문 한정 확률을 고른다.
        enum class HitKind : std::uint8_t
        {
            Attack,  // 근접·활·사이클론 타격
            Spell,   // 손 시전·발동 주문, 투사체 주문
        };

        struct SpellDamage : Core::DamagePacket
        {
            bool fireSpell = false;
            double projectileAilmentChanceIncPct = 0.0;
        };

        struct SpellHit
        {
            SpellDamage poeDamage;
            double skyrimDamagePerPoe = 0.0;
            double skyrimDamage = 0.0;
            double igniteChancePct = 0.0;
            bool isCrit = false;
            bool starforgeShock = false;
            std::optional<Core::DamageType> resolvedDominantType;
        };

        [[nodiscard]] static SpellHit RollSpellHit(const Core::TriggeredSpellCast& cast,
            double damageRoll01, double critRoll01, double damageMultiplier = 1.0) noexcept;
        static bool ResolveTargetAilmentExtra(RE::Actor* target, Core::DamagePacket& packet);
        static bool DealSpellHit(RE::Actor* target, const SpellHit& hit, Core::DamageType dominantType, bool isDirectHit);
        static std::size_t ExecuteSpellNova(const RE::NiPoint3& origin, float radius, const SpellHit& hit,
            RE::FormID vfxId, Core::DamageType dominantType);

        // 피해가 확인된 타격은 사망 동작을 시작한 대상도 포함한다. DeathListener는
        // 주 갱신 큐에서 이 타격의 상태를 읽은 뒤 지운다.
        static Core::AilmentApplyResult ApplyPlayerHitAilments(
            RE::Actor* target,
            const Core::DamagePacket& poeDamage,
            bool isCrit,
            HitKind hitKind,
            double igniteChancePct = 0.0,
            bool starforgeShock = false,
            double bleedChancePct = 0.0,
            double poisonChancePct = 0.0,
            double shockChancePct = 0.0,
            bool fireSpell = false,
            double projectileAilmentChanceIncPct = 0.0);
        static Core::AilmentApplyResult ApplyPlayerHitAilments(
            RE::Actor* target, const SpellDamage& damage, bool isCrit, HitKind kind,
            double igniteChancePct, bool starforgeShock);

        struct PlayerAilmentOutcome
        {
            bool ailmentApplied = false;
            bool purifyingCharmTriggered = false;
        };

        // 적 공격으로 플레이어에게 상태이상이 걸릴 때 정화의 참·면역 판정 및 실제 플레이어 상태이상 부여(C-22, N-3)
        // skyrimHitDamage > 0이면 해당 피해량 기준으로 점화/출혈 DoT 강도를 산정한다.
        static PlayerAilmentOutcome TryApplyEnemyAilmentToPlayer(
            Core::AilmentKind kind,
            double skyrimHitDamage = 0.0);

        // CombatHooks의 마법 지속 피해와 같은 매지카 예약으로 ES 잔여 피해를 분배한다.
        [[nodiscard]] static double ApplyPlayerDotMindOverMatter(
            double rawAfterEs, double difficultyMultiplier, RE::PlayerCharacter* player) noexcept;

        // 플레이어가 적의 감전에 걸려 있을 때 받는 피해 증폭 배율
        [[nodiscard]] static double PlayerShockDamageMultiplier() noexcept;

        // 스레드 안전 감전 증폭 배율 조회
        [[nodiscard]] static double ShockDamageMultiplier(RE::FormID actorId) noexcept;
        [[nodiscard]] static bool IsFrozen(RE::FormID actorId) noexcept;
        [[nodiscard]] static bool IsChilledOrFrozen(RE::FormID actorId) noexcept;
        [[nodiscard]] static double ChillSlowRatio(RE::FormID actorId) noexcept;
        [[nodiscard]] static std::optional<Core::ActorAilmentState> GetStateCopy(RE::FormID actorId);

        // DeathListener 호출: SpeedMult 원복 + Berek's Respite 점화·감전 전파 (victim null이어도 actorId 즉시 정리)
        static void OnActorKilled(RE::FormID actorId, RE::Actor* victim, bool playerHasBerekProlif);
        static void OnActorReset(RE::FormID actorId);
        // 6단계: 지도 사망 탈출/안전 제거 시 플레이어 상태이상 해제 및 전 액터 SpeedMult 즉시 원복
        static void ClearPlayerAilments() noexcept;
        static void RevertAllTrackedSpeedsNow();
        // Q-1: 세이브 시점에 적용 중인 kTemporary SpeedMult 델타 스냅샷(순수 메모리 읽기, 엔진 호출 금지)
        [[nodiscard]] static std::vector<std::pair<RE::FormID, float>> TakeActiveSpeedDeltasForSave();
        // SKSE OnRevert 전용: 순수 메모리 정리만 수행(엔진 함수 호출 금지)
        static void ResetOnRevert();
    };
}
