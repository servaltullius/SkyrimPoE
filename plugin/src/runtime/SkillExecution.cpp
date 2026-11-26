#include "PCH.h"
#include "runtime/SkillExecution.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/CombatHooks.h"
#include "runtime/FormIds.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/MonsterRuntime.h"

namespace SkyrimPoE::Runtime
{
    namespace
    {
        Core::KillTracker* g_killTracker = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        thread_local std::uint32_t t_pluginDirectDamageDepth = 0;

        struct DirectDamageScope
        {
            DirectDamageScope() noexcept { ++t_pluginDirectDamageDepth; }
            ~DirectDamageScope() noexcept
            {
                if (t_pluginDirectDamageDepth > 0) {
                    --t_pluginDirectDamageDepth;
                }
            }
            DirectDamageScope(const DirectDamageScope&) = delete;
            DirectDamageScope& operator=(const DirectDamageScope&) = delete;
        };

        void SpawnHarmlessExplosionAt(RE::PlayerCharacter* player, const RE::NiPoint3& origin, RE::FormID vfxLocalId)
        {
            if (vfxLocalId == 0 || !player) {
                return;
            }
            auto* expl = FormIds::Lookup<RE::BGSExplosion>(vfxLocalId);
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!expl || !dataHandler) {
                return;
            }
            // 고정된 좌표 origin에 직접 무해 폭발 참조를 생성해 대상 이동·소실 시에도 정확한 위치에서 연출이 터지게 한다.
            (void)dataHandler->CreateReferenceAtLocation(
                expl,
                origin,
                RE::NiPoint3{},
                player->GetParentCell(),
                player->GetWorldspace(),
                nullptr,
                nullptr,
                RE::ObjectRefHandle{},
                false,
                true);
        }
    }

    void SkillExecution::Initialize(Core::KillTracker* killTracker, Core::StatEngine* statEngine)
    {
        g_killTracker = killTracker;
        g_statEngine = statEngine;
    }

    bool SkillExecution::IsInPluginDirectDamage() noexcept
    {
        return t_pluginDirectDamageDepth > 0;
    }

    bool SkillExecution::IsAllyOfPlayer(const RE::PlayerCharacter* player, const RE::Actor* actor) noexcept
    {
        if (!actor || !player) {
            return true;
        }
        if (actor == player) {
            return true;
        }
        if (actor->IsPlayerTeammate()) {
            return true;
        }
        if (actor->IsSummonedByPlayer()) {
            return true;
        }
        if (actor->IsCommandedActor() && actor->GetCommandingActor().get() == player) {
            return true;
        }
        return false;
    }

    bool SkillExecution::IsDragonActor(const RE::Actor* actor) noexcept
    {
        if (!actor) {
            return false;
        }
        const auto* race = actor->GetRace();
        if (!race) {
            return false;
        }
        static constexpr RE::FormID kActorTypeDragonKw = 0x00035D59;
        return race->HasKeywordID(kActorTypeDragonKw);
    }

    bool SkillExecution::HasLocationBossRefType(const RE::Actor* actor) noexcept
    {
        if (!actor) {
            return false;
        }
        // m-1: 바닐라 Skyrim.esm LocTypeBoss(0x000130F7) 정확 FormID 우선, 보조로 정확 EDID("Boss"/"LocTypeBoss")만 허용
        // (타 모드의 BossGuard/BossMinion 부분 문자열 오판 차단)
        constexpr RE::FormID kVanillaBossLocRefTypeId = 0x000130F7;
        auto isBossRefType = [](const RE::BGSLocationRefType* refType) noexcept -> bool {
            if (!refType) {
                return false;
            }
            if (refType->GetFormID() == kVanillaBossLocRefTypeId) {
                return true;
            }
            const auto edid = refType->GetFormEditorID();
            if (!edid) {
                return false;
            }
            const std::string_view sv(edid);
            return sv == "Boss" || sv == "LocTypeBoss";
        };
        if (const auto* extraLocRef = actor->extraList.GetByType<RE::ExtraLocationRefType>()) {
            if (isBossRefType(extraLocRef->locRefType)) {
                return true;
            }
        }
        if (auto* loc = actor->GetCurrentLocation()) {
            for (const auto& ref : loc->specialRefs) {
                if (ref.refData.refID == actor->GetFormID() && isBossRefType(ref.type)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool SkillExecution::IsBossOrDragon(const RE::Actor* actor) noexcept
    {
        return IsDragonActor(actor) ||
               HasLocationBossRefType(actor) ||
               (actor && MapDeviceRuntime::IsActiveMapOrArenaBoss(actor->GetFormID()));
    }

    bool SkillExecution::IsHostileForFirstHitReward(RE::Actor* target, RE::Actor* player)
    {
        if (!target || !player) {
            return false;
        }
        // 결정 181: 지도 몬스터는 늘 적으로 만든 것이라 교전 전 첫 일격도 보상한다(결정 134의 "원래 적대인 대상").
        // 소환·되살림·지배는 사망 시작의 제외 판정이 따로 거른다(결정 114, 115).
        // 등록부는 FormID만 보므로, 번호가 재사용된 다른 액터를 거르게 회차 셀 안에 있는지도 본다.
        const auto* cell = target->GetParentCell();
        const bool inRunCell = cell && MapDeviceRuntime::IsActiveMapCell(cell->GetFormID());
        return (inRunCell && MapDeviceRuntime::IsActiveRunMapActor(target->GetFormID())) || target->IsHostileToActor(player);
    }

    bool SkillExecution::CanDamageTarget(RE::PlayerCharacter* player, RE::Actor* target, bool isDirectHit)
    {
        if (!player || !target) {
            return false;
        }
        if (target->IsDead() || !target->Is3DLoaded() || target->IsDeleted() || target->IsDisabled()) {
            return false;
        }
        if (target->IsGhost() || target->IsInKillMove()) {
            return false;
        }
        if (IsAllyOfPlayer(player, target)) {
            return false;
        }
        // 결정 109, 133, 139, 148: 번지는 피해는 현재 적대 필수, 직격은 예외.
        if (!isDirectHit && !target->IsHostileToActor(player)) {
            return false;
        }
        return true;
    }

    std::vector<RE::NiPointer<RE::Actor>> SkillExecution::CollectHostileTargets(
        RE::PlayerCharacter* player,
        const RE::NiPoint3& origin,
        float radius)
    {
        std::vector<RE::NiPointer<RE::Actor>> out;
        if (!player || radius <= 0.0f) {
            return out;
        }
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!lists) {
            return out;
        }
        const float radiusSq = radius * radius;
        for (const auto& handle : lists->highActorHandles) {
            auto actor = handle.get();
            if (!actor) {
                continue;
            }
            const auto delta = actor->GetPosition() - origin;
            if (std::abs(delta.z) > kVerticalHeightLimit ||
                (delta.x * delta.x + delta.y * delta.y) > radiusSq) {
                continue;
            }
            if (!CanDamageTarget(player, actor.get(), false)) {
                continue;
            }
            out.push_back(std::move(actor));
        }
        return out;
    }

    double SkillExecution::GetDifficultyMultiplier(bool toPlayer) noexcept
    {
        constexpr std::array<const char*, 6> kSuffix{ "VE", "E", "N", "H", "VH", "L" };
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* settings = RE::GameSettingCollection::GetSingleton();
        if (!player || !settings) {
            return 1.0;
        }
        const auto diff = player->GetGameStatsData().difficulty;
        if (diff < 0 || diff >= static_cast<std::int32_t>(kSuffix.size())) {
            return 1.0;
        }
        const auto name = std::format("{}{}", toPlayer ? "fDiffMultHPToPC" : "fDiffMultHPByPC", kSuffix[static_cast<std::size_t>(diff)]);
        auto* setting = settings->GetSetting(name.c_str());
        if (!setting) {
            return 1.0;
        }
        const double val = static_cast<double>(setting->GetFloat());
        const double result = (std::isfinite(val) && val > 0.0) ? val : 1.0;
        // 난이도 배율이 바뀔 때만 한 번 남긴다(결정 53 대조용).
        static std::atomic<std::int64_t> lastLoggedByPc{ -1 };
        static std::atomic<std::int64_t> lastLoggedToPc{ -1 };
        auto& lastLogged = toPlayer ? lastLoggedToPc : lastLoggedByPc;
        const auto key = static_cast<std::int64_t>(diff) * 1'000'000 + std::llround(result * 10'000.0);
        if (lastLogged.exchange(key, std::memory_order_relaxed) != key) {
            SKSE::log::info("SkillExecution: difficulty {} {} = {:.4f}.", diff, name, result);
        }
        return result;
    }

    void SkillExecution::SpawnHarmlessVfx(const RE::NiPoint3& origin, RE::FormID vfxExplosionLocalId)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded() || player->IsDead()) {
            return;
        }
        SpawnHarmlessExplosionAt(player, origin, vfxExplosionLocalId);
    }

    bool SkillExecution::DealDirectSkillDamage(
        RE::Actor* target,
        double rawSkyrimDamage,
        Core::DamageType dominantType,
        bool isDirectHit,
        bool bypassesMonsterEs,
        bool isDamageOverTime,
        std::optional<double> physicalHitFraction,
        const Core::DamagePacket* hitTypes)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || player->IsDead() || !CanDamageTarget(player, target, isDirectHit)) {
            return false;
        }
        if (!std::isfinite(rawSkyrimDamage) || rawSkyrimDamage <= 0.0) {
            return false;
        }

        const auto targetId = target->GetFormID();
        const auto snap = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
        if (!hitTypes && snap && snap->passiveAilments.dealOnlyFire && dominantType != Core::DamageType::Fire) {
            return false;
        }
        const double physicalFraction = physicalHitFraction.value_or(dominantType == Core::DamageType::Physical ? 1.0 : 0.0);
        const double beforeArmourBreak = rawSkyrimDamage * AilmentRuntime::ShockDamageMultiplier(targetId);
        double scaledDmg = CombatHooks::DamageWithEnemyArmourBreak(
            target, beforeArmourBreak, physicalFraction, isDamageOverTime);
        std::optional<Core::DamagePacket> adjustedHitTypes;
        if (hitTypes && snap && snap->poe2Passives && scaledDmg != beforeArmourBreak) {
            adjustedHitTypes = Core::MonsterModSystem::AdjustPhysicalHitDamage(*hitTypes, beforeArmourBreak, scaledDmg);
            hitTypes = &*adjustedHitTypes;
        }
        // 검수 m2: 헤드헌터 등 레어 대상 추가 피해를 주문/투사체/노바/전령 폭발에도 동일 적용
        if (snap && snap->incDamageVsRarePct > 0.0) {
            if (const auto prof = MonsterRuntime::GetProfileCopy(targetId);
                prof && prof->rarity == Core::MonsterRarity::Rare) {
                scaledDmg *= (1.0 + snap->incDamageVsRarePct / 100.0);
            }
        }
        // 결정 53: 우리 스킬 피해도 스카이림 난이도 배율을 그대로 받는다. Actor::DoDamage(37335)는 셋째 인자가 참이어도
        // 배율이 1보다 작으면 곱하고 1 이상일 때만 건너뛰므로(2026-11-23 디컴파일), 거짓으로 불러 늘 곱하게 하고
        // 엔진이 곱할 배율을 결투장 하한 같은 우리 계산에 그대로 넘긴다.
        const double engineDiffMult = GetDifficultyMultiplier(false);
        const auto hitRes = MonsterRuntime::ApplyPlayerHitToMonster(
            targetId, scaledDmg, dominantType, bypassesMonsterEs, target, engineDiffMult,
            std::nullopt, hitTypes, isDamageOverTime, false, snap.get());
        if (!hitRes.hadAnyImpact) {
            return false;
        }
        // 결정 185: 실제 타격만 적 ES 표시의 마지막 교전 적으로 기록한다(CanDamageTarget이 아군을 이미 걸렀음).
        if (!isDamageOverTime) {
            MonsterRuntime::NoteEngagedEnemy(targetId, MonsterRuntime::EngagementSource::PlayerSkill);
        }

        // 결정 113, 134, 140 + C-15: 피격 직전 적대 선기록 → 실제 피해 실행 → 0 감소(무적 등)면 기여 확정 생략!
        const bool wasHostile = g_killTracker && IsHostileForFirstHitReward(target, player);
        auto* av = target->AsActorValueOwner();
        const double hpBefore = av ? static_cast<double>(av->GetActorValue(RE::ActorValue::kHealth)) : 0.0;
        const bool targetAlive = !target->IsDead() && (!av || hpBefore > 0.0);
        if (!targetAlive) {
            return false;
        }
        if (g_killTracker) {
            g_killTracker->RecordFirstHitHostility(targetId, wasHostile, targetAlive);
        }

        double actualHpLoss = 0.0;
        if (hitRes.remainingHealthDamage > 0.0) {
            {
                const DirectDamageScope scope;
                const Core::KillTracker::DirectHitScope contribution(g_killTracker, targetId);
                (void)target->DoDamage(static_cast<float>(hitRes.remainingHealthDamage), player, false);
            }
            const double hpAfter = av ? static_cast<double>(av->GetActorValue(RE::ActorValue::kHealth)) : 0.0;
            actualHpLoss = std::max(0.0, hpBefore - hpAfter);
            if (target->IsDead() && actualHpLoss <= 0.0) {
                actualHpLoss = hitRes.remainingHealthDamage;
            }
        }

        const double confirmedLoss = std::max(hitRes.absorbedByEs, actualHpLoss);
        if (g_killTracker && confirmedLoss > 0.0) {
            g_killTracker->ConfirmDamageContribution(targetId, confirmedLoss);
        }
        return confirmedLoss > 0.0;
    }

    bool ResolveTargetAilmentHit(RE::Actor* target, Core::DamagePacket& packet,
        const Core::TargetAilmentHitDamage& modifiers, Core::DamagePacket& ailmentDamage);

    bool DealSpellHit(RE::Actor* target, const AilmentRuntime::SpellHit& hit,
        Core::DamageType dominantType, bool isDirectHit, const Core::TargetAilmentHitDamage& targetHit)
    {
        dominantType = hit.resolvedDominantType.value_or(dominantType);
        auto damage = hit.poeDamage;
        auto ailmentDamage = hit.poeDamage;
        double skyrimDamage = hit.skyrimDamage;
        if (ResolveTargetAilmentHit(target, damage, targetHit, ailmentDamage)) {
            skyrimDamage = damage.TotalAverage() * hit.skyrimDamagePerPoe;
            const auto strongest = std::ranges::max_element(damage.byType, {}, &Core::DamageRange::Average);
            dominantType = static_cast<Core::DamageType>(strongest - damage.byType.begin());
        }
        if (!SkillExecution::DealDirectSkillDamage(
                target, skyrimDamage, dominantType, isDirectHit, false,
                false, Core::PhysicalDamageFraction(damage), &damage)) {
            return false;
        }
        // DoDamage can emit TESDeathEvent synchronously. The listener reads the
        // result in its queued work, after this confirmed hit has been recorded.
        (void)AilmentRuntime::ApplyPlayerHitAilments(target, ailmentDamage, hit.isCrit, AilmentRuntime::HitKind::Spell,
            hit.igniteChancePct, hit.starforgeShock);
        return true;
    }

    bool AilmentRuntime::DealSpellHit(
        RE::Actor* target, const SpellHit& hit, Core::DamageType dominantType, bool isDirectHit)
    {
        return Runtime::DealSpellHit(target, hit, dominantType, isDirectHit, {});
    }

    namespace
    {
        std::size_t ExecuteAreaDamage(
            const RE::NiPoint3& origin,
            float radius,
            double skyrimDamage,
            RE::FormID vfxExplosionLocalId,
            Core::DamageType dominantType,
            const AilmentRuntime::SpellHit* spellHit, const Core::TargetAilmentHitDamage& targetHit)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded() || player->IsDead()) {
                return 0;
            }

            // 무해 시각·음향 EXPL 연출은 피해가 0이어도 제공한다.
            SpawnHarmlessExplosionAt(player, origin, vfxExplosionLocalId);
            const bool canGainTargetDamage = spellHit && (targetHit.HasModifiers() ||
                std::ranges::any_of(spellHit->poeDamage.targetAilmentExtra, [](const auto& range) { return range.max > 0.0; }));
            if (!std::isfinite(skyrimDamage) || (skyrimDamage <= 0.0 && !canGainTargetDamage) || radius <= 0.0f) {
                return 0;
            }

            const auto targets = SkillExecution::CollectHostileTargets(player, origin, radius);
            std::size_t hitCount = 0;
            for (const auto& victim : targets) {
                const bool landed = spellHit
                    ? DealSpellHit(victim.get(), *spellHit, dominantType, false, targetHit)
                    : SkillExecution::DealDirectSkillDamage(
                        victim.get(), skyrimDamage, dominantType, false, false);
                if (landed) {
                    ++hitCount;
                }
            }
            return hitCount;
        }
    }

    std::size_t SkillExecution::ExecuteAoESpell(
        const RE::NiPoint3& origin,
        float radius,
        double skyrimDamage,
        RE::FormID vfxExplosionLocalId,
        Core::DamageType dominantType)
    {
        return ExecuteAreaDamage(origin, radius, skyrimDamage, vfxExplosionLocalId, dominantType, nullptr, {});
    }

    std::size_t AilmentRuntime::ExecuteSpellNova(
        const RE::NiPoint3& origin, float radius, const SpellHit& hit,
        RE::FormID vfxId, Core::DamageType dominantType)
    {
        return ExecuteAreaDamage(origin, radius, hit.skyrimDamage, vfxId, dominantType, &hit, {});
    }

    std::size_t ExecuteSpellNova(const RE::NiPoint3& origin, float radius, const AilmentRuntime::SpellHit& hit,
        RE::FormID vfxId, Core::DamageType dominantType, const Core::TargetAilmentHitDamage& targetHit)
    {
        return ExecuteAreaDamage(origin, radius, hit.skyrimDamage, vfxId, dominantType, &hit, targetHit);
    }
}
