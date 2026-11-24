#include "PCH.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"

#include "core/DamageConversion.h"
#include "core/EnergyShield.h"
#include "core/RulesPack.h"
#include "core/SkillSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <unordered_map>
#include <utility>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        constexpr double kDoTTickIntervalSec = 0.25;
        constexpr double kDeadAilmentGraceSec = 3.0;  // Q-3: DeathListener 사망 확정 람다 전 상태 유예
        constexpr float kBerekProlifRadius = 450.0f;

        struct PlayerActiveAilments
        {
            double igniteRemainingSec = 0.0;
            double igniteDpsSkyrim = 0.0;
            double bleedRemainingSec = 0.0;
            double bleedDpsSkyrim = 0.0;
            double shockRemainingSec = 0.0;
            double shockMult = 1.0;
            double chillRemainingSec = 0.0;
        };

        Core::StatEngine* g_statEngine = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::Config* g_config = nullptr;

        std::mutex g_mutex;
        Core::AilmentSystem g_ailments;
        std::unordered_map<RE::FormID, float> g_appliedSpeedDelta;  // 음수 델타 추적(원복 보장)
        std::unordered_map<RE::FormID, double> g_deadGraceElapsed;  // Q-3: 사망 직후 상태이상 유예 타이머
        PlayerActiveAilments g_playerAilments{};                    // C-22, N-3: 적->플레이어 상태이상
        double g_tickAccumSec = 0.0;
        double g_ailmentTimeSec = 0.0;
        double g_playerAdvancedThroughSec = 0.0;
        double g_pendingPlayerIgnite = 0.0;
        double g_pendingPlayerBleed = 0.0;
        double g_pendingPlayerStaminaDrain = 0.0;

        bool IsMoving(RE::Actor* actor)
        {
            const auto* state = actor ? actor->AsActorState() : nullptr;
            return state && (state->IsSprinting() || state->IsRunning() || state->IsWalking());
        }

        void AdvancePlayerAilments(bool moving)
        {
            const double elapsed = g_ailmentTimeSec - g_playerAdvancedThroughSec;
            g_playerAdvancedThroughSec = g_ailmentTimeSec;
            auto& ailments = g_playerAilments;
            const double bleedMultiplier = moving ? Core::ActiveRuleValues().bleed.movingOrAggravatedMultiplier : 1.0;
            g_pendingPlayerIgnite += ailments.igniteDpsSkyrim * std::min(elapsed, ailments.igniteRemainingSec);
            g_pendingPlayerBleed += ailments.bleedDpsSkyrim * bleedMultiplier * std::min(elapsed, ailments.bleedRemainingSec);
            g_pendingPlayerStaminaDrain += 6.0 * std::min(elapsed, ailments.chillRemainingSec);
            ailments.igniteRemainingSec = std::max(0.0, ailments.igniteRemainingSec - elapsed);
            ailments.bleedRemainingSec = std::max(0.0, ailments.bleedRemainingSec - elapsed);
            ailments.chillRemainingSec = std::max(0.0, ailments.chillRemainingSec - elapsed);
            ailments.shockRemainingSec = std::max(0.0, ailments.shockRemainingSec - elapsed);
        }

        std::mt19937_64& AilmentRng()
        {
            static thread_local std::mt19937_64 rng(static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0xA113E47ULL);
            return rng;
        }

        void ApplyTrackedSpeedDelta(RE::Actor* actor, float desiredDelta)
        {
            if (!actor || actor->IsPlayerRef()) {
                return;  // 플레이어 SpeedMult는 절대 건드리지 않음(결정/검수 안전 규칙)
            }
            const auto fid = actor->GetFormID();
            float prevDelta = 0.0f;
            {
                std::scoped_lock lk(g_mutex);
                if (MapDeviceRuntime::IsUninstallCleanupActive()) {
                    return;
                }
                if (const auto it = g_appliedSpeedDelta.find(fid); it != g_appliedSpeedDelta.end()) {
                    prevDelta = it->second;
                }
                if (std::abs(desiredDelta - prevDelta) < 0.5f) {
                    return;
                }
                if (std::abs(desiredDelta) < 0.5f) {
                    g_appliedSpeedDelta.erase(fid);
                } else {
                    g_appliedSpeedDelta[fid] = desiredDelta;
                }
            }

            if (auto* av = actor->AsActorValueOwner()) {
                const float diff = desiredDelta - prevDelta;
                av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, diff);
                // Havok 이동 속도 즉시 반영을 위한 CarryWeight 0.01 미세 펄스
                av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, 0.01f);
                av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, -0.01f);
            }
        }

        void RevertTrackedSpeedByFormId(RE::FormID fid)
        {
            float prevDelta = 0.0f;
            {
                std::scoped_lock lk(g_mutex);
                const auto it = g_appliedSpeedDelta.find(fid);
                if (it == g_appliedSpeedDelta.end()) {
                    return;
                }
                prevDelta = it->second;
                g_appliedSpeedDelta.erase(it);
            }
            if (std::abs(prevDelta) < 0.01f) {
                return;
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid);
            if (!actor) {
                // 재검수 R-9: 지금 찾을 수 없는 액터(셀 언로드 등)의 원복을 버리면 세이브에 감속이 영구히 남는다. 맡겨서 다시 시도한다.
                SaveLoad::DeferSpeedRevert(fid, prevDelta);
                return;
            }
            if (actor->IsPlayerRef() || actor->IsDeleted()) {
                return;
            }
            auto* av = actor->AsActorValueOwner();
            if (!av) {
                SaveLoad::DeferSpeedRevert(fid, prevDelta);
                return;
            }
            av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, -prevDelta);
            av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, 0.01f);
            av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, -0.01f);
        }

        void SyncActorSpeedFromAilment(RE::Actor* actor)
        {
            if (!actor || actor->IsPlayerRef()) return;
            const auto fid = actor->GetFormID();
            float desired = 0.0f;
            {
                std::scoped_lock lk(g_mutex);
                if (g_ailments.IsFrozen(fid)) {
                    desired = -95.0f;
                } else {
                    const double chill = g_ailments.ChillSlowRatio(fid);
                    if (chill > 0.0) {
                        desired = -static_cast<float>(chill * 100.0);
                    }
                }
            }
            ApplyTrackedSpeedDelta(actor, desired);
        }
    }

    void AilmentRuntime::Initialize(
        Core::StatEngine* statEngine,
        Core::CharmSystem* charmSystem,
        const Core::DataPack* dataPack,
        const Core::Config* config)
    {
        g_statEngine = statEngine;
        g_charmSystem = charmSystem;
        g_dataPack = dataPack;
        g_config = config;
    }

    void AilmentRuntime::Tick(double deltaSec, bool gamePaused)
    {
        if (gamePaused || !std::isfinite(deltaSec) || deltaSec <= 0.0 || !g_dataPack || !g_config || !g_statEngine) {
            return;
        }
        double step;
        double now;
        {
            std::scoped_lock lk(g_mutex);
            g_ailmentTimeSec += deltaSec;
            g_tickAccumSec += deltaSec;
            if (g_tickAccumSec < kDoTTickIntervalSec) {
                return;
            }
            now = g_ailmentTimeSec;
            step = g_tickAccumSec;
            g_tickAccumSec = 0.0;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || player->IsDead()) {
            return;
        }

        const bool playerMoving = IsMoving(player);
        double playerIgniteSkyrim;
        double playerBleedSkyrim;
        double playerStaminaDrain;
        {
            std::scoped_lock lk(g_mutex);
            AdvancePlayerAilments(playerMoving);
            playerIgniteSkyrim = std::exchange(g_pendingPlayerIgnite, 0.0);
            playerBleedSkyrim = std::exchange(g_pendingPlayerBleed, 0.0);
            playerStaminaDrain = std::exchange(g_pendingPlayerStaminaDrain, 0.0);
        }
        if ((playerIgniteSkyrim > 0.0 || playerBleedSkyrim > 0.0 || playerStaminaDrain > 0.0) && !player->IsInKillMove()) {
            if (auto* pav = player->AsActorValueOwner()) {
                // DPS는 피격 방어·편향 후, 난이도 적용 전의 타격 피해에서 계산한다.
                // 마법 지속 피해와 같이 ES와 MoM에 난이도를 넘기고 남은 생명 피해에 한 번 적용한다.
                const double difficulty = SkillExecution::GetDifficultyMultiplier(true);
                auto& es = Core::GetPlayerEnergyShield();
                const auto ignite = es.AbsorbDamage(playerIgniteSkyrim, difficulty, false, Core::AilmentKind::Ignite);
                const auto bleed = es.AbsorbDamage(playerBleedSkyrim, difficulty, false, Core::AilmentKind::Bleed);
                const double rawAfterEs = ignite.remainingRawDamage + bleed.remainingRawDamage;
                const double rawAfterMom = ApplyPlayerDotMindOverMatter(rawAfterEs, difficulty, player);
                if (rawAfterMom > 0.0) {
                    const double curHp = static_cast<double>(pav->GetActorValue(RE::ActorValue::kHealth));
                    const double safeDmg = std::min(rawAfterMom * difficulty, std::max(0.0, curHp - 1.0));
                    if (safeDmg > 0.0) {
                        pav->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth,
                            -static_cast<float>(safeDmg));
                    }
                }
                if (playerStaminaDrain > 0.0) {
                    const double curStam = static_cast<double>(pav->GetActorValue(RE::ActorValue::kStamina));
                    const double safeDrain = std::min(playerStaminaDrain, std::max(0.0, curStam));
                    if (safeDrain > 0.0) {
                        pav->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina,
                            -static_cast<float>(safeDrain));
                    }
                }
            }
        }

        std::vector<RE::FormID> ids;
        {
            std::scoped_lock lk(g_mutex);
            ids = g_ailments.ActiveActorIds();
        }
        if (ids.empty()) {
            return;
        }

        const auto snap = g_statEngine->GetSnapshot();
        const std::uint32_t exileLevel = snap ? snap->exileLevel : 1u;

        for (const auto fid : ids) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid);
            if (!actor || !actor->Is3DLoaded() || actor->IsDeleted() || actor->IsDisabled()) {
                RevertTrackedSpeedByFormId(fid);
                std::scoped_lock lk(g_mutex);
                g_ailments.RemoveActor(fid);
                g_deadGraceElapsed.erase(fid);
                continue;
            }
            if (actor->IsDead()) {
                // Q-3: 사망 직후에는 감속만 즉시 원복하고, DeathListener의 Herald of Ice / Berek's Respite가
                // 상태를 읽을 수 있도록 최대 3초 유예한 뒤 정리한다.
                RevertTrackedSpeedByFormId(fid);
                std::scoped_lock lk(g_mutex);
                double& elapsed = g_deadGraceElapsed[fid];
                elapsed += step;
                if (elapsed >= kDeadAilmentGraceSec) {
                    g_ailments.RemoveActor(fid);
                    g_deadGraceElapsed.erase(fid);
                }
                continue;
            }

            const bool isMoving = IsMoving(actor);

            Core::AilmentTickDamage dot{};
            {
                std::scoped_lock lk(g_mutex);
                dot = g_ailments.TickActorAt(fid, now, isMoving);
            }

            SyncActorSpeedFromAilment(actor);

            if (dot.firePoeDamage > 0.0) {
                const double skDmg = Core::ConvertOffenseHit(dot.firePoeDamage, exileLevel, *g_dataPack, *g_config);
                (void)SkillExecution::DealDirectSkillDamage(actor, skDmg, Core::DamageType::Fire, false, false, true);
            }
            if (dot.physPoeDamage > 0.0 && !actor->IsDead()) {
                const double skDmg = Core::ConvertOffenseHit(dot.physPoeDamage, exileLevel, *g_dataPack, *g_config);
                (void)SkillExecution::DealDirectSkillDamage(actor, skDmg, Core::DamageType::Physical, false, false, true);
            }
            if (dot.chaosPoeDamage > 0.0 && !actor->IsDead()) {
                const double skDmg = Core::ConvertOffenseHit(dot.chaosPoeDamage, exileLevel, *g_dataPack, *g_config);
                (void)SkillExecution::DealDirectSkillDamage(actor, skDmg, Core::DamageType::Chaos, false, true, true);
            }
        }
    }

    AilmentRuntime::SpellHit AilmentRuntime::RollSpellHit(const Core::TriggeredSpellCast& cast,
        double damageRoll01, double critRoll01, double damageMultiplier) noexcept
    {
        SpellHit hit;
        if (!g_dataPack || !g_config) {
            return hit;
        }
        // The skill's damage and the packet use the same damage and critical rolls.
        hit.skyrimDamage = Core::SkillSystem::RollTriggeredSpellSkyrimDamage(
            cast, *g_dataPack, *g_config, damageRoll01, critRoll01, hit.isCrit) * damageMultiplier;
        const double critMultiplier = hit.isCrit ? std::max(1.0, cast.critMultiplierPct / 100.0) : 1.0;
        hit.poeDamage = Core::RollSpellDamagePacket(cast.poeBaseHitDamage, damageRoll01, critMultiplier * damageMultiplier);
        hit.igniteChancePct = cast.igniteChancePct;
        hit.starforgeShock = cast.starforgeShock;
        return hit;
    }

    Core::AilmentApplyResult AilmentRuntime::ApplyPlayerHitAilments(
        RE::Actor* target,
        const Core::DamagePacket& poeDamage,
        bool isCrit,
        HitKind hitKind,
        double igniteChancePct,
        bool starforgeShock,
        double bleedChancePct,
        double poisonChancePct,
        double shockChancePct)
    {
        Core::AilmentApplyResult res{};
        if (MapDeviceRuntime::IsUninstallCleanupActive() ||
            !target || target->IsDeleted() || target->IsPlayerRef() || !g_dataPack || !g_statEngine) {
            return res;
        }
        const auto snap = g_statEngine->GetSnapshot();
        const std::uint32_t exileLevel = snap ? snap->exileLevel : 1u;
        const auto targetId = target->GetFormID();
        const auto mapActor = MapDeviceRuntime::GetMapKillContext(targetId, 0);
        const auto monsterLevel = mapActor.rewardMonsterLevel != 0 ? mapActor.rewardMonsterLevel : exileLevel;
        const auto profile = MonsterRuntime::GetProfileCopy(targetId);
        const bool moving = IsMoving(target);

        std::uniform_real_distribution<double> u01(0.0, 1.0);
        Core::HitAilmentInput in{};
        in.physHitPoe = poeDamage.byType[static_cast<std::size_t>(Core::DamageType::Physical)].Average();
        in.fireHitPoe = poeDamage.byType[static_cast<std::size_t>(Core::DamageType::Fire)].Average();
        in.coldHitPoe = poeDamage.byType[static_cast<std::size_t>(Core::DamageType::Cold)].Average();
        in.lightningHitPoe = poeDamage.byType[static_cast<std::size_t>(Core::DamageType::Lightning)].Average();
        in.chaosHitPoe = poeDamage.byType[static_cast<std::size_t>(Core::DamageType::Chaos)].Average();
        in.targetAilmentThresholdPoe = Core::MapSystem::AilmentThreshold(monsterLevel);
        in.targetPoiseThresholdPoe = Core::MapSystem::PoiseThreshold(monsterLevel);
        in.isCrit = isCrit;
        in.isMapBoss = mapActor.isBoss;
        in.isBossOrDragon = SkillExecution::IsDragonActor(target) || SkillExecution::HasLocationBossRefType(target);
        if (profile) {
            in.targetRarity = profile->rarity;
            auto resistances = *profile;
            // Reuse the cold-resistance adjustment without the general rarity damage reduction.
            resistances.rarity = Core::MonsterRarity::Normal;
            in.coldDamageAfterResistanceMultiplier =
                Core::MonsterModSystem::IncomingDamageMultiplier(resistances, Core::DamageType::Cold);
        }
        if (in.coldHitPoe > 0.0 && mapActor.runId != 0) {
            Core::MapModSummary mapMods;
            if (MapDeviceRuntime::GetMapActorScaling(targetId, &mapMods).runId == mapActor.runId) {
                in.coldDamageAfterResistanceMultiplier *= std::max(0.25, 1.0 - mapMods.monsterElemResPct / 100.0);
            }
        }
        in.starforgeShock = starforgeShock || (snap && snap->starforgeShock);
        // N-1: 패시브/전역 상태이상 확률을 스킬·타격 개별 확률에 자동 합산
        // 공격·주문 한정 확률은 해당 적중에만 더한다.
        const bool isAttack = (hitKind == HitKind::Attack);
        in.igniteChancePct = igniteChancePct +
            (snap ? snap->igniteChancePct + (isAttack ? snap->attackIgniteChancePct : 0.0) : 0.0);
        in.shockChancePct = shockChancePct + (snap ? snap->shockChancePct : 0.0);
        in.bleedChancePct = bleedChancePct + (snap ? snap->BleedChanceForHitPct(isAttack) : 0.0);
        in.poisonChancePct = poisonChancePct +
            (snap ? snap->poisonChancePct + (isAttack ? snap->attackPoisonChancePct : snap->spellPoisonChancePct) : 0.0);
        in.igniteRoll01 = u01(AilmentRng());
        in.shockRoll01 = u01(AilmentRng());
        in.bleedRoll01 = u01(AilmentRng());
        in.poisonRoll01 = u01(AilmentRng());

        bool wasFrozenBefore = false;
        {
            std::scoped_lock lk(g_mutex);
            if (MapDeviceRuntime::IsUninstallCleanupActive()) {
                return res;
            }
            g_ailments.AdvanceActorTo(targetId, g_ailmentTimeSec, moving);
            wasFrozenBefore = g_ailments.IsFrozen(targetId);
            res = g_ailments.ApplyHitAilmentsAt(targetId, in, g_ailmentTimeSec, moving);
        }
        if (!target->IsDead() && (res.appliedChill || res.appliedFreeze)) {
            SyncActorSpeedFromAilment(target);
        }
        // 빙결이 시작될 때 경직 1회 유발(SpeedMult -95와 함께 적용).
        if (res.appliedFreeze && !wasFrozenBefore && target->Is3DLoaded() && !target->IsDead()) {
            target->SetGraphVariableFloat("staggerMagnitude", 0.5f);
            (void)target->NotifyAnimationGraph("staggerStart");
        }
        return res;
    }

    AilmentRuntime::PlayerAilmentOutcome AilmentRuntime::TryApplyEnemyAilmentToPlayer(
        Core::AilmentKind kind,
        double skyrimHitDamage)
    {
        PlayerAilmentOutcome out{};
        if (MapDeviceRuntime::IsUninstallCleanupActive()) {
            return out;
        }
        const auto snap = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
        // 검수 Major 3: 같은 프레임 직전 타격에서 이미 켜진 정화의 참 면역도 실시간 반영
        const auto liveCharm = (g_charmSystem && g_config && g_config->enableCharms)
            ? g_charmSystem->ComputeActiveBonuses()
            : Core::CharmActiveBonuses{};
        const bool freezeImmune = (snap && snap->cannotBeFrozen) || liveCharm.freezeImmune;
        const bool bleedImmune = (snap && snap->bleedImmune) || liveCharm.bleedImmune;
        if (snap && snap->elementalAilmentImmune &&
            (kind == Core::AilmentKind::Ignite || kind == Core::AilmentKind::Chill ||
             kind == Core::AilmentKind::Freeze || kind == Core::AilmentKind::Shock)) {
            return out;
        }
        if ((kind == Core::AilmentKind::Freeze || kind == Core::AilmentKind::Chill) && freezeImmune) {
            return out;
        }
        if (kind == Core::AilmentKind::Bleed && bleedImmune) {
            return out;
        }
        // C-22, N-3: 면역/참에 막히지 않았으면 실제 플레이어 측 상태이상 부여
        const double safeHit = (std::isfinite(skyrimHitDamage) && skyrimHitDamage > 0.0) ? skyrimHitDamage : 10.0;
        const auto& rules = Core::ActiveRuleValues();
        const bool moving = IsMoving(RE::PlayerCharacter::GetSingleton());
        {
            std::scoped_lock lk(g_mutex);
            if (MapDeviceRuntime::IsUninstallCleanupActive()) {
                return out;
            }
            AdvancePlayerAilments(moving);
            if (g_charmSystem && g_config && g_charmSystem->TryTriggerOnAilment(kind, g_config->enableCharms)) {
                out.purifyingCharmTriggered = true;
                // 정화의 참 발동 시 남아 있던 동결과 출혈도 즉시 정화한다(C-22).
                g_playerAilments.bleedRemainingSec = 0.0;
                g_playerAilments.bleedDpsSkyrim = 0.0;
                g_playerAilments.chillRemainingSec = 0.0;
                return out;
            }
            switch (kind) {
                case Core::AilmentKind::Ignite: {
                    const double dps = std::clamp(safeHit * 0.25, 1.0, 25.0);
                    if (dps >= g_playerAilments.igniteDpsSkyrim || g_playerAilments.igniteRemainingSec <= 0.0) {
                        g_playerAilments.igniteDpsSkyrim = dps;
                        g_playerAilments.igniteRemainingSec = rules.ignite.durationSeconds;
                    }
                    break;
                }
                case Core::AilmentKind::Bleed: {
                    const double dps = std::clamp(safeHit * 0.20, 1.0, 20.0);
                    if (dps >= g_playerAilments.bleedDpsSkyrim || g_playerAilments.bleedRemainingSec <= 0.0) {
                        g_playerAilments.bleedDpsSkyrim = dps;
                        g_playerAilments.bleedRemainingSec = rules.bleed.durationSeconds;
                    }
                    break;
                }
                case Core::AilmentKind::Shock:
                    g_playerAilments.shockMult = 1.0 + rules.shock.effectPercent / 100.0;
                    g_playerAilments.shockRemainingSec = rules.shock.playerDurationSeconds;
                    break;
                case Core::AilmentKind::Chill:
                    g_playerAilments.chillRemainingSec = rules.chill.playerDurationSeconds;
                    break;
                case Core::AilmentKind::Freeze:
                    g_playerAilments.chillRemainingSec = rules.freeze.playerDurationSeconds;
                    break;
                default:
                    break;
            }
        }
        out.ailmentApplied = true;
        return out;
    }

    double AilmentRuntime::PlayerShockDamageMultiplier() noexcept
    {
        std::scoped_lock lk(g_mutex);
        return (g_playerAilments.shockRemainingSec > g_ailmentTimeSec - g_playerAdvancedThroughSec) ? g_playerAilments.shockMult : 1.0;
    }

    double AilmentRuntime::ShockDamageMultiplier(RE::FormID actorId) noexcept
    {
        std::scoped_lock lk(g_mutex);
        return g_ailments.ShockDamageMultiplier(actorId);
    }

    bool AilmentRuntime::IsFrozen(RE::FormID actorId) noexcept
    {
        std::scoped_lock lk(g_mutex);
        return g_ailments.IsFrozen(actorId);
    }

    bool AilmentRuntime::IsChilledOrFrozen(RE::FormID actorId) noexcept
    {
        std::scoped_lock lk(g_mutex);
        return g_ailments.IsChilledOrFrozen(actorId);
    }

    double AilmentRuntime::ChillSlowRatio(RE::FormID actorId) noexcept
    {
        std::scoped_lock lk(g_mutex);
        return g_ailments.ChillSlowRatio(actorId);
    }

    std::optional<Core::ActorAilmentState> AilmentRuntime::GetStateCopy(RE::FormID actorId)
    {
        std::scoped_lock lk(g_mutex);
        if (const auto* st = g_ailments.GetState(actorId)) {
            return *st;
        }
        return std::nullopt;
    }

    void AilmentRuntime::OnActorKilled(RE::FormID actorId, RE::Actor* victim, bool playerHasBerekProlif)
    {
        const RE::FormID fid = (actorId != 0) ? actorId : (victim ? victim->GetFormID() : 0);
        if (fid == 0) return;

        // Codex 지적 12: 시체 핸들이 만료돼 victim이 없어도 FormID로 원복을 시도한다.
        // 찾지 못하면 RevertTrackedSpeedByFormId가 맡겨서(SaveLoad::DeferSpeedRevert) 다시 시도한다.
        RevertTrackedSpeedByFormId(fid);

        std::optional<Core::ActorAilmentState> prevState;
        {
            std::scoped_lock lk(g_mutex);
            if (const auto* st = g_ailments.GetState(fid)) {
                prevState = *st;
            }
            g_ailments.RemoveActor(fid);
            g_deadGraceElapsed.erase(fid);
        }

        // Berek's Respite: 점화·감전 적 처치 시 반경 450u 내 적대 대상에게 동일 점화·감전 전파
        if (victim && playerHasBerekProlif && prevState.has_value() &&
            (prevState->igniteRemaining > 0.0 || prevState->shockRemaining > 0.0)) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                const auto nearby = SkillExecution::CollectHostileTargets(player, victim->GetPosition(), kBerekProlifRadius);
                for (const auto& other : nearby) {
                    if (!other || other->GetFormID() == fid) continue;
                    const bool moving = IsMoving(other.get());
                    std::scoped_lock lk(g_mutex);
                    g_ailments.ProliferateIgniteAndShock(
                        other->GetFormID(),
                        prevState->igniteDps,
                        prevState->igniteRemaining,
                        prevState->shockRatio,
                        prevState->shockRemaining,
                        false, g_ailmentTimeSec, moving);
                }
            }
        }
    }

    void AilmentRuntime::OnActorReset(RE::FormID actorId)
    {
        RevertTrackedSpeedByFormId(actorId);
        std::scoped_lock lk(g_mutex);
        g_ailments.RemoveActor(actorId);
        g_deadGraceElapsed.erase(actorId);
    }

    void AilmentRuntime::ClearPlayerAilments() noexcept
    {
        std::scoped_lock lk(g_mutex);
        g_playerAilments = PlayerActiveAilments{};
        g_playerAdvancedThroughSec = g_ailmentTimeSec;
        g_pendingPlayerIgnite = 0.0;
        g_pendingPlayerBleed = 0.0;
        g_pendingPlayerStaminaDrain = 0.0;
    }

    void AilmentRuntime::RevertAllTrackedSpeedsNow()
    {
        std::vector<RE::FormID> fids;
        {
            std::scoped_lock lk(g_mutex);
            fids.reserve(g_appliedSpeedDelta.size());
            for (const auto& [fid, _] : g_appliedSpeedDelta) {
                fids.push_back(fid);
            }
        }
        for (const auto fid : fids) {
            RevertTrackedSpeedByFormId(fid);
        }
        std::scoped_lock lk(g_mutex);
        g_ailments.ClearAll();
        g_deadGraceElapsed.clear();
    }

    std::vector<std::pair<RE::FormID, float>> AilmentRuntime::TakeActiveSpeedDeltasForSave()
    {
        std::scoped_lock lk(g_mutex);
        std::vector<std::pair<RE::FormID, float>> out;
        out.reserve(g_appliedSpeedDelta.size());
        for (const auto& [fid, delta] : g_appliedSpeedDelta) {
            if (fid != 0 && std::isfinite(delta) && std::abs(delta) >= 0.01f) {
                out.emplace_back(fid, delta);
            }
        }
        return out;
    }

    void AilmentRuntime::ResetOnRevert()
    {
        // 검수 C1: SKSE OnRevert 콜백은 순수 메모리 정리만 수행(엔진 LookupByID/ModActorValue 절대 금지)
        std::scoped_lock lk(g_mutex);
        g_appliedSpeedDelta.clear();
        g_deadGraceElapsed.clear();
        g_playerAilments = PlayerActiveAilments{};
        g_playerAdvancedThroughSec = g_ailmentTimeSec;
        g_pendingPlayerIgnite = 0.0;
        g_pendingPlayerBleed = 0.0;
        g_pendingPlayerStaminaDrain = 0.0;
        g_ailments.ClearAll();
        g_tickAccumSec = 0.0;
        g_ailmentTimeSec = 0.0;
        g_playerAdvancedThroughSec = 0.0;
    }
}
