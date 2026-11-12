#pragma once

namespace poe
{
    // 우리 쪽에서 계산한 치명타 확률(0 ~ 1)에 스카이림의 치명타 확률(퍼센트)을 더한다(설계-현황 결정 51).
    // 스카이림의 값은 액터 값 CriticalChance에서 시작해 퍽의 진입점을 거친 것으로, 10이 10%이다.
    // 결과는 0 ~ 1로 자른다. 숫자가 아닌 값(NaN, 무한대)은 0으로 본다.
    float CombineCritChance(float ownChance, float skyrimPercent);
}
