#include "Core/Config.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

namespace poe
{
    namespace
    {
        std::string_view Trim(std::string_view text)
        {
            const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!text.empty() && isSpace(static_cast<unsigned char>(text.front()))) {
                text.remove_prefix(1);
            }
            while (!text.empty() && isSpace(static_cast<unsigned char>(text.back()))) {
                text.remove_suffix(1);
            }
            return text;
        }

        std::string Lower(std::string_view text)
        {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        std::optional<bool> ParseBool(std::string_view value)
        {
            const auto lower = Lower(value);
            if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
                return true;
            }
            if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
                return false;
            }
            return std::nullopt;
        }

        std::optional<float> ParseFloat(std::string_view value)
        {
            float out = 0.0f;
            const auto* first = value.data();
            const auto* last = value.data() + value.size();
            const auto result = std::from_chars(first, last, out);
            if (result.ec != std::errc{} || result.ptr != last) {
                return std::nullopt;
            }
            return out;
        }

        // 10진수 또는 0x로 시작하는 16진수. max를 넘으면 받지 않는다.
        std::optional<std::uint32_t> ParseUnsigned(std::string_view value, std::uint32_t max)
        {
            int base = 10;
            if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
                base = 16;
                value.remove_prefix(2);
            }
            std::uint32_t out = 0;
            const auto* first = value.data();
            const auto* last = value.data() + value.size();
            const auto result = std::from_chars(first, last, out, base);
            if (value.empty() || result.ec != std::errc{} || result.ptr != last || out > max) {
                return std::nullopt;
            }
            return out;
        }

        // 키보드 스캔 코드처럼 0~255인 값.
        std::optional<std::uint32_t> ParseKeyCode(std::string_view value)
        {
            return ParseUnsigned(value, 0xFF);
        }

        struct FormRef
        {
            std::string_view plugin;
            std::uint32_t    formId = 0;
        };

        // "플러그인.esp|0x1234" 형식. 플러그인 이름이 비었거나 FormID가 0이거나 24비트를 넘으면 받지 않는다.
        std::optional<FormRef> ParseFormRef(std::string_view value)
        {
            const auto bar = value.find('|');
            if (bar == std::string_view::npos) {
                return std::nullopt;
            }
            const auto plugin = Trim(value.substr(0, bar));
            const auto form = ParseUnsigned(Trim(value.substr(bar + 1)), 0xFFFFFF);
            if (plugin.empty() || !form || *form == 0) {
                return std::nullopt;
            }
            return FormRef{ plugin, *form };
        }

        void Warn(std::vector<std::string>& warnings, std::size_t line, std::string_view key, std::string_view why)
        {
            std::ostringstream message;
            message << line << "번째 줄 '" << key << "': " << why;
            warnings.push_back(message.str());
        }
    }

    std::string_view MotionName(Motion motion)
    {
        return motion == Motion::kInjector ? "injector" : "oar";
    }

    std::string_view TriggerName(Trigger trigger)
    {
        return trigger == Trigger::kHold ? "hold" : "powerAttack";
    }

    std::string_view TriggerModeName(TriggerMode mode)
    {
        return mode == TriggerMode::kSpell ? "spell" : "direct";
    }

    Config ParseConfig(std::string_view text, std::vector<std::string>& warnings)
    {
        Config config;
        // UTF-8 BOM이 있으면 건너뛴다.
        if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
            text.remove_prefix(3);
        }

        std::size_t lineNumber = 0;
        while (!text.empty()) {
            ++lineNumber;
            const auto end = text.find('\n');
            auto line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);

            if (const auto comment = line.find_first_of(";#"); comment != std::string_view::npos) {
                line = line.substr(0, comment);
            }
            line = Trim(line);
            if (line.empty() || line.front() == '[') {
                continue;
            }

            const auto equals = line.find('=');
            if (equals == std::string_view::npos) {
                Warn(warnings, lineNumber, line, "'키 = 값' 형식이 아니어서 무시함");
                continue;
            }
            const auto rawKey = Trim(line.substr(0, equals));
            const auto value = Trim(line.substr(equals + 1));
            const auto key = Lower(rawKey);

            if (key == "enabled") {
                if (const auto parsed = ParseBool(value)) {
                    config.enabled = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "true 또는 false가 아니어서 기본값 유지");
                }
            } else if (key == "logevents") {
                if (const auto parsed = ParseBool(value)) {
                    config.logEvents = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "true 또는 false가 아니어서 기본값 유지");
                }
            } else if (key == "motion") {
                const auto lower = Lower(value);
                if (lower == "oar") {
                    config.motion = Motion::kOar;
                } else if (lower == "injector") {
                    config.motion = Motion::kInjector;
                } else {
                    Warn(warnings, lineNumber, rawKey, "oar 또는 injector가 아니어서 기본값 유지");
                }
            } else if (key == "trigger") {
                const auto lower = Lower(value);
                if (lower == "powerattack") {
                    config.trigger = Trigger::kPowerAttack;
                } else if (lower == "hold") {
                    config.trigger = Trigger::kHold;
                } else {
                    Warn(warnings, lineNumber, rawKey, "powerAttack 또는 hold가 아니어서 기본값 유지");
                }
            } else if (key == "holdseconds") {
                const auto parsed = ParseFloat(value);
                if (parsed && *parsed >= kMinHoldSeconds && *parsed <= kMaxHoldSeconds) {
                    config.holdSeconds = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "0.05~2 범위의 숫자가 아니어서 기본값 유지");
                }
            } else if (key == "maxseconds") {
                const auto parsed = ParseFloat(value);
                if (parsed && *parsed >= kMinMaxSeconds && *parsed <= kMaxMaxSeconds) {
                    config.maxSeconds = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "1~300 범위의 숫자가 아니어서 기본값 유지");
                }
            } else if (key == "minseconds") {
                const auto parsed = ParseFloat(value);
                if (parsed && *parsed >= 0.0f && *parsed <= kMaxMinSeconds) {
                    config.minSeconds = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "0~10 범위의 숫자가 아니어서 기본값 유지");
                }
            } else if (key == "cyclekey") {
                if (const auto parsed = ParseKeyCode(value)) {
                    config.cycleKey = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "0~255 범위의 키 코드가 아니어서 기본값 유지");
                }
            } else if (key == "spinbydll" || key == "spinclockwise" || key == "hitsbydll" || key == "hitneutral" ||
                       key == "addskyrimcrit") {
                if (const auto parsed = ParseBool(value)) {
                    (key == "spinbydll" ? config.spinByDll :
                        key == "spinclockwise" ? config.spinClockwise :
                        key == "hitsbydll" ? config.hitsByDll :
                        key == "hitneutral" ? config.hitNeutral : config.addSkyrimCrit) = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "true 또는 false가 아니어서 기본값 유지");
                }
            } else if (key == "hitspersecond" || key == "turnsperhit" || key == "hitradius" || key == "hitdamagemult") {
                // 키마다 받는 범위: 초당 타격 0.5~20, 타격당 바퀴 0.1~4, 반경 50~600, 피해 배율 0.05~5
                const float low = key == "hitspersecond" ? 0.5f : key == "turnsperhit" ? 0.1f : key == "hitradius" ? 50.0f : 0.05f;
                const float high = key == "hitspersecond" ? 20.0f : key == "turnsperhit" ? 4.0f : key == "hitradius" ? 600.0f : 5.0f;
                const auto parsed = ParseFloat(value);
                if (parsed && *parsed >= low && *parsed <= high) {
                    (key == "hitspersecond" ? config.hitsPerSecond :
                        key == "turnsperhit" ? config.turnsPerHit :
                        key == "hitradius" ? config.hitRadius : config.hitDamageMult) = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "받는 범위의 숫자가 아니어서 기본값 유지");
                }
            } else if (key == "critchance") {
                const auto parsed = ParseFloat(value);
                if (parsed && *parsed >= 0.0f && *parsed <= 1.0f) {
                    config.critChance = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "0~1 범위의 숫자가 아니어서 기본값 유지");
                }
            } else if (key == "triggercooldown") {
                const auto parsed = ParseFloat(value);
                if (parsed && *parsed >= 0.0f && *parsed <= 5.0f) {
                    config.triggerCooldown = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "0~5 범위의 숫자가 아니어서 기본값 유지");
                }
            } else if (key == "triggermode") {
                const auto lower = Lower(value);
                if (lower == "direct") {
                    config.triggerMode = TriggerMode::kDirect;
                } else if (lower == "spell") {
                    config.triggerMode = TriggerMode::kSpell;
                } else {
                    Warn(warnings, lineNumber, rawKey, "direct 또는 spell이 아니어서 기본값 유지");
                }
            } else if (key == "triggerdamage") {
                const auto parsed = ParseFloat(value);
                if (parsed && *parsed >= kMinTriggerDamage && *parsed <= kMaxTriggerDamage) {
                    config.triggerDamage = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "1~100000 범위의 숫자가 아니어서 기본값 유지");
                }
            } else if (key == "triggerradius") {
                const auto parsed = ParseFloat(value);
                if (parsed && *parsed >= 0.0f && *parsed <= kMaxTriggerRadius) {
                    config.triggerRadius = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "0~1000 범위의 숫자가 아니어서 기본값 유지");
                }
            } else if (key == "triggerspell" || key == "triggerexplosion") {
                // "플러그인.esp|0x1234" 형식. 값이 비어 있으면 그 폼을 쓰지 않는다.
                auto& plugin = key == "triggerspell" ? config.triggerSpellPlugin : config.triggerExplosionPlugin;
                auto& formId = key == "triggerspell" ? config.triggerSpellFormId : config.triggerExplosionFormId;
                if (value.empty()) {
                    plugin.clear();
                    formId = 0;
                } else if (const auto parsed = ParseFormRef(value)) {
                    plugin.assign(parsed->plugin);
                    formId = parsed->formId;
                } else {
                    Warn(warnings, lineNumber, rawKey, "'플러그인 이름|FormID' 형식이 아니어서 기본값 유지");
                }
            } else if (key == "clipvariants" || key == "clipvariant") {
                const auto parsed = ParseKeyCode(value);
                if (parsed && *parsed >= 1 && *parsed <= kMaxClipVariants) {
                    (key == "clipvariants" ? config.clipVariants : config.clipVariant) = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "1~9 범위의 정수가 아니어서 기본값 유지");
                }
            } else if (key == "extrachannelkey") {
                if (const auto parsed = ParseKeyCode(value)) {
                    config.extraChannelKey = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "0~255 범위의 키 코드가 아니어서 기본값 유지");
                }
            } else {
                Warn(warnings, lineNumber, rawKey, "알 수 없는 키라서 무시함");
            }
        }
        if (config.clipVariant > config.clipVariants) {
            warnings.push_back("ClipVariant가 ClipVariants보다 커서 1번 클립으로 시작함");
            config.clipVariant = 1;
        }
        return config;
    }

    Config LoadConfig(const std::filesystem::path& path, std::vector<std::string>& warnings)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            warnings.push_back("설정 파일을 열지 못해 기본값을 씀: " + path.string());
            return {};
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        if (file.bad()) {
            warnings.push_back("설정 파일을 읽는 중 오류가 나 기본값을 씀: " + path.string());
            return {};
        }
        return ParseConfig(buffer.str(), warnings);
    }
}
