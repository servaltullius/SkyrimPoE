#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace poe
{
    // 회전 모션을 실제로 재생하는 쪽.
    enum class Motion
    {
        kOar,       // 액터 값 Variable09를 1로 두어 OAR이 이동 클립을 회전 모션으로 바꾸게 한다(프로토타입 1B)
        kInjector,  // Animation Injector의 반복 동작을 이벤트로 시작하고 끝낸다(프로토타입 1A)
    };

    // 채널링을 시작하는 계기.
    enum class Trigger
    {
        kPowerAttack,  // 게임이 강공격을 시작하려는 순간을 가로채 채널링으로 바꾼다
        kHold,         // 공격 버튼을 정해진 시간 넘게 누르고 있으면 시작한다
    };

    struct Config
    {
        bool          enabled = true;
        Motion        motion = Motion::kOar;
        Trigger       trigger = Trigger::kPowerAttack;
        float         holdSeconds = 0.3f;
        float         maxSeconds = 60.0f;
        std::uint32_t cycleKey = 0x44;       // 키보드 DirectInput 스캔 코드. 0x44 = F10, 0 = 쓰지 않음
        std::uint32_t extraChannelKey = 0;   // 강공격 전용 키를 따로 쓰는 환경용. 키보드 스캔 코드, 0 = 없음
        bool          logEvents = true;
    };

    inline constexpr float kMinHoldSeconds = 0.05f;
    inline constexpr float kMaxHoldSeconds = 2.0f;
    inline constexpr float kMinMaxSeconds = 1.0f;
    inline constexpr float kMaxMaxSeconds = 300.0f;

    // INI 본문을 읽어 설정을 만든다. 알 수 없는 키나 범위를 벗어난 값은 기본값을 유지하고 warnings에 사유를 남긴다.
    Config ParseConfig(std::string_view text, std::vector<std::string>& warnings);

    // 파일을 읽어 설정을 만든다. 파일이 없거나 읽지 못하면 기본값을 돌려주고 warnings에 사유를 남긴다.
    Config LoadConfig(const std::filesystem::path& path, std::vector<std::string>& warnings);

    std::string_view MotionName(Motion motion);
    std::string_view TriggerName(Trigger trigger);
}
