#include "Core/Crit.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace
{
    int g_failures = 0;

    void Expect(bool condition, const char* what)
    {
        if (!condition) {
            ++g_failures;
            std::printf("FAIL: %s\n", what);
        }
    }

    bool Near(float a, float b)
    {
        return std::abs(a - b) < 1e-5f;
    }
}

int main()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    Expect(Near(poe::CombineCritChance(0.25f, 0.0f), 0.25f), "스카이림 값이 0이면 우리 확률 그대로");
    Expect(Near(poe::CombineCritChance(0.25f, 10.0f), 0.35f), "퍼센트로 받은 스카이림 값을 더함");
    Expect(Near(poe::CombineCritChance(0.0f, 15.0f), 0.15f), "우리 확률이 0이어도 스카이림 값은 남음");
    Expect(Near(poe::CombineCritChance(0.9f, 30.0f), 1.0f), "합이 1을 넘으면 1");
    Expect(Near(poe::CombineCritChance(0.1f, -25.0f), 0.0f), "합이 0보다 작으면 0");
    Expect(Near(poe::CombineCritChance(0.25f, -10.0f), 0.15f), "스카이림 값이 음수면 깎임");
    Expect(Near(poe::CombineCritChance(0.25f, nan), 0.25f), "스카이림 값이 NaN이면 0으로 봄");
    Expect(Near(poe::CombineCritChance(0.25f, inf), 0.25f), "스카이림 값이 무한대면 0으로 봄");
    Expect(Near(poe::CombineCritChance(nan, 10.0f), 0.10f), "우리 확률이 NaN이면 0으로 봄");

    if (g_failures == 0) {
        std::printf("CritTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
