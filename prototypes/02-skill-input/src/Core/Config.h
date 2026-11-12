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
        float         minSeconds = 0.6f;     // 버튼을 일찍 떼도 이만큼은 돈다. 0 = 떼는 즉시 끝남
        std::uint32_t cycleKey = 0x44;       // 키보드 DirectInput 스캔 코드. 0x44 = F10, 0 = 쓰지 않음
        std::uint32_t extraChannelKey = 0;   // 강공격 전용 키를 따로 쓰는 환경용. 키보드 스캔 코드, 0 = 없음
        std::uint32_t clipVariants = 1;      // OAR 방식에서 비교할 회전 클립의 가짓수(서브모드 수)
        std::uint32_t clipVariant = 1;       // 처음에 쓸 클립 번호. 채널링 중 Variable09에 이 값을 넣는다

        // OAR 방식에서 회전과 타격을 DLL이 맡는다(클립은 정지 자세).
        bool  spinByDll = true;              // 스켈레톤의 NPC 노드를 매 프레임 돌린다
        bool  spinClockwise = false;         // 위에서 볼 때 시계 방향으로 돌린다
        bool  hitsByDll = true;              // 일정 주기로 주변의 적을 타격한다
        bool  hitNeutral = false;            // 적대적이지 않은 액터도 타격한다(시험용)
        float hitsPerSecond = 3.0f;          // 무기 속도 1.0일 때의 초당 타격 수. 실제 값은 무기 속도와 공격 속도 배율을 곱한다
        float turnsPerHit = 0.75f;           // 타격 한 번에 도는 바퀴 수. 회전 속도 = 초당 타격 수 x 이 값
        float hitRadius = 160.0f;            // 타격 반경(게임 단위)
        float hitDamageMult = 0.5f;          // 타격 한 번의 피해 배율(무기의 평소 타격 대비)
        bool          logEvents = true;
    };

    inline constexpr float kMinHoldSeconds = 0.05f;
    inline constexpr float kMaxHoldSeconds = 2.0f;
    inline constexpr float kMinMaxSeconds = 1.0f;
    inline constexpr float kMaxMaxSeconds = 300.0f;
    inline constexpr float kMaxMinSeconds = 10.0f;
    inline constexpr std::uint32_t kMaxClipVariants = 9;

    // INI 본문을 읽어 설정을 만든다. 알 수 없는 키나 범위를 벗어난 값은 기본값을 유지하고 warnings에 사유를 남긴다.
    Config ParseConfig(std::string_view text, std::vector<std::string>& warnings);

    // 파일을 읽어 설정을 만든다. 파일이 없거나 읽지 못하면 기본값을 돌려주고 warnings에 사유를 남긴다.
    Config LoadConfig(const std::filesystem::path& path, std::vector<std::string>& warnings);

    std::string_view MotionName(Motion motion);
    std::string_view TriggerName(Trigger trigger);
}
