#include "Core/Config.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

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
}

int main()
{
    {
        std::vector<std::string> warnings;
        const auto c = poe::ParseConfig("", warnings);
        Expect(c.enabled && c.motion == poe::Motion::kOar && c.trigger == poe::Trigger::kPowerAttack,
            "빈 본문은 기본값");
        Expect(c.cycleKey == 0x44 && c.extraChannelKey == 0 && c.logEvents, "기본 키와 로그 설정");
        Expect(warnings.empty(), "빈 본문에는 경고가 없음");
    }
    {
        std::vector<std::string> warnings;
        const auto c = poe::ParseConfig(
            "\xEF\xBB\xBF[General]\r\n"
            "Enabled = false\r\n"
            "Motion = Injector ; 주석\r\n"
            "Trigger=HOLD\r\n"
            "HoldSeconds = 0.5\r\n"
            "MaxSeconds = 120\r\n"
            "CycleKey = 0x57\r\n"
            "ExtraChannelKey = 47\r\n"
            "LogEvents = off\r\n",
            warnings);
        Expect(!c.enabled, "Enabled=false");
        Expect(c.motion == poe::Motion::kInjector, "Motion은 대소문자를 가리지 않음");
        Expect(c.trigger == poe::Trigger::kHold, "Trigger는 대소문자를 가리지 않음");
        Expect(c.holdSeconds == 0.5f && c.maxSeconds == 120.0f, "초 단위 값");
        Expect(c.cycleKey == 0x57 && c.extraChannelKey == 47, "16진수와 10진수 키 코드");
        Expect(!c.logEvents, "LogEvents=off");
        Expect(warnings.empty(), "올바른 파일에는 경고가 없음");
    }
    {
        std::vector<std::string> warnings;
        const auto c = poe::ParseConfig(
            "Motion = nemesis\n"
            "Trigger = click\n"
            "HoldSeconds = 9\n"
            "MaxSeconds = abc\n"
            "CycleKey = 0x1FF\n"
            "ExtraChannelKey = -3\n"
            "Enabled = maybe\n"
            "Unknown = 1\n"
            "no equals sign\n",
            warnings);
        Expect(c.enabled && c.motion == poe::Motion::kOar && c.trigger == poe::Trigger::kPowerAttack,
            "잘못된 값은 기본값 유지");
        Expect(c.holdSeconds == 0.3f && c.maxSeconds == 60.0f, "범위를 벗어난 초 값은 기본값 유지");
        Expect(c.cycleKey == 0x44 && c.extraChannelKey == 0, "잘못된 키 코드는 기본값 유지");
        Expect(warnings.size() == 9, "잘못된 줄마다 경고 하나");
    }
    {
        std::vector<std::string> warnings;
        const auto c = poe::LoadConfig(std::filesystem::temp_directory_path() / "poe_proto_missing_file.ini", warnings);
        Expect(c.enabled && c.motion == poe::Motion::kOar, "파일이 없으면 기본값");
        Expect(warnings.size() == 1, "파일이 없으면 경고 하나");
    }
    {
        Expect(poe::MotionName(poe::Motion::kInjector) == "injector", "MotionName");
        Expect(poe::TriggerName(poe::Trigger::kHold) == "hold", "TriggerName");
    }

    if (g_failures == 0) {
        std::printf("ConfigTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
