#include "PCH.h"
#include "runtime/SkillExecution.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/FormIds.h"
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
        return IsDragonActor(actor) || HasLocationBossRefType(actor);
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
        return (std::isfinite(val) && val > 0.0) ? val : 1.0;
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
        bool bypassesMonsterEs)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || player->IsDead() || !CanDamageTarget(player, target, isDirectHit)) {
            return false;
        }
        if (!std::isfinite(rawSkyrimDamage) || rawSkyrimDamage <= 0.0) {
            return false;
        }

        const auto targetId = target->GetFormID();
        double scaledDmg = rawSkyrimDamage * AilmentRuntime::ShockDamageMultiplier(targetId);
        // 검수 m2: 헤드헌터 등 레어 대상 추가 피해를 주문/투사체/노바/전령 폭발에도 동일 적용
        if (g_statEngine) {
            if (const auto snap = g_statEngine->GetSnapshot(); snap && snap->incDamageVsRarePct > 0.0) {
                if (const auto prof = MonsterRuntime::GetProfileCopy(targetId);
                    prof && prof->rarity == Core::MonsterRarity::Rare) {
                    scaledDmg *= (1.0 + snap->incDamageVsRarePct / 100.0);
                }
            }
        }
        const auto hitRes = MonsterRuntime::ApplyPlayerHitToMonster(
            targetId, scaledDmg, dominantType, bypassesMonsterEs);
        if (!hitRes.hadAnyImpact) {
            return false;
        }

        // 결정 113, 134, 140 + C-15: 피격 직전 적대 선기록 → 실제 피해 실행 → 0 감소(무적 등)면 기여 확정 생략!
        const bool targetAlive = !target->IsDead();
        if (g_killTracker) {
            g_killTracker->RecordFirstHitHostility(targetId, target->IsHostileToActor(player), targetAlive);
        }

        double actualHpLoss = 0.0;
        if (hitRes.remainingHealthDamage > 0.0) {
            auto* av = target->AsActorValueOwner();
            const double hpBefore = av ? static_cast<double>(av->GetActorValue(RE::ActorValue::kHealth)) : 0.0;
            {
                const DirectDamageScope scope;
                (void)target->DoDamage(static_cast<float>(hitRes.remainingHealthDamage), player, true);
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

    std::size_t SkillExecution::ExecuteAoESpell(
        const RE::NiPoint3& origin,
        float radius,
        double skyrimDamage,
        RE::FormID vfxExplosionLocalId,
        Core::DamageType dominantType)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded() || player->IsDead()) {
            return 0;
        }

        // 무해 시각·음향 EXPL 연출은 피해가 0이어도(예: Brutality) 시각 피드백으로 띄운다.
        SpawnHarmlessExplosionAt(player, origin, vfxExplosionLocalId);

        if (!std::isfinite(skyrimDamage) || skyrimDamage <= 0.0 || radius <= 0.0f) {
            return 0;
        }

        const auto targets = CollectHostileTargets(player, origin, radius);
        if (targets.empty()) {
            return 0;
        }

        std::size_t hitCount = 0;
        for (const auto& victim : targets) {
            if (DealDirectSkillDamage(victim.get(), skyrimDamage, dominantType, false, dominantType == Core::DamageType::Chaos)) {
                ++hitCount;
            }
        }
        return hitCount;
    }
}
