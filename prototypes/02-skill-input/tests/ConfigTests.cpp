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
        Expect(c.minSeconds == 0.6f && c.maxSeconds == 60.0f, "기본 최소·최대 시간");
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
            "MinSeconds = 0\r\n"
            "CycleKey = 0x57\r\n"
            "ClipVariants = 4\r\n"
            "ClipVariant = 3\r\n"
            "ExtraChannelKey = 47\r\n"
            "LogEvents = off\r\n",
            warnings);
        Expect(!c.enabled, "Enabled=false");
        Expect(c.motion == poe::Motion::kInjector, "Motion은 대소문자를 가리지 않음");
        Expect(c.trigger == poe::Trigger::kHold, "Trigger는 대소문자를 가리지 않음");
        Expect(c.holdSeconds == 0.5f && c.maxSeconds == 120.0f && c.minSeconds == 0.0f, "초 단위 값");
        Expect(c.cycleKey == 0x57 && c.extraChannelKey == 47, "16진수와 10진수 키 코드");
        Expect(!c.logEvents, "LogEvents=off");
        Expect(c.clipVariants == 4 && c.clipVariant == 3, "클립 가짓수와 시작 번호");
        Expect(warnings.empty(), "올바른 파일에는 경고가 없음");
    }
    {
        std::vector<std::string> warnings;
        const auto c = poe::ParseConfig(
            "Motion = nemesis\n"
            "Trigger = click\n"
            "HoldSeconds = 9\n"
            "MaxSeconds = abc\n"
            "MinSeconds = 11\n"
            "CycleKey = 0x1FF\n"
            "ExtraChannelKey = -3\n"
            "Enabled = maybe\n"
            "Unknown = 1\n"
            "no equals sign\n",
            warnings);
        Expect(c.enabled && c.motion == poe::Motion::kOar && c.trigger == poe::Trigger::kPowerAttack,
            "잘못된 값은 기본값 유지");
        Expect(c.holdSeconds == 0.3f && c.maxSeconds == 60.0f && c.minSeconds == 0.6f, "범위를 벗어난 초 값은 기본값 유지");
        Expect(c.cycleKey == 0x44 && c.extraChannelKey == 0, "잘못된 키 코드는 기본값 유지");
        Expect(warnings.size() == 10, "잘못된 줄마다 경고 하나");
    }
    {
        std::vector<std::string> warnings;
        const auto c = poe::LoadConfig(std::filesystem::temp_directory_path() / "poe_proto_missing_file.ini", warnings);
        Expect(c.enabled && c.motion == poe::Motion::kOar, "파일이 없으면 기본값");
        Expect(warnings.size() == 1, "파일이 없으면 경고 하나");
    }
    {
        std::vector<std::string> warnings;
        const auto c = poe::ParseConfig("ClipVariants = 2\nClipVariant = 5\n", warnings);
        Expect(c.clipVariants == 2 && c.clipVariant == 1, "시작 번호가 가짓수보다 크면 1번으로");
        Expect(warnings.size() == 1, "그때 경고 하나");
    }
    {
        std::vector<std::string> warnings;
        const auto c = poe::ParseConfig("ClipVariants = 0\nClipVariant = 10\n", warnings);
        Expect(c.clipVariants == 1 && c.clipVariant == 1, "범위를 벗어난 클립 번호는 기본값 유지");
        Expect(warnings.size() == 2, "잘못된 줄마다 경고 하나(클립)");
    }
    {
        std::vector<std::string> warnings;
        const auto d = poe::ParseConfig("", warnings);
        Expect(d.spinByDll && d.hitsByDll && !d.spinClockwise && !d.hitNeutral, "회전·타격 기본값");
        Expect(d.hitsPerSecond == 3.0f && d.turnsPerHit == 0.75f && d.hitRadius == 160.0f && d.hitDamageMult == 0.5f,
            "회전·타격 수치 기본값");
        const auto c = poe::ParseConfig(
            "SpinByDll = false\nSpinClockwise = true\nHitsByDll = no\nHitNeutral = on\n"
            "HitsPerSecond = 4.5\nTurnsPerHit = 1\nHitRadius = 200\nHitDamageMult = 0.25\n",
            warnings);
        Expect(!c.spinByDll && c.spinClockwise && !c.hitsByDll && c.hitNeutral, "회전·타격 켬/끔");
        Expect(c.hitsPerSecond == 4.5f && c.turnsPerHit == 1.0f && c.hitRadius == 200.0f && c.hitDamageMult == 0.25f,
            "회전·타격 수치");
        Expect(warnings.empty(), "올바른 회전·타격 설정에는 경고가 없음");
    }
    {
        std::vector<std::string> warnings;
        const auto c = poe::ParseConfig(
            "HitsPerSecond = 0\nTurnsPerHit = 9\nHitRadius = 10\nHitDamageMult = -1\nSpinByDll = maybe\n", warnings);
        Expect(c.hitsPerSecond == 3.0f && c.turnsPerHit == 0.75f && c.hitRadius == 160.0f && c.hitDamageMult == 0.5f &&
               c.spinByDll,
            "범위를 벗어난 회전·타격 값은 기본값 유지");
        Expect(warnings.size() == 5, "잘못된 줄마다 경고 하나(회전·타격)");
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
