#pragma once

#include "core/CommandQueue.h"
#include "core/Types.h"

#include <cstdint>

// 결정 236~238: 우리 스킬(DLL 직접 피해)에 맞은 대상의 반응.
// - 적대·범죄: 플레이어를 적으로 보지 않는 대상은 바닐라 폭행 경보(Papyrus Actor.SendAssaultAlarm),
//   이미 적대인데 전투 중이 아니면 Actor::StartCombat(플레이어).
// - 엔진 적중 이벤트(TESHitEvent)를 보내 다른 모드의 피격 반응을 살린다(INI로 끔).
// - 작은 경직(움찔)과 PoE2 강한 기절의 큰 경직.
// 엔진 호출은 모두 주 갱신 큐에서 한다. 타격 쪽(아무 스레드)은 대상별로 모아 두기만 한다.
namespace SkyrimPoE::Runtime
{
    class HitReaction
    {
    public:
        static void Initialize(Core::CommandQueue* queue, const Core::Config* config);

        // 직접 피해가 실제로 들어간 뒤(지속 피해 틱 제외). hitSkyrim은 체력 감소 + 에너지 실드 흡수(스카이림 단위).
        static void OnSkillHit(RE::Actor* target, double hitSkyrim) noexcept;
        // PoE2 강한 기절 누적이 이번 타격으로 찼을 때.
        static void OnHeavyStun(RE::Actor* target) noexcept;

        // 시험용 누계(주 갱신에서 실제로 한 반응의 수)
        struct Stats
        {
            std::uint64_t hitEvents = 0;
            std::uint64_t flinches = 0;
            std::uint64_t heavyStuns = 0;
            std::uint64_t assaultAlarms = 0;
            std::uint64_t combatStarts = 0;
            std::uint64_t heavyStunRequests = 0;   // 강한 기절이 찼다고 알려 온 수(경직을 그래프가 받았는지와 별개)
            std::uint64_t staggersRejected = 0;    // NotifyAnimationGraph가 거짓을 돌려준 수(실제 경직 여부와 맞지 않았음, 기록용)
        };
        [[nodiscard]] static Stats GetStatsForTest() noexcept;
    };
}
