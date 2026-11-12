#include "Core/EventNames.h"

#include <cstdio>
#include <string_view>

namespace
{
    int g_failures = 0;

    void Expect(bool condition, std::string_view what)
    {
        if (!condition) {
            ++g_failures;
            std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
        }
    }
}

int main()
{
    using poe::IsAttackRelatedEvent;
    using poe::IsAttackStartEvent;
    using poe::IsPowerAttackStartEvent;

    // 일반 공격 시작: 시작이지만 강공격은 아니다.
    for (const std::string_view name : { "attackStart", "attackStartLeftHand", "attackStartDualWield",
             "attackStartSprint", "AttackStart", "MCO_AttackInitiate" }) {
        Expect(IsAttackStartEvent(name), name);
        Expect(!IsPowerAttackStartEvent(name), name);
        Expect(IsAttackRelatedEvent(name), name);
    }

    // 강공격 시작.
    for (const std::string_view name : { "attackPowerStartInPlace", "attackPowerStartForward",
             "attackPowerStartBackward", "attackPowerStartLeft", "attackPowerStartRight",
             "attackPowerStartDualWield", "attackPowerStart_Sprint", "attackPowerStart_2HMSprint",
             "attackPowerStartForwardH2HRightHand", "ATTACKPOWERSTARTINPLACE", "MCO_PowerAttackInitiate" }) {
        Expect(IsAttackStartEvent(name), name);
        Expect(IsPowerAttackStartEvent(name), name);
    }

    // 공격과 관련은 있지만 시작이 아닌 것: 중단, 창 열림, 끝 알림, 놓기.
    for (const std::string_view name : { "attackStop", "attackWinStart", "attackWinStartLeft", "AttackWinEnd",
             "PowerAttack_Start_end", "attackRelease", "bashStart", "bashPowerStart", "bashStop" }) {
        Expect(!IsAttackStartEvent(name), name);
        Expect(!IsPowerAttackStartEvent(name), name);
        Expect(IsAttackRelatedEvent(name), name);
    }

    // 공격과 무관한 것. 이 DLL이 보내는 이벤트도 여기에 든다.
    for (const std::string_view name : { "", "JumpUp", "weaponSwing", "blockStart", "SprintStart", "staggerStart",
             "arrowAttach", "PoEProto_CycloneStart", "PoEProto_CycloneStop" }) {
        Expect(!IsAttackRelatedEvent(name), name);
        Expect(!IsAttackStartEvent(name), name);
    }

    if (g_failures == 0) {
        std::printf("EventNamesTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
