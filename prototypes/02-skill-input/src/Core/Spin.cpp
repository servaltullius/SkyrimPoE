#include "Core/Spin.h"

#include <algorithm>
#include <cmath>

namespace poe
{
    void SpinClock::Reset()
    {
        _angle = 0.0f;
        _settling = false;
        _settleSpeed = 0.0f;
    }

    void SpinClock::BeginSettle(float turnsPerSecond)
    {
        _settling = true;
        _settleSpeed = kTwoPi * std::max(turnsPerSecond, kSettleTurnsPerSecond);
    }

    void SpinClock::Resume()
    {
        _settling = false;
    }

    bool SpinClock::Advance(float turnsPerSecond, float seconds)
    {
        if (!(seconds > 0.0f) || !std::isfinite(turnsPerSecond)) {
            // 시간이 흐르지 않았으면 그대로다. 다만 이미 정면에 있는 채로 마무리를 시작했다면 끝난 것이다.
            return _settling && _angle == 0.0f;
        }
        if (_settling) {
            if (_angle == 0.0f) {
                return true;  // 정면에서 뗐다. 더 돌 것이 없다
            }
            const float remaining = kTwoPi - _angle;
            const float step = _settleSpeed * seconds;
            if (step >= remaining) {
                _angle = 0.0f;
                return true;
            }
            _angle += step;
            return false;
        }
        _angle = std::fmod(_angle + kTwoPi * turnsPerSecond * seconds, kTwoPi);
        if (_angle < 0.0f) {
            _angle += kTwoPi;
        }
        return false;
    }
}
