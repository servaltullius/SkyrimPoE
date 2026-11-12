#include "Core/Spin.h"

#include <cmath>
#include <cstdio>

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

    bool Near(float a, float b, float tolerance = 1e-3f)
    {
        return std::fabs(a - b) <= tolerance;
    }
}

int main()
{
    using poe::kTwoPi;
    using poe::SpinClock;
    constexpr float frame = 1.0f / 60.0f;

    {
        // 누르고 있는 동안: 초당 1바퀴면 0.25초에 4분의 1바퀴, 각도는 2파이 미만으로 유지된다.
        SpinClock spin;
        for (int i = 0; i < 15; ++i) {
            Expect(!spin.Advance(1.0f, frame), "누르고 있는 동안에는 끝났다고 하지 않음");
        }
        Expect(Near(spin.Angle(), kTwoPi * 0.25f), "0.25초에 4분의 1바퀴");
        for (int i = 0; i < 600; ++i) {
            spin.Advance(1.7f, frame);
            Expect(spin.Angle() >= 0.0f && spin.Angle() < kTwoPi, "각도는 0 이상 2파이 미만");
        }
    }
    {
        // 마무리: 남은 각을 같은 방향으로 마저 돌고 정확히 0에서 끝난다. 가장 긴 경우에도 1/3초 남짓이다.
        SpinClock spin;
        spin.Advance(1.0f, 0.01f);  // 정면을 막 지난 상태: 남은 각이 거의 한 바퀴
        spin.BeginSettle(1.0f);
        Expect(spin.Settling(), "마무리 중으로 표시됨");
        float previous = spin.Angle();
        int frames = 0;
        bool finished = false;
        while (!finished && frames < 600) {
            finished = spin.Advance(1.0f, frame);
            ++frames;
            if (!finished) {
                Expect(spin.Angle() > previous, "마무리 중에도 같은 방향으로만 돈다");
                previous = spin.Angle();
            }
        }
        Expect(finished, "마무리가 끝난다");
        Expect(spin.Angle() == 0.0f, "정확히 정면에서 끝난다");
        Expect(frames <= 21, "가장 긴 마무리도 약 1/3초(60fps에서 21프레임 이내)");
    }
    {
        // 평소 속도가 마무리 최소 속도보다 빠르면 평소 속도를 유지한다(느려지지 않는다).
        SpinClock spin;
        spin.Advance(5.0f, 0.02f);  // 0.1바퀴
        spin.BeginSettle(5.0f);
        int frames = 0;
        while (!spin.Advance(5.0f, frame) && frames < 600) {
            ++frames;
        }
        Expect(frames <= 11, "빠른 회전의 마무리는 평소 속도로(0.9바퀴를 초당 5바퀴로 = 0.18초)");
    }
    {
        // 정면에서 떼면 더 돌지 않고 바로 끝난다.
        SpinClock spin;
        spin.BeginSettle(1.0f);
        Expect(spin.Advance(1.0f, frame), "각도 0에서 마무리를 시작하면 바로 끝");
        Expect(spin.Angle() == 0.0f, "각도는 그대로 0");
    }
    {
        // 마무리 중에 다시 누르면 이어서 돈다.
        SpinClock spin;
        spin.Advance(1.0f, 0.3f);
        spin.BeginSettle(1.0f);
        spin.Advance(1.0f, frame);
        spin.Resume();
        Expect(!spin.Settling(), "다시 누르면 마무리 해제");
        const float before = spin.Angle();
        Expect(!spin.Advance(1.0f, frame), "이어서 도는 동안에는 끝났다고 하지 않음");
        Expect(Near(spin.Angle() - before, kTwoPi * frame), "평소 속도로 이어짐");
    }
    {
        // 시간이 흐르지 않은 프레임과 잘못된 속도는 각도를 바꾸지 않는다.
        SpinClock spin;
        spin.Advance(1.0f, 0.1f);
        const float before = spin.Angle();
        Expect(!spin.Advance(1.0f, 0.0f), "0초 프레임");
        Expect(!spin.Advance(std::nanf(""), frame), "숫자가 아닌 속도");
        Expect(spin.Angle() == before, "각도 유지");
        spin.Reset();
        Expect(spin.Angle() == 0.0f && !spin.Settling(), "Reset");
    }

    if (g_failures == 0) {
        std::printf("SpinTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
