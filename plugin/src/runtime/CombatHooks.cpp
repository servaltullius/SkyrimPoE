#include "PCH.h"
#include "runtime/CombatHooks.h"
#include "runtime/Addresses.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/FormIds.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"

#include <random>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        Core::EnergyShieldPool* g_esPool = nullptr;
        Core::KillTracker* g_killTracker = nullptr;
        Core::SkillSystem* g_skillSystem = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::Config* g_config = nullptr;
        bool g_installed = false;
        std::atomic_bool g_allHooksInstalled{ false };

        thread_local CycloneHitContext t_cycloneCtx{};
        thread_local std::mt19937 t_rng{ std::random_device{}() };

        // 폭발 비율(44004+0xC1 -> 43849+0x412는 동일 스레드 동기 호출이므로 thread_local로 관리해 스택 재사용 오염을 방지)
        thread_local const RE::HitData* t_lastExplHit = nullptr;
        thread_local float t_lastExplRatio = 1.0f;

        // 마법 효과별 스카이림 저항 배율 및 꼬리 판정 캐시(AddTarget::SetEffectiveness -> 35086).
        // ActiveEffect 풀 주소 재사용으로 인한 오적용을 막기 위해 (usUniqueID, spell, effect) 동일성 키를 함께 검증한다.
        std::mutex g_effectCacheMutex;
        struct EffectCacheEntry
        {
            std::uint16_t uniqueId = 0;
            const RE::MagicItem* spell = nullptr;
            const RE::Effect* effect = nullptr;
            bool hasSkyrimResist = false;
            float skyrimResistMult = 1.0f;
            bool hasHitRoll = false;
            bool spellDodged = false;
            bool spellBlocked = false;
            bool spellSuppressed = false;
            double suppressPreventPct = 50.0;

            [[nodiscard]] bool Matches(const RE::ActiveEffect* ae) const noexcept
            {
                return ae && uniqueId == ae->usUniqueID && spell == ae->spell && effect == ae->effect;
            }
        };
        std::unordered_map<const RE::ActiveEffect*, EffectCacheEntry> g_effectCache;

        // 주 갱신 스레드에서만 읽고 쓰는 동기화 기준값(스카이림 레벨업·장비 강화 변화도 감지)
        std::uint64_t g_lastSyncedSnapVersion = 0;
        double g_lastObservedPermHealth = -1.0;
        double g_lastObservedPermMagicka = -1.0;
        bool g_lastLowLife = false;
        bool g_lastEoActive = false;
        double g_eoRemainingSec = 0.0;
        std::atomic_bool g_eoTriggered{ false };

        // 3단계 Mind Over Matter: 피해 훅(임의 스레드 가능)에서 흡수한 매지카를 원자 누적해 주 갱신 스레드에서 일괄 차감(결정 119)
        std::atomic<double> g_pendingMomDrain{ 0.0 };

        // 진단·자동 시험용: 플레이어->동료 실제 체력 피해 누적 횟수
        std::atomic<std::uint64_t> g_playerToTeammateHits{ 0 };
        std::atomic<std::uint64_t> g_mapPhysicalDeathClamps{ 0 };
        std::atomic<std::uint64_t> g_mapMagicDeathClamps{ 0 };
        std::atomic<std::uint64_t> g_mapPhysicalDeathRepeatClamps{ 0 };
        std::atomic<std::uint64_t> g_mapMagicDeathRepeatClamps{ 0 };

        bool CanApplyCombatEffects(std::uint64_t generation) noexcept
        {
            return !MapDeviceRuntime::IsUninstallCleanupActive() &&
                (!g_queue || generation == g_queue->CurrentGeneration());
        }

        double ApplyMindOverMatterIfActive(
            double rawAfterEs,
            double diffMult,
            const Core::StatSnapshot& snap,
            RE::PlayerCharacter* player,
            std::uint64_t generation) noexcept
        {
            if (rawAfterEs <= 0.0 || snap.momRatio <= 0.0 || snap.bloodMagic || snap.poeMaxMana <= 0.0 || !player) {
                return rawAfterEs;
            }
            auto* avOwner = player->AsActorValueOwner();
            if (!avOwner) {
                return rawAfterEs;
            }
            const double curMagicka = static_cast<double>(avOwner->GetActorValue(RE::ActorValue::kMagicka));
            // 검수 Minor #5: 동시 멀티스레드 타격에서 매지카 초과 예약 방지를 위해 단일 CAS 루프로 흡수량을 원자 예약
            double curPending = g_pendingMomDrain.load(std::memory_order_acquire);
            for (;;) {
                if (!CanApplyCombatEffects(generation)) {
                    return rawAfterEs;
                }
                const double availMagicka = std::max(0.0, curMagicka - curPending);
                const auto res = Core::AbsorbWithMindOverMatter(rawAfterEs, diffMult, snap.momRatio, availMagicka);
                if (res.absorbedMagicka <= 0.0) {
                    return res.remainingRawDamage;
                }
                const double nextPending = curPending + res.absorbedMagicka;
                if (g_pendingMomDrain.compare_exchange_weak(
                        curPending, nextPending, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return res.remainingRawDamage;
                }
            }
        }

        [[nodiscard]] double RollUnit()
        {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return dist(t_rng);
        }

        void PostHudNotice(std::uint64_t generation, const char* text)
        {
            if (!text || !g_queue) {
                return;
            }
            std::string msg(text);
            g_queue->EnqueueForGeneration(generation, [msg = std::move(msg)]() {
                RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
            });
        }

        bool ContainsAddress(const REL::Segment& seg, std::uintptr_t addr, std::size_t size) noexcept
        {
            const auto begin = seg.address();
            const auto end = begin + seg.size();
            return addr >= begin && addr <= end && size <= (end - addr);
        }

        bool ReadRel32Target(std::uintptr_t site, std::uintptr_t expectedTarget, std::uintptr_t& outTarget) noexcept
        {
            const auto textSeg = REL::Module::get().segment(REL::Segment::textx);
            if (!ContainsAddress(textSeg, site, 5)) {
                return false;
            }
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(site);
            if (bytes[0] != 0xE8) {
                return false;
            }
            std::int32_t rel32 = 0;
            std::memcpy(&rel32, bytes + 1, sizeof(rel32));
            outTarget = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(site + 5) + static_cast<std::intptr_t>(rel32));
            // C-14: 대상이 엔진 .text 내부이면 기대한 AE ID 주소와 정확히 일치하는지 대조한다.
            // (.text 밖이면 타 SKSE 모드의 트램펄린 체인으로 간주하고 연쇄 훅을 허용한다.)
            if (expectedTarget != 0 && ContainsAddress(textSeg, outTarget, 1) && outTarget != expectedTarget) {
                return false;
            }
            return true;
        }

        bool WasRel32CallInstalled(std::uintptr_t site, std::uintptr_t previous) noexcept
        {
            std::uintptr_t current = 0;
            return previous != 0 && ReadRel32Target(site, 0, current) && current != previous;
        }

        void EnqueueCharmBonusSync(std::uint64_t generation, const char* toastMsg)
        {
            if (!g_queue) {
                return;
            }
            std::string msg = toastMsg ? toastMsg : "";
            g_queue->EnqueueForGeneration(generation, [msg = std::move(msg)]() {
                if (MapDeviceRuntime::IsUninstallCleanupActive()) {
                    return;
                }
                if (g_statEngine && g_charmSystem) {
                    const auto bonuses = g_charmSystem->ComputeActiveBonuses();
                    g_statEngine->UpdateLayer2([bonuses](Core::Layer2Input& l2) {
                        l2.charmBonuses = bonuses;
                    });
                    SaveLoad::PersistCharmSlotsFromSystem();
                }
                if (!msg.empty()) {
                    RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
                }
            });
        }

        // 검수 M6: 같은 프레임 연속 피격에서 Layer2 재빌드 전에 이미 켜진 참 보너스를 즉시 반영한다.
        void ApplyPendingCharmBonuses(Core::StatSnapshot& effectiveSnap)
        {
            if (!g_charmSystem || !g_config || !g_config->enableCharms) {
                return;
            }
            const auto live = g_charmSystem->ComputeActiveBonuses();
            const std::uint8_t missingMask = static_cast<std::uint8_t>(live.activeMask & ~effectiveSnap.activeCharmMask);
            if (missingMask == 0) {
                return;
            }
            auto has = [&](Core::CharmKind k) {
                return (missingMask & static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(k))) != 0;
            };
            if (has(Core::CharmKind::Ruby)) {
                effectiveSnap.fireResist += 25.0;
                effectiveSnap.maxFireResist = std::min(90.0, effectiveSnap.maxFireResist + 2.0);
            }
            if (has(Core::CharmKind::Sapphire)) {
                effectiveSnap.coldResist += 25.0;
                effectiveSnap.maxColdResist = std::min(90.0, effectiveSnap.maxColdResist + 2.0);
            }
            if (has(Core::CharmKind::Topaz)) {
                effectiveSnap.lightningResist += 25.0;
                effectiveSnap.maxLightningResist = std::min(90.0, effectiveSnap.maxLightningResist + 2.0);
            }
            if (has(Core::CharmKind::Amethyst)) {
                effectiveSnap.chaosResist += 25.0;
            }
            if (has(Core::CharmKind::Purifying)) {
                effectiveSnap.cannotBeFrozen = true;
                effectiveSnap.bleedImmune = true;
            }
            if (has(Core::CharmKind::Bastion)) {
                effectiveSnap.physDamageReduction = std::min(90.0, effectiveSnap.physDamageReduction + 15.0);
                effectiveSnap.armour += 600.0;
            }
            effectiveSnap.activeCharmMask |= missingMask;
        }

        void EnqueueMoltenShellBreakEffects(std::uint64_t generation, double reflectedFireSkyrim)
        {
            if (!g_queue) {
                return;
            }
            const auto runId = MapDeviceRuntime::GetActiveRunId();
            g_queue->EnqueueForGeneration(generation, [reflectedFireSkyrim, runId]() {
                if (MapDeviceRuntime::IsUninstallCleanupActive()) {
                    return;
                }
                if (g_statEngine) {
                    g_statEngine->UpdateLayer2([](Core::Layer2Input& l2) {
                        l2.moltenShellBonusArmour = 0.0;
                    });
                }
                if (reflectedFireSkyrim > 0.0 && MapDeviceRuntime::GetActiveRunId() == runId) {
                    if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                        // 검수 M4: 용암 방패 반사는 화염(Fire) 피해 및 만료 폭발과 동일한 320u 반경 적용
                        (void)SkillExecution::ExecuteAoESpell(
                            pl->GetPosition(), 320.0f, reflectedFireSkyrim, FormIds::kVfxFireBurst, Core::DamageType::Fire);
                    }
                }
            });
        }

        void TryTriggerBastionOnProjectedLowLife(
            std::uint64_t generation, RE::PlayerCharacter* player, double remainingRaw, double diffMult)
        {
            if (!CanApplyCombatEffects(generation) || !g_charmSystem || !g_config || !g_config->enableCharms || !player || remainingRaw <= 0.0) {
                return;
            }
            auto* av = player->AsActorValueOwner();
            if (!av) {
                return;
            }
            const double maxHp = std::max(1.0, static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth)));
            const double curHp = static_cast<double>(av->GetActorValue(RE::ActorValue::kHealth));
            const double projectedRatio = std::max(0.0, (curHp - remainingRaw * diffMult) / maxHp);
            if (CanApplyCombatEffects(generation) && g_charmSystem->TryTriggerOnLowLife(projectedRatio, true)) {
                EnqueueCharmBonusSync(generation, "[참] 수호의 참 자동 발동!");
            }
        }

        // ---- 1. HitData 적용(AE 38586) 8개 호출 자리 ----
        void ProcessApplyHit(std::size_t siteIdx, RE::Actor* target, RE::HitData* hit)
        {
            if (!target || !hit || !g_statEngine || !g_dataPack || !g_config || !g_dataPack->IsLoaded()) {
                return;
            }
            const auto generation = g_queue ? g_queue->CurrentGeneration() : 0;
            if (MapDeviceRuntime::IsUninstallCleanupActive()) {
                return;
            }
            if (SkillExecution::IsInPluginDirectDamage()) {
                return;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }
            const auto aggressor = hit->aggressor.get();

            // 6단계(검수 C-5): 동료/NPC가 4차 전직 결투장 보스를 타격해도 3페이즈 하한을 건너뛰지 못하도록 동기 클램프
            if (!target->IsPlayerRef() && (!aggressor || aggressor.get() != player) &&
                siteIdx < 7 && !hit->flags.any(RE::HitData::Flag::kPredictDamage) &&
                hit->totalDamage > 0.0f && MapDeviceRuntime::IsActiveArena4Boss(target->GetFormID())) {
                const double clamped = MapDeviceRuntime::FilterArena4BossIncomingDamage(
                    target, static_cast<double>(hit->totalDamage), 1.0, generation);
                hit->totalDamage = static_cast<float>(std::max(0.0, clamped));
                return;
            }

            // (A) 플레이어가 적을 공격하는 경로
            if (aggressor && aggressor.get() == player && !target->IsPlayerRef()) {
                // M8: 모든 타격(사이클론·바닐라 근접·활·작업 큐 지연)에서 피해 전 첫 적대 스냅샷 선기록(결정 134, 140)
                const bool isRealHit = !hit->flags.any(RE::HitData::Flag::kPredictDamage);
                if (g_killTracker && isRealHit) {
                    g_killTracker->RecordFirstHitHostility(
                        target->GetFormID(), SkillExecution::IsHostileForFirstHitReward(target, player), !target->IsDead());
                }

                // M6: 사이클론 근접 타격 문맥 활성 시(38627+0x4A8) PoE 피해로 치환(결정 65, 66, 93)
                if (siteIdx == 0 && t_cycloneCtx.active &&
                    (t_cycloneCtx.expectedTargetId == 0 || t_cycloneCtx.expectedTargetId == target->GetFormID())) {
                    // 결정 66: physicalDamage가 0이면 (1 - percentBlocked)를 폴백 비율로 사용
                    const double engineRatio = (hit->physicalDamage > 1e-4f)
                        ? std::max(0.0, static_cast<double>(hit->totalDamage) / static_cast<double>(hit->physicalDamage))
                        : std::clamp(1.0 - static_cast<double>(hit->percentBlocked), 0.0, 1.0);
                    const double sneakMult = (hit->bonusHealthDamageMult > 0.0f)
                        ? static_cast<double>(hit->bonusHealthDamageMult)
                        : 1.0;
                    const double replaced = std::max(0.0, t_cycloneCtx.convertedSkyrimDamage * engineRatio * sneakMult);
                    hit->totalDamage = static_cast<float>(replaced);
                    hit->flags.set(RE::HitData::Flag::kIgnoreCritical);
                }

                // 5단계(결정 163, 164) + 검수 C2: task_queue(siteIdx==7) 2중 적용 차단(siteIdx < 7)
                if (siteIdx < 7 && isRealHit && hit->totalDamage > 0.0f) {
                    // 상태이상 역산의 입력은 대상 쪽 증폭(감전·희귀 대상 추가 피해) 전 값이다.
                    // 지속 피해 틱이 DealDirectSkillDamage에서 같은 증폭을 다시 받으므로, 여기서 넣으면 두 번 곱해진다.
                    const double baseHitDmg = static_cast<double>(hit->totalDamage);
                    double dmg = baseHitDmg;
                    dmg *= AilmentRuntime::ShockDamageMultiplier(target->GetFormID());
                    const auto snap = g_statEngine->GetSnapshot();
                    if (snap && snap->incDamageVsRarePct > 0.0) {
                        if (const auto prof = MonsterRuntime::GetProfileCopy(target->GetFormID());
                            prof && prof->rarity == Core::MonsterRarity::Rare) {
                            dmg *= (1.0 + snap->incDamageVsRarePct / 100.0);
                        }
                    }
                    const auto monRes = MonsterRuntime::ApplyPlayerHitToMonster(
                        target->GetFormID(), dmg, Core::DamageType::Physical, false,
                        target, SkillExecution::GetDifficultyMultiplier(false), generation);
                    hit->totalDamage = static_cast<float>(std::max(0.0, monRes.remainingHealthDamage));
                    if (monRes.absorbedByEs > 1e-4) {
                        // 검수 M3: 적 ES가 사이클론 타격을 100% 흡수해도 CoC·상태이상 발동 경로에 적중 전달
                        if (siteIdx == 0 && t_cycloneCtx.active && t_cycloneCtx.outAbsorbedByMonsterEs) {
                            *t_cycloneCtx.outAbsorbedByMonsterEs = true;
                        }
                        if (monRes.remainingHealthDamage <= 1e-4 && g_killTracker) {
                            // 적 ES가 100% 흡수해 체력 감소가 0이어도 KillTracker 처치 기여 확정(검수 설계)
                            g_killTracker->ConfirmDamageContribution(target->GetFormID(), monRes.absorbedByEs);
                        }
                    }

                    // N-1: 바닐라 근접·활 타격(사이클론 제외)도 상태이상(출혈·중독·Starforge 감전 등) 경로에 연결
                    if (!t_cycloneCtx.active && g_queue && snap &&
                        (snap->bleedChancePct > 0.0 || snap->poisonChancePct > 0.0 ||
                         snap->attackPoisonChancePct > 0.0 || snap->attackIgniteChancePct > 0.0 ||
                         snap->igniteChancePct > 0.0 || snap->freezeChancePct > 0.0 ||
                         snap->shockChancePct > 0.0 || snap->starforgeShock)) {
                        const bool isHitCrit = hit->flags.any(RE::HitData::Flag::kCritical);
                        const RE::ActorHandle targetHandle = target->GetHandle();
                        const double rawHitDmg = baseHitDmg;
                        const std::uint32_t exileLevel = snap->exileLevel;
                        const auto runId = MapDeviceRuntime::GetMapActorScaling(target->GetFormID()).runId;
                        g_queue->EnqueueForGeneration(generation, [targetHandle, rawHitDmg, isHitCrit, exileLevel, runId]() {
                            if (MapDeviceRuntime::IsUninstallCleanupActive() ||
                                (runId != 0 && !MapDeviceRuntime::IsRunRewardable(runId))) {
                                return;
                            }
                            auto ptr = targetHandle.get();
                            auto* tgt = ptr.get();
                            if (!tgt || tgt->IsDead() || !g_dataPack) {
                                return;
                            }
                            const double estPoePhys = Core::MapSystem::EstimateVanillaHitPoe(
                                rawHitDmg, exileLevel, *g_dataPack);
                            Core::DamagePacket pkt{};
                            pkt.byType[static_cast<std::size_t>(Core::DamageType::Physical)] = { estPoePhys, estPoePhys };
                            (void)AilmentRuntime::ApplyPlayerHitAilments(
                                tgt, pkt, isHitCrit, AilmentRuntime::HitKind::Attack);
                        });
                    }
                }
                return;
            }

            // 6단계(검수 C-2 + Minor #1): siteIdx==7(task_queue) 포함 탈출 진행 중 무효화 + 지연 치명타 가로채기
            if (target->IsPlayerRef() && siteIdx == 7) {
                if (MapDeviceRuntime::IsDeathEscapeInFlight()) {
                    if (hit->totalDamage > 0.0f && !hit->flags.any(RE::HitData::Flag::kPredictDamage)) {
                        g_mapPhysicalDeathClamps.fetch_add(1, std::memory_order_relaxed);
                        g_mapPhysicalDeathRepeatClamps.fetch_add(1, std::memory_order_relaxed);
                    }
                    hit->totalDamage = 0.0f;
                    return;
                }
                if (!target->IsDead() && !target->IsInKillMove() &&
                    !hit->flags.any(RE::HitData::Flag::kPredictDamage) &&
                    hit->totalDamage > 0.0f && MapDeviceRuntime::IsPlayerInActiveMapCell()) {
                    const double diffMult7 = SkillExecution::GetDifficultyMultiplier(true);
                    const auto* pav = player->AsActorValueOwner();
                    const double curHp = pav ? static_cast<double>(pav->GetActorValue(RE::ActorValue::kHealth)) : 100.0;
                    if (MapDeviceRuntime::TryInterceptLethalHitFromHook(curHp,
                            static_cast<double>(hit->totalDamage) * diffMult7, generation, aggressor ? aggressor->GetFormID() : 0)) {
                        g_mapPhysicalDeathClamps.fetch_add(1, std::memory_order_relaxed);
                        hit->totalDamage = 0.0f;
                    }
                }
                return;
            }

            // (B) 플레이어가 타격을 받는 경로(siteIdx 0..6; 7=task_queue는 위에서 탈출 가드 후 제외)
            if (target->IsPlayerRef() && siteIdx < 7) {
                if (target->IsDead() || target->IsInKillMove() || hit->flags.any(RE::HitData::Flag::kPredictDamage)) {
                    return;
                }
                if (MapDeviceRuntime::IsDeathEscapeInFlight()) {
                    if (hit->totalDamage > 0.0f) {
                        g_mapPhysicalDeathClamps.fetch_add(1, std::memory_order_relaxed);
                        g_mapPhysicalDeathRepeatClamps.fetch_add(1, std::memory_order_relaxed);
                    }
                    hit->totalDamage = 0.0f;
                    return;
                }

                float explRatio = 1.0f;
                if (siteIdx == 1) {  // explosion
                    if (t_lastExplHit == hit) {
                        explRatio = t_lastExplRatio;
                    }
                    t_lastExplHit = nullptr;
                    t_lastExplRatio = 1.0f;
                }

                double preBlock = (hit->physicalDamage > 1e-4f)
                    ? static_cast<double>(hit->physicalDamage) * static_cast<double>(explRatio) *
                          std::clamp(1.0 - static_cast<double>(hit->percentBlocked), 0.0, 1.0)
                    : static_cast<double>(hit->totalDamage);

                Core::MonsterOutgoingModifier monOut{};
                if (aggressor && !aggressor->IsPlayerRef()) {
                    monOut = MonsterRuntime::GetOutgoingModifier(aggressor->GetFormID());
                    preBlock *= monOut.damageMultiplier;
                    // N-2 + 6단계 지도 치명타 수치 반영: Deadly(치명) 몬스터 치명타 확률
                    if (monOut.isDeadly) {
                        const double critProb = std::clamp(0.20 * (1.0 + monOut.critChanceIncPct / 100.0), 0.20, 0.50);
                        if (RollUnit() < critProb) {
                            preBlock *= 1.30;
                        }
                    }
                }
                // C-22, N-3: 플레이어가 적 감전 상태면 받는 피해 증폭
                preBlock *= AilmentRuntime::PlayerShockDamageMultiplier();

                if (!std::isfinite(preBlock) || preBlock <= 0.0) {
                    return;
                }

                const auto snap = g_statEngine->GetSnapshot();
                if (!snap) {
                    return;
                }

                // 1) PoE 회피 판정(결정 72, 170: 유배자 레벨 기준)
                if (snap->evasion > 0.0) {
                    const double acc = g_dataPack->MonsterAccuracy(snap->exileLevel);
                    const double hitChance = Core::EvasionHitChance(snap->evasion, acc, g_dataPack->Constants().minHitChance);
                    if (RollUnit() >= hitChance) {
                        hit->totalDamage = 0.0f;
                        PostHudNotice(generation, "회피");
                        return;
                    }
                }

                // 2) PoE 공격 막기 판정(결정 73)
                if (snap->attackBlockChance > 0.0 && RollUnit() * 100.0 < snap->attackBlockChance) {
                    hit->totalDamage = 0.0f;
                    PostHudNotice(generation, "막음");
                    return;
                }

                // 3) PoE 물리 방어 + N-2 적 추가 원소 피해 + 기대 방어 되돌림 환산 (결정 170: 유배자 레벨)
                Core::StatSnapshot effectiveSnap = *snap;
                ApplyPendingCharmBonuses(effectiveSnap);
                double postDefense = Core::ConvertDefensePhysicalHit(
                    preBlock, effectiveSnap.exileLevel, effectiveSnap, *g_dataPack, *g_config);
                // 6단계 지도 취약(Vulnerability) 모드: 플레이어 받는 물리 피해 +%
                if (effectiveSnap.inActiveMap && effectiveSnap.activeMapMods.playerPhysTakenIncPct > 0.0 && postDefense > 0.0) {
                    postDefense *= (1.0 + effectiveSnap.activeMapMods.playerPhysTakenIncPct / 100.0);
                }
                // U5 확정(2026-11-20): Astramentis(-4 Physical Damage taken from Attack Hits) 등 고정 물리 공격 피해 감산
                if (effectiveSnap.flatPhysTakenFromAttacksDelta < 0.0 && postDefense > 0.0) {
                    const double monDmg = std::max(1.0, g_dataPack->MonsterDamage(effectiveSnap.exileLevel));
                    const double flatSkyrimDelta = effectiveSnap.flatPhysTakenFromAttacksDelta * (10.0 / monDmg);
                    postDefense = std::max(0.0, postDefense + flatSkyrimDelta);
                }
                if (monOut.extraFireRatio > 0.0) {
                    postDefense += Core::ConvertDefenseResistDamage(
                        preBlock * monOut.extraFireRatio, Core::DamageType::Fire,
                        effectiveSnap.exileLevel, effectiveSnap, *g_dataPack, *g_config);
                }
                if (monOut.extraColdRatio > 0.0) {
                    postDefense += Core::ConvertDefenseResistDamage(
                        preBlock * monOut.extraColdRatio, Core::DamageType::Cold,
                        effectiveSnap.exileLevel, effectiveSnap, *g_dataPack, *g_config);
                }
                if (monOut.extraLightningRatio > 0.0) {
                    postDefense += Core::ConvertDefenseResistDamage(
                        preBlock * monOut.extraLightningRatio, Core::DamageType::Lightning,
                        effectiveSnap.exileLevel, effectiveSnap, *g_dataPack, *g_config);
                }

                // C-22, N-3: 유효 물리 타격(최대 체력의 5% 이상) 시 20% 확률로 적->플레이어 출혈 판정(정화의 참이 실제 출혈을 차단)
                if (!CanApplyCombatEffects(generation)) {
                    return;
                }
                if (postDefense > 0.0) {
                    const auto* pav = player->AsActorValueOwner();
                    const double pMaxHp = pav ? std::max(50.0, static_cast<double>(pav->GetPermanentActorValue(RE::ActorValue::kHealth))) : 100.0;
                    if (postDefense >= pMaxHp * 0.05 && RollUnit() < 0.20) {
                        const auto ailOut = AilmentRuntime::TryApplyEnemyAilmentToPlayer(Core::AilmentKind::Bleed, postDefense);
                        if (ailOut.purifyingCharmTriggered) {
                            EnqueueCharmBonusSync(generation, "[참] 정화의 참 자동 발동! (출혈 해제·면역)");
                        }
                    }
                }

                // 4) 5단계 7단계 방어 순서: ④ 가드(용암 방패) → ⑤ 에너지 실드 → ⑥ MoM → ⑦ 저체력 수호의 참
                const double diffMult = SkillExecution::GetDifficultyMultiplier(true);
                double remainingRaw = postDefense;
                if (g_skillSystem) {
                    const auto guard = g_skillSystem->AbsorbWithGuardShield(remainingRaw, diffMult, false);
                    remainingRaw = guard.remainingRawDamage;
                    if (guard.brokeNow) {
                        EnqueueMoltenShellBreakEffects(generation, guard.reflectedFireSkyrim);
                    }
                }
                if (g_esPool && CanApplyCombatEffects(generation)) {
                    const auto absorb = g_esPool->AbsorbDamage(remainingRaw, diffMult, false);
                    remainingRaw = absorb.remainingRawDamage;
                }
                remainingRaw = ApplyMindOverMatterIfActive(remainingRaw, diffMult, effectiveSnap, player, generation);
                TryTriggerBastionOnProjectedLowLife(generation, player, remainingRaw, diffMult);

                // 6단계(검수 C-2): 지도·결투장 내부 치명타 가로채기(난이도 배율 diffMult 반영, 발동 시 피해 0.0f 클램프)
                if (remainingRaw > 0.0 && MapDeviceRuntime::IsPlayerInActiveMapCell()) {
                    const auto* pav = player->AsActorValueOwner();
                    const double curHp = pav ? static_cast<double>(pav->GetActorValue(RE::ActorValue::kHealth)) : 100.0;
                    if (MapDeviceRuntime::TryInterceptLethalHitFromHook(
                            curHp, remainingRaw * diffMult, generation, aggressor ? aggressor->GetFormID() : 0)) {
                        g_mapPhysicalDeathClamps.fetch_add(1, std::memory_order_relaxed);
                        hit->totalDamage = 0.0f;
                        return;
                    }
                }
                hit->totalDamage = static_cast<float>(std::max(0.0, remainingRaw));
            }
        }

        template <std::size_t SiteIdx>
        struct ApplyHitHook
        {
            static void Thunk(RE::Actor* target, RE::HitData* hit)
            {
                try {
                    ProcessApplyHit(SiteIdx, target, hit);
                } catch (...) {
                }
                const auto orig = originalAddr.load(std::memory_order_acquire);
                if (orig != 0) {
                    using Fn = void (*)(RE::Actor*, RE::HitData*);
                    reinterpret_cast<Fn>(orig)(target, hit);
                }
            }

            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        template <std::size_t SiteIdx>
        bool InstallApplyHitSite()
        {
            const auto& spec = Addresses::kAeApplyHitSites[SiteIdx];
            const auto site = REL::ID(spec.id).address() + spec.offset;
            std::uintptr_t preTarget = 0;
            if (!ReadRel32Target(site, REL::ID(Addresses::kAeApplyHitId).address(), preTarget)) {
                SKSE::log::warn("CombatHooks: ApplyHit site '{}' (ID {}+0x{:X}) target mismatch or invalid E8; skipped.",
                    spec.name, spec.id, spec.offset);
                return false;
            }
            auto& trampoline = SKSE::GetTrampoline();
            if (trampoline.free_size() < 14) {
                SKSE::log::error("CombatHooks: insufficient trampoline space for ApplyHit '{}'.", spec.name);
                return false;
            }
            ApplyHitHook<SiteIdx>::originalAddr.store(preTarget, std::memory_order_release);
            const auto previous = trampoline.write_call<5>(site, ApplyHitHook<SiteIdx>::Thunk);
            ApplyHitHook<SiteIdx>::originalAddr.store(previous, std::memory_order_release);
            return WasRel32CallInstalled(site, previous);
        }

        // ---- 2. 폭발 비율 훅 (44004+0xC1 -> 44016) ----
        struct ExplosionStepsHook
        {
            static void Thunk(RE::HitData* hit)
            {
                const float totalBefore = hit ? hit->totalDamage : 0.0f;
                const auto orig = originalAddr.load(std::memory_order_acquire);
                if (orig != 0) {
                    using Fn = void (*)(RE::HitData*);
                    reinterpret_cast<Fn>(orig)(hit);
                }
                if (!hit) {
                    t_lastExplHit = nullptr;
                    t_lastExplRatio = 1.0f;
                    return;
                }
                const auto target = hit->target.get();
                if (target && target->IsPlayerRef()) {
                    t_lastExplHit = hit;
                    t_lastExplRatio = (totalBefore > 1e-4f)
                        ? std::clamp(hit->totalDamage / totalBefore, 0.0f, 10.0f)
                        : 1.0f;
                }
            }
            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        // ---- 3. AddTarget::SetEffectiveness (34547+0x656 -> 34052) ----
        struct SetEffectivenessHook
        {
            static void Thunk(RE::ActiveEffect* self, float effectiveness, bool flag)
            {
                try {
                    if (!MapDeviceRuntime::IsUninstallCleanupActive() &&
                        self && self->spell && self->effect && self->effect->baseEffect &&
                        self->effect->baseEffect->IsDetrimental()) {
                        if (auto* target = self->GetTargetActor(); target && target->IsPlayerRef()) {
                            float resMult = 1.0f;
                            if (self->spell->GetSpellType() != RE::MagicSystem::SpellType::kAbility &&
                                !self->spell->IgnoresResistance()) {
                                if (auto* mt = target->AsMagicTarget()) {
                                    resMult = mt->CheckResistance(self->spell, self->effect, self->source);
                                }
                            }
                            std::scoped_lock lock(g_effectCacheMutex);
                            if (g_effectCache.size() > 2048) {
                                g_effectCache.clear();
                            }
                            // 풀 주소 재사용 시 이전 효과의 판정이 남지 않도록 엔트리 전체를 새로 덮어쓴다.
                            EffectCacheEntry fresh;
                            fresh.uniqueId = self->usUniqueID;
                            fresh.spell = self->spell;
                            fresh.effect = self->effect;
                            fresh.hasSkyrimResist = true;
                            fresh.skyrimResistMult = resMult;
                            g_effectCache[self] = fresh;
                        } else if (target && !target->IsPlayerRef() && g_killTracker &&
                            self->effect->baseEffect && self->effect->baseEffect->IsDetrimental()) {
                            // Q-4: 바닐라 주문·마법부여 첫 피해 적용(35086)보다 앞선 SetEffectiveness에서 사전 적대 선기록
                            if (const auto caster = self->GetCasterActor(); caster && caster->IsPlayerRef()) {
                                g_killTracker->RecordFirstHitHostility(
                                    target->GetFormID(), SkillExecution::IsHostileForFirstHitReward(target, caster.get()),
                                    !target->IsDead());
                            }
                        }
                    }
                } catch (...) {
                }

                const auto orig = originalAddr.load(std::memory_order_acquire);
                if (orig != 0) {
                    using Fn = void (*)(RE::ActiveEffect*, float, bool);
                    reinterpret_cast<Fn>(orig)(self, effectiveness, flag);
                }
            }
            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        // ---- 4. 받는 마법 피해 (ValueModifierEffect::ModifyActorValue, ID 35086) ----
        // 효과의 ActorValue 변경을 실행하는 자리다. AddTarget의 크기/저항 계산이나 HitData 예측과는 별개다.
        struct MagicDamageMapping
        {
            Core::DamageType type = Core::DamageType::Fire;
            bool useAverageElemental = false;
        };

        MagicDamageMapping ClassifyMagicDamage(const RE::EffectSetting* baseEffect) noexcept
        {
            if (!baseEffect) {
                return { Core::DamageType::Fire, true };
            }
            switch (baseEffect->data.resistVariable) {
                case RE::ActorValue::kResistFire:
                    return { Core::DamageType::Fire, false };
                case RE::ActorValue::kResistFrost:
                    return { Core::DamageType::Cold, false };
                case RE::ActorValue::kResistShock:
                    return { Core::DamageType::Lightning, false };
                case RE::ActorValue::kPoisonResist:
                case RE::ActorValue::kResistDisease:
                    return { Core::DamageType::Chaos, false };
                default:
                    return { Core::DamageType::Fire, true };
            }
        }

        float ProcessMagicModifyActorValue(
            RE::ValueModifierEffect* self,
            RE::Actor* actor,
            float value,
            RE::ActorValue av)
        {
            if (!self || !actor || !(value < 0.0f)) {
                return value;
            }
            const auto generation = g_queue ? g_queue->CurrentGeneration() : 0;
            if (MapDeviceRuntime::IsUninstallCleanupActive()) {
                return value;
            }
            const auto actualAv = (av == RE::ActorValue::kNone) ? self->actorValue : av;
            // 결정 83: 냉기·전격의 지구력·매지카 몫은 건드리지 않고 체력 몫만 PoE 방어 적용
            if (actualAv != RE::ActorValue::kHealth) {
                return value;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return value;
            }

            const auto* baseEffect = self->effect ? self->effect->baseEffect : nullptr;
            const bool recoversOnExpiry = baseEffect &&
                baseEffect->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kRecover);

            // (A) 플레이어가 적에게 거는 바닐라 주문·마법부여·독: 사전 적대 선기록 + C-23 승격 적 ES/감전/희귀 증폭 반영
            if (!actor->IsPlayerRef()) {
                if (const auto caster = self->GetCasterActor(); caster && caster.get() == player) {
                    const RE::FormID targetId = actor->GetFormID();
                    if (g_killTracker) {
                        g_killTracker->RecordFirstHitHostility(
                            targetId, SkillExecution::IsHostileForFirstHitReward(actor, player), !actor->IsDead());
                    }
                    // 결정 81·172: kRecover(만료 때 엔진이 원래 크기로 되돌리는 일시 체력 감소)는 피해가 아니므로
                    // 생명 배율·ES·희귀도 보정을 걸지 않는다. 걸 때만 줄이면 되돌릴 때 체력이 늘어난 채 남는다.
                    if (baseEffect && baseEffect->IsDetrimental() && !recoversOnExpiry) {
                        const auto mapping = ClassifyMagicDamage(baseEffect);
                        double magDmg = static_cast<double>(-value);
                        const double shockMult = AilmentRuntime::ShockDamageMultiplier(targetId);
                        magDmg *= shockMult;
                        if (g_statEngine) {
                            if (const auto snap = g_statEngine->GetSnapshot(); snap && snap->incDamageVsRarePct > 0.0) {
                                if (const auto prof = MonsterRuntime::GetProfileCopy(targetId);
                                    prof && prof->rarity == Core::MonsterRarity::Rare) {
                                    magDmg *= (1.0 + snap->incDamageVsRarePct / 100.0);
                                }
                            }
                        }
                        const bool bypassEs = (mapping.type == Core::DamageType::Chaos);
                        // 검수 Minor 8: 저항 변수 없는 무속성 마법(체력 흡수 등)은 FireResistant 오적용 방지를 위해 DamageType::Count 전달
                        const auto monDmgType = mapping.useAverageElemental ? Core::DamageType::Count : mapping.type;
                        const auto hitRes = MonsterRuntime::ApplyPlayerHitToMonster(
                            targetId, magDmg, monDmgType, bypassEs,
                            actor, SkillExecution::GetDifficultyMultiplier(false), generation);
                        if (hitRes.absorbedByEs > 0.0 && g_killTracker) {
                            g_killTracker->ConfirmDamageContribution(targetId, hitRes.absorbedByEs);
                        }
                        return -static_cast<float>(std::max(0.0, hitRes.remainingHealthDamage));
                    }
                } else if (!recoversOnExpiry && MapDeviceRuntime::IsActiveArena4Boss(actor->GetFormID())) {
                    // 6단계(검수 C-5): 동료/NPC 마법도 4차 전직 보스 페이즈 하한을 건너뛰지 못하도록 동기 클램프
                    const double clamped = MapDeviceRuntime::FilterArena4BossIncomingDamage(
                        actor, static_cast<double>(-value), 1.0, generation);
                    return -static_cast<float>(std::max(0.0, clamped));
                }
                return value;
            }

            // (B) 플레이어가 받는 마법 피해
            if (actor->IsDead() || actor->IsInKillMove() ||
                !g_statEngine || !g_dataPack || !g_config || !g_dataPack->IsLoaded()) {
                return value;
            }
            if (MapDeviceRuntime::IsDeathEscapeInFlight()) {
                if (self->spell && baseEffect && baseEffect->IsDetrimental() && !recoversOnExpiry) {
                    g_mapMagicDeathClamps.fetch_add(1, std::memory_order_relaxed);
                    g_mapMagicDeathRepeatClamps.fetch_add(1, std::memory_order_relaxed);
                }
                return 0.0f;
            }
            if (!self->spell || !baseEffect) {
                // 6단계(검수 C-2): 주문/효과 정보 없는 마법·환경 체력 피해라도 지도 내부 치명타면 사망 가로채기 적용
                if (MapDeviceRuntime::IsPlayerInActiveMapCell()) {
                    const double diffMultEnv = SkillExecution::GetDifficultyMultiplier(true);
                    const auto* pav = player->AsActorValueOwner();
                    const double curHp = pav ? static_cast<double>(pav->GetActorValue(RE::ActorValue::kHealth)) : 100.0;
                    const auto caster = self->GetCasterActor();
                    if (MapDeviceRuntime::TryInterceptLethalHitFromHook(curHp,
                            static_cast<double>(-value) * diffMultEnv, generation, caster ? caster->GetFormID() : 0)) {
                        return 0.0f;
                    }
                }
                // 결정 77: 그 외 환경 피해는 엔진 그대로 둔다
                return value;
            }
            // 결정 81: kRecover(일시 체력 증가 만료 되돌림)은 피해로 보지 않는다
            if (recoversOnExpiry) {
                return value;
            }
            if (!baseEffect->IsDetrimental()) {
                return value;
            }

            const auto snap = g_statEngine->GetSnapshot();
            if (!snap) {
                return value;
            }

            // 3단계 Chaos Inoculation: 카오스 피해(독·질병)는 저항·ES 계산 전에 즉시 0 단락(ES 재충전 타이머 미초기화)
            const auto mapping = ClassifyMagicDamage(baseEffect);
            const bool isChaos = (mapping.type == Core::DamageType::Chaos);
            if (isChaos && snap->chaosImmune) {
                return 0.0f;
            }

            // 1) AddTarget::SetEffectiveness에서 기록한 스카이림 저항 배율 r 복원(동일성 키 일치 시에만 신뢰, 결정 71·78)
            float skyrimRes = 1.0f;
            bool hasCachedRes = false;
            {
                std::scoped_lock lock(g_effectCacheMutex);
                if (const auto it = g_effectCache.find(self); it != g_effectCache.end()) {
                    if (it->second.Matches(self)) {
                        if (it->second.hasSkyrimResist) {
                            skyrimRes = it->second.skyrimResistMult;
                            hasCachedRes = true;
                        }
                    } else {
                        // 풀 주소가 새 효과에 재사용됐으므로 낡은 엔트리를 즉시 폐기한다.
                        g_effectCache.erase(it);
                    }
                }
            }
            if (!hasCachedRes) {
                if (self->spell->GetSpellType() != RE::MagicSystem::SpellType::kAbility &&
                    !self->spell->IgnoresResistance()) {
                    if (auto* mt = actor->AsMagicTarget()) {
                        skyrimRes = mt->CheckResistance(self->spell, self->effect, self->source);
                    }
                }
            }

            double preResistD = static_cast<double>(-value);
            if (std::isfinite(skyrimRes) && skyrimRes > 1e-4f && skyrimRes < 10.0f) {
                preResistD /= static_cast<double>(skyrimRes);
            }
            const auto caster = self->GetCasterActor();
            if (caster && !caster->IsPlayerRef()) {
                const auto monOut = MonsterRuntime::GetOutgoingModifier(caster->GetFormID());
                preResistD *= monOut.damageMultiplier;
                if (monOut.isDeadly) {
                    const double critProb = std::clamp(0.20 * (1.0 + monOut.critChanceIncPct / 100.0), 0.20, 0.50);
                    if (RollUnit() < critProb) {
                        preResistD *= 1.30;
                    }
                }
            }
            // C-22, N-3: 플레이어 감전 상태 시 받는 마법 피해 증폭
            preResistD *= AilmentRuntime::PlayerShockDamageMultiplier();

            // 2) 타격 vs 지속 피해 분류 및 주문 회피(Acrobatics)·주문 막기·주문 억제(결정 82, 87)
            const bool isConcentration =
                (self->spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration);
            const bool isHit = (self->effect->effectItem.duration == 0) || isConcentration;

            if (isHit) {
                if (isConcentration) {
                    const double dodgePass = 1.0 - std::clamp(snap->spellDodgeChance, 0.0, 75.0) / 100.0;
                    const double blockPass = 1.0 - std::clamp(snap->spellBlockChance, 0.0, 75.0) / 100.0;
                    const double supChance = std::clamp(snap->spellSuppressionChance, 0.0, 100.0) / 100.0;
                    const double supPrevent = std::clamp(snap->spellSuppressionPrevent, 0.0, 100.0) / 100.0;
                    preResistD *= dodgePass * blockPass * (1.0 - supChance * supPrevent);
                } else {
                    bool dodged = false;
                    bool blocked = false;
                    bool suppressed = false;
                    double preventPct = snap->spellSuppressionPrevent;
                    bool firstRoll = false;
                    {
                        std::scoped_lock lock(g_effectCacheMutex);
                        if (g_effectCache.size() > 2048) {
                            g_effectCache.clear();
                        }
                        auto& entry = g_effectCache[self];
                        if (!entry.Matches(self)) {
                            entry = EffectCacheEntry{};
                            entry.uniqueId = self->usUniqueID;
                            entry.spell = self->spell;
                            entry.effect = self->effect;
                        }
                        if (!entry.hasHitRoll) {
                            entry.hasHitRoll = true;
                            entry.spellDodged = (snap->spellDodgeChance > 0.0 && RollUnit() * 100.0 < snap->spellDodgeChance);
                            entry.spellBlocked = (!entry.spellDodged && snap->spellBlockChance > 0.0 &&
                                                  RollUnit() * 100.0 < snap->spellBlockChance);
                            entry.spellSuppressed = (!entry.spellDodged && !entry.spellBlocked &&
                                                     snap->spellSuppressionChance > 0.0 &&
                                                     RollUnit() * 100.0 < snap->spellSuppressionChance);
                            entry.suppressPreventPct = snap->spellSuppressionPrevent;
                            firstRoll = true;
                        }
                        dodged = entry.spellDodged;
                        blocked = entry.spellBlocked;
                        suppressed = entry.spellSuppressed;
                        preventPct = entry.suppressPreventPct;
                    }
                    if (dodged) {
                        if (firstRoll) {
                            PostHudNotice(generation, "주문 회피");
                        }
                        return 0.0f;
                    }
                    if (blocked) {
                        if (firstRoll) {
                            PostHudNotice(generation, "막음");
                        }
                        return 0.0f;
                    }
                    if (suppressed) {
                        preResistD *= (1.0 - std::clamp(preventPct, 0.0, 100.0) / 100.0);
                    }
                }
            }

            // 2.5) 5단계(결정 162) + 검수 M5/M6: 원소·카오스 피격 시 참 자동 발동 + 정화의 참(냉기) + 같은 프레임 연속 피격 보정
            if (!CanApplyCombatEffects(generation)) {
                return value;
            }
            Core::StatSnapshot effectiveSnap = *snap;
            if (CanApplyCombatEffects(generation) && g_charmSystem && g_config->enableCharms && !mapping.useAverageElemental) {
                if (g_charmSystem->TryTriggerOnIncomingDamage(mapping.type, true)) {
                    switch (mapping.type) {
                        case Core::DamageType::Fire:
                            EnqueueCharmBonusSync(generation, "[참] 루비 참 자동 발동! (+25% 화염 저항)");
                            break;
                        case Core::DamageType::Cold:
                            EnqueueCharmBonusSync(generation, "[참] 사파이어 참 자동 발동! (+25% 냉기 저항)");
                            break;
                        case Core::DamageType::Lightning:
                            EnqueueCharmBonusSync(generation, "[참] 토파즈 참 자동 발동! (+25% 번개 저항)");
                            break;
                        case Core::DamageType::Chaos:
                            EnqueueCharmBonusSync(generation, "[참] 자수정 참 자동 발동! (+25% 카오스 저항)");
                            break;
                        default:
                            break;
                    }
                }
            }
            // 방금 켜진 참뿐 아니라 같은 프레임 직전 타격에서 켜진 참 보너스도 모두 즉시 반영(검수 M6)
            ApplyPendingCharmBonuses(effectiveSnap);

            // 3) PoE 저항 + PoE 3.30 인내 원소 피해 감소 + 유배자 레벨 기준 기대 저항 되돌림(결정 71, 74, 142, 170)
            double postDefense = 0.0;
            if (mapping.useAverageElemental) {
                Core::StatSnapshot avgSnap = effectiveSnap;
                avgSnap.fireResist = (effectiveSnap.fireResist + effectiveSnap.coldResist + effectiveSnap.lightningResist) / 3.0;
                avgSnap.maxFireResist = (effectiveSnap.maxFireResist + effectiveSnap.maxColdResist + effectiveSnap.maxLightningResist) / 3.0;
                postDefense = Core::ConvertDefenseResistDamage(
                    preResistD, Core::DamageType::Fire, effectiveSnap.exileLevel, avgSnap, *g_dataPack, *g_config);
            } else {
                postDefense = Core::ConvertDefenseResistDamage(
                    preResistD, mapping.type, effectiveSnap.exileLevel, effectiveSnap, *g_dataPack, *g_config);
            }

            // C-22, N-3: 유효 원소 마법 타격(최대 체력의 5% 이상) 시 적->플레이어 상태이상(점화·냉각/빙결·감전) 적용
            // (정화의 참은 실제 동결/냉각이 발생할 유효 냉기 타격에서만 발동하여 충전 낭비 방지)
            if (isHit && !mapping.useAverageElemental && postDefense > 0.0) {
                const auto* pav = player->AsActorValueOwner();
                const double pMaxHp = pav ? std::max(50.0, static_cast<double>(pav->GetPermanentActorValue(RE::ActorValue::kHealth))) : 100.0;
                if (postDefense >= pMaxHp * 0.05) {
                    if (mapping.type == Core::DamageType::Cold) {
                        const auto ailOut = AilmentRuntime::TryApplyEnemyAilmentToPlayer(Core::AilmentKind::Freeze, postDefense);
                        if (ailOut.purifyingCharmTriggered) {
                            EnqueueCharmBonusSync(generation, "[참] 정화의 참 자동 발동! (빙결·출혈 면역)");
                        }
                    } else if (mapping.type == Core::DamageType::Fire && RollUnit() < 0.25) {
                        (void)AilmentRuntime::TryApplyEnemyAilmentToPlayer(Core::AilmentKind::Ignite, postDefense);
                    } else if (mapping.type == Core::DamageType::Lightning && RollUnit() < 0.25) {
                        (void)AilmentRuntime::TryApplyEnemyAilmentToPlayer(Core::AilmentKind::Shock, postDefense);
                    }
                }
            }

            // 4) 5단계 7단계 방어 순서: ④ 가드(용암 방패, DoT 우회) → ⑤ 에너지 실드(샤브론 시 카오스 차단) → ⑥ MoM → ⑦ 저체력 수호의 참
            if (!CanApplyCombatEffects(generation)) {
                return value;
            }
            const double diffMult = SkillExecution::GetDifficultyMultiplier(true);
            double finalRaw = postDefense;
            if (g_skillSystem) {
                const auto guard = g_skillSystem->AbsorbWithGuardShield(finalRaw, diffMult, !isHit);
                finalRaw = guard.remainingRawDamage;
                if (guard.brokeNow) {
                    EnqueueMoltenShellBreakEffects(generation, guard.reflectedFireSkyrim);
                }
            }
            if (g_esPool && CanApplyCombatEffects(generation)) {
                const bool chaosBypassesEs = isChaos && !effectiveSnap.shavronneChaosEs;
                const auto absorb = g_esPool->AbsorbDamage(finalRaw, diffMult, chaosBypassesEs);
                finalRaw = absorb.remainingRawDamage;
            }
            finalRaw = ApplyMindOverMatterIfActive(finalRaw, diffMult, effectiveSnap, player, generation);
            TryTriggerBastionOnProjectedLowLife(generation, player, finalRaw, diffMult);

            // 6단계(검수 C-2): 지도·결투장 내부 치명 마법 피해 가로채기(diffMult 반영, 발동 시 0.0f 클램프)
            if (finalRaw > 0.0 && MapDeviceRuntime::IsPlayerInActiveMapCell()) {
                const auto* pav = player->AsActorValueOwner();
                const double curHp = pav ? static_cast<double>(pav->GetActorValue(RE::ActorValue::kHealth)) : 100.0;
                if (MapDeviceRuntime::TryInterceptLethalHitFromHook(
                        curHp, finalRaw * diffMult, generation, caster ? caster->GetFormID() : 0)) {
                    g_mapMagicDeathClamps.fetch_add(1, std::memory_order_relaxed);
                    return 0.0f;
                }
            }
            return -static_cast<float>(std::max(0.0, finalRaw));
        }

        template <std::size_t Index>
        struct MagicModifyHook
        {
            static void Thunk(RE::ValueModifierEffect* self, RE::Actor* actor, float value, RE::ActorValue av)
            {
                float modified = value;
                try {
                    modified = ProcessMagicModifyActorValue(self, actor, value, av);
                } catch (...) {
                    modified = value;
                }
                const auto orig = originalAddr.load(std::memory_order_acquire);
                if (orig != 0) {
                    using Fn = void (*)(RE::ValueModifierEffect*, RE::Actor*, float, RE::ActorValue);
                    reinterpret_cast<Fn>(orig)(self, actor, modified, av);
                }
            }
            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        struct MagicVtableSpec
        {
            std::string_view name;
            REL::VariantID id;
        };

        constexpr std::array<MagicVtableSpec, 15> kMagicVtables = {{
            { "ValueModifierEffect",             RE::VTABLE_ValueModifierEffect[0] },
            { "AccumulatingValueModifierEffect", RE::VTABLE_AccumulatingValueModifierEffect[0] },
            { "PeakValueModifierEffect",         RE::VTABLE_PeakValueModifierEffect[0] },
            { "AbsorbEffect",                    RE::VTABLE_AbsorbEffect[0] },
            { "TargetValueModifierEffect",       RE::VTABLE_TargetValueModifierEffect[0] },
            { "DarknessEffect",                  RE::VTABLE_DarknessEffect[0] },
            { "InvisibilityEffect",              RE::VTABLE_InvisibilityEffect[0] },
            { "NightEyeEffect",                  RE::VTABLE_NightEyeEffect[0] },
            { "ParalysisEffect",                 RE::VTABLE_ParalysisEffect[0] },
            { "RallyEffect",                     RE::VTABLE_RallyEffect[0] },
            { "DemoralizeEffect",                RE::VTABLE_DemoralizeEffect[0] },
            { "TurnUndeadEffect",                RE::VTABLE_TurnUndeadEffect[0] },
            { "BanishEffect",                    RE::VTABLE_BanishEffect[0] },
            { "CalmEffect",                      RE::VTABLE_CalmEffect[0] },
            { "FrenzyEffect",                    RE::VTABLE_FrenzyEffect[0] },
        }};

        template <std::size_t I>
        bool InstallMagicVtable()
        {
            REL::Relocation<std::uintptr_t> vtable{ kMagicVtables[I].id };
            const auto slotAddr = vtable.address() + Addresses::kModifyActorValueVtableSlot * sizeof(std::uintptr_t);
            const auto currentTarget = *reinterpret_cast<const std::uintptr_t*>(slotAddr);
            if (currentTarget != REL::ID(Addresses::kAeModifyActorValueId).address()) {
                SKSE::log::warn("CombatHooks: magic vtable '{}' slot 0x20 points to 0x{:X} (expected 35086); chaining anyway.",
                    kMagicVtables[I].name, currentTarget);
            }
            MagicModifyHook<I>::originalAddr.store(currentTarget, std::memory_order_release);
            const auto prev = vtable.write_vfunc(Addresses::kModifyActorValueVtableSlot, MagicModifyHook<I>::Thunk);
            MagicModifyHook<I>::originalAddr.store(prev, std::memory_order_release);
            return prev != 0 &&
                *reinterpret_cast<const std::uintptr_t*>(slotAddr) == reinterpret_cast<std::uintptr_t>(MagicModifyHook<I>::Thunk);
        }

        template <std::size_t I>
        bool InstallMagicDirectCall()
        {
            constexpr std::size_t HookIdx = kMagicVtables.size() + I;
            const auto& spec = Addresses::kAeMagicDirectCalls[I];
            const auto site = REL::ID(spec.id).address() + spec.offset;
            std::uintptr_t preTarget = 0;
            if (!ReadRel32Target(site, REL::ID(Addresses::kAeModifyActorValueId).address(), preTarget)) {
                SKSE::log::warn("CombatHooks: magic direct call '{}' (ID {}+0x{:X}) target mismatch or invalid; skipped.",
                    spec.name, spec.id, spec.offset);
                return false;
            }
            auto& trampoline = SKSE::GetTrampoline();
            if (trampoline.free_size() < 14) {
                SKSE::log::error("CombatHooks: insufficient trampoline space for magic call '{}'.", spec.name);
                return false;
            }
            MagicModifyHook<HookIdx>::originalAddr.store(preTarget, std::memory_order_release);
            const auto prev = trampoline.write_call<5>(site, MagicModifyHook<HookIdx>::Thunk);
            MagicModifyHook<HookIdx>::originalAddr.store(prev, std::memory_order_release);
            return WasRel32CallInstalled(site, prev);
        }

        // ---- 5. 실제 체력 감소 기여 확정 (HandleHealthDamage, vtable 0x104) ----
        template <std::size_t Which>
        struct HandleHealthDamageHook
        {
            static void Thunk(RE::Actor* self, RE::Actor* attacker, float damage)
            {
                // 결정 140: HandleHealthDamage는 이미 체력이 실제로 깎인 뒤 음수 damage로 불리며,
                // orig 안에서 KillImpl/TESDeathEvent가 동기 발송될 수 있으므로 반드시 orig 호출 전에 기여를 확정한다.
                try {
                    if (!MapDeviceRuntime::IsUninstallCleanupActive() &&
                        self && !self->IsPlayerRef() && attacker && attacker->IsPlayerRef() && damage < 0.0f) {
                        if (self->IsPlayerTeammate()) {
                            g_playerToTeammateHits.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (g_killTracker) {
                            g_killTracker->ConfirmDamageContribution(self->GetFormID(), static_cast<double>(-damage));
                        }
                    }
                } catch (...) {
                }

                const auto orig = originalAddr.load(std::memory_order_acquire);
                if (orig != 0) {
                    using Fn = void (*)(RE::Actor*, RE::Actor*, float);
                    reinterpret_cast<Fn>(orig)(self, attacker, damage);
                }
            }
            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        template <std::size_t Which>
        bool InstallHandleHealthDamage(const REL::VariantID& vtableId)
        {
            REL::Relocation<std::uintptr_t> vtable{ vtableId };
            const auto slotAddr = vtable.address() + Addresses::kHandleHealthDamageVtableSlot * sizeof(std::uintptr_t);
            const auto current = *reinterpret_cast<const std::uintptr_t*>(slotAddr);
            HandleHealthDamageHook<Which>::originalAddr.store(current, std::memory_order_release);
            const auto prev = vtable.write_vfunc(Addresses::kHandleHealthDamageVtableSlot, HandleHealthDamageHook<Which>::Thunk);
            HandleHealthDamageHook<Which>::originalAddr.store(prev, std::memory_order_release);
            return prev != 0 &&
                *reinterpret_cast<const std::uintptr_t*>(slotAddr) == reinterpret_cast<std::uintptr_t>(HandleHealthDamageHook<Which>::Thunk);
        }

        template <std::size_t... Is>
        std::size_t InstallAllApplyHit(std::index_sequence<Is...>)
        {
            return (static_cast<std::size_t>(InstallApplyHitSite<Is>()) + ...);
        }

        template <std::size_t... Is>
        std::size_t InstallAllMagicVtables(std::index_sequence<Is...>)
        {
            return (static_cast<std::size_t>(InstallMagicVtable<Is>()) + ...);
        }

        template <std::size_t... Is>
        std::size_t InstallAllMagicCalls(std::index_sequence<Is...>)
        {
            return (static_cast<std::size_t>(InstallMagicDirectCall<Is>()) + ...);
        }
    }

    ScopedCycloneHitContext::ScopedCycloneHitContext(
        double convertedSkyrimDamage,
        bool isCrit,
        RE::FormID targetId,
        bool* outAbsorbedByMonsterEs) noexcept
        : previous_(t_cycloneCtx)
    {
        t_cycloneCtx.active = true;
        t_cycloneCtx.convertedSkyrimDamage = convertedSkyrimDamage;
        t_cycloneCtx.isCrit = isCrit;
        t_cycloneCtx.expectedTargetId = targetId;
        t_cycloneCtx.outAbsorbedByMonsterEs = outAbsorbedByMonsterEs;
    }

    ScopedCycloneHitContext::~ScopedCycloneHitContext() noexcept
    {
        t_cycloneCtx = previous_;
    }

    bool CombatHooks::Install(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        Core::EnergyShieldPool* esPool,
        Core::KillTracker* killTracker,
        const Core::DataPack* dataPack,
        const Core::Config* config,
        Core::SkillSystem* skillSystem,
        Core::CharmSystem* charmSystem)
    {
        if (g_installed) {
            return true;
        }
        g_queue = queue;
        g_statEngine = statEngine;
        g_esPool = esPool;
        g_killTracker = killTracker;
        g_skillSystem = skillSystem;
        g_charmSystem = charmSystem;
        g_dataPack = dataPack;
        g_config = config;

        const auto ver = REL::Module::get().version();
        if (!Addresses::IsVerifiedRuntime(ver)) {
            SKSE::log::warn("CombatHooks: skipped on unverified runtime {}.", ver.string("."));
            return false;
        }

        if (g_esPool && g_dataPack && g_dataPack->IsLoaded()) {
            g_esPool->Configure(g_dataPack->Constants().esRechargeDelay, g_dataPack->Constants().esRechargeRate);
        }

        const auto hitInstalled = InstallAllApplyHit(std::make_index_sequence<Addresses::kAeApplyHitSites.size()>{});

        bool explInstalled = false;
        {
            const auto site = REL::ID(Addresses::kAeExplosionStepsSite.id).address() + Addresses::kAeExplosionStepsSite.offset;
            std::uintptr_t preTarget = 0;
            if (ReadRel32Target(site, REL::ID(Addresses::kAeExplosionDamageStepsId).address(), preTarget) &&
                SKSE::GetTrampoline().free_size() >= 14) {
                ExplosionStepsHook::originalAddr.store(preTarget, std::memory_order_release);
                const auto prev = SKSE::GetTrampoline().write_call<5>(site, ExplosionStepsHook::Thunk);
                ExplosionStepsHook::originalAddr.store(prev, std::memory_order_release);
                explInstalled = WasRel32CallInstalled(site, prev);
            }
        }

        bool effInstalled = false;
        {
            const auto site = REL::ID(Addresses::kAeSetEffectivenessSite.id).address() + Addresses::kAeSetEffectivenessSite.offset;
            std::uintptr_t preTarget = 0;
            if (ReadRel32Target(site, REL::ID(Addresses::kAeSetEffectivenessId).address(), preTarget) &&
                SKSE::GetTrampoline().free_size() >= 14) {
                SetEffectivenessHook::originalAddr.store(preTarget, std::memory_order_release);
                const auto prev = SKSE::GetTrampoline().write_call<5>(site, SetEffectivenessHook::Thunk);
                SetEffectivenessHook::originalAddr.store(prev, std::memory_order_release);
                effInstalled = WasRel32CallInstalled(site, prev);
            }
        }

        const auto magicVtInstalled = InstallAllMagicVtables(std::make_index_sequence<kMagicVtables.size()>{});
        const auto magicCallInstalled = InstallAllMagicCalls(std::make_index_sequence<Addresses::kAeMagicDirectCalls.size()>{});

        const bool characterHealthInstalled = InstallHandleHealthDamage<0>(RE::VTABLE_Character[0]);
        const bool playerHealthInstalled = InstallHandleHealthDamage<1>(RE::VTABLE_PlayerCharacter[0]);

        SaveLoad::AddRevertCallback([]() {
            {
                std::scoped_lock lock(g_effectCacheMutex);
                g_effectCache.clear();
            }
            if (g_esPool) {
                g_esPool->Clear();
            }
            g_lastSyncedSnapVersion = 0;
            g_lastObservedPermHealth = -1.0;
            g_lastObservedPermMagicka = -1.0;
            g_lastLowLife = false;
            g_lastEoActive = false;
            g_eoRemainingSec = 0.0;
            g_eoTriggered.store(false, std::memory_order_release);
            g_pendingMomDrain.store(0.0, std::memory_order_release);
        });

        g_installed = true;
        g_allHooksInstalled.store(
            hitInstalled == Addresses::kAeApplyHitSites.size() && explInstalled && effInstalled &&
            magicVtInstalled == kMagicVtables.size() && magicCallInstalled == Addresses::kAeMagicDirectCalls.size() &&
            characterHealthInstalled && playerHealthInstalled,
            std::memory_order_release);
        SKSE::log::info(
            "CombatHooks: installed ApplyHit={}/{}, ExplosionSteps={}, SetEffectiveness={}, MagicVtable={}/{}, MagicCalls={}/{}, HandleHealthDamage={}/2.",
            hitInstalled, Addresses::kAeApplyHitSites.size(),
            explInstalled, effInstalled,
            magicVtInstalled, kMagicVtables.size(),
            magicCallInstalled, Addresses::kAeMagicDirectCalls.size(),
            static_cast<unsigned>(characterHealthInstalled) + static_cast<unsigned>(playerHealthInstalled));
        return true;
    }

    bool CombatHooks::AreAllHooksInstalled() noexcept
    {
        return g_allHooksInstalled.load(std::memory_order_acquire);
    }

    void CombatHooks::TickPlayerPools(double deltaSec, bool gamePaused)
    {
        if (MapDeviceRuntime::IsUninstallCleanupActive()) {
            return;
        }
        if (g_esPool) {
            g_esPool->Tick(deltaSec, gamePaused);
        }
        if (g_charmSystem && g_charmSystem->Tick(deltaSec, gamePaused)) {
            if (g_statEngine) {
                const auto bonuses = g_charmSystem->ComputeActiveBonuses();
                g_statEngine->UpdateLayer2([bonuses](Core::Layer2Input& l2) {
                    l2.charmBonuses = bonuses;
                });
            }
        }

        if (gamePaused || !g_statEngine || !g_dataPack || !g_config || !g_dataPack->IsLoaded()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded() || player->IsDead()) {
            return;
        }
        auto* avOwner = player->AsActorValueOwner();
        if (!avOwner) {
            return;
        }

        const auto snap = g_statEngine->GetSnapshot();
        if (!snap) {
            return;
        }

        // 3단계 Mind Over Matter: 피해 훅에서 누적한 매지카 흡수분을 주 갱신 스레드에서 안전하게 일괄 차감(결정 119)
        const double pendingMom = g_pendingMomDrain.exchange(0.0, std::memory_order_acq_rel);
        if (std::isfinite(pendingMom) && pendingMom > 0.0) {
            const float curMag = avOwner->GetActorValue(RE::ActorValue::kMagicka);
            const float drain = static_cast<float>(std::min(pendingMom, std::max(0.0, static_cast<double>(curMag))));
            if (drain > 0.0f) {
                avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka, -drain);
            }
        }

        const double currentPermHealth = static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kHealth));
        const double currentPermMagicka = static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kMagicka));

        // 3단계 Pain Attunement (Low Life <= 50%, CI 제외) 및 Elemental Overload (최근 8초 치명타) 감시:
        // 상태가 실제로 변할 때만 Layer2Input을 갱신해 유휴 프레임 재빌드 비용 0 유지.
        const double curHealth = static_cast<double>(avOwner->GetActorValue(RE::ActorValue::kHealth));
        const bool nowLowLife = !snap->chaosInoculation && currentPermHealth > 0.0 && (curHealth <= currentPermHealth * 0.50);
        if (g_eoTriggered.exchange(false, std::memory_order_acq_rel)) {
            g_eoRemainingSec = 8.0;
        } else if (g_eoRemainingSec > 0.0 && std::isfinite(deltaSec) && deltaSec > 0.0) {
            g_eoRemainingSec = std::max(0.0, g_eoRemainingSec - deltaSec);
        }
        const bool nowEoActive = (g_eoRemainingSec > 0.0);
        if (nowLowLife != g_lastLowLife || nowEoActive != g_lastEoActive) {
            g_lastLowLife = nowLowLife;
            g_lastEoActive = nowEoActive;
            g_statEngine->UpdateLayer2([nowLowLife, nowEoActive](Core::Layer2Input& l2) {
                l2.lowLife = nowLowLife;
                l2.elementalOverloadActive = nowEoActive;
            });
        }

        // 스탯 스냅샷 변경뿐 아니라 스카이림 레벨업·장비 강화로 영구 체력/매지카가 바뀐 경우도 즉시 재동기화한다(결정 70, 125).
        const bool snapChanged = (snap->version != g_lastSyncedSnapVersion);
        const bool permChanged = (g_lastObservedPermHealth < 0.0) ||
            (std::abs(currentPermHealth - g_lastObservedPermHealth) > 0.25) ||
            (std::abs(currentPermMagicka - g_lastObservedPermMagicka) > 0.25);

        if (!snapChanged && !permChanged) {
            return;
        }

        if (snapChanged && g_esPool) {
            g_esPool->Configure(
                g_dataPack->Constants().esRechargeDelay * snap->esRechargeDelayMult,
                g_dataPack->Constants().esRechargeRate * snap->esRechargeRateMult);
        }

        const auto stateCopy = SaveLoad::GetStateCopy();
        double appliedHealth = stateCopy.appliedHealthDelta;
        double appliedMagicka = stateCopy.appliedMagickaDelta;
        bool deltasChanged = false;

        // 결정 67, 70: 기대 생명력 대비 배수로 최대 체력 보정(CI는 안전 하한 적용)
        const double baseSkyrimHealth = std::max(1.0, currentPermHealth - appliedHealth);
        const double targetHealth = Core::ComputeSkyrimMaxHealth(
            baseSkyrimHealth, snap->poeMaxLife, snap->exileLevel, *g_dataPack, *g_config, snap->chaosInoculation);
        const double desiredHealthDelta = targetHealth - baseSkyrimHealth;
        if (std::abs(desiredHealthDelta - appliedHealth) > 0.25) {
            const float diff = static_cast<float>(desiredHealthDelta - appliedHealth);
            // 음수 보정(예: 패시브 미배분 레벨업)이 저체력 플레이어를 즉사시키지 않도록 최소 체력 1.0 보장
            if (diff < 0.0f) {
                const float curHealthBeforeDiff = avOwner->GetActorValue(RE::ActorValue::kHealth);
                if (curHealthBeforeDiff > 0.0f && (curHealthBeforeDiff + diff) < 1.0f) {
                    const float compensate = std::min(-diff, 1.0f - (curHealthBeforeDiff + diff));
                    if (compensate > 0.0f) {
                        avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, compensate);
                    }
                }
            }
            avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kHealth, diff);
            appliedHealth += static_cast<double>(diff);
            deltasChanged = true;
        }

        // 결정 70, 85: 에너지 실드 최대치 동기화
        if (g_esPool) {
            const double targetMaxEs = Core::ComputeSkyrimMaxEnergyShield(
                baseSkyrimHealth, snap->poeMaxEnergyShield, snap->exileLevel, *g_dataPack, *g_config);
            g_esPool->SetMax(targetMaxEs, false);
        }

        // 결정 125, 144: 기대 마나 대비 배수로 최대 매지카 보정(Blood Magic/마나 0이면 원본 유지)
        const double baseSkyrimMagicka = std::max(1.0, currentPermMagicka - appliedMagicka);
        const auto manaRes = Core::ComputeMagickaScaling(
            baseSkyrimMagicka, snap->poeMaxMana, 0.0, snap->exileLevel, *g_dataPack, *g_config);
        const double desiredMagickaDelta = manaRes.skyrimMaxMagicka - baseSkyrimMagicka;
        if (std::abs(desiredMagickaDelta - appliedMagicka) > 0.25) {
            const float diff = static_cast<float>(desiredMagickaDelta - appliedMagicka);
            avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kMagicka, diff);
            appliedMagicka += static_cast<double>(diff);
            deltasChanged = true;
        }

        if (deltasChanged) {
            SaveLoad::UpdateState([appliedHealth, appliedMagicka](PersistentState& st) {
                st.appliedHealthDelta = appliedHealth;
                st.appliedMagickaDelta = appliedMagicka;
            });
        }

        g_lastSyncedSnapVersion = snap->version;
        g_lastObservedPermHealth = static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kHealth));
        g_lastObservedPermMagicka = static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kMagicka));
    }

    void CombatHooks::NotifyPlayerElementalCrit() noexcept
    {
        g_eoTriggered.store(true, std::memory_order_release);
    }

    double CombatHooks::GetBaseSkyrimMaxMagicka(RE::PlayerCharacter* player) noexcept
    {
        if (!player) {
            return 100.0;
        }
        auto* avOwner = player->AsActorValueOwner();
        if (!avOwner) {
            return 100.0;
        }
        const double perm = static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kMagicka));
        const double appliedMagicka = SaveLoad::GetStateCopy().appliedMagickaDelta;
        return std::max(1.0, perm - appliedMagicka);
    }

    std::uint64_t CombatHooks::GetPlayerToTeammateHitCount() noexcept
    {
        return g_playerToTeammateHits.load(std::memory_order_relaxed);
    }

    std::uint64_t CombatHooks::GetMapDeathDamageClampCount(bool magic) noexcept
    {
        return (magic ? g_mapMagicDeathClamps : g_mapPhysicalDeathClamps).load(std::memory_order_relaxed);
    }

    std::uint64_t CombatHooks::GetMapDeathRepeatClampCount(bool magic) noexcept
    {
        return (magic ? g_mapMagicDeathRepeatClamps : g_mapPhysicalDeathRepeatClamps).load(std::memory_order_relaxed);
    }
}
