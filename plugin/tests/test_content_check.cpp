#include "test_harness.h"

#include "core/ContentCheck.h"
#include "core/UiBuildCheck.h"

#include <nlohmann/json.hpp>

#include <algorithm>

using namespace SkyrimPoE::Core;

namespace
{
    using nlohmann::json;

    struct ContentFixture
    {
        std::array<json, 4> meta;
        json manifest;
        std::array<ContentFile, 5> files{{
            { "SKSE/Plugins/SkyrimPoE/packs/core.json", std::string(64, 'a') },
            { "SKSE/Plugins/SkyrimPoE/packs/tree.json", std::string(64, 'b') },
            { "SKSE/Plugins/SkyrimPoE/packs/items.json", std::string(64, 'c') },
            { "SKSE/Plugins/SkyrimPoE/packs/rules.json", std::string(64, 'e') },
            { "SkyrimPoE.esp", std::string(64, 'd') },
        }};

        ContentFixture()
        {
            constexpr auto buildId = "1234567890abcdef";
            const json common = {
                { "game", "poe1" }, { "gameVersion", "3.30" }, { "pobVersion", "2.68.1" },
                { "contentGeneration", 0 }, { "buildId", buildId },
            };
            meta.fill(common);
            meta[0].update({ { "schema", "skyrimpoe.core" }, { "schemaVersion", 1 } });
            meta[1].update({ { "schema", "skyrimpoe.tree" }, { "schemaVersion", 1 } });
            meta[2].update({ { "schema", "skyrimpoe.items" }, { "schemaVersion", 2 } });
            meta[3].update({ { "schema", "skyrimpoe.rules" }, { "schemaVersion", 1 }, { "pobVersion", "0.23.1" } });

            manifest = {
                { "manifestVersion", 1 }, { "game", "poe1" }, { "contentGeneration", 0 },
                { "buildId", buildId }, { "gameVersion", "3.30" }, { "files", json::array() },
            };
            for (const auto& file : files) {
                manifest["files"].push_back({
                    { "path", file.path }, { "sha256", *file.sha256 }, { "size", 123 },
                });
            }
        }

        ContentCheckResult Check(std::optional<std::uint32_t> espGeneration = std::nullopt) const
        {
            return CheckContent(meta, manifest, files, espGeneration);
        }
    };

    bool HasReason(const ContentCheckResult& result, std::string_view source, std::string_view message)
    {
        return std::ranges::any_of(result.reasons, [&](const std::string& reason) {
            return reason.starts_with(source) && reason.find(message) != std::string::npos;
        });
    }
}

POE_TEST("ContentCheck: accepts stamped generation-0 packs and an ESP without its generation GLOB")
{
    const ContentFixture fixture;
    const auto result = fixture.Check();
    POE_CHECK(result.reasons.empty());
    POE_CHECK(result.identity.has_value());
    POE_CHECK(result.identity->game == "poe1");
    POE_CHECK(result.identity->contentGeneration == 0);
    POE_CHECK(result.identity->gameVersion == "3.30");
    POE_CHECK(result.identity->buildId == "1234567890abcdef");
}

POE_TEST("ContentCheck: rejects a different pack game")
{
    ContentFixture fixture;
    fixture.meta[1]["game"] = "poe3";
    auto result = fixture.Check();
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files[1].path, "unsupported game 'poe3'"));
    POE_CHECK(HasReason(result, fixture.files[1].path, "game mismatch"));

    // G1부터 poe2는 허용표에 있지만 세대 1로만 있다.
    fixture.meta[1]["game"] = "poe2";
    result = fixture.Check();
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files[1].path, "unsupported contentGeneration 0 for poe2"));
    POE_CHECK(HasReason(result, fixture.files[1].path, "game mismatch"));
}

POE_TEST("ContentCheck: rejects a different pack generation")
{
    ContentFixture fixture;
    fixture.meta[2]["contentGeneration"] = 1;
    const auto result = fixture.Check();
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files[2].path, "unsupported contentGeneration 1"));
    POE_CHECK(HasReason(result, fixture.files[2].path, "contentGeneration mismatch"));
}

POE_TEST("ContentCheck: rejects a different pack build ID")
{
    ContentFixture fixture;
    fixture.meta[1]["buildId"] = "fedcba0987654321";
    const auto result = fixture.Check();
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files[1].path, "buildId mismatch"));
}

POE_TEST("ContentCheck: rejects a manifest with a mismatched package identity")
{
    ContentFixture fixture;
    fixture.manifest["game"] = "poe2";
    fixture.manifest["contentGeneration"] = 1;
    fixture.manifest["buildId"] = "fedcba0987654321";
    fixture.manifest["gameVersion"] = "0.5";
    const auto result = fixture.Check();
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, "manifest.json", "game mismatch"));
    POE_CHECK(HasReason(result, "manifest.json", "contentGeneration mismatch"));
    POE_CHECK(HasReason(result, "manifest.json", "buildId mismatch"));
    POE_CHECK(HasReason(result, "manifest.json", "gameVersion mismatch"));
}

POE_TEST("ContentCheck: compares the measured SHA-256 of each pack and ESP with the manifest")
{
    for (std::size_t file = 0; file < ContentFixture{}.files.size(); ++file) {
        ContentFixture fixture;
        fixture.manifest["files"][file]["sha256"] = std::string(64, 'f');
        const auto result = fixture.Check();
        POE_CHECK(!result.identity);
        POE_CHECK(HasReason(result, fixture.files[file].path, "sha256 mismatch"));
    }
}

POE_TEST("ContentCheck: reports a missing or unreadable file and a missing manifest entry")
{
    ContentFixture fixture;
    fixture.files[0].sha256.reset();
    fixture.manifest["files"].erase(2);
    const auto result = fixture.Check();
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files[0].path, "file missing or could not read SHA-256"));
    POE_CHECK(HasReason(result, fixture.files[2].path, "file missing from manifest.json"));
}

POE_TEST("ContentCheck: rejects an ESP generation different from the packs")
{
    const ContentFixture fixture;
    const auto result = fixture.Check(1);
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files.back().path, "ESP contentGeneration mismatch (packs 0, ESP 1)"));
}

POE_TEST("ContentCheck: generation 0 rejects a GLOB even when its value is zero")
{
    const ContentFixture fixture;
    const auto result = fixture.Check(0);
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files.back().path, "generation 0 must omit the SPOE_ContentGeneration GLOB"));
}

POE_TEST("ContentCheck: requires 16 lowercase hexadecimal digits in every build ID")
{
    for (const auto* buildId : { "1234567890abcde", "1234567890ABCDEf", "1234567890abcdeg" }) {
        ContentFixture fixture;
        for (auto& meta : fixture.meta) {
            meta["buildId"] = buildId;
        }
        fixture.manifest["buildId"] = buildId;
        const auto result = fixture.Check();
        POE_CHECK(!result.identity);
        constexpr auto reason = "buildId must be 16 lowercase hexadecimal digits";
        for (std::size_t pack = 0; pack < fixture.meta.size(); ++pack) {
            POE_CHECK(HasReason(result, fixture.files[pack].path, reason));
        }
        POE_CHECK(HasReason(result, "manifest.json", reason));
    }
}

POE_TEST("ContentCheck: requires the same gameVersion across the three packs")
{
    ContentFixture fixture;
    fixture.meta[1]["gameVersion"] = "3.29";
    fixture.meta[2]["gameVersion"] = "3.31";
    const auto result = fixture.Check();
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files[1].path, "gameVersion mismatch (expected 3.30, found 3.29)"));
    POE_CHECK(HasReason(result, fixture.files[2].path, "gameVersion mismatch (expected 3.30, found 3.31)"));
}

POE_TEST("ContentCheck: rejects unsupported pack schema names and versions")
{
    ContentFixture fixture;
    fixture.meta[0]["schema"] = "skyrimpoe.items";
    fixture.meta[1]["schemaVersion"] = 2;
    fixture.meta[2]["schemaVersion"] = 1;
    const auto result = fixture.Check();
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files[0].path, "unsupported schema 'skyrimpoe.items'"));
    POE_CHECK(HasReason(result, fixture.files[1].path, "unsupported schemaVersion 2"));
    POE_CHECK(HasReason(result, fixture.files[2].path, "unsupported schemaVersion 1"));
}

namespace
{
    // G1(결정 204): 세대 1 패키지의 네 팩. core v2, 트리 v2, 빈 items v3, 공용 rules v1.
    ContentFixture GenerationOneFixture()
    {
        ContentFixture fixture;
        for (auto& meta : fixture.meta) {
            meta.update({ { "game", "poe2" }, { "gameVersion", "0.5" }, { "contentGeneration", 1 } });
        }
        fixture.meta[0]["schemaVersion"] = 2;
        fixture.meta[1]["schemaVersion"] = 2;
        fixture.meta[2]["schemaVersion"] = 3;
        fixture.manifest.update({ { "game", "poe2" }, { "gameVersion", "0.5" }, { "contentGeneration", 1 } });
        return fixture;
    }
}

POE_TEST("ContentCheck: accepts the generation-1 PoE2 package with its generation GLOB (G1)")
{
    const auto fixture = GenerationOneFixture();
    const auto result = fixture.Check(1u);
    POE_CHECK(result.reasons.empty());
    POE_CHECK(result.identity && result.identity->game == "poe2" && result.identity->contentGeneration == 1 &&
              result.identity->gameVersion == "0.5");
    POE_CHECK(HasReason(fixture.Check(), "SkyrimPoE.esp", "ESP contentGeneration mismatch (packs 1, ESP 0)"));
}

POE_TEST("ContentCheck: a generation-1 package rejects generation-0 schema versions and a wrong game version")
{
    auto fixture = GenerationOneFixture();
    fixture.meta[0]["schemaVersion"] = 1;
    fixture.meta[1]["schemaVersion"] = 1;
    fixture.meta[2]["schemaVersion"] = 2;
    auto result = fixture.Check(1u);
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files[0].path, "unsupported schemaVersion 1"));
    POE_CHECK(HasReason(result, fixture.files[1].path, "unsupported schemaVersion 1"));
    POE_CHECK(HasReason(result, fixture.files[2].path, "unsupported schemaVersion 2"));

    fixture = GenerationOneFixture();
    for (auto& meta : fixture.meta) meta["gameVersion"] = "0.4";
    fixture.manifest["gameVersion"] = "0.4";
    result = fixture.Check(1u);
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, "packs", "unsupported gameVersion 0.4 for poe2 generation 1 (expected 0.5)"));

    // 세대 0 팩 하나를 섞은 묶음.
    fixture = GenerationOneFixture();
    fixture.meta[1].update({ { "game", "poe1" }, { "gameVersion", "3.30" }, { "contentGeneration", 0 },
        { "schemaVersion", 1 } });
    result = fixture.Check(1u);
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files[1].path, "game mismatch (expected poe2, found poe1)"));
}

POE_TEST("ContentCheck: a generation-0 package keeps rejecting the PoE1 game version of another generation")
{
    ContentFixture fixture;
    for (auto& meta : fixture.meta) meta["gameVersion"] = "0.5";
    fixture.manifest["gameVersion"] = "0.5";
    const auto result = fixture.Check();
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, "packs", "unsupported gameVersion 0.5 for poe1 generation 0 (expected 3.30)"));
}

POE_TEST("ContentCheck: collects malformed metadata, manifest format, duplicate entry and SHA errors")
{
    ContentFixture fixture;
    fixture.meta[0] = nullptr;
    fixture.meta[1]["schemaVersion"] = -1;
    fixture.meta[2]["contentGeneration"] = std::uint64_t{ 1 } << 32;
    fixture.manifest["manifestVersion"] = 2;
    fixture.manifest["files"][0]["sha256"] = "bad";
    const auto duplicate = fixture.manifest["files"][1];
    fixture.manifest["files"].push_back(duplicate);
    const auto result = fixture.Check();
    POE_CHECK(!result.identity);
    POE_CHECK(HasReason(result, fixture.files[0].path, "missing or invalid meta object"));
    POE_CHECK(HasReason(result, fixture.files[1].path, "missing or invalid schemaVersion"));
    POE_CHECK(HasReason(result, fixture.files[2].path, "missing or invalid contentGeneration"));
    POE_CHECK(HasReason(result, "manifest.json", "missing or unsupported manifestVersion"));
    POE_CHECK(HasReason(result, fixture.files[0].path, "missing or invalid sha256"));
    POE_CHECK(HasReason(result, fixture.files[1].path, "duplicate entries"));
}

POE_TEST("UiBuildCheck: classifies every known operation and treats unknown operations as state-changing")
{
    using namespace UiBuildCheck;
    constexpr std::string_view readOnly[] = { "close", "refresh", "treeSearchFocus", "switchTab" };
    constexpr std::string_view stateChanging[] = {
        "equip", "unequip", "socket", "unsocket", "link", "toggleAura",
        "treeAllocatePath", "treeSetAttribute", "treeRefundNode", "treeRefundAll", "treeSelectClass", "treeSelectAscendancy",
        "treeClearAscendancy", "treeSelectMastery", "treeSocketJewel", "treeUnsocketJewel",
        "gearIdentifyToken", "gearIdentifyUnique", "gearApplyOrb", "gearApplyJeweller",
        "gearEquipRing2", "gearUnequipRing2", "gearEquipBelt", "gearUnequipBelt", "gearEquipCharm", "gearUnequipCharm",
        "mapOpen", "mapOpenArena4", "mapReenter", "mapReturnPortal", "mapAbandon", "mapSafeEscape", "mapSafeUninstall",
    };

    static_assert(std::size(kOperations) == std::size(readOnly) + std::size(stateChanging));
    for (const auto op : readOnly) {
        POE_CHECK(ClassifyOperation(op) == OperationKind::ReadOnly);
    }
    for (const auto op : stateChanging) {
        POE_CHECK(std::ranges::any_of(kOperations, [op](const auto& entry) { return entry.first == op; }));
        POE_CHECK(ClassifyOperation(op) == OperationKind::StateChanging);
    }
    POE_CHECK(ClassifyOperation("") == OperationKind::StateChanging);
    POE_CHECK(ClassifyOperation("treeFutureCommand") == OperationKind::StateChanging);
}

POE_TEST("UiBuildCheck: accepts matching build ID and protocol")
{
    using namespace UiBuildCheck;
    constexpr auto buildId = "1234567890abcdef";
    static_assert(kUiProtocolVersion == 1);
    const auto result = Evaluate(buildId, 1, buildId, kUiProtocolVersion);
    POE_CHECK(result.match);
    POE_CHECK(result.reason.empty());
}

POE_TEST("UiBuildCheck: rejects absent or differing build IDs")
{
    using namespace UiBuildCheck;
    constexpr auto buildId = "1234567890abcdef";

    const auto unavailable = Evaluate(buildId, 1, "", 1);
    POE_CHECK(!unavailable.match);
    POE_CHECK(unavailable.reason == "DLL build ID is unavailable");
    POE_CHECK(!Evaluate("", 1, "", 1).match);

    const auto missing = Evaluate("", 1, buildId, 1);
    POE_CHECK(!missing.match);
    POE_CHECK(missing.reason == "UI build ID is missing or invalid");

    const auto differing = Evaluate("fedcba0987654321", 1, buildId, 1);
    POE_CHECK(!differing.match);
    POE_CHECK(differing.reason == "build ID mismatch");
}

POE_TEST("UiBuildCheck: rejects an absent or differing UI protocol")
{
    using namespace UiBuildCheck;
    constexpr auto buildId = "1234567890abcdef";

    const auto missing = Evaluate(buildId, std::nullopt, buildId, 1);
    POE_CHECK(!missing.match);
    POE_CHECK(missing.reason == "UI protocol is missing or invalid");

    const auto differing = Evaluate(buildId, 2, buildId, 1);
    POE_CHECK(!differing.match);
    POE_CHECK(differing.reason == "UI protocol mismatch");
    POE_CHECK(!Evaluate(buildId, 0, buildId, 1).match);
}
