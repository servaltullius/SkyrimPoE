#include "PCH.h"
#include "runtime/DeathListener.h"
#include "runtime/SaveLoad.h"

namespace SkyrimPoE::Runtime
{
    namespace
    {
        // Skyrim.esm 표준 키워드: ActorTypeDragon (0x00035D59)
        constexpr RE::FormID kActorTypeDragonKeywordId = 0x00035D59;

        Core::KillTracker* g_killTracker = nullptr;
        Core::CommandQueue* g_queue = nullptr;
        const Core::Config* g_config = nullptr;
        RE::BGSKeyword* g_dragonKeyword = nullptr;
        bool g_installed = false;

        [[nodiscard]] bool IsDragonActor(const RE::Actor* actor)
        {
            return actor && g_dragonKeyword && actor->HasKeyword(g_dragonKeyword);
        }

        [[nodiscard]] bool HasLocationBossRefType(const RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }
            // 검수 M5: ExtraLocationRefType의 Boss 참조 타입을 우선 검사
            if (const auto* extraLocRef = actor->extraList.GetByType<RE::ExtraLocationRefType>()) {
                if (extraLocRef->locRefType) {
                    const auto edid = extraLocRef->locRefType->GetFormEditorID();
                    if (edid && std::string_view(edid).find("Boss") != std::string_view::npos) {
                        return true;
                    }
                }
            }
            // 현재 로케이션의 specialRefs에서 보스 참조 타입 검사
            if (auto* loc = actor->GetCurrentLocation()) {
                for (const auto& ref : loc->specialRefs) {
                    if (ref.refData.refID == actor->GetFormID() && ref.type) {
                        const auto edid = ref.type->GetFormEditorID();
                        if (edid && std::string_view(edid).find("Boss") != std::string_view::npos) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        [[nodiscard]] bool IsBossActor(const RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }
            if (HasLocationBossRefType(actor)) {
                return true;
            }
            if (const auto* base = actor->GetActorBase()) {
                if (base->IsUnique()) {
                    return true;
                }
            }
            return false;
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
                const bool isBoss = IsBossActor(actor);
                // 검수 M5: 이정표 보스 카운트는 드래곤과 배타적으로 분리하고 고유 비보스 NPC 오판 방지를 위해 Boss 참조 타입만 인정
                const bool isMilestoneBoss = !isDragon && HasLocationBossRefType(actor);

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
                    fallbackExcluded);

                // 검수 M5: reward.awarded가 참이면 XP가 0(유배자 Lv.100 등)이어도 1~3차 보스 이정표 판정을 수행
                if (!reward.awarded) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const bool hasXp = (reward.exileXpGained > 0 || reward.gemXpGained > 0);
                if (!hasXp && !isDragon && !isMilestoneBoss) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                // 상태 갱신·스탯 재빌드·HUD 알림은 주 갱신 스레드 예약 큐로 넘긴다(결정 119).
                g_queue->Enqueue([reward, hasXp, isDragon, isMilestoneBoss]() {
                    if (!g_config) {
                        return;
                    }
                    Core::ExileLevelUpResult exileRes;
                    Core::GemLevelUpSummary gemRes;
                    std::uint32_t unlockedTier = 0;
                    std::uint32_t bossCountAfter = 0;
                    std::uint32_t dragonCountAfter = 0;

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

                    if (exileRes.levelsGained > 0 || gemRes.gemLevelsGained > 0) {
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
                        const auto msg = std::format("[SkyrimPoE] {}차 보스 이정표 달성! 전직 포인트 +2 획득!", unlockedTier);
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
                if (event && event->object && g_killTracker) {
                    g_killTracker->OnActorReset(event->object->GetFormID());
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
        const Core::Config* config)
    {
        if (g_installed) {
            return true;
        }
        g_killTracker = killTracker;
        g_queue = commandQueue;
        g_config = config;
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
