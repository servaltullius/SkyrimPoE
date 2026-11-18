#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace AutoTest
{
    using IniSection = std::map<std::string, std::string, std::less<>>;

    std::string_view Trim(std::string_view text);
    bool ParseUnsigned(std::string_view value, std::uint32_t& out);

    struct Settings
    {
        std::uint32_t startKey = 0x42;
        double mainMenuDelay = 3;
        double startDelay = 5;
        double checkpoint = 10;
        double menuHold = 8;
        double equipSettle = 1.5;
        double holdExtra = 0.7;
        double afterRelease = 0.5;
        double barHold = 4;
        double saveWait = 4;
        std::string saveName = "PoEProto10_RT";
        bool quitWhenDone = true;
    };

    using WarnSetting = void (*)(std::string_view key);
    Settings ReadSettings(const IniSection& section, WarnSetting warn);

    struct Marker
    {
        int phase;
        std::string save;
    };
    std::optional<Marker> ParseMarker(std::string_view text);

    enum class Action
    {
        Setup, EquipRight, EquipBoth, CoolFirst, CoolBlocked, CoolAfter, Dual, Single,
        Console1, Console2a, Console2b, HudA, Swap, HudB, FavoritesB, MagicB, AfterSwap,
        Save, WriteMarker, State, MagicLoaded, FavoritesLoaded
    };

    struct Step
    {
        std::string_view stage;
        Action action;
    };

    std::string_view HandAttemptLabel(Action action);

    std::string FormatAttempt(std::string_view label, std::string_view slot, std::string_view hand,
        float before, float minimum, float released, float after, const std::array<std::uint64_t, 3>& castCounts,
        double cooldown, std::uint64_t authorizedLaunches, std::uint64_t unauthorizedLaunches);

    class Plan
    {
    public:
        void Start(int phase);
        void Stop();
        const Step* Current() const;
        void Advance();
        void ObserveFirstHandAttempt(std::uint64_t checks);
        bool SkipCurrent() const;

    private:
        int phase_ = 0;
        std::size_t index_ = 0;
        bool inputAvailable_ = true;
    };

    std::string_view AbortReason(bool hasPlayer, bool dead, bool injecting, bool menuOpen);
}
