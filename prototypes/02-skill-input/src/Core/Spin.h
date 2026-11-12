#pragma once

namespace poe
{
    inline constexpr float kTwoPi = 6.28318530718f;

    // 버튼을 뗀 뒤 남은 바퀴를 마저 돌 때의 최소 속도(초당 바퀴 수).
    // 남은 각이 가장 클 때(한 바퀴)에도 1/3초 안에 정면으로 돌아온다.
    inline constexpr float kSettleTurnsPerSecond = 3.0f;

    // 채널링 중 몸이 돈 각도를 센다. 게임 타입에 기대지 않는다.
    //   - 누르고 있는 동안: 정해진 속도로 계속 돈다. 각도는 0 이상 2파이 미만으로 유지한다.
    //   - 버튼을 떼면(BeginSettle): 같은 방향으로 남은 바퀴를 마저 돌아 0(정면)에서 끝낸다.
    //   - 그 사이에 다시 누르면(Resume): 마무리를 그만두고 평소 속도로 이어 돈다.
    class SpinClock
    {
    public:
        void Reset();

        // 마무리를 시작한다. turnsPerSecond는 평소의 회전 속도.
        void BeginSettle(float turnsPerSecond);

        void Resume();

        // seconds만큼 진행한다. 마무리 중이고 정면(0)에 닿았으면 true.
        bool Advance(float turnsPerSecond, float seconds);

        [[nodiscard]] float Angle() const { return _angle; }
        [[nodiscard]] bool  Settling() const { return _settling; }

    private:
        float _angle = 0.0f;         // 라디안, 0 이상 2파이 미만
        bool  _settling = false;
        float _settleSpeed = 0.0f;   // 라디안/초
    };
}
