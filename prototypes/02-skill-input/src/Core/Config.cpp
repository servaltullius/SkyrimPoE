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

        // 10진수 또는 0x로 시작하는 16진수.
        std::optional<std::uint32_t> ParseKeyCode(std::string_view value)
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
            if (value.empty() || result.ec != std::errc{} || result.ptr != last || out > 0xFF) {
                return std::nullopt;
            }
            return out;
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
            } else if (key == "cyclekey") {
                if (const auto parsed = ParseKeyCode(value)) {
                    config.cycleKey = *parsed;
                } else {
                    Warn(warnings, lineNumber, rawKey, "0~255 범위의 키 코드가 아니어서 기본값 유지");
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
