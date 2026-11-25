#include "PCH.h"
#include "runtime/MonsterRuntime.h"
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
#include <unordered_set>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        Core::StatEngine* g_statEngine = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::Config* g_config = nullptr;

        std::mutex g_mutex;
        std::unordered_map<RE::FormID, Core::MonsterProfile> g_profiles;
        // 결정 185: 마지막 교전 적. 위 32비트는 FormID, 아래 32비트는 EngagedClockMs() 시각(0이면 기록 없음).
        std::atomic<std::uint64_t> g_engagedEnemy{ 0 };

        // 처음 부른 때부터의 밀리초(32비트, 약 49일마다 돌아감. 나이는 부호 없는 뺄셈이라 돌아가도 맞음)
        std::uint32_t EngagedClockMs() noexcept
        {
            static const auto base = std::chrono::steady_clock::now();
            return static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - base).count());
        }

        // actorId가 지금 기록된 교전 적이면 지운다(사망·개별 리셋)
        void ClearEngagedEnemyIf(RE::FormID actorId) noexcept
        {
            auto current = g_engagedEnemy.load(std::memory_order_acquire);
            while ((current >> 32) == actorId && current != 0) {
                if (g_engagedEnemy.compare_exchange_weak(current, 0, std::memory_order_acq_rel)) {
                    break;
                }
            }
        }
        std::unordered_set<RE::FormID> g_rolledActors;
        std::unordered_map<RE::FormID, float> g_appliedHasteDelta;  // N-2: Hasted SpeedMult 추적(원복 보장)
        std::vector<StolenHeadhunterBuff> g_hhBuffs;
        std::uint16_t g_lastSyncedHhMask = 0;
        double g_scanAccumSec = 0.0;

        void ApplyHasteSpeedIfNeeded(RE::Actor* actor, float speedBonus = 20.0f)
        {
            if (!actor || actor->IsPlayerRef() || actor->IsDead() || actor->IsDeleted()) {
                return;
            }
            const auto fid = actor->GetFormID();
            const float bonus = std::clamp(speedBonus, 5.0f, 50.0f);
            {
                std::scoped_lock lk(g_mutex);
                if (g_appliedHasteDelta.contains(fid)) {
                    return;
                }
                g_appliedHasteDelta[fid] = bonus;
            }
            if (auto* av = actor->AsActorValueOwner()) {
                av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, bonus);
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
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(fid);
            if (!actor) {
                // 재검수 R-9: 지금 찾을 수 없는 액터의 가속분을 버리면 세이브에 영구히 남는다. 맡겨서 다시 시도한다.
                SaveLoad::DeferSpeedRevert(fid, prev);
                return;
            }
            {
                if (!actor->IsPlayerRef() && !actor->IsDeleted()) {
                    const float curTemp = actor->GetActorValueModifier(
                        RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult);
                    // M-2: 엔진 리셋으로 이미 0이면 생략하되, 냉각/빙결(-95)과 공존해 curTemp < 0이어도 걸었던 가속분(prev)을 정확히 차감한다.
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
        const Core::DataPack* dataPack,
        const Core::Config* config)
    {
        g_statEngine = statEngine;
        g_itemData = itemData;
        g_dataPack = dataPack;
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
            Core::MapModSummary mapMods{};
            const bool mapOwned = MapDeviceRuntime::GetMapActorScaling(fid, &mapMods).runId != 0;
            Core::Config effCfg = *g_config;
            if (mapOwned && mapMods.moreMagicRarePct > 0.0) {
                const double boost = 1.0 + mapMods.moreMagicRarePct / 100.0;
                effCfg.magicMonsterChance = std::clamp(effCfg.magicMonsterChance * boost, 0.0, 0.80);
                effCfg.rareMonsterChance = std::clamp(effCfg.rareMonsterChance * boost, 0.0, 0.45);
            }
            auto prof = Core::MonsterModSystem::RollPromotion(isBoss, isDragon, maxHp, effCfg, MonsterRng());
            if (prof.rarity != Core::MonsterRarity::Normal) {
                if (mapOwned && mapMods.forceHasted) {
                    prof.modMask |= static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted));
                }
                if (mapOwned && mapMods.forceDeadly) {
                    prof.modMask |= static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Deadly));
                }
                {
                    std::scoped_lock lk(g_mutex);
                    g_profiles[fid] = prof;
                }
                if (prof.HasMod(Core::MonsterModKind::Hasted)) {
                    const float hasteVal = (mapOwned && mapMods.monsterHasteSpeedBonus > 0.0)
                        ? static_cast<float>(mapMods.monsterHasteSpeedBonus)
                        : 20.0f;
                    ApplyHasteSpeedIfNeeded(actor, hasteVal);
                }
            } else if (mapOwned && mapMods.forceHasted) {
                // 6단계(검수 M-4): 미승격 일반 지도 몹에도 MapMod_MonsterHaste 속도 적용
                const float hasteVal = (mapMods.monsterHasteSpeedBonus > 0.0)
                    ? static_cast<float>(mapMods.monsterHasteSpeedBonus)
                    : 20.0f;
                ApplyHasteSpeedIfNeeded(actor, hasteVal);
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

    void MonsterRuntime::NoteEngagedEnemy(RE::FormID actorId) noexcept
    {
        if (actorId == 0) {
            return;
        }
        g_engagedEnemy.store((static_cast<std::uint64_t>(actorId) << 32) | EngagedClockMs(), std::memory_order_release);
    }

    MonsterRuntime::EngagedEnemy MonsterRuntime::GetLastEngagedEnemy() noexcept
    {
        EngagedEnemy out;
        const auto packed = g_engagedEnemy.load(std::memory_order_acquire);
        out.actorId = static_cast<RE::FormID>(packed >> 32);
        if (out.actorId != 0) {
            const auto stampMs = static_cast<std::uint32_t>(packed & 0xFFFFFFFFu);
            out.ageSec = static_cast<double>(static_cast<std::uint32_t>(EngagedClockMs() - stampMs)) / 1000.0;
        }
        return out;
    }

    std::vector<MonsterRuntime::EnergyShieldEntry> MonsterRuntime::ListEnergyShieldProfiles()
    {
        std::vector<EnergyShieldEntry> out;
        std::scoped_lock lk(g_mutex);
        out.reserve(g_profiles.size());
        for (const auto& [actorId, profile] : g_profiles) {
            if (profile.maxEsSkyrim > 0.0) {
                out.push_back({ actorId, profile.currentEsSkyrim, profile.maxEsSkyrim });
            }
        }
        return out;
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
        bool bypassesEs,
        RE::Actor* targetActor,
        double engineDiffMult,
        std::optional<std::uint64_t> generation,
        const Core::DamagePacket* hitTypes,
        bool isDamageOverTime,
        bool isAttack,
        const Core::StatSnapshot* hitSnapshot)
    {
        double adjustedDmg = std::max(0.0, std::isfinite(rawSkyrimDamage) ? rawSkyrimDamage : 0.0);
        const auto current = !hitSnapshot && g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
        const auto* snap = hitSnapshot ? hitSnapshot : current.get();
        Core::MonsterHitResistance resistance;
        resistance.poe2 = snap && snap->poe2Passives;
        resistance.isDamageOverTime = isDamageOverTime;
        if (resistance.poe2 && !isDamageOverTime) {
            resistance.hitTypes = hitTypes;
            resistance.penetrationPct = snap->penetrationPct;
            if (isAttack) {
                for (const auto type : { Core::DamageType::Fire, Core::DamageType::Cold, Core::DamageType::Lightning }) {
                    resistance.penetrationPct[static_cast<std::size_t>(type)] += snap->attackElementalPenetrationPct;
                }
            }
        }

        // 결정 170: 회차 소유 액터는 ES 흡수와 희귀도 피해 보정 전에 생명 배율과 지도 어픽스를 적용한다.
        Core::MapModSummary mods{};
        const auto scaling = MapDeviceRuntime::GetMapActorScaling(actorId, &mods);
        if (adjustedDmg > 0.0 && scaling.runId != 0) {
            const auto* values = targetActor ? targetActor->AsActorValueOwner() : nullptr;
            const double health = values ? values->GetPermanentActorValue(RE::ActorValue::kHealth) : 0.0;
            if (!snap || !g_dataPack || !g_config || !std::isfinite(health) || health < 1.0) {
                SKSE::log::debug("Map actor {:08X}: health matching unavailable (max health={}); using tier scaling.",
                    actorId, health);
                adjustedDmg = scaling.ScaleIncomingDamage(adjustedDmg);
            } else {
                const double referenceLife = Core::OffenseReferenceLife(snap->exileLevel, *g_dataPack, *g_config);
                adjustedDmg = scaling.ScaleIncomingDamage(adjustedDmg, referenceLife, health);
            }
            if (mods.monsterReducedTakenPct > 0.0) {
                adjustedDmg *= std::max(0.25, 1.0 - mods.monsterReducedTakenPct / 100.0);
            }
            if (resistance.poe2) {
                resistance.mapElementalPct = mods.monsterElemResPct;
                resistance.mapChaosPct = mods.monsterChaosResPct;
            } else if ((dominantType == Core::DamageType::Fire ||
                 dominantType == Core::DamageType::Cold ||
                 dominantType == Core::DamageType::Lightning) &&
                mods.monsterElemResPct > 0.0) {
                adjustedDmg *= std::max(0.25, 1.0 - mods.monsterElemResPct / 100.0);
            } else if (dominantType == Core::DamageType::Chaos && mods.monsterChaosResPct > 0.0) {
                adjustedDmg *= std::max(0.25, 1.0 - mods.monsterChaosResPct / 100.0);
            }
        }

        Core::MonsterIncomingResult res{};
        {
            std::scoped_lock lk(g_mutex);
            if (MapDeviceRuntime::IsUninstallCleanupActive()) {
                res.remainingHealthDamage = std::max(0.0, std::isfinite(rawSkyrimDamage) ? rawSkyrimDamage : 0.0);
                res.hadAnyImpact = res.remainingHealthDamage > 0.0;
                return res;
            }
            const auto it = g_profiles.find(actorId);
            if (it == g_profiles.end()) {
                Core::MonsterProfile unpromoted;
                res = Core::MonsterModSystem::ApplyPlayerHitToMonster(unpromoted, adjustedDmg, dominantType, bypassesEs, resistance);
            } else {
                res = Core::MonsterModSystem::ApplyPlayerHitToMonster(it->second, adjustedDmg, dominantType, bypassesEs, resistance);
            }
        }

        // 6단계(배치2 검수 Critical #1): 호출부가 전달한 실제 엔진 난이도 배율(직접 피해·CombatHooks 모두 fDiffMultHPByPC, 결정 53)과
        // 직접 RE::Actor* 포인터를 사용해 이중 역보정 및 워커 스레드 LookupByID 전역 폼 락을 제거한다.
        if (res.remainingHealthDamage > 0.0 && targetActor && MapDeviceRuntime::IsActiveArena4Boss(actorId)) {
            res.remainingHealthDamage = MapDeviceRuntime::FilterArena4BossIncomingDamage(
                targetActor, res.remainingHealthDamage, engineDiffMult, generation);
        }
        return res;
    }

    Core::MonsterOutgoingModifier MonsterRuntime::GetOutgoingModifier(RE::FormID actorId)
    {
        Core::MonsterOutgoingModifier out{};
        {
            std::scoped_lock lk(g_mutex);
            if (const auto it = g_profiles.find(actorId); it != g_profiles.end()) {
                out = Core::MonsterModSystem::ComputeOutgoingModifier(it->second);
            }
        }
        // 결정 170: 희귀도와 무관하게 공격자 소속 회차의 피해 배율과 지도 어픽스를 적용한다.
        Core::MapModSummary mods{};
        const auto scaling = MapDeviceRuntime::GetMapActorScaling(actorId, &mods);
        if (scaling.runId != 0) {
            out.damageMultiplier *= scaling.damageMultiplier;
            if (mods.monsterDamageIncPct > 0.0) {
                out.damageMultiplier *= (1.0 + mods.monsterDamageIncPct / 100.0);
            }
            if (mods.monsterExtraFirePct > 0.0) {
                out.extraFireRatio += mods.monsterExtraFirePct / 100.0;
            }
            if (mods.monsterExtraColdPct > 0.0) {
                out.extraColdRatio += mods.monsterExtraColdPct / 100.0;
            }
            if (mods.monsterExtraLightningPct > 0.0) {
                out.extraLightningRatio += mods.monsterExtraLightningPct / 100.0;
            }
            if (mods.forceDeadly) {
                out.isDeadly = true;
            }
            out.critChanceIncPct = mods.monsterCritChanceIncPct;
        }
        return out;
    }

    Core::MonsterProfile MonsterRuntime::OnMonsterKilled(RE::FormID actorId, bool playerHasHeadhunter)
    {
        RevertHasteSpeedByFormId(actorId);
        ClearEngagedEnemyIf(actorId);
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
        ClearEngagedEnemyIf(actorId);
        std::scoped_lock lk(g_mutex);
        g_profiles.erase(actorId);
        g_rolledActors.erase(actorId);
    }

    void MonsterRuntime::RevertAllHasteSpeedsNow()
    {
        std::vector<RE::FormID> fids;
        {
            std::scoped_lock lk(g_mutex);
            fids.reserve(g_appliedHasteDelta.size());
            for (const auto& [fid, _] : g_appliedHasteDelta) {
                fids.push_back(fid);
            }
        }
        for (const auto fid : fids) {
            RevertHasteSpeedByFormId(fid);
        }
        std::scoped_lock lk(g_mutex);
        g_profiles.clear();
        g_rolledActors.clear();
        g_hhBuffs.clear();
        g_lastSyncedHhMask = 0;
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
            g_engagedEnemy.store(0, std::memory_order_release);
            g_lastSyncedHhMask = 0;
            g_scanAccumSec = 0.0;
        }
        SyncHeadhunterMaskIfChanged(0);
    }
}
