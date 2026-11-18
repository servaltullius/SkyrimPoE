#include "Survey.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Survey
{
    namespace
    {
        constexpr std::string_view kSpace = " \t\r\n";

        std::string_view Trim(std::string_view value)
        {
            const auto first = value.find_first_not_of(kSpace);
            if (first == value.npos) {
                return {};
            }
            return value.substr(first, value.find_last_not_of(kSpace) - first + 1);
        }

        template <class Item>
        void SplitList(std::string_view value, Item item)
        {
            for (;;) {
                const auto comma = value.find(',');
                item(Trim(value.substr(0, comma)));
                if (comma == value.npos) {
                    return;
                }
                value.remove_prefix(comma + 1);
            }
        }

        template <class Number>
        bool ParseNumber(std::string_view token, Number& number, int base = 10)
        {
            if (token.empty()) {
                return false;
            }
            const auto end = token.data() + token.size();
            const auto [ptr, error] = std::from_chars(token.data(), end, number, base);
            return error == std::errc{} && ptr == end;
        }

        char LowerAscii(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        bool EqualAscii(std::string_view a, std::string_view b)
        {
            return std::ranges::equal(a, b, {}, LowerAscii, LowerAscii);
        }

        bool ValidFilename(std::string_view name)
        {
            return !name.empty() && name != "." && name != ".." &&
                std::ranges::none_of(name, [](unsigned char c) {
                    return c < 32 || std::string_view(R"(<>:"/\|?*)").find(static_cast<char>(c)) != std::string_view::npos;
                });
        }

        void ReadCell(std::string_view token, std::vector<Cell>& cells, const Warn& warn)
        {
            const auto first = token.find(':');
            const auto second = first == token.npos ? token.npos : token.find(':', first + 1);
            if (second == token.npos) {
                warn(std::format("Cells [{}] 항목에 EditorID:폼 ID:플러그인이 필요함", token));
                return;
            }
            const auto editor = Trim(token.substr(0, first));
            const auto plugin = Trim(token.substr(second + 1));
            std::uint32_t id = 0;
            if (editor.empty() || editor.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != editor.npos) {
                warn(std::format("Cells [{}] EditorID에 영문자·숫자·밑줄이 필요함", token));
            } else if (!ParseNumber(Trim(token.substr(first + 1, second - first - 1)), id, 16) || id == 0 || id > 0xFFFFFF) {
                warn(std::format("Cells [{}] 로컬 폼 ID는 1~FFFFFF 범위의 16진수여야 함", token));
            } else if (!ValidFilename(plugin)) {
                warn(std::format("Cells [{}] 플러그인 파일 이름이 잘못됨", token));
            } else if (std::ranges::any_of(cells, [&](const Cell& cell) {
                return EqualAscii(cell.editorId, editor) || (cell.localId == id && EqualAscii(cell.plugin, plugin));
            })) {
                warn(std::format("Cells [{}] 셀이 이미 목록에 있음", token));
            } else {
                cells.push_back({ std::string(editor), id, std::string(plugin) });
            }
        }

        void ReadSeconds(std::string_view key, std::string_view value, std::chrono::seconds& target, const Warn& warn)
        {
            int seconds = 0;
            if (ParseNumber(value, seconds) && seconds >= (key == "CellTimeoutSeconds" ? 1 : 0)) {
                target = std::chrono::seconds(seconds);
            } else {
                warn(std::format("{} [{}] 지연값이 잘못됨. 이전 값 {}초 사용", key, value, target.count()));
            }
        }

        void ReadBool(std::string_view key, std::string_view value, bool& target, const Warn& warn)
        {
            if (EqualAscii(value, "true") || EqualAscii(value, "false")) {
                target = EqualAscii(value, "true");
            } else {
                warn(std::format("{} [{}] true 또는 false가 필요함. 이전 값 {} 사용", key, value, target));
            }
        }
    }

    Config ReadConfig(std::istream& input, const Warn& warn)
    {
        Config config;
        bool surveySection = false;
        std::string line;
        for (std::size_t lineNumber = 1; std::getline(input, line); ++lineNumber) {
            auto text = Trim(line);
            if (lineNumber == 1 && text.starts_with("\xEF\xBB\xBF")) {
                text = Trim(text.substr(3));
            }
            if (text.empty() || text.starts_with('#') || text.starts_with(';')) {
                continue;
            }
            if (text.starts_with('[')) {
                surveySection = text == "[Survey]";
                if (!text.ends_with(']')) {
                    warn(std::format("{}행 섹션 이름에 닫는 괄호가 없음", lineNumber));
                }
                continue;
            }
            if (!surveySection) {
                continue;
            }
            const auto equals = text.find('=');
            if (equals == text.npos) {
                warn(std::format("{}행 키=값 형식이 필요함", lineNumber));
                continue;
            }
            const auto key = Trim(text.substr(0, equals));
            const auto value = Trim(text.substr(equals + 1));
            if (key == "Levels") {
                SplitList(value, [&](std::string_view token) {
                    int level = 0;
                    if (ParseNumber(token, level) && level >= 1 && level <= 81) {
                        config.levels.push_back(level);
                    } else {
                        warn(std::format("Levels [{}] 1~81 범위의 정수가 필요함", token));
                    }
                });
            } else if (key == "Cells") {
                SplitList(value, [&](std::string_view token) { ReadCell(token, config.cells, warn); });
            } else if (key == "MainMenuDelaySeconds") {
                ReadSeconds(key, value, config.mainMenuDelay, warn);
            } else if (key == "StartDelaySeconds") {
                ReadSeconds(key, value, config.startDelay, warn);
            } else if (key == "SettleSeconds") {
                ReadSeconds(key, value, config.settle, warn);
            } else if (key == "CellTimeoutSeconds") {
                ReadSeconds(key, value, config.cellTimeout, warn);
            } else if (key == "QuitWhenDone") {
                ReadBool(key, value, config.quitWhenDone, warn);
            } else if (key == "Notify") {
                ReadBool(key, value, config.notify, warn);
            } else {
                warn(std::format("{}행 알 수 없는 Survey 키 [{}]", lineNumber, key));
            }
        }
        if (input.bad()) {
            throw std::runtime_error("INI 읽기 실패");
        }
        return config;
    }

    std::optional<std::string> ParseMarker(std::string_view text)
    {
        if (text.starts_with("\xEF\xBB\xBF")) {
            text.remove_prefix(3);
        }
        const auto end = text.find('\n');
        if (end != text.npos && !Trim(text.substr(end + 1)).empty()) {
            return std::nullopt;
        }
        auto save = Trim(text.substr(0, end));
        if (!ValidFilename(save)) {
            return std::nullopt;
        }
        return std::string(save);
    }

    bool DeferCurrentCell(std::deque<std::uint32_t>& order, std::uint32_t current)
    {
        if (order.empty()) {
            return false;
        }
        if (order.front() != current) {
            return true;
        }
        if (order.size() == 1) {
            return false;
        }
        std::rotate(order.begin(), order.begin() + 1, order.end());
        return true;
    }

    std::filesystem::path UniqueLogPath(const std::filesystem::path& directory, std::string_view stamp,
        const std::function<bool(const std::filesystem::path&)>& occupied)
    {
        auto path = directory / std::format("survey-{}.tsv", stamp);
        for (std::size_t i = 2; occupied(path); ++i) {
            path = directory / std::format("survey-{}-{}.tsv", stamp, i);
        }
        return path;
    }

    std::string Clean(std::string_view field)
    {
        if (field.empty()) {
            return "-";
        }
        std::string result(field);
        std::ranges::replace_if(result, [](unsigned char c) { return c < 32; }, ' ');
        return result;
    }

    std::string Row(char type, std::int64_t ms, std::string_view fields)
    {
        return std::format("{}\t{}\t{}\n", type, ms, fields);
    }

    std::string LevelRow(std::int64_t ms, int requested, int actual, float maxHealth)
    {
        return Row('L', ms, std::format("{}\t{}\t{}", requested, actual, maxHealth));
    }

    std::string VisitRow(std::int64_t ms, int level, std::string_view cell, std::uint32_t form, std::string_view result,
        std::int64_t entryMs, std::size_t actors)
    {
        return Row('V', ms, std::format("{}\t{}\t{:08X}\t{}\t{}\t{}", level, Clean(cell), form, result, entryMs, actors));
    }

    std::string ActorRow(std::int64_t ms, int level, std::string_view cell, const Actor& actor)
    {
        return Row('A', ms, std::format("{}\t{}\t{:08X}\t{:08X}\t{:08X}\t{}\t{}\t{:08X}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:08X}\t{}\t{}\t{}\t{:08X}",
            level, Clean(cell), actor.reference, actor.base, actor.templateBase, Clean(actor.plugin), Clean(actor.name),
            actor.race, actor.level, actor.maxHealth, actor.baseHealth, actor.health, actor.aggression, Clean(actor.flags),
            actor.damageResist, actor.hitPhysical, actor.hitTotal, actor.hitBlocked,
            actor.outWeapon, actor.outWeaponType, actor.outPhysical, actor.outTotal, actor.outFlags));
    }
}
