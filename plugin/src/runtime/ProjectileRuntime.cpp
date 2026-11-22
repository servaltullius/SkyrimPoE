#include "PCH.h"
#include "runtime/ProjectileRuntime.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/CombatHooks.h"
#include "runtime/FormIds.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numbers>
#include <random>
#include <unordered_map>
#include <vector>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr std::size_t kMaxTrackedShots = 256;
        constexpr std::uint8_t kMaxGeneration = 6;
        constexpr std::uint8_t kMaxAllyPassHops = 4;
        constexpr float kRelaunchLift = 35.0f;
        constexpr float kBaseNovaRadius = 350.0f;
        constexpr float kFireballSplashRadius = 210.0f;
        constexpr float kChainSearchRadius = 700.0f;
        constexpr float kArcInitialRange = 1600.0f;

        struct TrackedShot
        {
            std::uint64_t shotId = 0;
            RE::FormID projLocalId = 0;
            RE::ProjectileHandle handle{};
            Core::TriggeredSpellCast cast{};
            std::uint8_t remainingPierce = 0;
            std::uint8_t remainingChain = 0;
            bool canFork = false;
            bool alwaysPierce = false;
            std::uint8_t generation = 0;
            std::uint8_t allyPassHops = 0;
            bool isAimedPrimary = true;
            std::vector<RE::FormID> alreadyHit;
            double elapsedUnpausedSec = 0.0;
            bool impacted = false;
        };

        struct PendingRepeatCast
        {
            Core::TriggeredSpellCast cast{};
            double remainingSec = 0.18;
        };

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        Core::SkillSystem* g_skillSystem = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::Config* g_config = nullptr;

        std::mutex g_shotMutex;
        std::unordered_map<std::uint32_t, std::uint64_t> g_handleToShotId;
        std::unordered_map<std::uint64_t, TrackedShot> g_shots;
        std::vector<PendingRepeatCast> g_pendingRepeats;
        std::uint64_t g_nextShotId = 1;

        std::atomic<std::uint64_t> g_totalLaunched{ 0 };
        std::atomic<std::uint64_t> g_totalImpacts{ 0 };
        std::atomic<std::uint64_t> g_totalRelaunches{ 0 };
        bool g_hookInstalled = false;

        std::mt19937_64& ProjRng()
        {
            static std::mt19937_64 rng(static_cast<std::uint64_t>(
                Clock::now().time_since_epoch().count()) ^ 0x99887766ULL);
            return rng;
        }

        float VecLen(const RE::NiPoint3& v) noexcept
        {
            return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        }

        RE::NiPoint3 NormalizeVec(const RE::NiPoint3& v) noexcept
        {
            const float len = VecLen(v);
            return (len > 1e-4f) ? RE::NiPoint3{ v.x / len, v.y / len, v.z / len } : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
        }

        RE::NiPoint3 RotateYaw(const RE::NiPoint3& dir, float yawRad) noexcept
        {
            const float c = std::cos(yawRad);
            const float s = std::sin(yawRad);
            return NormalizeVec(RE::NiPoint3{
                dir.x * c + dir.y * s,
                -dir.x * s + dir.y * c,
                dir.z
            });
        }

        RE::NiPoint3 ActorAimCenter(RE::Actor* actor) noexcept
        {
            if (!actor) return {};
            auto pos = actor->GetPosition();
            const auto lo = actor->GetBoundMin();
            const auto hi = actor->GetBoundMax();
            pos.z += std::max(28.0f, (lo.z + hi.z) * 0.5f);
            return pos;
        }

        float ActorHorizontalClearance(RE::Actor* actor) noexcept
        {
            if (!actor) return 60.0f;
            const auto lo = actor->GetBoundMin();
            const auto hi = actor->GetBoundMax();
            const float r = std::clamp(std::max(hi.x - lo.x, hi.y - lo.y) * 0.5f, 20.0f, 110.0f);
            return r * 2.0f + 30.0f;
        }

        Core::DamageType DominantDamageTypeOf(const Core::DamagePacket& pkt) noexcept
        {
            Core::DamageType best = Core::DamageType::Physical;
            double bestVal = -1.0;
            for (std::size_t i = 0; i < static_cast<std::size_t>(Core::DamageType::Count); ++i) {
                const double avg = pkt.byType[i].Average();
                if (avg > bestVal) {
                    bestVal = avg;
                    best = static_cast<Core::DamageType>(i);
                }
            }
            return best;
        }

        RE::FormID ProjectileLocalIdForGem(std::string_view gemId) noexcept
        {
            if (gemId == "Fireball") return FormIds::kProjFireball;
            if (gemId == "FreezingPulse") return FormIds::kProjFreezingPulse;
            if (gemId == "Spark") return FormIds::kProjSpark;
            // C-25: 알 수 없는 투사체 젬은 Fireball로 무음 대체하지 않고 0 반환
            return 0;
        }

        void LaunchSingleMissile(
            RE::FormID projLocalId,
            const RE::NiPoint3& origin,
            const RE::NiPoint3& dir,
            RE::Actor* shooter,
            const Core::TriggeredSpellCast& cast,
            std::uint8_t remPierce,
            std::uint8_t remChain,
            bool canFork,
            bool alwaysPierce,
            std::uint8_t generation,
            std::uint8_t allyPassHops,
            bool isAimedPrimary,
            std::vector<RE::FormID> alreadyHit)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* baseProj = (projLocalId != 0) ? FormIds::Lookup<RE::BGSProjectile>(projLocalId) : nullptr;
            if (!player || !baseProj) {
                return;
            }
            {
                std::scoped_lock lk(g_shotMutex);
                if (g_shots.size() >= kMaxTrackedShots) {
                    return;
                }
            }

            const auto ndir = NormalizeVec(dir);
            const RE::Projectile::ProjectileRot angles{
                -std::asin(std::clamp(ndir.z, -1.0f, 1.0f)),
                std::atan2(ndir.x, ndir.y)
            };
            RE::ProjectileHandle handle{};
            RE::Projectile::LaunchData data(baseProj, shooter ? shooter : player, origin, angles);
            data.useOrigin = true;
            data.autoAim = false;
            RE::Projectile::Launch(&handle, data);
            if (!handle) {
                return;
            }

            TrackedShot shot{};
            shot.projLocalId = projLocalId;
            shot.handle = handle;
            shot.cast = cast;
            shot.remainingPierce = remPierce;
            shot.remainingChain = remChain;
            shot.canFork = canFork;
            shot.alwaysPierce = alwaysPierce;
            shot.generation = generation;
            shot.allyPassHops = allyPassHops;
            shot.isAimedPrimary = isAimedPrimary;
            shot.alreadyHit = std::move(alreadyHit);
            shot.elapsedUnpausedSec = 0.0;

            {
                std::scoped_lock lk(g_shotMutex);
                shot.shotId = g_nextShotId++;
                g_handleToShotId[handle.native_handle()] = shot.shotId;
                g_shots[shot.shotId] = std::move(shot);
            }
            g_totalLaunched.fetch_add(1, std::memory_order_relaxed);
            if (generation > 0) {
                g_totalRelaunches.fetch_add(1, std::memory_order_relaxed);
            }
        }

        [[nodiscard]] bool HasElementalDamage(const Core::DamagePacket& pkt) noexcept
        {
            return pkt.byType[static_cast<std::size_t>(Core::DamageType::Fire)].max > 0.0 ||
                   pkt.byType[static_cast<std::size_t>(Core::DamageType::Cold)].max > 0.0 ||
                   pkt.byType[static_cast<std::size_t>(Core::DamageType::Lightning)].max > 0.0;
        }

        // 검수 M7: Arc 전용 3D 구형 탐색(hop 0 직격은 비적대 선제공격·고도차 150u 초과 허용, 연쇄 hop>=1은 적대만)
        [[nodiscard]] std::vector<RE::NiPointer<RE::Actor>> CollectArcCandidates3D(
            RE::PlayerCharacter* player,
            const RE::NiPoint3& origin,
            float radius,
            bool isDirectHit)
        {
            std::vector<RE::NiPointer<RE::Actor>> out;
            auto* lists = RE::ProcessLists::GetSingleton();
            if (!player || !lists || radius <= 0.0f) {
                return out;
            }
            const float radiusSq = radius * radius;
            for (const auto& handle : lists->highActorHandles) {
                auto ptr = handle.get();
                auto* actor = ptr.get();
                if (!actor || !SkillExecution::CanDamageTarget(player, actor, isDirectHit)) {
                    continue;
                }
                const auto d = ActorAimCenter(actor) - origin;
                if ((d.x * d.x + d.y * d.y + d.z * d.z) <= radiusSq) {
                    out.push_back(std::move(ptr));
                }
            }
            return out;
        }

        void ExecuteArcInstantChain(
            RE::PlayerCharacter* player,
            const Core::TriggeredSpellCast& cast,
            const RE::NiPoint3& aimOrigin,
            const RE::NiPoint3& aimDir)
        {
            if (!player || !g_dataPack || !g_config) return;
            const auto ndir = NormalizeVec(aimDir);
            const auto directPool = CollectArcCandidates3D(player, aimOrigin, kArcInitialRange, true);
            if (directPool.empty()) {
                SkillExecution::SpawnHarmlessVfx(aimOrigin + ndir * 260.0f, FormIds::kVfxShockNova);
                return;
            }

            // 조준선과 가장 잘 맞는 첫 표적 선택(전방 원뿔 내 비적대 선제공격·고도차 포함 허용)
            RE::Actor* firstTarget = nullptr;
            double bestScore = -1e9;
            for (const auto& h : directPool) {
                if (!h) continue;
                const auto toTarget = ActorAimCenter(h.get()) - aimOrigin;
                const float dist = VecLen(toTarget);
                if (dist < 1.0f || dist > kArcInitialRange) continue;
                const auto dirTo = NormalizeVec(toTarget);
                const float dot = dirTo.x * ndir.x + dirTo.y * ndir.y + dirTo.z * ndir.z;
                const bool hostile = h->IsHostileToActor(player);
                // 비적대 대상은 명확한 정면 조준(dot >= 0.85, 약 31° 원뿔)일 때만 선제공격 허용
                const float minDot = hostile ? 0.15f : 0.85f;
                if (dot < minDot) continue;
                const double score = static_cast<double>(dot) * 1000.0 - static_cast<double>(dist) * 0.35 + (hostile ? 80.0 : 0.0);
                if (score > bestScore) {
                    bestScore = score;
                    firstTarget = h.get();
                }
            }
            if (!firstTarget) {
                // 전방 원뿔 밖이면 가장 가까운 적대 대상만 폴백(비적대 오폭 방지)
                float minDist = kArcInitialRange;
                for (const auto& h : directPool) {
                    if (!h || !h->IsHostileToActor(player)) continue;
                    const float d = VecLen(ActorAimCenter(h.get()) - aimOrigin);
                    if (d < minDist) {
                        minDist = d;
                        firstTarget = h.get();
                    }
                }
            }
            if (!firstTarget) {
                SkillExecution::SpawnHarmlessVfx(aimOrigin + ndir * 260.0f, FormIds::kVfxShockNova);
                return;
            }

            std::uniform_real_distribution<double> u01(0.0, 1.0);
            std::vector<RE::FormID> hitIds;
            RE::Actor* current = firstTarget;
            const std::uint8_t totalChains = cast.chainCount;
            const auto domType = DominantDamageTypeOf(cast.poeBaseHitDamage);
            const bool elemEligible = HasElementalDamage(cast.poeBaseHitDamage);

            for (std::uint8_t hop = 0; hop <= totalChains && current; ++hop) {
                hitIds.push_back(current->GetFormID());
                const std::uint8_t remainingChains = static_cast<std::uint8_t>(totalChains - hop);
                const double arcMore = 1.0 + static_cast<double>(remainingChains) * (cast.arcRemainingChainMorePct / 100.0);

                bool isCrit = false;
                const double baseSkyrimDmg = Core::SkillSystem::RollTriggeredSpellSkyrimDamage(
                    cast, *g_dataPack, *g_config, u01(ProjRng()), u01(ProjRng()), isCrit);
                const double finalDmg = baseSkyrimDmg * arcMore;

                SkillExecution::SpawnHarmlessVfx(ActorAimCenter(current), FormIds::kVfxShockNova);
                if (SkillExecution::DealDirectSkillDamage(current, finalDmg, domType, hop == 0, domType == Core::DamageType::Chaos)) {
                    (void)AilmentRuntime::ApplyPlayerHitAilments(
                        current, cast.poeBaseHitDamage, isCrit, AilmentRuntime::HitKind::Spell, cast.igniteChancePct, cast.starforgeShock);
                    if (isCrit && elemEligible) {
                        CombatHooks::NotifyPlayerElementalCrit();
                    }
                }

                if (remainingChains == 0) break;

                // 다음 연쇄 대상 탐색(이미 맞힌 대상 제외, 연쇄는 적대 대상만 + 3D 거리)
                const auto curPos = ActorAimCenter(current);
                const auto nextPool = CollectArcCandidates3D(player, curPos, kChainSearchRadius, false);
                RE::Actor* nextTarget = nullptr;
                float bestDist = kChainSearchRadius;
                for (const auto& cand : nextPool) {
                    if (!cand) continue;
                    if (std::find(hitIds.begin(), hitIds.end(), cand->GetFormID()) != hitIds.end()) continue;
                    const float d = VecLen(ActorAimCenter(cand.get()) - curPos);
                    if (d < bestDist) {
                        bestDist = d;
                        nextTarget = cand.get();
                    }
                }
                current = nextTarget;
            }
        }

        void ResolveImpactOnMainThread(
            std::uint64_t shotId,
            RE::ObjectRefHandle hitHandle,
            RE::NiPoint3 location,
            RE::NiPoint3 backwardVel)
        {
            TrackedShot shot{};
            {
                std::scoped_lock lk(g_shotMutex);
                const auto it = g_shots.find(shotId);
                if (it == g_shots.end()) {
                    return;
                }
                shot = std::move(it->second);
                g_handleToShotId.erase(shot.handle.native_handle());
                g_shots.erase(it);
            }

            if (!g_dataPack || !g_config) {
                return;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || player->IsDead()) {
                return;
            }

            auto refPtr = hitHandle.get();
            auto* ref = refPtr.get();
            auto* hitActor = ref ? ref->As<RE::Actor>() : nullptr;

            // 프로토타입 6 핵심 규약: AddImpact 3번째 인자는 진행 방향 반대 벡터이므로 반드시 부호 반전!
            const auto travelDir = NormalizeVec(RE::NiPoint3{ -backwardVel.x, -backwardVel.y, -backwardVel.z });

            // 결정 111(C-21): 아군·비조준(GMP 옆 탄·재발사) 비적대 통과 — 피해 0, 관통/분기/연쇄 미소모
            if (hitActor && !hitActor->IsDead()) {
                const bool isAlly = SkillExecution::IsAllyOfPlayer(player, hitActor);
                const bool isIndirectNonHostile = (shot.generation > 0 || !shot.isAimedPrimary) && !hitActor->IsHostileToActor(player);
                if (isAlly || isIndirectNonHostile) {
                    if (shot.allyPassHops < kMaxAllyPassHops && shot.generation < kMaxGeneration) {
                        const float clearance = ActorHorizontalClearance(hitActor);
                        auto nextOrigin = location + travelDir * clearance;
                        nextOrigin.z += kRelaunchLift * 0.4f;
                        LaunchSingleMissile(
                            shot.projLocalId, nextOrigin, travelDir, hitActor, shot.cast,
                            shot.remainingPierce, shot.remainingChain, shot.canFork, shot.alwaysPierce,
                            shot.generation, static_cast<std::uint8_t>(shot.allyPassHops + 1),
                            shot.isAimedPrimary,
                            std::move(shot.alreadyHit));
                    }
                    return;
                }
            }

            // 이미 맞힌 대상이면 중복 피해 차단
            if (hitActor && std::find(shot.alreadyHit.begin(), shot.alreadyHit.end(), hitActor->GetFormID()) != shot.alreadyHit.end()) {
                return;
            }

            std::uniform_real_distribution<double> u01(0.0, 1.0);
            bool isCrit = false;
            const double rolledSkyrimDmg = Core::SkillSystem::RollTriggeredSpellSkyrimDamage(
                shot.cast, *g_dataPack, *g_config, u01(ProjRng()), u01(ProjRng()), isCrit);
            const auto domType = DominantDamageTypeOf(shot.cast.poeBaseHitDamage);

            const bool elemEligible = HasElementalDamage(shot.cast.poeBaseHitDamage);
            bool anyElemCritHit = false;

            // 화염구 적중 시 지형·적 모두 소형 화염 폭발 연출 + 범위 스플래시
            if (shot.cast.gemId == "Fireball") {
                const float splashRadius = static_cast<float>(kFireballSplashRadius * std::clamp(shot.cast.areaMult, 0.5, 2.5));
                SkillExecution::SpawnHarmlessVfx(location, FormIds::kVfxFireBurst);
                const auto splashTargets = SkillExecution::CollectHostileTargets(player, location, splashRadius);
                for (const auto& st : splashTargets) {
                    if (!st || (hitActor && st.get() == hitActor)) continue;
                    if (SkillExecution::DealDirectSkillDamage(st.get(), rolledSkyrimDmg, domType, false, domType == Core::DamageType::Chaos)) {
                        (void)AilmentRuntime::ApplyPlayerHitAilments(
                            st.get(), shot.cast.poeBaseHitDamage, isCrit, AilmentRuntime::HitKind::Spell, shot.cast.igniteChancePct, shot.cast.starforgeShock);
                        if (isCrit && elemEligible) {
                            anyElemCritHit = true;
                        }
                    }
                }
            }

            if (!hitActor || hitActor->IsDead()) {
                if (anyElemCritHit) {
                    CombatHooks::NotifyPlayerElementalCrit();
                }
                return;
            }

            if (SkillExecution::DealDirectSkillDamage(
                    hitActor, rolledSkyrimDmg, domType, shot.generation == 0 && shot.isAimedPrimary, domType == Core::DamageType::Chaos)) {
                (void)AilmentRuntime::ApplyPlayerHitAilments(
                    hitActor, shot.cast.poeBaseHitDamage, isCrit, AilmentRuntime::HitKind::Spell, shot.cast.igniteChancePct, shot.cast.starforgeShock);
                if (isCrit && elemEligible) {
                    anyElemCritHit = true;
                }
            }
            if (anyElemCritHit) {
                CombatHooks::NotifyPlayerElementalCrit();
            }

            if (shot.generation >= kMaxGeneration) {
                return;
            }

            auto nextHitList = std::move(shot.alreadyHit);
            nextHitList.push_back(hitActor->GetFormID());
            const float clearance = ActorHorizontalClearance(hitActor);

            // PoE1 재발사 우선순위: 관통(Pierce) > 분기(Fork) > 연쇄(Chain)
            // 프로토타입 6 3차 검증: 모든 재발사에 shooter = hitActor + clearance + lift 적용!
            if (shot.alwaysPierce || shot.remainingPierce > 0) {
                const std::uint8_t nextPierce = shot.alwaysPierce ? shot.remainingPierce : static_cast<std::uint8_t>(shot.remainingPierce - 1);
                auto nextOrigin = location + travelDir * clearance;
                nextOrigin.z += kRelaunchLift * 0.35f;
                LaunchSingleMissile(
                    shot.projLocalId, nextOrigin, travelDir, hitActor, shot.cast,
                    nextPierce, shot.remainingChain, shot.canFork, shot.alwaysPierce,
                    static_cast<std::uint8_t>(shot.generation + 1), shot.allyPassHops,
                    false,
                    std::move(nextHitList));
            } else if (shot.canFork) {
                constexpr float kForkAngleRad = static_cast<float>(std::numbers::pi / 3.0);  // ±60°
                for (float sign : { -1.0f, 1.0f }) {
                    const auto forkDir = RotateYaw(travelDir, sign * kForkAngleRad);
                    auto nextOrigin = location + forkDir * clearance;
                    nextOrigin.z += kRelaunchLift;
                    LaunchSingleMissile(
                        shot.projLocalId, nextOrigin, forkDir, hitActor, shot.cast,
                        0, shot.remainingChain, false, false,
                        static_cast<std::uint8_t>(shot.generation + 1), shot.allyPassHops,
                        false,
                        nextHitList);
                }
            } else if (shot.remainingChain > 0) {
                const auto nearby = SkillExecution::CollectHostileTargets(player, location, kChainSearchRadius);
                RE::Actor* bestChainTarget = nullptr;
                float bestDist = kChainSearchRadius;
                for (const auto& cand : nearby) {
                    if (!cand) continue;
                    if (std::find(nextHitList.begin(), nextHitList.end(), cand->GetFormID()) != nextHitList.end()) continue;
                    const float d = VecLen(ActorAimCenter(cand.get()) - location);
                    if (d < bestDist) {
                        bestDist = d;
                        bestChainTarget = cand.get();
                    }
                }
                if (bestChainTarget) {
                    const auto aimPt = ActorAimCenter(bestChainTarget);
                    const auto chainDir = NormalizeVec(aimPt - location);
                    auto nextOrigin = location + chainDir * clearance;
                    nextOrigin.z += kRelaunchLift;
                    LaunchSingleMissile(
                        shot.projLocalId, nextOrigin, chainDir, hitActor, shot.cast,
                        0, static_cast<std::uint8_t>(shot.remainingChain - 1), false, false,
                        static_cast<std::uint8_t>(shot.generation + 1), shot.allyPassHops,
                        false,
                        std::move(nextHitList));
                }
            }
        }

        struct MissileAddImpactHook
        {
            static constexpr std::size_t kIndex = 0xBD;

            static RE::Projectile::ImpactData* Thunk(
                RE::MissileProjectile* self,
                RE::TESObjectREFR* hitRef,
                const RE::NiPoint3& location,
                const RE::NiPoint3& velocity,
                RE::hkpCollidable* collidable,
                std::int32_t arg6,
                std::uint32_t arg7)
            {
                auto* result = original(self, hitRef, location, velocity, collidable, arg6, arg7);
                if (!self || !g_queue) {
                    return result;
                }

                const auto handleVal = self->GetHandle().native_handle();
                std::uint64_t matchedShotId = 0;
                {
                    std::scoped_lock lk(g_shotMutex);
                    const auto hitIt = g_handleToShotId.find(handleVal);
                    if (hitIt != g_handleToShotId.end()) {
                        const auto shotIt = g_shots.find(hitIt->second);
                        if (shotIt != g_shots.end() && !shotIt->second.impacted) {
                            shotIt->second.impacted = true;
                            matchedShotId = shotIt->second.shotId;
                        }
                    }
                }

                if (matchedShotId != 0) {
                    g_totalImpacts.fetch_add(1, std::memory_order_relaxed);
                    const RE::ObjectRefHandle refHandle = hitRef ? hitRef->GetHandle() : RE::ObjectRefHandle{};
                    const RE::NiPoint3 locCopy = location;
                    const RE::NiPoint3 velCopy = velocity;
                    g_queue->Enqueue([matchedShotId, refHandle, locCopy, velCopy]() {
                        ResolveImpactOnMainThread(matchedShotId, refHandle, locCopy, velCopy);
                    });
                }
                return result;
            }

            static inline REL::Relocation<decltype(Thunk)> original;
        };

        template <class Base, class Result, class... Args>
        auto MemberFnToThunk(Result (Base::*)(Args...)) -> Result (*)(Base*, Args...);
        static_assert(std::is_same_v<decltype(&MissileAddImpactHook::Thunk), decltype(MemberFnToThunk(&RE::MissileProjectile::AddImpact))>);
    }

    bool ProjectileRuntime::Install(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        Core::SkillSystem* skillSystem,
        const Core::DataPack* dataPack,
        const Core::Config* config)
    {
        g_queue = queue;
        g_statEngine = statEngine;
        g_skillSystem = skillSystem;
        g_dataPack = dataPack;
        g_config = config;

        if (!g_hookInstalled) {
            REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_MissileProjectile[0] };
            MissileAddImpactHook::original = vtable.write_vfunc(MissileAddImpactHook::kIndex, MissileAddImpactHook::Thunk);
            g_hookInstalled = true;
            SKSE::log::info("ProjectileRuntime: installed MissileProjectile::AddImpact (0xBD) hook.");
        }
        return true;
    }

    void ProjectileRuntime::ComputePlayerAim(
        RE::PlayerCharacter* player,
        RE::NiPoint3& outOrigin,
        RE::NiPoint3& outDir) noexcept
    {
        if (!player) {
            outOrigin = {};
            outDir = { 0.0f, 1.0f, 0.0f };
            return;
        }
        const float pitch = player->GetAngleX();
        const float yaw = player->GetAngleZ();
        const float cosP = std::cos(pitch);
        outDir = NormalizeVec(RE::NiPoint3{
            std::sin(yaw) * cosP,
            std::cos(yaw) * cosP,
            -std::sin(pitch)
        });
        outOrigin = player->GetPosition();
        outOrigin.z += 110.0f;
        outOrigin = outOrigin + outDir * 48.0f;
    }

    bool ProjectileRuntime::LaunchSpellDelivery(
        const Core::TriggeredSpellCast& cast,
        const RE::NiPoint3& aimOrigin,
        const RE::NiPoint3& aimDir)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || player->IsDead() || !g_dataPack || !g_config) {
            return false;
        }

        // SpellEcho 손 시전 반복 예약(1회 반복 시 0.18초 뒤 재실행)
        if (cast.isHandCast && cast.spellRepeatCount > 0) {
            Core::TriggeredSpellCast repeatCast = cast;
            repeatCast.spellRepeatCount = static_cast<std::uint8_t>(cast.spellRepeatCount - 1);
            std::scoped_lock lk(g_shotMutex);
            if (g_pendingRepeats.size() < 16) {
                g_pendingRepeats.push_back(PendingRepeatCast{ std::move(repeatCast), 0.18 });
            }
        }

        // 1) 범위 노바 주문(IceNova / ShockNova)
        if (cast.gemId == "IceNova" || cast.gemId == "ShockNova") {
            std::uniform_real_distribution<double> u01(0.0, 1.0);
            bool isCrit = false;
            const double rolledDmg = Core::SkillSystem::RollTriggeredSpellSkyrimDamage(
                cast, *g_dataPack, *g_config, u01(ProjRng()), u01(ProjRng()), isCrit);
            const RE::FormID vfxId = (cast.gemId == "ShockNova") ? FormIds::kVfxShockNova : FormIds::kVfxIceNova;
            const float radius = static_cast<float>(kBaseNovaRadius * std::clamp(cast.areaMult, 0.5, 2.5));
            const auto domType = DominantDamageTypeOf(cast.poeBaseHitDamage);
            const auto center = cast.isHandCast ? player->GetPosition() : aimOrigin;
            const auto targets = SkillExecution::CollectHostileTargets(player, center, radius);
            SkillExecution::SpawnHarmlessVfx(center, vfxId);
            for (const auto& t : targets) {
                if (SkillExecution::DealDirectSkillDamage(t.get(), rolledDmg, domType, false, domType == Core::DamageType::Chaos)) {
                    (void)AilmentRuntime::ApplyPlayerHitAilments(
                        t.get(), cast.poeBaseHitDamage, isCrit, AilmentRuntime::HitKind::Spell, cast.igniteChancePct, cast.starforgeShock);
                }
            }
            if (isCrit && !targets.empty() && HasElementalDamage(cast.poeBaseHitDamage)) {
                CombatHooks::NotifyPlayerElementalCrit();
            }
            return true;
        }

        // 2) 연쇄 번개(Arc): 즉발 표적 타격 + 연쇄
        if (cast.gemId == "Arc") {
            ExecuteArcInstantChain(player, cast, aimOrigin, aimDir);
            return true;
        }

        // 3) 투사체 주문(Fireball / FreezingPulse / Spark): 부채꼴 다중 발사
        const RE::FormID projLocalId = ProjectileLocalIdForGem(cast.gemId);
        if (projLocalId == 0) {
            SKSE::log::warn("ProjectileRuntime: unsupported projectile gemId '{}' — skipping launch.", cast.gemId);
            return false;
        }
        const std::uint8_t count = std::max<std::uint8_t>(1, cast.projectileCount);
        const float spreadStepRad = (cast.gemId == "Spark") ? 0.24f : 0.12f;
        // 결정 111(C-21, 재검수 R-8): 첫 발(i == 0)은 언제나 조준선 그대로 날아가는 조준탄이고, 나머지는 그 양옆으로
        // 0, -s, +s, -2s, +2s ... 순서로 벌린다. 홀수 발수는 예전과 같은 대칭 부채꼴이 되고, 짝수 발수도
        // 조준탄이 정확히 하나 있게 된다(예전 판정 |yaw| < 0.08은 Spark 짝수 발수에서 조준탄 0개, 간격 0.12 짝수에서 2개였음).
        for (std::uint8_t i = 0; i < count; ++i) {
            const int ring = (static_cast<int>(i) + 1) / 2;
            const float side = (i % 2 == 1) ? -1.0f : 1.0f;
            const float yawOff = side * static_cast<float>(ring) * spreadStepRad;
            const auto dir = RotateYaw(aimDir, yawOff);
            const bool isAimedPrimary = (i == 0);
            LaunchSingleMissile(
                projLocalId, aimOrigin, dir, player, cast,
                cast.pierceCount, cast.chainCount, cast.forkEnabled, cast.alwaysPierce,
                0, 0, isAimedPrimary, {});
        }
        return true;
    }

    void ProjectileRuntime::Tick(double deltaSec, bool gamePaused)
    {
        if (gamePaused || !std::isfinite(deltaSec) || deltaSec <= 0.0) {
            return;
        }

        // 1) SpellEcho 지연 반복 시전 처리 + 비일시정지 시간 기준 투사체 TTL(C-26)
        std::vector<Core::TriggeredSpellCast> readyRepeats;
        {
            std::scoped_lock lk(g_shotMutex);
            for (auto it = g_pendingRepeats.begin(); it != g_pendingRepeats.end();) {
                it->remainingSec -= deltaSec;
                if (it->remainingSec <= 0.0) {
                    readyRepeats.push_back(std::move(it->cast));
                    it = g_pendingRepeats.erase(it);
                } else {
                    ++it;
                }
            }
            // 2) 비일시정지 8초 초과 미적중/만료 투사체 메모리 정리(C-26)
            for (auto it = g_shots.begin(); it != g_shots.end();) {
                it->second.elapsedUnpausedSec += deltaSec;
                if (it->second.elapsedUnpausedSec > 8.0) {
                    g_handleToShotId.erase(it->second.handle.native_handle());
                    it = g_shots.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (!readyRepeats.empty()) {
            // 검수 M9: SpellEcho 지연 반복 실행 전 플레이어 행동 가능 상태 및 해당 주문 젬 장착 유지 검증
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* controls = RE::ControlMap::GetSingleton();
            const auto* actorState = player ? player->AsActorState() : nullptr;
            const bool canRepeatCast = player && player->Is3DLoaded() && !player->IsDead() &&
                !player->IsInKillMove() && !player->IsInRagdollState() &&
                (!actorState || (!actorState->IsStaggered() && !actorState->IsSwimming() && !actorState->IsBleedingOut())) &&
                (!controls || controls->IsFightingControlsEnabled());
            if (canRepeatCast) {
                const auto savedState = SaveLoad::GetStateCopy();
                RE::NiPoint3 origin{}, dir{};
                ComputePlayerAim(player, origin, dir);
                for (const auto& rep : readyRepeats) {
                    bool stillEquipped = false;
                    for (const auto& sl : savedState.slots) {
                        if (sl.occupied && sl.gem.instanceId == rep.spellInstanceId) {
                            stillEquipped = true;
                            break;
                        }
                    }
                    if (stillEquipped) {
                        (void)LaunchSpellDelivery(rep, origin, dir);
                    }
                }
            }
        }
    }

    void ProjectileRuntime::TriggerHeraldOfIceBurst(const RE::NiPoint3& origin, const Core::StatSnapshot& snap)
    {
        if (!snap.heraldOfIceActive || !g_dataPack || !g_config) {
            return;
        }
        const double avgPoe = 0.5 * (snap.heraldOfIceExplosionMin + snap.heraldOfIceExplosionMax);
        const double skDmg = Core::ConvertOffenseHit(avgPoe, snap.combatAreaLevel, *g_dataPack, *g_config);
        (void)SkillExecution::ExecuteAoESpell(origin, 260.0f, skDmg, FormIds::kVfxIceNova, Core::DamageType::Cold);
    }

    void ProjectileRuntime::TriggerHeraldOfAshBurst(
        const RE::NiPoint3& origin,
        double overkillSkyrimDmg,
        const Core::StatSnapshot& snap)
    {
        if (!snap.heraldOfAshActive || !g_dataPack || !g_config) {
            return;
        }
        const double burnFactor = std::clamp(snap.heraldOfAshOverkillPct / 100.0, 0.15, 1.5);
        const double burstDmg = std::max(5.0, overkillSkyrimDmg * burnFactor);
        (void)SkillExecution::ExecuteAoESpell(origin, 260.0f, burstDmg, FormIds::kVfxFireBurst, Core::DamageType::Fire);
    }

    std::uint64_t ProjectileRuntime::TotalLaunchedCount() noexcept
    {
        return g_totalLaunched.load(std::memory_order_relaxed);
    }

    std::uint64_t ProjectileRuntime::TotalImpactCount() noexcept
    {
        return g_totalImpacts.load(std::memory_order_relaxed);
    }

    std::uint64_t ProjectileRuntime::TotalRelaunchCount() noexcept
    {
        return g_totalRelaunches.load(std::memory_order_relaxed);
    }

    void ProjectileRuntime::ResetOnRevert()
    {
        std::scoped_lock lk(g_shotMutex);
        g_handleToShotId.clear();
        g_shots.clear();
        g_pendingRepeats.clear();
        g_nextShotId = 1;
    }
}
