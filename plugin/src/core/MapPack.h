#pragma once

#include "core/MapTypes.h"

#include <array>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

namespace SkyrimPoE::Core
{
    // 결정 169·173: 복제 레코드와 스폰 원형의 로컬 FormID는 서로 다른 플러그인에 속한다.
    inline constexpr auto kMapsPluginName = "SkyrimPoE_Maps.esp";
    inline constexpr auto kMapsSourceMaster = "Skyrim.esm";
    inline constexpr int kMapsManifestFormat = 3;

    struct MapAutoExit
    {
        std::uint32_t localId;
        std::array<float, 3> position;
        float rotationZ;
    };

    struct MapDungeonDef
    {
        std::string key;
        std::string name;
        std::string type;
        std::uint32_t cell = 0;
        double navmeshMinZ = 0.0;
        std::uint32_t entryMarker = 0;
        std::vector<std::uint32_t> returnDoors;
        std::vector<MapAutoExit> autoExits;
        std::uint32_t bossAnchor = 0;
        std::vector<std::uint32_t> packAnchors;
        std::uint32_t bossNpc = 0;
        std::vector<std::uint32_t> packNpcs;
        std::vector<std::uint32_t> addNpcs;
    };

    struct MapPack
    {
        std::uint32_t version = 0;
        std::string espSha256;
        std::vector<MapDungeonDef> maps;
        MapDungeonDef arena;

        [[nodiscard]] const MapDungeonDef* ForTier(std::uint8_t tier) const noexcept;
        [[nodiscard]] const MapDungeonDef* Find(std::string_view key) const noexcept;
        [[nodiscard]] const MapDungeonDef* ForRun(const ActiveMapState& run) const noexcept;
    };

    // 잘못된 목록은 필드 또는 던전 키를 포함한 예외로 보고한다.
    [[nodiscard]] MapPack ParseMapManifest(std::istream& input);
}
