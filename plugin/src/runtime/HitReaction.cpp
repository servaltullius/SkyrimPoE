#include "PCH.h"
#include "runtime/HitReaction.h"
#include "runtime/FormIds.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/ModState.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"
#include "core/HitReaction.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        // 같은 대상에게 폭행 경보를 다시 보내기까지의 시간. 다발 투사체·연쇄가 경보를 쏟지 않게 한다.
        constexpr double kAssaultAlarmRepeatSec = 5.0;
        // 한 대상에 한 번의 처리에서 보내는 적중 이벤트의 상한(타격마다 한 번 보내되, OnHit 처리기가 쏟아지지 않게)
        constexpr std::uint32_t kMaxHitEventsPerDrain = 8;

        Core::CommandQueue* g_queue = nullptr;
        const Core::Config* g_config = nullptr;

        struct PendingHit
        {
            RE::ActorHandle target;
            // 기록한 큐 세대. 되돌리기·안전 제거로 세대가 바뀌면 그 전의 타격은 버린다.
            std::uint64_t generation = 0;
            double maxHitSkyrim = 0.0;
            // 우리 직접 피해의 수(타격마다 적중 이벤트 하나). 엔진 근접 타격(강한 기절만 알려 옴)은 엔진이 이벤트와 전투를 처리한다.
            std::uint32_t skillHits = 0;
            bool heavyStun = false;
        };

        std::mutex g_mutex;
        // 다음 주 갱신에서 처리할 대상(프레임 안의 여러 타격은 대상마다 하나로 묶임)
        std::unordered_map<RE::FormID, PendingHit> g_pending;
        // 대기 중인 처리 예약의 큐 세대 + 1(0이면 예약 없음). 세대가 바뀌어 버려진 예약은 새 세대가 다시 넣는다.
        std::uint64_t g_drainQueuedFor = 0;
        // 대상별 마지막 작은 경직·폭행 경보 시각(되돌리기 때 비움. FF FormID는 다시 쓰인다)
        std::unordered_map<RE::FormID, double> g_lastFlinch;
        std::unordered_map<RE::FormID, double> g_lastAlarm;

        std::atomic<std::uint64_t> g_hitEvents{ 0 };
        std::atomic<std::uint64_t> g_flinches{ 0 };
        std::atomic<std::uint64_t> g_heavyStuns{ 0 };
        std::atomic<std::uint64_t> g_assaultAlarms{ 0 };
        std::atomic<std::uint64_t> g_combatStarts{ 0 };
        std::atomic<std::uint64_t> g_heavyStunRequests{ 0 };
        std::atomic<std::uint64_t> g_staggersRejected{ 0 };

        double NowSec() noexcept
        {
            static const auto base = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - base).count();
        }

        // 경직을 요청한다. NotifyAnimationGraph의 반환값은 이 모드팩에서 실제 경직과 맞지 않았다(2026-11-26: 거절을
        // 돌려준 실행에서 경직이 보였고, 받아들인 실행에서 보이지 않음. 경직 모드가 이 함수를 가로채는 것으로 보임, 가설).
        // 그래서 결과로 동작을 가르지 않고 거절 수만 기록한다.
        void Stagger(RE::Actor* target, double magnitude)
        {
            (void)target->SetGraphVariableFloat("staggerMagnitude", static_cast<float>(magnitude));
            if (!target->NotifyAnimationGraph("staggerStart")) {
                g_staggersRejected.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // 그래프 조작(경직)을 해도 되는 대상인지. 적중 이벤트 수신기가 대상을 바꿀 수 있어 조작 직전에 다시 본다.
        bool CanAnimate(RE::Actor* target)
        {
            return target && !target->IsDead() && !target->IsDeleted() && !target->IsDisabled() && target->Is3DLoaded() &&
                !target->IsInKillMove();
        }

        // 바닐라 폭행 경보(Papyrus Actor.SendAssaultAlarm, 인자 없는 네이티브). VM이 받아들였는지만 알 수 있다.
        bool DispatchAssaultAlarm(RE::Actor* target)
        {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
            if (!policy) {
                return false;
            }
            const auto handle = policy->GetHandleForObject(target->GetFormType(), target);
            if (handle == policy->EmptyHandle()) {
                return false;
            }
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback;
            return vm->DispatchMethodCall2(handle, "Actor", "SendAssaultAlarm", RE::MakeFunctionArguments(), noCallback);
        }

        // 오래된 시각(쉬는 시간·경보 간격을 넘긴 것)을 지워 표가 계속 커지지 않게 한다. g_mutex 안에서 부른다.
        void PruneLocked(double now)
        {
            const double keep = std::max(kAssaultAlarmRepeatSec, g_config ? g_config->skillFlinchCooldownSec : 0.0);
            std::erase_if(g_lastFlinch, [&](const auto& kv) { return now < kv.second || now - kv.second >= keep; });
            std::erase_if(g_lastAlarm, [&](const auto& kv) { return now < kv.second || now - kv.second >= keep; });
        }

        void SendHitEvent(RE::Actor* target, RE::PlayerCharacter* player)
        {
            // 결정 238: 엔진 적중 이벤트. 수단은 반드시 실제 폼이어야 한다(엔진은 비우지 않고, 다른 모드의 처리기가
            // 수단을 null 검사 없이 읽어 튕긴 일이 있음: DtryKeyUtil HitSink, 2026-11-26). 이 자리에서는 어느 스킬인지
            // 알 수 없어 우리 대리 주문(1번 칸)을 수단으로 쓴다. 그 폼을 찾지 못하면 보내지 않는다. 투사체 칸은 근접 타격처럼 비운다.
            auto* source = FormIds::Lookup<RE::SpellItem>(FormIds::kProxySpells[0]);
            auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
            if (source && holder) {
                RE::TESHitEvent event(target, player, source->GetFormID(), 0, RE::TESHitEvent::Flag::kNone);
                holder->SendEvent(&event);
                g_hitEvents.fetch_add(1, std::memory_order_relaxed);
            } else {
                static std::once_flag warned;
                std::call_once(warned, [source] {
                    SKSE::log::warn("HitReaction: {} unavailable; hit events are not sent.", source ? "event source holder" : "hit event source form");
                });
            }
        }

        void React(const PendingHit& hit, double now)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto targetPtr = hit.target.get();
            auto* target = targetPtr.get();
            if (!player || !target) {
                return;
            }
            const auto id = target->GetFormID();
            // 결정 238: 엔진은 죽인 타격에도, 대상이 화면 밖이어도 적중 이벤트를 낸다. 타격마다 하나(상한 있음).
            const auto sendEvents = [&] {
                if (hit.skillHits > 0 && g_config && g_config->sendSkillHitEvents) {
                    for (std::uint32_t i = 0; i < std::min(hit.skillHits, kMaxHitEventsPerDrain); ++i) {
                        SendHitEvent(target, player);
                    }
                }
            };
            if (target->IsDead() || target->IsDeleted() || target->IsDisabled()) {
                sendEvents();
                return;
            }

            // 결정 236: 적대·범죄 전환(결정 112의 "따로 걸어 줌"). 지도 몬스터는 결정 181대로 처음부터 적이라 전투를 건다.
            if (hit.skillHits == 0) {
                // 엔진 타격의 강한 기절만 처리한다.
            } else if (!SkillExecution::IsHostileForFirstHitReward(target, player)) {
                // 이미 전투 중(도망 포함)인 피해자에게는 다시 보내지 않는다. 같은 폭행으로 현상금이 거듭 붙지 않게 한다.
                bool send = !target->IsInCombat();
                if (send) {
                    std::scoped_lock lk(g_mutex);
                    const auto it = g_lastAlarm.find(id);
                    send = it == g_lastAlarm.end() || now < it->second || now - it->second >= kAssaultAlarmRepeatSec;
                }
                if (send) {
                    if (DispatchAssaultAlarm(target)) {
                        g_assaultAlarms.fetch_add(1, std::memory_order_relaxed);
                        // 받아들여진 경보만 간격을 쓴다(거절되면 다음 타격이 다시 시도한다).
                        std::scoped_lock lk(g_mutex);
                        g_lastAlarm[id] = now;
                    } else {
                        SKSE::log::warn("HitReaction: assault alarm for {:08X} was not accepted by the VM.", id);
                    }
                }
            } else if (!target->IsInCombat()) {
                if (target->StartCombat(player)) {
                    g_combatStarts.fetch_add(1, std::memory_order_relaxed);
                }
            }

            sendEvents();

            // 결정 237: 강한 기절이 차면 큰 경직, 아니면 조건이 맞을 때 작은 경직. 요청한 경직을 세고 쉬는 시간을 쓴다.
            if (!CanAnimate(target) || !g_config) {
                return;
            }
            if (hit.heavyStun && g_config->heavyStunStaggerMagnitude > 0.0) {
                Stagger(target, g_config->heavyStunStaggerMagnitude);
                g_heavyStuns.fetch_add(1, std::memory_order_relaxed);
                std::scoped_lock lk(g_mutex);
                g_lastFlinch[id] = now;
                return;
            }
            if (hit.maxHitSkyrim <= 0.0 || g_config->skillFlinchMagnitude <= 0.0) {
                return;
            }
            // 분모는 임시 변화까지 더한 실제 최대 체력
            const double maxHealth = static_cast<double>(target->GetActorValueMax(RE::ActorValue::kHealth));
            const Core::FlinchRule rule{ g_config->skillFlinchMinLifeRatio, g_config->skillFlinchCooldownSec };
            bool flinch = false;
            {
                std::scoped_lock lk(g_mutex);
                const auto it = g_lastFlinch.find(id);
                flinch = Core::ShouldFlinch(hit.maxHitSkyrim, maxHealth, now,
                    it != g_lastFlinch.end() ? std::optional{ it->second } : std::nullopt, rule);
            }
            if (flinch) {
                Stagger(target, g_config->skillFlinchMagnitude);
                g_flinches.fetch_add(1, std::memory_order_relaxed);
                std::scoped_lock lk(g_mutex);
                g_lastFlinch[id] = now;
            }
        }

        void Drain()
        {
            std::unordered_map<RE::FormID, PendingHit> pending;
            {
                std::scoped_lock lk(g_mutex);
                pending.swap(g_pending);
                g_drainQueuedFor = 0;
            }
            if (!g_queue || !ModState::AllowsGameplay() || MapDeviceRuntime::IsUninstallCleanupActive()) {
                return;
            }
            const auto generation = g_queue->CurrentGeneration();
            const double now = NowSec();
            {
                std::scoped_lock lk(g_mutex);
                PruneLocked(now);
            }
            for (const auto& [id, hit] : pending) {
                // 되돌리기·안전 제거 전의 타격은 새 세계에 섞지 않는다.
                if (hit.generation == generation) {
                    React(hit, now);
                }
            }
        }

        void Record(RE::Actor* target, double hitSkyrim, bool skillHit, bool heavyStun) noexcept
        {
            if (!g_queue || !target || target->IsPlayerRef()) {
                return;
            }
            try {
                const auto generation = g_queue->CurrentGeneration();
                const auto handle = target->GetHandle();  // 엔진 핸들 관리자는 우리 잠금 밖에서 부른다
                const auto id = target->GetFormID();
                bool schedule = false;
                {
                    std::scoped_lock lk(g_mutex);
                    auto& entry = g_pending[id];
                    if (entry.generation != generation || entry.target != handle) {
                        entry = PendingHit{ handle, generation };
                    }
                    entry.maxHitSkyrim = std::max(entry.maxHitSkyrim, hitSkyrim);
                    entry.skillHits += skillHit ? 1u : 0u;
                    entry.heavyStun |= heavyStun;
                    if (g_drainQueuedFor != generation + 1) {
                        g_drainQueuedFor = generation + 1;
                        schedule = true;
                    }
                }
                if (schedule) {
                    g_queue->EnqueueForGeneration(generation, [] { Drain(); });
                }
            } catch (...) {
                // 반응은 피해 처리의 부수 효과다. 기록에 실패해도 타격 자체는 그대로 둔다.
                std::scoped_lock lk(g_mutex);
                g_drainQueuedFor = 0;
            }
        }
    }

    void HitReaction::Initialize(Core::CommandQueue* queue, const Core::Config* config)
    {
        g_queue = queue;
        g_config = config;
        SaveLoad::AddRevertCallback([]() {
            std::scoped_lock lk(g_mutex);
            g_pending.clear();
            g_drainQueuedFor = 0;
            g_lastFlinch.clear();
            g_lastAlarm.clear();
        });
    }

    void HitReaction::OnSkillHit(RE::Actor* target, double hitSkyrim) noexcept
    {
        if (std::isfinite(hitSkyrim) && hitSkyrim > 0.0) {
            Record(target, hitSkyrim, true, false);
        }
    }

    void HitReaction::OnHeavyStun(RE::Actor* target) noexcept
    {
        g_heavyStunRequests.fetch_add(1, std::memory_order_relaxed);
        Record(target, 0.0, false, true);
    }

    HitReaction::Stats HitReaction::GetStatsForTest() noexcept
    {
        return { g_hitEvents.load(std::memory_order_relaxed), g_flinches.load(std::memory_order_relaxed),
            g_heavyStuns.load(std::memory_order_relaxed), g_assaultAlarms.load(std::memory_order_relaxed),
            g_combatStarts.load(std::memory_order_relaxed), g_heavyStunRequests.load(std::memory_order_relaxed),
            g_staggersRejected.load(std::memory_order_relaxed) };
    }
}
