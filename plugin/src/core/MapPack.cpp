#include "core/MapPack.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace SkyrimPoE::Core
{
    namespace
    {
        using nlohmann::json;

        bool IsHexDigit(char c)
        {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        std::string ReadText(const json& object, const char* field)
        {
            auto value = object.at(field).get<std::string>();
            if (value.find_first_not_of(" \t\r\n") == std::string::npos) {
                throw std::runtime_error(std::format("{} 필드가 비었습니다.", field));
            }
            return value;
        }

        std::uint32_t ReadFormId(const json& value, const char* field, bool mapsPlugin)
        {
            std::string_view hex = value.get_ref<const std::string&>();
            if (mapsPlugin && hex.starts_with("0x")) {
                hex.remove_prefix(2);
            } else if (mapsPlugin) {
                throw std::runtime_error(std::format("{}의 지도 팩 FormID에 0x 접두사가 없습니다.", field));
            }
            std::uint32_t id = 0;
            const auto end = hex.data() + hex.size();
            const auto result = std::from_chars(hex.data(), end, id, 16);
            if (hex.size() != 6 || result.ec != std::errc{} || result.ptr != end || id == 0) {
                throw std::runtime_error(std::format("{}의 로컬 FormID가 잘못되었습니다.", field));
            }
            return id;
        }

        std::vector<std::uint32_t> ReadIds(const json& entry, const char* field, bool mapsPlugin)
        {
            const auto& array = entry.at(field).get_ref<const json::array_t&>();
            std::vector<std::uint32_t> ids;
            ids.reserve(array.size());
            for (const auto& value : array) {
                ids.push_back(ReadFormId(value, field, mapsPlugin));
            }
            return ids;
        }

        void ReadDoors(const json& entry, MapDungeonDef& dungeon)
        {
            dungeon.returnDoors = ReadIds(entry, "returnDoors", true);
            (void)ReadIds(entry, "sealedDoors", true);
            for (const auto& exit : entry.at("autoExits").get_ref<const json::array_t&>()) {
                const auto id = ReadFormId(exit.at("id"), "autoExits.id", true);
                const auto leads = ReadText(exit, "leads");
                if (leads != "interior" && leads != "exterior") {
                    throw std::runtime_error("autoExits.leads 값이 잘못되었습니다.");
                }
                for (const auto* field : { "position", "rotation" }) {
                    const auto& coords = exit.at(field).get_ref<const json::array_t&>();
                    if (coords.size() != 3 || !std::ranges::all_of(coords, [](const json& axis) {
                            return axis.is_number() && std::isfinite(axis.get<float>());
                        })) {
                        throw std::runtime_error(std::format("autoExits.{} 좌표가 잘못되었습니다.", field));
                    }
                }
                dungeon.autoExits.push_back({ id, exit.at("position").get<std::array<float, 3>>(),
                    exit.at("rotation").at(2).get<float>() });
            }
        }

        MapDungeonDef ReadDungeon(const json& entry, std::string key, bool arena)
        {
            MapDungeonDef dungeon;
            dungeon.key = std::move(key);
            dungeon.name = ReadText(entry, "name");
            dungeon.type = ReadText(entry, "type");
            (void)ReadFormId(entry.at("sourceCell"), "sourceCell", false);
            dungeon.cell = ReadFormId(entry.at("cell"), "cell", true);
            const auto& navmeshMinZ = entry.at("navmeshMinZ");
            if (!navmeshMinZ.is_number()) {
                throw std::runtime_error("navmeshMinZ는 유한한 실수여야 합니다.");
            }
            dungeon.navmeshMinZ = navmeshMinZ.get<double>();
            // 도구와 엔진 좌표는 float이다. 손으로 고친 목록의 터무니없는 값이 낙하 판정을 망가뜨리지 않게 막는다.
            constexpr double kMaxNavmeshZ = 1.0e6;
            if (!std::isfinite(dungeon.navmeshMinZ) || std::abs(dungeon.navmeshMinZ) > kMaxNavmeshZ) {
                throw std::runtime_error("navmeshMinZ는 유한한 실수여야 합니다.");
            }
            dungeon.entryMarker = ReadFormId(entry.at("entryMarker"), "entryMarker", true);
            dungeon.bossAnchor = ReadFormId(entry.at("bossAnchor"), "bossAnchor", true);
            dungeon.packAnchors = ReadIds(entry, "packAnchors", true);
            std::unordered_set<std::uint32_t> markers{ dungeon.entryMarker, dungeon.bossAnchor };
            markers.insert(dungeon.packAnchors.begin(), dungeon.packAnchors.end());
            if (dungeon.packAnchors.empty() || markers.size() != dungeon.packAnchors.size() + 2) {
                throw std::runtime_error("무리 앵커가 없거나 입구·보스·무리 마커 ID가 겹칩니다.");
            }
            dungeon.bossNpc = ReadFormId(entry.at("bossNpc"), "bossNpc", false);
            if (!arena || entry.contains("packNpcs")) {
                dungeon.packNpcs = ReadIds(entry, "packNpcs", false);
            }
            if (arena || entry.contains("addNpcs")) {
                dungeon.addNpcs = ReadIds(entry, "addNpcs", false);
            }
            if (arena ? dungeon.addNpcs.empty() : dungeon.packNpcs.empty()) {
                throw std::runtime_error(arena ? "addNpcs가 비었습니다." : "packNpcs가 비었습니다.");
            }
            ReadDoors(entry, dungeon);
            return dungeon;
        }
    }

    MapPack ParseMapManifest(std::istream& input)
    {
        const auto root = json::parse(input);
        const auto& format = root.at("manifestFormat");
        if (!format.is_number_integer() || format != kMapsManifestFormat) {
            throw std::runtime_error(std::format("지도 목록의 형식 번호가 {}여야 합니다.", kMapsManifestFormat));
        }
        if (ReadText(root, "plugin") != kMapsPluginName || ReadText(root, "sourceMaster") != kMapsSourceMaster) {
            throw std::runtime_error("지도 목록의 plugin 또는 sourceMaster가 올바르지 않습니다.");
        }

        MapPack pack;
        const auto& version = root.at("packVersion");
        if (!version.is_number_integer() || version <= 0 || version > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("packVersion은 양의 32비트 정수여야 합니다.");
        }
        pack.version = version.get<std::uint32_t>();
        pack.espSha256 = ReadText(root, "espSha256");
        if (pack.espSha256.size() != 64 || !std::ranges::all_of(pack.espSha256, IsHexDigit)) {
            throw std::runtime_error("espSha256은 64자리 16진수여야 합니다.");
        }
        std::ranges::transform(pack.espSha256, pack.espSha256.begin(), [](char c) {
            return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c;
        });

        std::unordered_set<std::string> keys;
        std::unordered_set<std::uint32_t> cells;
        for (const auto& entry : root.at("dungeons").get_ref<const json::array_t&>()) {
            auto key = ReadText(entry, "key");
            if (key.size() > kMaxMapDungeonKeyBytes || !std::ranges::all_of(key, [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                })) {
                throw std::runtime_error(std::format("던전 키 '{}'가 올바르지 않습니다.", key));
            }
            if (!keys.insert(key).second) {
                throw std::runtime_error(std::format("중복 던전 키 '{}'.", key));
            }
            try {
                const auto role = ReadText(entry, "role");
                if (role != "map" && role != "arena") {
                    throw std::runtime_error("role은 map 또는 arena여야 합니다.");
                }
                auto dungeon = ReadDungeon(entry, key, role == "arena");
                if (!cells.insert(dungeon.cell).second) {
                    throw std::runtime_error("복제 셀 ID가 겹칩니다.");
                }
                if (role == "arena") {
                    if (!pack.arena.key.empty()) {
                        throw std::runtime_error("결투장 역할은 정확히 하나여야 합니다.");
                    }
                    pack.arena = std::move(dungeon);
                } else {
                    pack.maps.push_back(std::move(dungeon));
                }
            } catch (const std::exception& e) {
                throw std::runtime_error(std::format("{}: {}", key, e.what()));
            }
        }
        if (pack.maps.empty() || pack.arena.key.empty()) {
            throw std::runtime_error("지도 역할은 하나 이상, 결투장 역할은 정확히 하나 필요합니다.");
        }
        return pack;
    }

    const MapDungeonDef* MapPack::ForTier(std::uint8_t tier) const noexcept
    {
        const auto clamped = std::clamp(tier, kMinMapTier, kMaxMapTier);
        return maps.empty() ? nullptr : &maps[static_cast<std::size_t>(clamped - 1) % maps.size()];
    }

    const MapDungeonDef* MapPack::Find(std::string_view key) const noexcept
    {
        if (key.empty()) {
            return nullptr;
        }
        if (arena.key == key) {
            return &arena;
        }
        const auto found = std::ranges::find(maps, key, &MapDungeonDef::key);
        return found == maps.end() ? nullptr : &*found;
    }

    const MapDungeonDef* MapPack::ForRun(const ActiveMapState& run) const noexcept
    {
        if (!run.active || run.packVersion != version) {
            return nullptr;
        }
        const auto* dungeon = Find(run.dungeonKey);
        return dungeon && ((dungeon == &arena) == run.isArena4) ? dungeon : nullptr;
    }
}
