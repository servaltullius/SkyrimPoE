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

        [[nodiscard]] bool IsBossActor(const RE::Actor* actor)
        {
            if (!actor) {
                return false;
            }
            if (const auto* base = actor->GetActorBase()) {
                if (base->IsUnique()) {
                    return true;
                }
            }
            // 현재 로케이션의 보스 레퍼런스 타입을 가졌는지 검사(안전 null 체크)
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
                const bool isBoss = IsBossActor(actor);
                const bool isDragon = IsDragonActor(actor);

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

                if (!reward.awarded || (reward.exileXpGained == 0 && reward.gemXpGained == 0)) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                // 상태 갱신·스탯 재빌드·HUD 알림은 주 갱신 스레드 예약 큐로 넘긴다(결정 119).
                g_queue->Enqueue([reward]() {
                    if (!g_config) {
                        return;
                    }
                    Core::ExileLevelUpResult exileRes;
                    Core::GemLevelUpSummary gemRes;
                    SaveLoad::UpdateState([&](PersistentState& st) {
                        exileRes = Core::KillTracker::ApplyExileXp(
                            st.exileLevel,
                            st.exileXp,
                            st.unallocatedPassivePoints,
                            reward.exileXpGained,
                            *g_config);
                        gemRes = Core::KillTracker::ApplyGemXpToSlots(st.slots, reward.gemXpGained);
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
