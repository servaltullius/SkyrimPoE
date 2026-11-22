#include "PCH.h"
#include "runtime/DeathListener.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/ProjectileRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"
#include "runtime/WorldDropTracker.h"

#include "core/MapSystem.h"

namespace SkyrimPoE::Runtime
{
    namespace
    {
        // Skyrim.esm 표준 키워드: ActorTypeDragon (0x00035D59)
        constexpr RE::FormID kActorTypeDragonKeywordId = 0x00035D59;

        Core::KillTracker* g_killTracker = nullptr;
        Core::CommandQueue* g_queue = nullptr;
        const Core::Config* g_config = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;
        RE::BGSKeyword* g_dragonKeyword = nullptr;
        bool g_installed = false;

        [[nodiscard]] bool IsDragonActor(const RE::Actor* actor)
        {
            // U3 확정(2026-11-20): 전 모듈 단일 보스·드래곤 판정(SkillExecution 공용 헬퍼)
            return SkillExecution::IsDragonActor(actor) || (actor && g_dragonKeyword && actor->HasKeyword(g_dragonKeyword));
        }

        [[nodiscard]] bool HasLocationBossRefType(const RE::Actor* actor)
        {
            return SkillExecution::HasLocationBossRefType(actor);
        }

        [[nodiscard]] bool IsBossActor(const RE::Actor* actor)
        {
            // U3 확정: 일반 고유 NPC(IsUnique)는 보스로 오판하지 않고 LocTypeBoss 참조 타입만 보스로 판정
            return SkillExecution::HasLocationBossRefType(actor);
        }

        class DeathEventSink final : public RE::BSTEventSink<RE::TESDeathEvent>
        {
        public:
            static DeathEventSink& Get()
            {
                static DeathEventSink instance;
                return instance;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESDeathEvent* event,
                RE::BSTEventSource<RE::TESDeathEvent>*) override
            {
                if (!event || !event->actorDying || !g_killTracker) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (MapDeviceRuntime::IsUninstallCleanupArmed()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* actor = event->actorDying->As<RE::Actor>();
                if (!actor || actor->IsPlayerRef()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                const auto actorId = actor->GetFormID();
                if (!event->dead) {
                    // 결정 114, 115: 사망 시작(dead=false) 시 소환/되살림/지배 상태 고정.
                    // 듀얼 SE+AE 안전 규칙: IsReanimated()는 반드시 AsActorState()를 거쳐 호출한다.
                    const auto* actorState = actor->AsActorState();
                    const bool isSummoned = actor->IsSummoned();
                    const bool isReanimated = actorState ? actorState->IsReanimated() : false;
                    const bool isCommanded = actor->IsCommandedActor();
                    g_killTracker->OnDeathStart(actorId, isSummoned, isReanimated, isCommanded);
                    return RE::BSEventNotifyControl::kContinue;
                }

                // 결정 114, 137(가): 사망 확정(dead=true) 시 보상 1회 지급.
                if (!g_config || !g_queue) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* player = RE::PlayerCharacter::GetSingleton();
                const std::uint32_t enemyLvl = actor->GetLevel();
                const std::uint32_t playerLvl = player ? player->GetLevel() : 1;
                const bool isDragon = IsDragonActor(actor);
                // 결정 116, 170 U-4: 사망 확정 때 희생자의 회차와 보상 조건을 함께 캡처한다.
                const auto mapKill = MapDeviceRuntime::GetMapKillContext(actorId);
                MapDeviceRuntime::NoteConfirmedMapKill(mapKill);
                const bool isBoss = IsBossActor(actor) || mapKill.isBoss;
                // 검수 M5 + 6단계: 이정표 보스 카운트는 오픈월드 바닐라 LocTypeBoss만 인정(동적 지도/결투장 보스는 아틀라스·4차 전용)
                const bool isMilestoneBoss = !isDragon && mapKill.runId == 0 && HasLocationBossRefType(actor);

                const auto* actorState = actor->AsActorState();
                const bool fallbackExcluded = actor->IsSummoned() ||
                    (actorState ? actorState->IsReanimated() : false) ||
                    actor->IsCommandedActor();

                const auto stateCopy = SaveLoad::GetStateCopy();
                const auto reward = g_killTracker->OnDeathConfirm(
                    actorId,
                    enemyLvl,
                    playerLvl,
                    isBoss,
                    isDragon,
                    stateCopy.exileLevel,
                    *g_config,
                    fallbackExcluded,
                    mapKill.rewardMonsterLevel);

                // 결정 119: 원시 Actor* 대신 ActorHandle을 캡처해 주 갱신에서 유효성 재확인
                const RE::ActorHandle corpseHandle = actor->GetHandle();

                // 검수 M5 & 4단계 검수 M2: reward.awarded가 참이면 XP가 0(유배자 Lv.100 등)이어도
                // 1~3차 보스 이정표 및 전리품 드랍 판정을 항상 수행한다.
                // 결정 176 ④: 허가 없이 회차 셀에 있거나 실패한 회차의 액터는 경험치·드랍·클리어를 주지 않는다.
                const bool mapRewardBlocked = mapKill.runId != 0 && mapKill.rewardBlocked;
                if (!reward.awarded || mapRewardBlocked) {
                    // 보상 대상이 아닌 사망자도 상태이상(SpeedMult 원복)·승격 테이블 정리 + 동적 보스 처치(동료 마무리 등) 누락 방지
                    g_queue->Enqueue([actorId, corpseHandle, runId = mapRewardBlocked ? std::uint64_t{ 0 } : mapKill.runId]() {
                        if (MapDeviceRuntime::IsUninstallCleanupArmed()) {
                            return;
                        }
                        MonsterRuntime::OnActorReset(actorId);
                        auto corpsePtr = corpseHandle.get();
                        AilmentRuntime::OnActorKilled(actorId, corpsePtr.get(), false);
                        MapDeviceRuntime::OnActorKilled(actorId, corpsePtr.get(), runId);
                    });
                    return RE::BSEventNotifyControl::kContinue;
                }
                const bool hasXp = (reward.exileXpGained > 0 || reward.gemXpGained > 0);

                // 상태 갱신·드랍 판정·참 충전·전령 폭발·헤드헌터·스탯 재빌드·HUD 알림은 주 갱신 스레드 예약 큐로 넘긴다(결정 119).
                g_queue->Enqueue([actorId, reward, hasXp, isBoss, isDragon, isMilestoneBoss, corpseHandle, mapKill]() {
                    if (!g_config || MapDeviceRuntime::IsUninstallCleanupArmed()) {
                        return;
                    }
                    auto corpsePtr = corpseHandle.get();
                    auto* victim = corpsePtr.get();
                    const auto snap = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;

                    // 5단계(결정 163, 164): 처치 시 몬스터 승격 정리(헤드헌터 훔치기), 상태이상 원복(베렉 전파), 전령 폭발, 참 충전
                    const bool wasChilledOrFrozen =
                        AilmentRuntime::IsFrozen(actorId) || (AilmentRuntime::ChillSlowRatio(actorId) > 0.0);
                    const auto killedProf = MonsterRuntime::OnMonsterKilled(actorId, snap && snap->headhunter);
                    AilmentRuntime::OnActorKilled(actorId, victim, snap && snap->berekProlif);

                    if (victim && snap) {
                        if (snap->heraldOfIceActive && wasChilledOrFrozen) {
                            ProjectileRuntime::TriggerHeraldOfIceBurst(victim->GetPosition(), *snap);
                        }
                        if (snap->heraldOfAshActive) {
                            double overkillEst = 25.0;
                            if (auto* av = victim->AsActorValueOwner()) {
                                const double curHp = static_cast<double>(av->GetActorValue(RE::ActorValue::kHealth));
                                const double maxHp = std::max(1.0, static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth)));
                                overkillEst = (curHp < 0.0) ? std::max(-curHp, maxHp * 0.20) : (maxHp * 0.20);
                            }
                            ProjectileRuntime::TriggerHeraldOfAshBurst(victim->GetPosition(), overkillEst, *snap);
                        }
                    }

                    if (g_charmSystem) {
                        g_charmSystem->OnKillRecharge(killedProf.rarity, isBoss, isDragon);
                        SaveLoad::PersistCharmSlotsFromSystem();
                    }

                    Core::ExileLevelUpResult exileRes;
                    Core::GemLevelUpSummary gemRes;
                    std::uint32_t unlockedTier = 0;
                    std::uint32_t bossCountAfter = 0;
                    std::uint32_t dragonCountAfter = 0;

                    if (hasXp || isDragon || isMilestoneBoss) {
                        SaveLoad::UpdateState([&](PersistentState& st) {
                            if (hasXp) {
                                exileRes = Core::KillTracker::ApplyExileXp(
                                    st.exileLevel,
                                    st.exileXp,
                                    st.unallocatedPassivePoints,
                                    reward.exileXpGained,
                                    *g_config);
                                gemRes = Core::KillTracker::ApplyGemXpToSlots(st.slots, reward.gemXpGained);
                            }
                            if (isDragon) {
                                ++st.tree.dragonKillCount;
                            } else if (isMilestoneBoss) {
                                ++st.tree.bossKillCount;
                            }
                            bossCountAfter = st.tree.bossKillCount;
                            dragonCountAfter = st.tree.dragonKillCount;

                            const std::uint32_t prevMask = st.tree.ascendancyMilestonesBitmask;
                            if ((st.tree.dragonKillCount >= 1 || st.tree.bossKillCount >= 5) && (prevMask & 0x1u) == 0) {
                                st.tree.ascendancyMilestonesBitmask |= 0x1u;
                                unlockedTier = std::max(unlockedTier, 1u);
                            }
                            if ((st.tree.dragonKillCount >= 3 || st.tree.bossKillCount >= 15) && (prevMask & 0x2u) == 0) {
                                st.tree.ascendancyMilestonesBitmask |= 0x2u;
                                unlockedTier = std::max(unlockedTier, 2u);
                            }
                            if ((st.tree.dragonKillCount >= 6 || st.tree.bossKillCount >= 30) && (prevMask & 0x4u) == 0) {
                                st.tree.ascendancyMilestonesBitmask |= 0x4u;
                                unlockedTier = std::max(unlockedTier, 3u);
                            }
                        });
                    }

                    // 4~6단계(결정 16, 157~159, 164, 166): 지도·아틀라스 IIQ/IIR 반영 시체/바닥 드랍 + 지도/결투장 보스 처치 처리
                    const auto curSt = SaveLoad::GetStateCopy();
                    double mapIiq = mapKill.bonusIiqPct;
                    double mapIir = mapKill.bonusIirPct + (snap ? snap->itemRarityPct : 0.0);
                    if (mapKill.dropMapTier != 0) {
                        mapIiq += Core::MapSystem::AtlasBonusIiqPct(curSt.atlas.completedTierMask);
                        mapIir += Core::MapSystem::AtlasBonusIirPct(curSt.atlas.completedTierMask);
                    }

                    WorldDropTracker::OnKillAwarded(
                        corpseHandle, reward.effectiveMonsterLevel, isBoss, isDragon, killedProf.rarity,
                        mapKill.dropMapTier, mapIiq, mapIir, false);
                    MapDeviceRuntime::OnActorKilled(actorId, victim, mapKill.runId);

                    // 5단계(결정 161): 보스 이정표 달성 시 최대 정신력 +30 즉시 반영을 위해 StatEngine 동기화
                    if (exileRes.levelsGained > 0 || gemRes.gemLevelsGained > 0 || unlockedTier > 0) {
                        SaveLoad::SyncStateToStatEngine();
                    }

                    if (exileRes.levelsGained > 0) {
                        const auto msg = std::format("[SkyrimPoE] 유배자 레벨 {} 달성!", exileRes.newLevel);
                        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
                        SKSE::log::info("DeathListener: Exile leveled up to {} (+{} lvl, +{} XP, M_eff={}, mult={:.2f}).",
                            exileRes.newLevel, exileRes.levelsGained, reward.exileXpGained,
                            reward.effectiveMonsterLevel, reward.xpMultiplier);
                    }
                    if (gemRes.gemLevelsGained > 0) {
                        const auto msg = std::format("[SkyrimPoE] 장착 젬 {}개 레벨 상승!", gemRes.gemLevelsGained);
                        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
                        SKSE::log::info("DeathListener: {} equipped gem level-up(s) (+{} gem XP).",
                            gemRes.gemLevelsGained, reward.gemXpGained);
                    }
                    if (unlockedTier > 0) {
                        const auto msg = std::format("[SkyrimPoE] {}차 보스 이정표 달성! 전직 포인트 +2, 패시브 +2, 최대 정신력 +30!", unlockedTier);
                        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
                        SKSE::log::info("DeathListener: Ascendancy milestone tier {} unlocked (dragons={}, bosses={}).",
                            unlockedTier, dragonCountAfter, bossCountAfter);
                    }
                });

                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            DeathEventSink() = default;
        };

        class ResetEventSink final : public RE::BSTEventSink<RE::TESResetEvent>
        {
        public:
            static ResetEventSink& Get()
            {
                static ResetEventSink instance;
                return instance;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESResetEvent* event,
                RE::BSTEventSource<RE::TESResetEvent>*) override
            {
                if (event && event->object) {
                    const auto id = event->object->GetFormID();
                    if (g_killTracker) {
                        g_killTracker->OnActorReset(id);
                    }
                    if (g_queue) {
                        // 검수 Major 1: MonsterRuntime::OnActorReset도 Hasted SpeedMult 원복(엔진 API)을 수행하므로 주 갱신 큐로 이연
                        g_queue->Enqueue([id]() {
                            MonsterRuntime::OnActorReset(id);
                            AilmentRuntime::OnActorReset(id);
                            // 재검수 R-9: 리셋으로 엔진이 임시 수정치를 지웠으므로 맡겨 둔 원복(위 두 호출이 맡긴 것 포함)을 버린다.
                            SaveLoad::ForgetSpeedRevert(id);
                        });
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            ResetEventSink() = default;
        };
    }

    bool DeathListener::Install(
        Core::KillTracker* killTracker,
        Core::CommandQueue* commandQueue,
        const Core::Config* config,
        Core::StatEngine* statEngine,
        Core::CharmSystem* charmSystem)
    {
        if (g_installed) {
            return true;
        }
        g_killTracker = killTracker;
        g_queue = commandQueue;
        g_config = config;
        g_statEngine = statEngine;
        g_charmSystem = charmSystem;
        g_dragonKeyword = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeDragonKeywordId);

        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            SKSE::log::error("DeathListener: ScriptEventSourceHolder unavailable.");
            return false;
        }
        holder->AddEventSink<RE::TESDeathEvent>(&DeathEventSink::Get());
        holder->AddEventSink<RE::TESResetEvent>(&ResetEventSink::Get());

        SaveLoad::AddRevertCallback([]() {
            if (g_killTracker) {
                g_killTracker->ClearOnRevert();
            }
        });

        g_installed = true;
        SKSE::log::info("DeathListener: TESDeathEvent & TESResetEvent sinks registered.");
        return true;
    }
}
