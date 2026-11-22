#include "PCH.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"

#include "core/DamageConversion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <unordered_map>

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

        std::mt19937_64& AilmentRng()
        {
            static std::mt19937_64 rng(static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0xA113E47ULL);
            return rng;
        }

        bool IsBossOrDragon(const RE::Actor* actor) noexcept
        {
            // U3 확정(2026-11-20): 전 모듈 단일 보스 판정(드래곤 또는 LocTypeBoss)
            return SkillExecution::IsBossOrDragon(actor);
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
            {
                if (!actor->IsPlayerRef() && !actor->IsDeleted()) {
                    if (auto* av = actor->AsActorValueOwner()) {
                        // Q-1 + M-2: 엔진 리셋(OnActorReset)으로 이미 0이면 생략하되,
                        // Hasted(+20) 등 다른 SpeedMult와 공존해 부호가 섞여 있어도 -prevDelta를 정확히 원복한다.
                        const float curTemp = actor->GetActorValueModifier(
                            RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult);
                        if (std::abs(curTemp) < 0.01f) {
                            return;
                        }
                        const float restoreDelta = -prevDelta;
                        if (std::abs(restoreDelta) < 0.01f) {
                            return;
                        }
                        av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, restoreDelta);
                        av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, 0.01f);
                        av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, -0.01f);
                    }
                }
            }
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
        g_tickAccumSec += deltaSec;
        if (g_tickAccumSec < kDoTTickIntervalSec) {
            return;
        }
        const double step = g_tickAccumSec;
        g_tickAccumSec = 0.0;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || player->IsDead()) {
            return;
        }

        // C-22, N-3: 플레이어에 걸린 적 상태이상(점화·출혈 DoT, 감전·냉각 타이머) 진행
        // 검수 Minor 9: g_mutex 바깥에서 플레이어 이동 상태 조회 및 출혈 이동 배율 3.0 통일(C-10)
        const auto* pst = player->AsActorState();
        const bool pMoving = pst && (pst->IsSprinting() || pst->IsRunning() || pst->IsWalking());
        double playerDotSkyrim = 0.0;
        double playerStaminaDrain = 0.0;
        {
            std::scoped_lock lk(g_mutex);
            if (g_playerAilments.igniteRemainingSec > 0.0) {
                const double dur = std::min(step, g_playerAilments.igniteRemainingSec);
                playerDotSkyrim += g_playerAilments.igniteDpsSkyrim * dur;
                g_playerAilments.igniteRemainingSec = std::max(0.0, g_playerAilments.igniteRemainingSec - step);
                if (g_playerAilments.igniteRemainingSec <= 0.0) {
                    g_playerAilments.igniteDpsSkyrim = 0.0;
                }
            }
            if (g_playerAilments.bleedRemainingSec > 0.0) {
                const double dur = std::min(step, g_playerAilments.bleedRemainingSec);
                playerDotSkyrim += g_playerAilments.bleedDpsSkyrim * (pMoving ? 3.0 : 1.0) * dur;
                g_playerAilments.bleedRemainingSec = std::max(0.0, g_playerAilments.bleedRemainingSec - step);
                if (g_playerAilments.bleedRemainingSec <= 0.0) {
                    g_playerAilments.bleedDpsSkyrim = 0.0;
                }
            }
            if (g_playerAilments.shockRemainingSec > 0.0) {
                g_playerAilments.shockRemainingSec = std::max(0.0, g_playerAilments.shockRemainingSec - step);
                if (g_playerAilments.shockRemainingSec <= 0.0) {
                    g_playerAilments.shockMult = 1.0;
                }
            }
            if (g_playerAilments.chillRemainingSec > 0.0) {
                const double dur = std::min(step, g_playerAilments.chillRemainingSec);
                playerStaminaDrain += 6.0 * dur;  // 플레이어 SpeedMult 무변경 원칙 유지, 스태미나 냉각 부담 부여
                g_playerAilments.chillRemainingSec = std::max(0.0, g_playerAilments.chillRemainingSec - step);
            }
        }
        if ((playerDotSkyrim > 0.0 || playerStaminaDrain > 0.0) && !player->IsInKillMove()) {
            if (auto* pav = player->AsActorValueOwner()) {
                if (playerDotSkyrim > 0.0) {
                    const double curHp = static_cast<double>(pav->GetActorValue(RE::ActorValue::kHealth));
                    const double safeDmg = std::min(playerDotSkyrim, std::max(0.0, curHp - 1.0));
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

            const auto* st = actor->AsActorState();
            const bool isMoving = st && (st->IsSprinting() || st->IsRunning() || st->IsWalking());

            Core::AilmentTickDamage dot{};
            {
                std::scoped_lock lk(g_mutex);
                dot = g_ailments.TickActor(fid, step, isMoving);
            }

            SyncActorSpeedFromAilment(actor);

            if (dot.firePoeDamage > 0.0) {
                const double skDmg = Core::ConvertOffenseHit(dot.firePoeDamage, exileLevel, *g_dataPack, *g_config);
                (void)SkillExecution::DealDirectSkillDamage(actor, skDmg, Core::DamageType::Fire, false, false);
            }
            if (dot.physPoeDamage > 0.0 && !actor->IsDead()) {
                const double skDmg = Core::ConvertOffenseHit(dot.physPoeDamage, exileLevel, *g_dataPack, *g_config);
                (void)SkillExecution::DealDirectSkillDamage(actor, skDmg, Core::DamageType::Physical, false, false);
            }
            if (dot.chaosPoeDamage > 0.0 && !actor->IsDead()) {
                const double skDmg = Core::ConvertOffenseHit(dot.chaosPoeDamage, exileLevel, *g_dataPack, *g_config);
                (void)SkillExecution::DealDirectSkillDamage(actor, skDmg, Core::DamageType::Chaos, false, true);
            }
        }
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
        double freezeChancePct,
        double shockChancePct)
    {
        Core::AilmentApplyResult res{};
        if (!target || target->IsDead() || target->IsPlayerRef() || !g_dataPack || !g_statEngine) {
            return res;
        }
        const auto snap = g_statEngine->GetSnapshot();
        const std::uint32_t exileLevel = snap ? snap->exileLevel : 1u;
        const auto scaling = MapDeviceRuntime::GetMapActorScaling(target->GetFormID());

        std::uniform_real_distribution<double> u01(0.0, 1.0);
        Core::HitAilmentInput in{};
        in.physHitPoe = poeDamage.byType[static_cast<std::size_t>(Core::DamageType::Physical)].Average();
        in.fireHitPoe = poeDamage.byType[static_cast<std::size_t>(Core::DamageType::Fire)].Average();
        in.coldHitPoe = poeDamage.byType[static_cast<std::size_t>(Core::DamageType::Cold)].Average();
        in.lightningHitPoe = poeDamage.byType[static_cast<std::size_t>(Core::DamageType::Lightning)].Average();
        in.chaosHitPoe = poeDamage.byType[static_cast<std::size_t>(Core::DamageType::Chaos)].Average();
        in.targetAilmentThresholdPoe = Core::MapSystem::AilmentThreshold(exileLevel, scaling.lifeMultiplier, *g_dataPack);
        in.isCrit = isCrit;
        in.isBossOrDragon = IsBossOrDragon(target);
        in.starforgeShock = starforgeShock || (snap && snap->starforgeShock);
        // N-1: 패시브/전역 상태이상 확률을 스킬·타격 개별 확률에 자동 합산
        // 재검수 R-1: 출혈은 PoE에서 공격으로만 걸리고, 공격·주문 한정 확률은 해당 적중에만 더한다.
        const bool isAttack = (hitKind == HitKind::Attack);
        in.igniteChancePct = igniteChancePct +
            (snap ? snap->igniteChancePct + (isAttack ? snap->attackIgniteChancePct : 0.0) : 0.0);
        in.freezeChancePct = freezeChancePct + (snap ? snap->freezeChancePct : 0.0);
        in.shockChancePct = shockChancePct + (snap ? snap->shockChancePct : 0.0);
        in.bleedChancePct = isAttack ? bleedChancePct + (snap ? snap->bleedChancePct : 0.0) : 0.0;
        in.poisonChancePct = poisonChancePct +
            (snap ? snap->poisonChancePct + (isAttack ? snap->attackPoisonChancePct : snap->spellPoisonChancePct) : 0.0);
        in.roll01 = u01(AilmentRng());

        bool wasFrozenBefore = false;
        {
            std::scoped_lock lk(g_mutex);
            wasFrozenBefore = g_ailments.IsFrozen(target->GetFormID());
            res = g_ailments.ApplyHitAilments(target->GetFormID(), in);
        }
        if (res.appliedChill || res.appliedFreeze) {
            SyncActorSpeedFromAilment(target);
        }
        // N-4: 비보스 적에게 빙결이 새로 발동될 때 경직 1회 유발(완전 행동 정지 대신 경직 + SpeedMult -95)
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
        if (g_charmSystem && g_config && g_charmSystem->TryTriggerOnAilment(kind, g_config->enableCharms)) {
            out.purifyingCharmTriggered = true;
            // 정화의 참 발동 시 플레이어에 남아 있던 동결/출혈 잔여 상태도 즉시 정화(C-22)
            std::scoped_lock lk(g_mutex);
            g_playerAilments.bleedRemainingSec = 0.0;
            g_playerAilments.bleedDpsSkyrim = 0.0;
            g_playerAilments.chillRemainingSec = 0.0;
            return out;
        }

        // C-22, N-3: 면역/참에 막히지 않았으면 실제 플레이어 측 상태이상 부여
        const double safeHit = (std::isfinite(skyrimHitDamage) && skyrimHitDamage > 0.0) ? skyrimHitDamage : 10.0;
        {
            std::scoped_lock lk(g_mutex);
            switch (kind) {
                case Core::AilmentKind::Ignite: {
                    const double dps = std::clamp(safeHit * 0.25, 1.0, 25.0);
                    if (dps >= g_playerAilments.igniteDpsSkyrim || g_playerAilments.igniteRemainingSec <= 0.0) {
                        g_playerAilments.igniteDpsSkyrim = dps;
                        g_playerAilments.igniteRemainingSec = 3.0;
                    }
                    break;
                }
                case Core::AilmentKind::Bleed: {
                    const double dps = std::clamp(safeHit * 0.20, 1.0, 20.0);
                    if (dps >= g_playerAilments.bleedDpsSkyrim || g_playerAilments.bleedRemainingSec <= 0.0) {
                        g_playerAilments.bleedDpsSkyrim = dps;
                        g_playerAilments.bleedRemainingSec = 4.0;
                    }
                    break;
                }
                case Core::AilmentKind::Shock:
                    g_playerAilments.shockMult = 1.15;
                    g_playerAilments.shockRemainingSec = 2.0;
                    break;
                case Core::AilmentKind::Chill:
                case Core::AilmentKind::Freeze:
                    g_playerAilments.chillRemainingSec = 2.5;
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
        return (g_playerAilments.shockRemainingSec > 0.0) ? g_playerAilments.shockMult : 1.0;
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
                std::scoped_lock lk(g_mutex);
                for (const auto& other : nearby) {
                    if (!other || other->GetFormID() == fid) continue;
                    g_ailments.ProliferateIgniteAndShock(
                        other->GetFormID(),
                        prevState->igniteDps,
                        prevState->igniteRemaining,
                        prevState->shockRatio,
                        prevState->shockRemaining,
                        false);
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
        g_ailments.ClearAll();
        g_tickAccumSec = 0.0;
    }
}
