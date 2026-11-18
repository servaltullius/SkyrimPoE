#include "AutoTest.h"

#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <span>

namespace AutoTest
{
    using namespace std::string_view_literals;

    std::string_view Trim(std::string_view text)
    {
        constexpr auto space = " \t\r\n"sv;
        const auto first = text.find_first_not_of(space);
        return first == text.npos ? ""sv : text.substr(first, text.find_last_not_of(space) - first + 1);
    }

    bool ParseUnsigned(std::string_view value, std::uint32_t& out)
    {
        const bool hex = value.starts_with("0x") || value.starts_with("0X");
        const auto digits = hex ? value.substr(2) : value;
        if (digits.empty()) {
            return false;
        }
        const auto end = digits.data() + digits.size();
        const auto result = std::from_chars(digits.data(), end, out, hex ? 16 : 10);
        return result.ec == std::errc{} && result.ptr == end;
    }

    namespace
    {
        bool ValidUtf8(std::string_view text)
        {
            while (!text.empty()) {
                const auto lead = static_cast<unsigned char>(text.front());
                if (lead < 0x80) {
                    text.remove_prefix(1);
                    continue;
                }
                if (lead < 0xC2 || lead > 0xF4) {
                    return false;
                }
                const std::size_t width = lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
                if (text.size() < width) {
                    return false;
                }
                const auto second = static_cast<unsigned char>(text[1]);
                if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0) ||
                    (lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second >= 0x90)) {
                    return false;
                }
                for (std::size_t i = 1; i < width; ++i) {
                    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
                        return false;
                    }
                }
                text.remove_prefix(width);
            }
            return true;
        }

        bool ValidSaveName(std::string_view value)
        {
            if (value.empty() || value.size() > 251 || value.find_first_of(".<>:\"/\\|?*") != value.npos || !ValidUtf8(value)) {
                return false;
            }
            for (const unsigned char byte : value) {
                if (byte < 32 || byte == 127) {
                    return false;
                }
            }
            std::string upper(value);
            for (auto& letter : upper) {
                if (letter >= 'a' && letter <= 'z') {
                    letter = static_cast<char>(letter - 'a' + 'A');
                }
            }
            const auto device = [&upper](std::string_view prefix) {
                if (!upper.starts_with(prefix)) {
                    return false;
                }
                const auto suffix = std::string_view(upper).substr(prefix.size());
                return (suffix.size() == 1 && suffix.front() >= '1' && suffix.front() <= '9') ||
                    suffix == "¹" || suffix == "²" || suffix == "³";
            };
            return upper != "CON" && upper != "PRN" && upper != "AUX" && upper != "NUL" && !device("COM") && !device("LPT");
        }

        using enum Action;
        constexpr std::array kPhase1{
            Step{ "P1-setup", Setup },
            Step{ "P1-cooldown", EquipRight }, Step{ "P1-cooldown", CoolFirst },
            Step{ "P1-cooldown", CoolBlocked }, Step{ "P1-cooldown", CoolAfter },
            Step{ "P1-dual", EquipBoth }, Step{ "P1-dual", Dual }, Step{ "P1-dual", EquipRight }, Step{ "P1-dual", Single },
            Step{ "P1-console", Console1 }, Step{ "P1-console", Console2a }, Step{ "P1-console", Console2b },
            Step{ "P1-names", EquipRight }, Step{ "P1-names", HudA }, Step{ "P1-names", Swap }, Step{ "P1-names", HudB },
            Step{ "P1-names", FavoritesB }, Step{ "P1-names", MagicB }, Step{ "P1-names", AfterSwap },
            Step{ "P1-save", Save }, Step{ "P1-save", WriteMarker },
        };
        constexpr std::array kPhase2{
            Step{ "P2-state", State }, Step{ "P2-menus", MagicLoaded }, Step{ "P2-menus", FavoritesLoaded },
        };
    }

    Settings ReadSettings(const IniSection& section, WarnSetting warn)
    {
        Settings settings;
        const auto read = [&](std::string_view key, auto apply) {
            if (const auto entry = section.find(key); entry != section.end() && !apply(Trim(entry->second))) {
                warn(key);
            }
        };
        const auto seconds = [&](std::string_view key, double& out, double min, double max) {
            read(key, [&](std::string_view value) {
                double parsed = 0;
                if (value.empty()) {
                    return false;
                }
                const auto end = value.data() + value.size();
                const auto result = std::from_chars(value.data(), end, parsed);
                if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(parsed) || parsed < min || parsed > max) {
                    return false;
                }
                out = parsed;
                return true;
            });
        };
        read("StartKey", [&](std::string_view value) {
            std::uint32_t key = 0;
            if (!ParseUnsigned(value, key) || key > 255) {
                return false;
            }
            settings.startKey = key;
            return true;
        });
        seconds("MainMenuDelaySeconds", settings.mainMenuDelay, 0, 120);
        seconds("StartDelaySeconds", settings.startDelay, 0, 120);
        seconds("CheckpointSeconds", settings.checkpoint, 0, 120);
        seconds("MenuHoldSeconds", settings.menuHold, 0, 120);
        seconds("EquipSettleSeconds", settings.equipSettle, 0, 120);
        seconds("HoldExtraSeconds", settings.holdExtra, 0, 30);
        seconds("AfterReleaseSeconds", settings.afterRelease, 0, 30);
        seconds("BarHoldSeconds", settings.barHold, 1.1, 60);
        seconds("SaveWaitSeconds", settings.saveWait, 0, 120);
        read("SaveName", [&](std::string_view value) {
            if (!ValidSaveName(value)) {
                return false;
            }
            settings.saveName = value;
            return true;
        });
        read("QuitWhenDone", [&](std::string_view value) {
            if (value == "true" || value == "1") {
                settings.quitWhenDone = true;
                return true;
            }
            if (value == "false" || value == "0") {
                settings.quitWhenDone = false;
                return true;
            }
            return false;
        });
        return settings;
    }

    std::optional<Marker> ParseMarker(std::string_view text)
    {
        if (text.starts_with("\xEF\xBB\xBF")) {
            text.remove_prefix(3);
        }
        Marker marker{};
        while (!text.empty()) {
            const auto newline = text.find('\n');
            const auto line = Trim(text.substr(0, newline));
            text = newline == text.npos ? ""sv : text.substr(newline + 1);
            if (line.empty()) {
                continue;
            }
            const auto equals = line.find('=');
            if (equals == line.npos) {
                return std::nullopt;
            }
            const auto key = Trim(line.substr(0, equals));
            const auto value = Trim(line.substr(equals + 1));
            if (key == "phase" && marker.phase == 0 && (value == "1" || value == "2")) {
                marker.phase = value.front() - '0';
            } else if (key == "save" && marker.save.empty() && ValidSaveName(value)) {
                marker.save = value;
            } else {
                return std::nullopt;
            }
        }
        return marker.phase && !marker.save.empty() ? std::optional(std::move(marker)) : std::nullopt;
    }

    void Plan::Start(int phase)
    {
        phase_ = phase;
        index_ = 0;
        inputAvailable_ = true;
    }

    void Plan::Stop() { phase_ = 0; }

    const Step* Plan::Current() const
    {
        std::span<const Step> steps;
        if (phase_ == 1) {
            steps = kPhase1;
        } else if (phase_ == 2) {
            steps = kPhase2;
        }
        return index_ < steps.size() ? &steps[index_] : nullptr;
    }

    void Plan::Advance()
    {
        if (Current()) {
            ++index_;
        }
    }

    void Plan::ObserveFirstHandAttempt(std::uint64_t checks) { inputAvailable_ = checks != 0; }

    std::string_view HandAttemptLabel(Action action)
    {
        switch (action) {
        case CoolFirst: return "cd-first";
        case CoolBlocked: return "cd-blocked";
        case CoolAfter: return "cd-after";
        case Dual: return "dual";
        case Single: return "single-ref";
        case AfterSwap: return "after-swap";
        default: return {};
        }
    }

    std::string FormatAttempt(std::string_view label, std::string_view slot, std::string_view hand,
        float before, float minimum, float released, float after, const std::array<std::uint64_t, 3>& castCounts,
        double cooldown, std::uint64_t authorizedLaunches, std::uint64_t unauthorizedLaunches)
    {
        return std::format("{}\t{}\t{}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{}\t{}\t{}\t{:.3f}\t{}\t{}",
            label, slot, hand, before, minimum, released, after, castCounts[0], castCounts[1], castCounts[2],
            cooldown, authorizedLaunches, unauthorizedLaunches);
    }

    bool Plan::SkipCurrent() const
    {
        const auto* step = Current();
        if (inputAvailable_ || !step) {
            return false;
        }
        return (!HandAttemptLabel(step->action).empty() && step->action != CoolFirst) ||
            step->action == EquipBoth || (step->action == EquipRight && step->stage == "P1-dual");
    }

    std::string_view AbortReason(bool hasPlayer, bool dead, bool injecting, bool menuOpen)
    {
        if (!hasPlayer) {
            return "player-missing";
        }
        if (dead) {
            return "player-dead";
        }
        return injecting && menuOpen ? "external-menu" : ""sv;
    }
}
