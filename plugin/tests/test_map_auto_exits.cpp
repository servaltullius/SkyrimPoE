#include "test_harness.h"

#include "core/MapPack.h"

#include <filesystem>
#include <fstream>

POE_TEST("Map pack keeps automatic exit probe positions and rotation from format 3")
{
    std::ifstream input(std::filesystem::path(POE_TEST_PLUGIN_DIR) / "tests" / "maps_format3.json");
    POE_CHECK(input.is_open());
    const auto pack = SkyrimPoE::Core::ParseMapManifest(input);

    const auto* mine = pack.Find("embershard");
    POE_CHECK(mine && mine->autoExits.size() == 1);
    const auto& exit = mine->autoExits.front();
    POE_CHECK(exit.localId == 0x81A);
    POE_CHECK_NEAR(exit.position[0], -5861.3076, 0.001);
    POE_CHECK_NEAR(exit.position[1], 1561.0718, 0.001);
    POE_CHECK_NEAR(exit.position[2], 8820.79, 0.001);
    POE_CHECK_NEAR(exit.rotationZ, 0.21119037, 1e-6);
    POE_CHECK(pack.arena.autoExits.size() == 1 && pack.arena.autoExits.front().localId == 0x8804);

    const auto* ruins = pack.Find("raldbthar");
    POE_CHECK(ruins && ruins->autoExits.empty());
}
