#include "Core/Config.h"

#include <cstdio>
#include <filesystem>
#include <limits>
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
        std::vector<std::string> tryWarnings;
        const auto missing =
            poe::TryLoadConfig(std::filesystem::temp_directory_path() / "poe_proto_missing_file.ini", tryWarnings);
        Expect(!missing.has_value() && tryWarnings.size() == 1, "TryLoadConfig는 파일이 없으면 값을 돌려주지 않음");
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
        std::vector<std::string> warnings;
        const auto d = poe::ParseConfig("", warnings);
        Expect(d.critChance == 0.25f && d.triggerCooldown == 0.15f, "발동 기본 확률과 쿨다운");
        Expect(d.triggerSpellPlugin == "Skyrim.esm" && d.triggerSpellFormId == 0x01C789, "발동 기본 주문");
        Expect(d.addSkyrimCrit, "스카이림 치명타 확률 합산은 기본으로 켜짐");
        const auto noSkyrim = poe::ParseConfig("AddSkyrimCrit = off\n", warnings);
        Expect(!noSkyrim.addSkyrimCrit, "AddSkyrimCrit=off");
        const auto c = poe::ParseConfig(
            "CritChance = 1\nTriggerCooldown = 0\nTriggerSpell = My Mod.esp | 0x00ABCD\n", warnings);
        Expect(c.critChance == 1.0f && c.triggerCooldown == 0.0f, "발동 확률과 쿨다운");
        Expect(c.triggerSpellPlugin == "My Mod.esp" && c.triggerSpellFormId == 0xABCD, "플러그인 이름의 공백과 16진수 FormID");
        const auto dec = poe::ParseConfig("TriggerSpell = Update.esm|4660\n", warnings);
        Expect(dec.triggerSpellPlugin == "Update.esm" && dec.triggerSpellFormId == 4660, "10진수 FormID");
        const auto off = poe::ParseConfig("TriggerSpell =\n", warnings);
        Expect(off.triggerSpellPlugin.empty() && off.triggerSpellFormId == 0, "빈 값이면 발동 끔");
        Expect(warnings.empty(), "올바른 발동 설정에는 경고가 없음");
    }
    {
        std::vector<std::string> warnings;
        const auto c = poe::ParseConfig(
            "CritChance = 1.5\nTriggerCooldown = -1\nTriggerSpell = Skyrim.esm\nTriggerSpell = |0x10\n"
            "TriggerSpell = Skyrim.esm|0x1000000\nTriggerSpell = Skyrim.esm|0\nAddSkyrimCrit = sometimes\n",
            warnings);
        Expect(c.critChance == 0.25f && c.triggerCooldown == 0.15f, "범위를 벗어난 발동 수치는 기본값 유지");
        Expect(c.triggerSpellPlugin == "Skyrim.esm" && c.triggerSpellFormId == 0x01C789, "잘못된 주문 지정은 기본값 유지");
        Expect(c.addSkyrimCrit, "잘못된 합산 설정은 기본값 유지");
        Expect(warnings.size() == 7, "잘못된 줄마다 경고 하나(발동)");
    }
    {
        std::vector<std::string> warnings;
        const auto d = poe::ParseConfig("", warnings);
        Expect(d.triggerMode == poe::TriggerMode::kDirect && d.triggerDamage == 40.0f && d.triggerRadius == 200.0f,
            "직접 피해 발동 기본값");
        Expect(d.triggerExplosionPlugin == "Skyrim.esm" && d.triggerExplosionFormId == 0x0439C0, "연출용 폭발 기본값");
        Expect(!d.triggerDifficultyTest, "난이도 인자 시험은 기본으로 꺼짐");
        const auto diff = poe::ParseConfig("TriggerDifficultyTest = true\n", warnings);
        Expect(diff.triggerDifficultyTest, "TriggerDifficultyTest=true");
        const auto c = poe::ParseConfig(
            "TriggerMode = SPELL\nTriggerDamage = 125.5\nTriggerRadius = 0\nTriggerExplosion = My Mod.esp|0x801\n", warnings);
        Expect(c.triggerMode == poe::TriggerMode::kSpell, "TriggerMode는 대소문자를 가리지 않음");
        Expect(c.triggerDamage == 125.5f && c.triggerRadius == 0.0f, "직접 피해와 반경(0 허용)");
        Expect(c.triggerExplosionPlugin == "My Mod.esp" && c.triggerExplosionFormId == 0x801, "연출용 폭발 지정");
        Expect(c.triggerSpellPlugin == "Skyrim.esm" && c.triggerSpellFormId == 0x01C789, "폭발을 바꿔도 주문 지정은 그대로");
        const auto none = poe::ParseConfig("TriggerExplosion =\n", warnings);
        Expect(none.triggerExplosionPlugin.empty() && none.triggerExplosionFormId == 0, "빈 값이면 연출 없음");
        Expect(none.triggerSpellPlugin == "Skyrim.esm", "폭발을 비워도 주문 지정은 그대로");
        Expect(warnings.empty(), "올바른 직접 피해 설정에는 경고가 없음");
    }
    {
        std::vector<std::string> warnings;
        const auto c = poe::ParseConfig(
            "TriggerMode = melee\nTriggerDamage = 0\nTriggerRadius = 1001\nTriggerExplosion = Skyrim.esm\n"
            "TriggerExplosion = Skyrim.esm|0\n",
            warnings);
        Expect(c.triggerMode == poe::TriggerMode::kDirect && c.triggerDamage == 40.0f && c.triggerRadius == 200.0f,
            "범위를 벗어난 직접 피해 설정은 기본값 유지");
        Expect(c.triggerExplosionPlugin == "Skyrim.esm" && c.triggerExplosionFormId == 0x0439C0,
            "잘못된 폭발 지정은 기본값 유지");
        Expect(warnings.size() == 5, "잘못된 줄마다 경고 하나(직접 피해)");
    }
    {
        std::vector<std::string> warnings;
        const auto d = poe::ParseConfig("", warnings);
        Expect(d.powerInput == poe::PowerInput::kRightHand, "강공격 입력의 기본값은 오른손 공격 버튼");
        const auto left = poe::ParseConfig("PowerAttackInput = LeftHand\n", warnings);
        Expect(left.powerInput == poe::PowerInput::kLeftHand, "PowerAttackInput=leftHand(대소문자를 가리지 않음)");
        const auto any = poe::ParseConfig("PowerAttackInput = any ; 둘 다\n", warnings);
        Expect(any.powerInput == poe::PowerInput::kAny, "PowerAttackInput=any");
        const auto right = poe::ParseConfig("PowerAttackInput = any\nPowerAttackInput = rightHand\n", warnings);
        Expect(right.powerInput == poe::PowerInput::kRightHand, "PowerAttackInput=rightHand");
        Expect(warnings.empty(), "올바른 강공격 입력 설정에는 경고가 없음");
        const auto bad = poe::ParseConfig("PowerAttackInput = both\nPowerAttackInput =\n", warnings);
        Expect(bad.powerInput == poe::PowerInput::kRightHand, "잘못된 강공격 입력은 기본값 유지");
        Expect(warnings.size() == 2, "잘못된 줄마다 경고 하나(강공격 입력)");
        Expect(poe::PowerInputName(poe::PowerInput::kRightHand) == "rightHand" &&
               poe::PowerInputName(poe::PowerInput::kLeftHand) == "leftHand" &&
               poe::PowerInputName(poe::PowerInput::kAny) == "any",
            "PowerInputName");
    }
    {
        std::vector<std::string> warnings;
        const auto d = poe::ParseConfig("", warnings);
        Expect(d.channelIgnoreStagger == 0.5f, "채널링 중 무시하는 경직 세기의 기본값");
        const auto off = poe::ParseConfig("ChannelIgnoreStagger = -1\n", warnings);
        Expect(off.channelIgnoreStagger == -1.0f, "ChannelIgnoreStagger=-1(무시하지 않음)");
        const auto all = poe::ParseConfig("ChannelIgnoreStagger = 1\n", warnings);
        Expect(all.channelIgnoreStagger == 1.0f, "ChannelIgnoreStagger=1(모두 무시)");
        Expect(warnings.empty(), "올바른 경직 설정에는 경고가 없음");
        const auto bad = poe::ParseConfig("ChannelIgnoreStagger = 2\nChannelIgnoreStagger = all\n", warnings);
        Expect(bad.channelIgnoreStagger == 0.5f, "범위를 벗어난 경직 설정은 기본값 유지");
        Expect(warnings.size() == 2, "잘못된 줄마다 경고 하나(경직)");

        Expect(d.testStaggerKey == 0, "시험용 경직 키는 기본으로 꺼짐");
        const auto key = poe::ParseConfig("TestStaggerKey = 0x42\n", warnings);
        Expect(key.testStaggerKey == 0x42 && key.extraChannelKey == 0, "TestStaggerKey=0x42");
        Expect(warnings.size() == 2, "올바른 시험 키에는 경고가 늘지 않음");
        const auto badKey = poe::ParseConfig("TestStaggerKey = 0x1FF\n", warnings);
        Expect(badKey.testStaggerKey == 0, "잘못된 시험 키는 기본값 유지");
        Expect(warnings.size() == 3, "잘못된 시험 키에 경고 하나");

        Expect(poe::ShouldIgnoreStagger(0.5f, 0.0f) && poe::ShouldIgnoreStagger(0.5f, 0.25f) &&
               poe::ShouldIgnoreStagger(0.5f, 0.5f),
            "한도 이하의 경직은 무시(한도 포함)");
        Expect(!poe::ShouldIgnoreStagger(0.5f, 0.75f) && !poe::ShouldIgnoreStagger(0.5f, 1.0f), "한도를 넘는 경직은 무시하지 않음");
        Expect(poe::ShouldIgnoreStagger(0.5f, 0.5004f) && !poe::ShouldIgnoreStagger(0.5f, 0.51f), "한도 근처의 오차 허용");
        Expect(!poe::ShouldIgnoreStagger(-1.0f, 0.0f), "음수 한도면 무시하지 않음");
        Expect(poe::ShouldIgnoreStagger(0.0f, 0.0f) && !poe::ShouldIgnoreStagger(0.0f, 0.5f), "한도 0이면 세기 0만 무시");
        Expect(poe::ShouldIgnoreStagger(1.0f, 1.0f), "한도 1이면 모두 무시");
        const float nan = std::numeric_limits<float>::quiet_NaN();
        Expect(!poe::ShouldIgnoreStagger(0.5f, nan) && !poe::ShouldIgnoreStagger(nan, 0.0f), "NaN은 무시하지 않음");
    }
    {
        // 목록으로 바꿨다가 "키 = 값" 줄로 다시 읽으면 같은 설정이 된다(키 이름과 값 표기가 읽는 쪽과 맞는지).
        const auto roundTrip = [](const poe::Config& config, const char* what) {
            std::string text;
            for (const auto& [key, value] : poe::ConfigEntries(config)) {
                text += key + " = " + value + "\n";
            }
            std::vector<std::string> warnings;
            const auto again = poe::ParseConfig(text, warnings);
            Expect(warnings.empty(), what);
            Expect(poe::ConfigEntries(again) == poe::ConfigEntries(config), what);
            Expect(poe::DescribeConfigChanges(config, again).empty(), what);
        };
        std::vector<std::string> warnings;
        const auto defaults = poe::ParseConfig("", warnings);
        roundTrip(defaults, "기본 설정의 왕복");
        const auto custom = poe::ParseConfig(
            "Enabled = false\nMotion = injector\nTrigger = hold\nPowerAttackInput = any\nHoldSeconds = 0.45\n"
            "MinSeconds = 0\nMaxSeconds = 120\nChannelIgnoreStagger = -1\nSpinByDll = false\nSpinClockwise = true\n"
            "HitsByDll = false\nHitNeutral = true\nHitsPerSecond = 4.5\nTurnsPerHit = 1\nHitRadius = 200\n"
            "HitDamageMult = 0.25\nCritChance = 0.15\nAddSkyrimCrit = false\nTriggerCooldown = 0\nTriggerMode = spell\n"
            "TriggerDamage = 125.5\nTriggerRadius = 0\nTriggerDifficultyTest = true\nTriggerExplosion =\n"
            "TriggerSpell = My Mod.esp|0xABCD\nClipVariants = 4\nClipVariant = 3\nCycleKey = 0\nExtraChannelKey = 47\n"
            "TestStaggerKey = 0x42\nReloadKey = 0x41\nLogEvents = false\n",
            warnings);
        Expect(warnings.empty(), "모든 키를 바꾼 설정에는 경고가 없음");
        Expect(custom.reloadKey == 0x41 && defaults.reloadKey == 0, "ReloadKey와 그 기본값");
        roundTrip(custom, "모든 키를 바꾼 설정의 왕복");

        const auto changes = poe::DescribeConfigChanges(defaults, custom);
        Expect(changes.size() == poe::ConfigEntries(defaults).size(), "모든 키를 바꾸면 모든 키가 바뀐 것으로 나옴");
        const auto one = poe::ParseConfig("CritChance = 0.3\n", warnings);
        const auto oneChange = poe::DescribeConfigChanges(defaults, one);
        Expect(oneChange.size() == 1 && oneChange[0] == "CritChance: 0.25 -> 0.3", "값 하나가 바뀐 경우의 표기");
        const auto noEffect = poe::ParseConfig("TriggerExplosion =\n", warnings);
        const auto cleared = poe::DescribeConfigChanges(defaults, noEffect);
        Expect(cleared.size() == 1 && cleared[0] == "TriggerExplosion: Skyrim.esm|0x0439C0 -> (비어 있음)", "비운 값의 표기");
        Expect(poe::DescribeConfigChanges(defaults, defaults).empty(), "같은 설정이면 바뀐 것이 없음");
    }
    {
        Expect(poe::TriggerModeName(poe::TriggerMode::kDirect) == "direct" &&
               poe::TriggerModeName(poe::TriggerMode::kSpell) == "spell",
            "TriggerModeName");
        Expect(poe::MotionName(poe::Motion::kInjector) == "injector", "MotionName");
        Expect(poe::TriggerName(poe::Trigger::kHold) == "hold", "TriggerName");
    }

    if (g_failures == 0) {
        std::printf("ConfigTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
