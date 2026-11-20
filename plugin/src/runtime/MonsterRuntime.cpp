#include "PCH.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/SkillExecution.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        Core::StatEngine* g_statEngine = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        const Core::Config* g_config = nullptr;

        std::mutex g_mutex;
        std::unordered_map<RE::FormID, Core::MonsterProfile> g_profiles;
        std::unordered_set<RE::FormID> g_rolledActors;
        std::unordered_map<RE::FormID, float> g_appliedHasteDelta;  // N-2: Hasted SpeedMult 추적(원복 보장)
        std::vector<StolenHeadhunterBuff> g_hhBuffs;
        std::uint16_t g_lastSyncedHhMask = 0;
        double g_scanAccumSec = 0.0;

        void ApplyHasteSpeedIfNeeded(RE::Actor* actor)
        {
            if (!actor || actor->IsPlayerRef() || actor->IsDead() || actor->IsDeleted()) {
                return;
            }
            const auto fid = actor->GetFormID();
            constexpr float kHastedSpeedBonus = 20.0f;
            {
                std::scoped_lock lk(g_mutex);
                if (g_appliedHasteDelta.contains(fid)) {
                    return;
                }
                g_appliedHasteDelta[fid] = kHastedSpeedBonus;
            }
            if (auto* av = actor->AsActorValueOwner()) {
                av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, kHastedSpeedBonus);
                av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, 0.01f);
                av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, -0.01f);
            }
        }

        void RevertHasteSpeedByFormId(RE::FormID fid)
        {
            float prev = 0.0f;
            {
                std::scoped_lock lk(g_mutex);
                const auto it = g_appliedHasteDelta.find(fid);
                if (it == g_appliedHasteDelta.end()) {
                    return;
                }
                prev = it->second;
                g_appliedHasteDelta.erase(it);
            }
            if (prev <= 0.01f) {
                return;
            }
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid)) {
                if (!actor->IsPlayerRef() && !actor->IsDeleted()) {
                    const float curTemp = actor->GetActorValueModifier(
                        RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult);
                    // M-2: 엔진 리셋으로 이미 0이면 생략하되, 냉각/빙결(-95)과 공존해 curTemp < 0이어도 +20 가속분을 정확히 차감한다.
                    if (std::abs(curTemp) < 0.01f) {
                        return;
                    }
                    const float restore = -prev;
                    if (auto* av = actor->AsActorValueOwner()) {
                        av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, restore);
                        av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, 0.01f);
                        av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, -0.01f);
                    }
                }
            }
        }

        std::mt19937_64& MonsterRng()
        {
            static std::mt19937_64 rng(static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x5A17C0DEULL);
            return rng;
        }

        bool IsDragonActor(const RE::Actor* actor) noexcept
        {
            // U3 확정(2026-11-20): 전 모듈 단일 보스 판정
            return SkillExecution::IsDragonActor(actor);
        }

        bool IsBossActor(const RE::Actor* actor) noexcept
        {
            return SkillExecution::IsBossOrDragon(actor);
        }

        bool IsEligibleForPromotion(RE::PlayerCharacter* player, RE::Actor* actor) noexcept
        {
            if (!player || !actor || actor == player) return false;
            if (actor->IsDead() || !actor->Is3DLoaded() || actor->IsDeleted() || actor->IsDisabled()) return false;
            if (actor->IsEssential() || actor->IsProtected() || actor->IsGhost() || actor->IsChild()) return false;
            if (SkillExecution::IsAllyOfPlayer(player, actor)) return false;
            if (actor->IsSummoned() || actor->IsCommandedActor()) return false;
            if (const auto* st = actor->AsActorState(); st && st->IsReanimated()) return false;
            if (!actor->IsHostileToActor(player)) return false;
            // N-5(결정 164): 적대 전투 진입 시에만 승격
            if (!actor->IsInCombat()) return false;
            return true;
        }

        std::uint16_t ComputeCombinedHhMaskLocked() noexcept
        {
            std::uint16_t mask = 0;
            for (const auto& b : g_hhBuffs) {
                if (b.remainingSec > 0.0) {
                    mask |= b.modMask;
                }
            }
            return mask;
        }

        void SyncHeadhunterMaskIfChanged(std::uint16_t newMask)
        {
            if (g_statEngine) {
                g_statEngine->UpdateLayer2([newMask](Core::Layer2Input& l2) {
                    l2.headhunterModMask = newMask;
                });
            }
        }
    }

    void MonsterRuntime::Initialize(
        Core::StatEngine* statEngine,
        const Core::ItemData* itemData,
        const Core::Config* config)
    {
        g_statEngine = statEngine;
        g_itemData = itemData;
        g_config = config;
    }

    void MonsterRuntime::Tick(double deltaSec, bool gamePaused)
    {
        if (gamePaused || !std::isfinite(deltaSec) || deltaSec <= 0.0) {
            return;
        }

        // 1) 적 ES 재충전 + 헤드헌터 버프 타이머 진행
        bool hhChanged = false;
        std::uint16_t nextHhMask = 0;
        {
            std::scoped_lock lk(g_mutex);
            for (auto& [_, prof] : g_profiles) {
                Core::MonsterModSystem::TickMonsterEs(prof, deltaSec);
            }
            for (auto it = g_hhBuffs.begin(); it != g_hhBuffs.end();) {
                it->remainingSec -= deltaSec;
                if (it->remainingSec <= 0.0) {
                    it = g_hhBuffs.erase(it);
                } else {
                    ++it;
                }
            }
            nextHhMask = ComputeCombinedHhMaskLocked();
            if (nextHhMask != g_lastSyncedHhMask) {
                g_lastSyncedHhMask = nextHhMask;
                hhChanged = true;
            }
        }
        if (hhChanged) {
            SyncHeadhunterMaskIfChanged(nextHhMask);
        }

        // 2) 0.5초 주기로 고프로세스 적대 액터 선승격(워커 스레드 락/엔진 API 부하 회피)
        if (!g_config || !g_config->enableMonsterPromotion) {
            return;
        }
        g_scanAccumSec += deltaSec;
        if (g_scanAccumSec < 0.5) {
            return;
        }
        g_scanAccumSec = 0.0;

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!player || !lists || player->IsDead()) {
            return;
        }

        for (const auto& handle : lists->highActorHandles) {
            auto actorPtr = handle.get();
            auto* actor = actorPtr.get();
            if (!IsEligibleForPromotion(player, actor)) {
                continue;
            }
            const auto fid = actor->GetFormID();
            {
                std::scoped_lock lk(g_mutex);
                if (g_rolledActors.contains(fid)) {
                    continue;
                }
                g_rolledActors.insert(fid);
            }

            const bool isDragon = IsDragonActor(actor);
            const bool isBoss = IsBossActor(actor);
            const auto* av = actor->AsActorValueOwner();
            const double maxHp = std::max(10.0, static_cast<double>(
                av ? av->GetPermanentActorValue(RE::ActorValue::kHealth) : 100.0f));
            auto prof = Core::MonsterModSystem::RollPromotion(isBoss, isDragon, maxHp, *g_config, MonsterRng());
            if (prof.rarity != Core::MonsterRarity::Normal) {
                {
                    std::scoped_lock lk(g_mutex);
                    g_profiles[fid] = prof;
                }
                if (prof.HasMod(Core::MonsterModKind::Hasted)) {
                    ApplyHasteSpeedIfNeeded(actor);
                }
            }
        }

        // 검수 m4: 사망/TESResetEvent 없이 언로드·소멸된 액터가 누적되지 않도록 주기적 청소
        std::vector<RE::FormID> toInspect;
        {
            std::scoped_lock lk(g_mutex);
            if (g_profiles.size() > 256 || g_rolledActors.size() > 1024) {
                toInspect.reserve(g_rolledActors.size());
                for (const auto fid : g_rolledActors) {
                    toInspect.push_back(fid);
                }
            }
        }
        if (!toInspect.empty()) {
            std::vector<RE::FormID> stale;
            for (const auto fid : toInspect) {
                const auto* a = RE::TESForm::LookupByID<RE::Actor>(fid);
                if (!a || a->IsDeleted() || a->IsDead()) {
                    stale.push_back(fid);
                }
            }
            if (!stale.empty()) {
                // M-2: stale 정리 전 Hasted 속도 원복을 먼저 수행(g_mutex 밖에서 엔진 호출)
                for (const auto fid : stale) {
                    RevertHasteSpeedByFormId(fid);
                }
                std::scoped_lock lk(g_mutex);
                for (const auto fid : stale) {
                    g_profiles.erase(fid);
                    g_rolledActors.erase(fid);
                    g_appliedHasteDelta.erase(fid);
                }
            }
        }
    }

    std::optional<Core::MonsterProfile> MonsterRuntime::GetProfileCopy(RE::FormID actorId)
    {
        std::scoped_lock lk(g_mutex);
        const auto it = g_profiles.find(actorId);
        if (it == g_profiles.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    Core::MonsterIncomingResult MonsterRuntime::ApplyPlayerHitToMonster(
        RE::FormID actorId,
        double rawSkyrimDamage,
        Core::DamageType dominantType,
        bool bypassesEs)
    {
        std::scoped_lock lk(g_mutex);
        const auto it = g_profiles.find(actorId);
        if (it == g_profiles.end()) {
            Core::MonsterIncomingResult plain{};
            plain.remainingHealthDamage = std::max(0.0, std::isfinite(rawSkyrimDamage) ? rawSkyrimDamage : 0.0);
            plain.hadAnyImpact = plain.remainingHealthDamage > 0.0;
            return plain;
        }
        return Core::MonsterModSystem::ApplyPlayerHitToMonster(it->second, rawSkyrimDamage, dominantType, bypassesEs);
    }

    Core::MonsterOutgoingModifier MonsterRuntime::GetOutgoingModifier(RE::FormID actorId)
    {
        std::scoped_lock lk(g_mutex);
        const auto it = g_profiles.find(actorId);
        if (it == g_profiles.end()) {
            return {};
        }
        return Core::MonsterModSystem::ComputeOutgoingModifier(it->second);
    }

    Core::MonsterProfile MonsterRuntime::OnMonsterKilled(RE::FormID actorId, bool playerHasHeadhunter)
    {
        RevertHasteSpeedByFormId(actorId);
        Core::MonsterProfile prof{};
        bool hhChanged = false;
        std::uint16_t nextHhMask = 0;
        {
            std::scoped_lock lk(g_mutex);
            if (const auto it = g_profiles.find(actorId); it != g_profiles.end()) {
                prof = it->second;
                g_profiles.erase(it);
            }
            g_rolledActors.erase(actorId);

            if (playerHasHeadhunter && prof.rarity == Core::MonsterRarity::Rare && prof.modMask != 0) {
                g_hhBuffs.push_back(StolenHeadhunterBuff{ prof.modMask, kHeadhunterDurationSec });
                if (g_hhBuffs.size() > 16) {
                    g_hhBuffs.erase(g_hhBuffs.begin());
                }
                nextHhMask = ComputeCombinedHhMaskLocked();
                if (nextHhMask != g_lastSyncedHhMask) {
                    g_lastSyncedHhMask = nextHhMask;
                    hhChanged = true;
                }
            }
        }
        if (hhChanged) {
            SyncHeadhunterMaskIfChanged(nextHhMask);
        }
        return prof;
    }

    std::uint16_t MonsterRuntime::ActiveHeadhunterModMask() noexcept
    {
        std::scoped_lock lk(g_mutex);
        return g_lastSyncedHhMask;
    }

    std::size_t MonsterRuntime::ActiveHeadhunterStackCount() noexcept
    {
        std::scoped_lock lk(g_mutex);
        return g_hhBuffs.size();
    }

    std::size_t MonsterRuntime::PromotedActorCount() noexcept
    {
        std::scoped_lock lk(g_mutex);
        return g_profiles.size();
    }

    void MonsterRuntime::ForcePromoteForTest(
        RE::Actor* actor,
        Core::MonsterRarity rarity,
        std::uint16_t modMask,
        double customMaxEs)
    {
        if (!actor) return;
        Core::MonsterProfile prof{};
        prof.rarity = rarity;
        prof.modMask = modMask;
        if (customMaxEs > 0.0) {
            prof.maxEsSkyrim = customMaxEs;
            prof.currentEsSkyrim = customMaxEs;
        } else if (prof.HasMod(Core::MonsterModKind::EnergyShielded)) {
            const auto* av = actor->AsActorValueOwner();
            const double hp = std::max(10.0, static_cast<double>(
                av ? av->GetPermanentActorValue(RE::ActorValue::kHealth) : 100.0f));
            const double ratio = g_config ? g_config->monsterEsRatio : 0.35;
            prof.maxEsSkyrim = hp * ratio * (rarity == Core::MonsterRarity::Rare ? 1.35 : 1.0);
            prof.currentEsSkyrim = prof.maxEsSkyrim;
        }
        {
            std::scoped_lock lk(g_mutex);
            g_rolledActors.insert(actor->GetFormID());
            if (rarity == Core::MonsterRarity::Normal) {
                g_profiles.erase(actor->GetFormID());
            } else {
                g_profiles[actor->GetFormID()] = prof;
            }
        }
        // m-2: 강제 승격(AutoTest F6 등)에서도 Hasted 속도 경로를 실제 적용
        if (rarity != Core::MonsterRarity::Normal && prof.HasMod(Core::MonsterModKind::Hasted)) {
            ApplyHasteSpeedIfNeeded(actor);
        }
    }

    void MonsterRuntime::OnActorReset(RE::FormID actorId)
    {
        RevertHasteSpeedByFormId(actorId);
        std::scoped_lock lk(g_mutex);
        g_profiles.erase(actorId);
        g_rolledActors.erase(actorId);
    }

    std::vector<std::pair<RE::FormID, float>> MonsterRuntime::TakeActiveSpeedDeltasForSave()
    {
        std::scoped_lock lk(g_mutex);
        std::vector<std::pair<RE::FormID, float>> out;
        out.reserve(g_appliedHasteDelta.size());
        for (const auto& [fid, delta] : g_appliedHasteDelta) {
            if (fid != 0 && std::isfinite(delta) && std::abs(delta) >= 0.01f) {
                out.emplace_back(fid, delta);
            }
        }
        return out;
    }

    void MonsterRuntime::ResetOnRevert()
    {
        {
            std::scoped_lock lk(g_mutex);
            g_profiles.clear();
            g_rolledActors.clear();
            g_appliedHasteDelta.clear();
            g_hhBuffs.clear();
            g_lastSyncedHhMask = 0;
            g_scanAccumSec = 0.0;
        }
        SyncHeadhunterMaskIfChanged(0);
    }
}
