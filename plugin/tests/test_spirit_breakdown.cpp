#include "test_harness.h"

#include "core/StatEngine.h"

#include <filesystem>

POE_TEST("StatEngine: base maximum Spirit excludes INI overrides and equipment")
{
    using namespace SkyrimPoE::Core;

    DataPack pack;
    POE_CHECK(pack.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "core_3_30.json"));
    Config config;
    config.spiritBase = 140;
    config.spiritPerMilestone = 12;

    Layer1Input input;
    input.completedMilestoneCount = 3;
    auto& shield = input.equippedGear[static_cast<std::size_t>(EquipSlot::OffHand)];
    shield.occupied = true;
    shield.isCrafted = true;
    shield.stats.flatSpirit = 40.0;

    StatEngine engine;
    engine.SetLayer1(input);
    POE_CHECK(engine.RebuildIfDirty(pack, config));
    const auto before = engine.GetSnapshot();
    POE_CHECK(before->baseMaxSpirit == 190);
    POE_CHECK(before->maxSpirit == 216);

    input.completedMilestoneCount = 4;
    engine.SetLayer1(input);
    POE_CHECK(engine.RebuildIfDirty(pack, config));
    const auto after = engine.GetSnapshot();
    POE_CHECK(after->baseMaxSpirit == 220);
    POE_CHECK(after->maxSpirit == 228);
}
