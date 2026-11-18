#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Survey
{
    struct Cell
    {
        std::string editorId;
        std::uint32_t localId;
        std::string plugin;
    };

    struct Config
    {
        std::vector<int> levels;
        std::vector<Cell> cells;
        std::chrono::seconds mainMenuDelay{ 3 };
        std::chrono::seconds startDelay{ 5 };
        std::chrono::seconds settle{ 4 };
        std::chrono::seconds cellTimeout{ 90 };
        bool quitWhenDone = true;
        bool notify = true;

        [[nodiscard]] bool CanRun() const { return !levels.empty() && cells.size() >= 2; }
    };

    using Warn = std::function<void(std::string_view)>;
    Config ReadConfig(std::istream& input, const Warn& warn);
    std::optional<std::string> ParseMarker(std::string_view text);

    // 초기화할 수 없는 현재 셀을 뒤로 보낸다. 그것만 남으면 false를 돌려준다.
    bool DeferCurrentCell(std::deque<std::uint32_t>& order, std::uint32_t current);

    std::filesystem::path UniqueLogPath(const std::filesystem::path& directory, std::string_view stamp,
        const std::function<bool(const std::filesystem::path&)>& occupied);

    struct Actor
    {
        std::uint32_t reference = 0;
        std::uint32_t base = 0;
        std::uint32_t templateBase = 0;
        std::string_view plugin;
        std::string_view name;
        std::uint32_t race = 0;
        int level = 0;
        float maxHealth = 0;
        float baseHealth = 0;
        float health = 0;
        float aggression = 0;
        std::string_view flags;
        // 플레이어의 오른손 무기로 이 액터를 쳤다고 할 때 엔진이 채우는 값(HitData::Populate, 피해는 주지 않음).
        float damageResist = 0;
        float hitPhysical = 0;
        float hitTotal = 0;
        float hitBlocked = 0;
        // 반대로 이 액터가 오른손 장비로 플레이어를 쳤다고 할 때의 HitData(2026-11-19 추가, 피해는 주지 않음).
        std::uint32_t outWeapon = 0;  // 오른손 장비 항목의 폼 ID. 항목이 없으면 0(맨손·생물의 기본 공격)
        int outWeaponType = -1;       // TESObjectWEAP의 WEAPON_TYPE. 무기가 아니거나 항목이 없으면 -1, 처리 단계가 없어 장비를 모르면 -2
        float outPhysical = 0;
        float outTotal = 0;
        std::uint32_t outFlags = 0;  // HitData::Flag(치명타 0x8, 은신 0x800 등)
    };

    inline constexpr std::string_view kHeader =
        "# S\tms\tstart\tplugin_version\truntime_version\tsave\n"
        "# S\tms\tconfig\tlevels\tcells\tmain_menu_s\tstart_s\tsettle_s\tcell_timeout_s\tquit\tnotify\n"
        "# S\tms\tsetting\tname\tcollection\toriginal\taction\tvalue\n"
        "# S\tms\treload\trequested_level\n"
        "# S\tms\tdetection-still-on\n"
        "# S\tms\tcell-not-unloaded\trequested_level\tcell\tcell_state\n"
        "# S\tms\thit-weapon\trequested_level\tweapon_form\tweapon_name\n"
        "# S\tms\tDONE / ABORT\t[reason]\n"
        "# L\tms\trequested_level\tplayer_level\tplayer_max_health\n"
        "# V\tms\trequested_level\tcell\tcell_form\tresult\tentry_ms\tactors\n"
        "# A\tms\trequested_level\tcell\tref\tbase\ttemplate\tplugin\tname\trace\tactor_level\tmax_health\tbase_health\thealth\taggression\tflags\tdamage_resist\thit_physical\thit_total\thit_blocked"
        "\tout_weapon\tout_weapon_type\tout_physical\tout_total\tout_flags\n";

    std::string Clean(std::string_view field);
    std::string Row(char type, std::int64_t ms, std::string_view fields);
    std::string LevelRow(std::int64_t ms, int requested, int actual, float maxHealth);
    std::string VisitRow(std::int64_t ms, int level, std::string_view cell, std::uint32_t form, std::string_view result,
        std::int64_t entryMs, std::size_t actors);
    std::string ActorRow(std::int64_t ms, int level, std::string_view cell, const Actor& actor);
}
