#pragma once

#include <string_view>

namespace poe
{
    // 플레이어의 애니메이션 그래프로 보내지는 이벤트를 이름으로 가려낸다. 대소문자는 가리지 않는다.

    // 공격이나 방패 가격과 관련된 이름인가(로그에 남길 대상).
    bool IsAttackRelatedEvent(std::string_view name);

    // 공격을 시작하라는 이벤트인가.
    // 바닐라는 attackStart…, attackPowerStart… 이고 MCO 계열에는 …Initiate 이름도 있다.
    // 공격 창 열림(attackWinStart), 끝 알림(…_end), 중단(attackStop) 같은 것은 시작이 아니다.
    bool IsAttackStartEvent(std::string_view name);

    // 그 가운데 강공격을 시작하라는 이벤트인가.
    bool IsPowerAttackStartEvent(std::string_view name);

    // 경직을 시작하라는 이벤트인가(staggerStart). 경직의 세기는 그래프 변수 staggerMagnitude에 들어 있다.
    bool IsStaggerStartEvent(std::string_view name);
}
