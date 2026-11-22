#include "test_harness.h"

#include "core/DamageConversion.h"
#include "core/MapPack.h"
#include "core/MapSaveIdentity.h"
#include "core/MapSystem.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#include <nlohmann/json.hpp>

using namespace SkyrimPoE::Core;

namespace
{
    using nlohmann::json;

    json ManifestFixture()
    {
        std::ifstream input(std::filesystem::path(POE_TEST_PLUGIN_DIR) / "tests" / "maps_format2.json");
        return json::parse(input);
    }

    MapPack Parse(const json& fixture)
    {
        std::istringstream input(fixture.dump());
        return ParseMapManifest(input);
    }

    void CheckRejected(const json& fixture, std::string_view reason)
    {
        std::string error;
        try {
            (void)Parse(fixture);
        } catch (const std::exception& e) {
            error = e.what();
        }
        POE_CHECK(error.find(reason) != std::string::npos);
    }
}

POE_TEST("Map pack format 2: resolved local IDs, ordered tiers and arena selection")
{
    const auto pack = Parse(ManifestFixture());
    POE_CHECK(pack.version == 1 && pack.espSha256.size() == 64);
    POE_CHECK(pack.maps.size() == 4);
    constexpr std::array<std::string_view, 4> keys{ "embershard", "pinemoon", "ironbind", "raldbthar" };
    for (std::uint8_t tier = 1; tier <= 16; ++tier) {
        const auto* dungeon = pack.ForTier(tier);
        POE_CHECK(dungeon != nullptr);
        POE_CHECK(dungeon->key == keys[static_cast<std::size_t>(tier - 1) % keys.size()]);
        POE_CHECK(dungeon == pack.Find(dungeon->key));
    }
    const auto& mine = pack.maps.front();
    POE_CHECK(mine.name == "엠버샤드 광산");
    POE_CHECK(mine.cell == 0x800 && mine.entryMarker == 0xB32 && mine.bossAnchor == 0xBBE);
    POE_CHECK(mine.packAnchors.size() == 3 && mine.packNpcs.size() == 4 && mine.bossNpc == 0x03DF0C);
    POE_CHECK(pack.arena.key == "shrouded_grove");
    POE_CHECK(pack.arena.cell == 0x008800 && pack.arena.addNpcs.front() == 0x016FF8);

    auto run = MapSystem::CreateArena4Run(1, 0, 0, {}, {});
    run.dungeonKey = pack.arena.key;
    run.packVersion = pack.version;
    POE_CHECK(pack.ForRun(run) == &pack.arena);
    ++run.packVersion;
    POE_CHECK(pack.ForRun(run) == nullptr);
    run.packVersion = pack.version;
    run.dungeonKey = "missing";
    POE_CHECK(pack.ForRun(run) == nullptr);
    run.dungeonKey = mine.key;
    POE_CHECK(pack.ForRun(run) == nullptr);
    run.isArena4 = false;
    POE_CHECK(pack.ForRun(run) == &mine);
}

POE_TEST("Map pack format 2 rejects invalid headers and missing dungeon fields")
{
    const auto fixture = ManifestFixture();
    auto invalid = fixture;
    invalid["manifestFormat"] = 1;
    CheckRejected(invalid, "형식 번호");
    invalid["manifestFormat"] = 2.0;
    CheckRejected(invalid, "형식 번호");
    invalid = fixture;
    invalid["plugin"] = "SkyrimPoE.esp";
    CheckRejected(invalid, "plugin");
    invalid = fixture;
    invalid["packVersion"] = 0;
    CheckRejected(invalid, "packVersion");
    invalid = fixture;
    invalid["espSha256"] = std::string(64, 'z');
    CheckRejected(invalid, "espSha256");
    for (const auto* field : { "name", "cell", "entryMarker", "bossAnchor", "packAnchors", "returnDoors" }) {
        invalid = fixture;
        invalid["dungeons"][0].erase(field);
        CheckRejected(invalid, field);
    }
    invalid = fixture;
    invalid["dungeons"][0]["cell"] = "0x00FG12";
    CheckRejected(invalid, "FormID");
    invalid = fixture;
    invalid["dungeons"][0]["packAnchors"] = json::array();
    CheckRejected(invalid, "무리 앵커");
}

POE_TEST("Map pack format 2 rejects duplicate keys, invalid role counts and missing NPC templates")
{
    const auto fixture = ManifestFixture();
    auto invalid = fixture;
    invalid["dungeons"][1]["key"] = "embershard";
    CheckRejected(invalid, "중복 던전 키");
    invalid = fixture;
    invalid["dungeons"] = json::array({ fixture["dungeons"][4] });
    CheckRejected(invalid, "지도 역할");
    invalid = fixture;
    invalid["dungeons"].erase(4);
    CheckRejected(invalid, "결투장 역할");
    invalid = fixture;
    invalid["dungeons"][3]["role"] = "arena";
    invalid["dungeons"][3]["addNpcs"] = json::array({ "016FF8" });
    CheckRejected(invalid, "결투장 역할");
    for (const auto* field : { "bossNpc", "packNpcs" }) {
        invalid = fixture;
        invalid["dungeons"][0].erase(field);
        CheckRejected(invalid, field);
    }
    invalid = fixture;
    invalid["dungeons"][4].erase("addNpcs");
    CheckRejected(invalid, "addNpcs");
    invalid["dungeons"][4]["addNpcs"] = json::array();
    CheckRejected(invalid, "addNpcs");
    invalid["dungeons"][4]["addNpcs"].push_back("016FFG");
    CheckRejected(invalid, "addNpcs");
}

POE_TEST("MAPS v2 identity round-trips and rejects truncation and oversized keys")
{
    std::vector<char> bytes;
    const auto write = [&](const void* data, std::uint32_t size) {
        const auto* begin = static_cast<const char*>(data);
        bytes.insert(bytes.end(), begin, begin + size);
        return true;
    };
    std::size_t offset = 0;
    const auto read = [&](void* data, std::uint32_t size) {
        if (size > bytes.size() - offset) {
            return false;
        }
        std::memcpy(data, bytes.data() + offset, size);
        offset += size;
        return true;
    };

    ActiveMapState saved;
    saved.dungeonKey = std::string(kMaxMapDungeonKeyBytes, 'x');
    saved.packVersion = 87;
    POE_CHECK(WriteMapSaveIdentity(write, saved));
    ActiveMapState loaded;
    POE_CHECK(ReadMapSaveIdentity(read, loaded));
    POE_CHECK(offset == bytes.size());
    POE_CHECK(loaded.dungeonKey == saved.dungeonKey && loaded.packVersion == saved.packVersion);

    bytes.pop_back();
    offset = 0;
    loaded = {};
    POE_CHECK(!ReadMapSaveIdentity(read, loaded));
    POE_CHECK(loaded.dungeonKey.empty() && loaded.packVersion == 0);

    bytes.clear();
    offset = 0;
    saved.dungeonKey.push_back('x');
    POE_CHECK(!WriteMapSaveIdentity(write, saved) && bytes.empty());
    const auto oversized = static_cast<std::uint32_t>(kMaxMapDungeonKeyBytes + 1);
    POE_CHECK(write(&oversized, sizeof(oversized)));
    POE_CHECK(!ReadMapSaveIdentity(read, loaded));
}

POE_TEST("Map health matching uses exile-level reference health and the unchanged tier multiplier")
{
    DataPack pack;
    std::string error;
    POE_CHECK(pack.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "core_3_30.json", &error));
    const Config config;
    for (const auto level : { 50u, 68u, 83u }) {
        const auto expected = config.offenseK *
            std::pow(pack.MonsterLife(level) / pack.MonsterLife(1), 1.0 - config.offenseRollback);
        POE_CHECK_NEAR(OffenseReferenceLife(level, pack, config), expected, 1e-6);
    }
    POE_CHECK_NEAR(OffenseReferenceLife(50, pack, config), 702.0, 0.5);
    POE_CHECK_NEAR(OffenseReferenceLife(68, pack, config), 1252.0, 0.5);
    POE_CHECK_NEAR(OffenseReferenceLife(83, pack, config), 1979.0, 0.5);
    const auto scale = MapSystem::GetMapMonsterMultipliers(MapSystem::MonsterLevelForTier(16), pack);
    const MapActorScaling actor{ 1, scale.lifeMultiplier, scale.damageMultiplier };
    const auto reference = OffenseReferenceLife(68, pack, config);
    const auto expectedDivisor = 4.976678 * reference / 300.0;
    POE_CHECK_NEAR(300.0 / actor.ScaleIncomingDamage(300.0, reference, 300.0), expectedDivisor, 3e-6);
    POE_CHECK_NEAR(actor.ScaleIncomingDamage(300.0, reference, 0.5), 300.0 / scale.lifeMultiplier, 1e-6);
    POE_CHECK_NEAR(actor.ScaleIncomingDamage(300.0, reference, std::numeric_limits<double>::quiet_NaN()),
        300.0 / scale.lifeMultiplier, 1e-6);
}
