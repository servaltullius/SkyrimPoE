#include "test_harness.h"

#include "core/MapSave.h"
#include "core/ItemData.h"

#include <cstring>
#include <limits>
#include <span>

using namespace SkyrimPoE::Core;

namespace
{
    using Bytes = std::vector<std::uint8_t>;
    constexpr std::size_t kAtlasBytes = 8;
    constexpr std::uint8_t kLegacyDungeonIndex = 3;

    auto Writer(Bytes& bytes)
    {
        return [&bytes](const void* data, std::uint32_t size) {
            const auto* first = static_cast<const std::uint8_t*>(data);
            bytes.insert(bytes.end(), first, first + size);
            return true;
        };
    }

    auto Reader(std::span<const std::uint8_t> bytes)
    {
        return [bytes](void* data, std::size_t size) mutable {
            if (size > bytes.size()) {
                bytes = {};
                return false;
            }
            std::memcpy(data, bytes.data(), size);
            bytes = bytes.subspan(size);
            return true;
        };
    }

    bool PassThrough(std::uint32_t raw, std::uint32_t& result)
    {
        result = raw;
        return true;
    }

    auto ParseAtlasSave(std::uint32_t version, std::span<const std::uint8_t> bytes)
    {
        return ReadAtlasSave(version, bytes.size(), Reader(bytes));
    }

    template <class Resolve>
    auto ParseMapSave(std::uint32_t version, std::span<const std::uint8_t> bytes, Resolve&& resolve)
    {
        return ReadMapSave(version, bytes.size(), Reader(bytes), std::forward<Resolve>(resolve));
    }

    template <class T>
    void ChangeValue(Bytes& bytes, std::size_t offset, T value)
    {
        POE_CHECK(offset + sizeof(value) <= bytes.size());
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    ActiveMapState TestRun()
    {
        ActiveMapState run;
        run.active = true;
        run.isArena4 = true;
        run.bossKilled = false;
        run.failed = false;
        run.playerAuthorized = true;
        run.endRequested = true;
        run.runId = 147;
        run.tier = 8;
        run.dungeonKey = "test_arena";
        run.packVersion = 5;
        run.portalsRemaining = 1;
        run.arenaPhase = 2;
        run.arenaPhaseInvulnSec = 0.38f;
        run.mods = { { "MapMod_MonsterDamage", { 12.0, 32.25 } } };
        run.returnCellFormId = 0x000B6BE6;
        run.returnWorldspaceFormId = 0x0000003C;
        run.returnPos = { 11.0f, -12.0f, 17.25f };
        run.returnAngle = { -0.25f, 0.0f, 2.25f };
        run.bossRefFormId = 0xFF001230;
        run.spawnedRefFormIds = { run.bossRefFormId, 0xFF001231 };
        return run;
    }

    Bytes AtlasFixture()
    {
        // 완료 마스크 0x0244, 클리어 11회, 최고 T10, 첫 공물 수령.
        return { 0x44, 0x02, 11, 0, 0, 0, 10, 1 };
    }

    // v1/v2 디스크 순서를 독립적으로 조립한다. v3 인코더와 v2의 공통 부분도 대조한다.
    Bytes LegacyFixture(std::uint32_t version, const ActiveMapState& run)
    {
        Bytes bytes = AtlasFixture();
        const auto write = Writer(bytes);
        const auto pod = [&write](const auto& value) { write(&value, sizeof(value)); };
        const auto string = [&pod, &write](const std::string& value) {
            const auto length = static_cast<std::uint32_t>(value.size());
            pod(length);
            write(value.data(), length);
        };
        const auto flag = [&pod](bool value) { pod(static_cast<std::uint8_t>(value)); };
        flag(run.active);
        flag(run.isArena4);
        flag(run.bossKilled);
        flag(run.failed);
        if (version == kLegacyKeyMapSaveVersion) {
            flag(run.playerAuthorized);
            flag(run.endRequested);
        }
        pod(run.runId);
        pod(run.tier);
        if (version == kLegacyIndexMapSaveVersion) {
            pod(kLegacyDungeonIndex);
        } else {
            string(run.dungeonKey);
            pod(run.packVersion);
        }
        pod(run.portalsRemaining);
        pod(run.arenaPhase);
        pod(static_cast<std::uint32_t>(run.mods.size()));
        for (const auto& affix : run.mods) {
            string(affix.affixId);
            pod(static_cast<std::uint32_t>(affix.values.size()));
            for (const double value : affix.values) {
                pod(value);
            }
        }
        pod(run.returnCellFormId);
        pod(run.returnWorldspaceFormId);
        write(run.returnPos.data(), sizeof(run.returnPos));
        write(run.returnAngle.data(), sizeof(run.returnAngle));
        pod(run.bossRefFormId);
        pod(static_cast<std::uint32_t>(run.spawnedRefFormIds.size()));
        for (const auto id : run.spawnedRefFormIds) {
            pod(id);
        }
        return bytes;
    }

    void CheckAtlas(const AtlasState& actual, const AtlasState& expected)
    {
        POE_CHECK(actual.completedTierMask == expected.completedTierMask);
        POE_CHECK(actual.totalMapsCleared == expected.totalMapsCleared);
        POE_CHECK(actual.highestTierCleared == expected.highestTierCleared);
        POE_CHECK(actual.firstTributeGiven == expected.firstTributeGiven);
    }

    void CheckRun(const ActiveMapState& actual, const ActiveMapState& expected)
    {
        POE_CHECK(actual.active == expected.active && actual.isArena4 == expected.isArena4);
        POE_CHECK(actual.bossKilled == expected.bossKilled && actual.failed == expected.failed);
        POE_CHECK(actual.playerAuthorized == expected.playerAuthorized && actual.endRequested == expected.endRequested);
        POE_CHECK(actual.spawnsDisabled == expected.spawnsDisabled);
        POE_CHECK(actual.runId == expected.runId && actual.tier == expected.tier);
        POE_CHECK(actual.dungeonKey == expected.dungeonKey && actual.packVersion == expected.packVersion);
        POE_CHECK(actual.portalsRemaining == expected.portalsRemaining && actual.arenaPhase == expected.arenaPhase);
        POE_CHECK(actual.arenaPhaseInvulnSec == expected.arenaPhaseInvulnSec);
        POE_CHECK(actual.mods == expected.mods);
        POE_CHECK(actual.summary == MapSystem::AggregateMapMods(expected.mods));
        POE_CHECK(actual.returnCellFormId == expected.returnCellFormId);
        POE_CHECK(actual.returnWorldspaceFormId == expected.returnWorldspaceFormId);
        POE_CHECK(actual.returnPos == expected.returnPos && actual.returnAngle == expected.returnAngle);
        POE_CHECK(actual.bossRefFormId == expected.bossRefFormId);
        POE_CHECK(actual.spawnedRefFormIds == expected.spawnedRefFormIds);
        POE_CHECK(actual.unresolvedBossRefFormId == 0);
    }
}

POE_TEST("ATLS v1 round-trips atlas fields in eight bytes")
{
    const AtlasState saved{ 0x0244, 11, 10, true };
    Bytes bytes;
    POE_CHECK(WriteAtlasSave(Writer(bytes), saved));
    POE_CHECK(bytes == AtlasFixture());
    const auto loaded = ParseAtlasSave(kAtlasSaveVersion, bytes);
    POE_CHECK(loaded);
    CheckAtlas(*loaded, saved);
}

POE_TEST("ATLS rejects every truncation, unsupported versions and out-of-range tier or tribute")
{
    const auto valid = AtlasFixture();
    for (std::size_t size = 0; size < valid.size(); ++size) {
        POE_CHECK(!ParseAtlasSave(kAtlasSaveVersion, std::span(valid).first(size)));
    }
    POE_CHECK(!ParseAtlasSave(kAtlasSaveVersion + 1, valid));
    auto bytes = valid;
    bytes[6] = kMaxMapTier + 1;
    POE_CHECK(!ParseAtlasSave(kAtlasSaveVersion, bytes));
    bytes = valid;
    bytes[7] = 2;
    POE_CHECK(!ParseAtlasSave(kAtlasSaveVersion, bytes));
}

POE_TEST("ATLS only accepts atlas progress reachable by recording map clears")
{
    AtlasState atlas;
    const auto roundTrip = [&atlas]() {
        Bytes bytes;
        POE_CHECK(WriteAtlasSave(Writer(bytes), atlas));
        return ParseAtlasSave(kAtlasSaveVersion, bytes);
    };
    POE_CHECK(roundTrip());
    POE_CHECK(MapSystem::RecordMapClear(atlas, 3));
    POE_CHECK(MapSystem::RecordMapClear(atlas, kMaxMapTier));
    POE_CHECK(!MapSystem::RecordMapClear(atlas, 3));
    POE_CHECK(roundTrip());

    const auto valid = atlas;
    atlas.highestTierCleared = 3;
    POE_CHECK(!roundTrip());
    atlas = valid;
    atlas.totalMapsCleared = 1;
    POE_CHECK(!roundTrip());
    atlas = { 0, 1, 0, false };
    POE_CHECK(!roundTrip());
    atlas = { 0xFFFF, 0, 0, true };
    POE_CHECK(!roundTrip());
    atlas = valid;
    atlas.totalMapsCleared = std::numeric_limits<std::uint32_t>::max();
    POE_CHECK(!MapSystem::RecordMapClear(atlas, 3));
    POE_CHECK(atlas.totalMapsCleared == std::numeric_limits<std::uint32_t>::max());
    POE_CHECK(roundTrip());
}

POE_TEST("Active MAPS v3 appends the arena invulnerability and spawn-disable flag to v2 run bytes")
{
    auto saved = TestRun();
    saved.spawnsDisabled = true;
    Bytes bytes;
    POE_CHECK(WriteMapRunSave(Writer(bytes), saved));
    auto expected = LegacyFixture(kLegacyKeyMapSaveVersion, saved);
    expected.erase(expected.begin(), expected.begin() + kAtlasBytes);
    Writer(expected)(&saved.arenaPhaseInvulnSec, sizeof(saved.arenaPhaseInvulnSec));
    expected.push_back(1);
    POE_CHECK(bytes == expected);
    const auto record = ParseMapSave(kMapSaveVersion, bytes, PassThrough);
    POE_CHECK(record.run && !record.legacyAtlas);
    CheckRun(*record.run, saved);
}

POE_TEST("Inactive MAPS v3 only stores the active flag")
{
    auto saved = TestRun();
    saved.active = false;
    Bytes bytes;
    POE_CHECK(WriteMapRunSave(Writer(bytes), saved));
    POE_CHECK(bytes.size() == 1 && bytes.front() == 0);
    const auto loaded = ParseMapSave(kMapSaveVersion, bytes, PassThrough);
    POE_CHECK(loaded.run);
    CheckRun(*loaded.run, {});
}

POE_TEST("Active MAPS rejects truncation at every byte and unsupported versions")
{
    Bytes bytes;
    POE_CHECK(WriteMapRunSave(Writer(bytes), TestRun()));
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const auto record = ParseMapSave(kMapSaveVersion, std::span(bytes).first(size), PassThrough);
        POE_CHECK(!record.run && !record.legacyAtlas);
    }
    POE_CHECK(!ParseMapSave(kMapSaveVersion + 1, bytes, PassThrough).run);
}

POE_TEST("ATLS and MAPS v3 require the exact declared record length")
{
    auto atlas = AtlasFixture();
    POE_CHECK(!ReadAtlasSave(kAtlasSaveVersion, atlas.size() + 1, Reader(atlas)));
    POE_CHECK(!ReadAtlasSave(kAtlasSaveVersion, atlas.size() - 1, Reader(atlas)));
    POE_CHECK(!ReadAtlasSave(kAtlasSaveVersion, atlas.size(), Reader(std::span(atlas).first(atlas.size() - 1))));
    atlas.push_back(0);
    POE_CHECK(!ParseAtlasSave(kAtlasSaveVersion, atlas));

    Bytes run;
    POE_CHECK(WriteMapRunSave(Writer(run), TestRun()));
    POE_CHECK(!ReadMapSave(kMapSaveVersion, run.size() + 1, Reader(run), PassThrough).run);
    POE_CHECK(!ReadMapSave(kMapSaveVersion, run.size() - 1, Reader(run), PassThrough).run);
    POE_CHECK(!ReadMapSave(kMapSaveVersion, run.size(), Reader(std::span(run).first(run.size() - 1)), PassThrough).run);
    auto wrongActiveFlag = run;
    wrongActiveFlag[0] = 0;
    POE_CHECK(!ParseMapSave(kMapSaveVersion, wrongActiveFlag, PassThrough).run);
    run.push_back(0);
    POE_CHECK(!ParseMapSave(kMapSaveVersion, run, PassThrough).run);
    POE_CHECK(!ParseMapSave(kMapSaveVersion, Bytes{ 0, 0 }, PassThrough).run);
}

POE_TEST("MAPS rejects out-of-range flags, run identity, tiers, phase, portals and collection lengths")
{
    const auto run = TestRun();
    Bytes valid;
    POE_CHECK(WriteMapRunSave(Writer(valid), run));
    const auto rejects = [&](std::size_t offset, auto value) {
        auto modified = valid;
        ChangeValue(modified, offset, value);
        POE_CHECK(!ParseMapSave(kMapSaveVersion, modified, PassThrough).run);
    };
    for (std::size_t flag = 0; flag < 6; ++flag) {
        rejects(flag, std::uint8_t{ 2 });
    }
    rejects(6, std::uint64_t{ 0 });
    rejects(6, kMapRunIdLimit);
    rejects(6, std::numeric_limits<std::uint64_t>::max() - 1);
    rejects(14, static_cast<std::uint8_t>(kMaxMapTier + 1));
    rejects(15, static_cast<std::uint32_t>(kMaxMapDungeonKeyBytes + 1));
    const auto portalsOffset = 23 + run.dungeonKey.size();
    const auto affixCountOffset = portalsOffset + 2;
    const auto valueCountOffset = affixCountOffset + 8 + run.mods.front().affixId.size();
    rejects(portalsOffset, static_cast<std::uint8_t>(kArena4MaxPortals + 1));
    rejects(portalsOffset + 1, static_cast<std::uint8_t>(kArena4FinalPhase + 1));
    rejects(affixCountOffset, kMaxSavedAffixes + 1);
    rejects(affixCountOffset + 4, kMaxSaveStringBytes + 1);
    rejects(valueCountOffset, kMaxSavedAffixValues + 1);
    const auto spawnCountOffset = valid.size() - sizeof(float) - 1 - (run.spawnedRefFormIds.size() + 1) * sizeof(std::uint32_t);
    rejects(spawnCountOffset, static_cast<std::uint32_t>(kMaxMapSpawnedActors + 1));
    rejects(spawnCountOffset - sizeof(std::uint32_t), run.returnCellFormId);
    rejects(spawnCountOffset + sizeof(std::uint32_t), run.returnCellFormId);
    rejects(spawnCountOffset + sizeof(std::uint32_t), std::uint32_t{ 0 });
    rejects(valueCountOffset + sizeof(std::uint32_t), std::numeric_limits<double>::quiet_NaN());
    rejects(valid.size() - 1, std::uint8_t{ 2 });
}

POE_TEST("Legacy MAPS v1 and v2 restore both sections and v1's legacy cell index")
{
    for (const auto version : { kLegacyIndexMapSaveVersion, kLegacyKeyMapSaveVersion }) {
        auto expected = TestRun();
        expected.spawnsDisabled = true;
        const auto bytes = LegacyFixture(version, expected);
        expected.spawnsDisabled = false;
        expected.arenaPhaseInvulnSec = 0.0f;
        const auto loaded = ParseMapSave(version, bytes, PassThrough);
        POE_CHECK(loaded.run && loaded.legacyAtlas);
        if (version == kLegacyIndexMapSaveVersion) {
            expected.playerAuthorized = false;
            expected.endRequested = false;
            expected.dungeonKey.clear();
            expected.packVersion = 0;
            POE_CHECK(loaded.legacyDungeonIndex == kLegacyDungeonIndex);
        }
        CheckRun(*loaded.run, expected);
        CheckAtlas(*loaded.legacyAtlas, { 0x0244, 11, 10, true });
    }
}

POE_TEST("Legacy MAPS finds atlas and run damage independently")
{
    for (const auto version : { kLegacyIndexMapSaveVersion, kLegacyKeyMapSaveVersion }) {
        const auto bytes = LegacyFixture(version, TestRun());
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            const auto loaded = ParseMapSave(version, std::span(bytes).first(size), PassThrough);
            POE_CHECK(!loaded.run);
            const auto atlas = GetMapSaveStatus(false, std::nullopt, loaded);
            POE_CHECK(atlas.atlasDamaged == (size < kAtlasBytes) && atlas.runDamaged);
            if (size < kAtlasBytes) {
                CheckAtlas(atlas.atlas, { 0, 0, 0, true });
            } else {
                CheckAtlas(atlas.atlas, { 0x0244, 11, 10, true });
            }
        }
    }
}

POE_TEST("Legacy MAPS coerces flag bytes, clamps bounds and ignores non-dynamic bosses")
{
    constexpr auto kOutOfRangeByte = std::numeric_limits<std::uint8_t>::max();
    for (const auto version : { kLegacyIndexMapSaveVersion, kLegacyKeyMapSaveVersion }) {
        for (const bool isArena : { false, true }) {
            auto saved = TestRun();
            saved.isArena4 = isArena;
            saved.tier = isArena ? kOutOfRangeByte : 0;
            saved.arenaPhase = isArena ? kOutOfRangeByte : 0;
            saved.portalsRemaining = kOutOfRangeByte;
            saved.bossRefFormId = saved.returnCellFormId;
            auto bytes = LegacyFixture(version, saved);
            bytes[6] = kOutOfRangeByte; // 최고 티어
            bytes[7] = kOutOfRangeByte; // 첫 공물
            bytes[kAtlasBytes] = kOutOfRangeByte; // active
            bytes[kAtlasBytes + 1] = isArena ? kOutOfRangeByte : 0;
            const std::size_t flagCount = version == kLegacyIndexMapSaveVersion ? 4 : 6;
            for (std::size_t flag = 2; flag < flagCount; ++flag) {
                bytes[kAtlasBytes + flag] = kOutOfRangeByte;
            }

            const auto loaded = ParseMapSave(version, bytes, PassThrough);
            POE_CHECK(loaded.run && loaded.legacyAtlas);
            const auto& run = *loaded.run;
            POE_CHECK(run.active && run.isArena4 == isArena && run.bossKilled && run.failed);
            POE_CHECK(run.playerAuthorized == (version == kLegacyKeyMapSaveVersion));
            POE_CHECK(run.endRequested == (version == kLegacyKeyMapSaveVersion));
            POE_CHECK(run.tier == (isArena ? kMaxMapTier : kMinMapTier));
            POE_CHECK(run.arenaPhase == (isArena ? kArena4FinalPhase : kArena4FirstPhase));
            POE_CHECK(run.portalsRemaining == (isArena ? kArena4MaxPortals : kNormalMapMaxPortals));
            POE_CHECK(run.bossRefFormId == 0 && run.unresolvedBossRefFormId == 0);
            const auto status = GetMapSaveStatus(false, std::nullopt, loaded);
            POE_CHECK(!status.atlasDamaged && !status.runDamaged);
            CheckAtlas(status.atlas, { 0x0244, 11, kMaxMapTier, true });

            const auto withoutStaticForm = [&saved](std::uint32_t raw, std::uint32_t& resolved) {
                return raw != saved.bossRefFormId && PassThrough(raw, resolved);
            };
            const auto unresolved = ParseMapSave(version, bytes, withoutStaticForm);
            POE_CHECK(unresolved.run && unresolved.run->bossRefFormId == 0 && unresolved.run->unresolvedBossRefFormId == 0);
        }
    }
}

POE_TEST("Legacy MAPS v1 and v2 read inactive runs with their full saved payload")
{
    for (const auto version : { kLegacyIndexMapSaveVersion, kLegacyKeyMapSaveVersion }) {
        const auto bytes = LegacyFixture(version, {});
        const auto loaded = ParseMapSave(version, bytes, PassThrough);
        POE_CHECK(loaded.run && loaded.legacyAtlas);
        CheckRun(*loaded.run, {});
        const auto status = GetMapSaveStatus(false, std::nullopt, loaded);
        POE_CHECK(!status.atlasDamaged && !status.runDamaged);
        CheckAtlas(status.atlas, { 0x0244, 11, 10, true });

        const auto truncated = ParseMapSave(version, std::span(bytes).first(kAtlasBytes + 1), PassThrough);
        POE_CHECK(!truncated.run && truncated.legacyAtlas);
    }
}

POE_TEST("ATLS takes precedence and first tribute falls back only for unreadable map history")
{
    const auto legacy = ParseMapSave(kLegacyKeyMapSaveVersion, LegacyFixture(kLegacyKeyMapSaveVersion, TestRun()), PassThrough);
    const AtlasState standalone{ 0x0020, 2, 6, false };
    auto atlas = GetMapSaveStatus(true, standalone, legacy);
    POE_CHECK(!atlas.atlasDamaged && !atlas.runDamaged);
    CheckAtlas(atlas.atlas, standalone);

    atlas = GetMapSaveStatus(true, std::nullopt, legacy);
    POE_CHECK(atlas.atlasDamaged && !atlas.runDamaged);
    CheckAtlas(atlas.atlas, { 0, 0, 0, true });

    const Bytes inactive{ 0 };
    const auto separatedRun = ParseMapSave(kMapSaveVersion, inactive, PassThrough);
    POE_CHECK(separatedRun.run);
    atlas = GetMapSaveStatus(false, std::nullopt, separatedRun);
    POE_CHECK(atlas.atlasDamaged && !atlas.runDamaged);
    CheckAtlas(atlas.atlas, { 0, 0, 0, true });

    const auto brokenRun = ParseMapSave(kMapSaveVersion, std::span<const std::uint8_t>{}, PassThrough);
    POE_CHECK(!brokenRun.run);
    atlas = GetMapSaveStatus(true, standalone, brokenRun);
    POE_CHECK(!atlas.atlasDamaged && atlas.runDamaged);
    CheckAtlas(atlas.atlas, standalone);

    atlas = GetMapSaveStatus(true, standalone, std::nullopt);
    POE_CHECK(!atlas.atlasDamaged && atlas.runDamaged);
    CheckAtlas(atlas.atlas, standalone);

    const auto unsupported = ParseMapSave(kMapSaveVersion + 1, inactive, PassThrough);
    atlas = GetMapSaveStatus(false, std::nullopt, unsupported);
    POE_CHECK(atlas.atlasDamaged && atlas.runDamaged);
    CheckAtlas(atlas.atlas, { 0, 0, 0, true });

    atlas = GetMapSaveStatus(false, std::nullopt, std::nullopt);
    POE_CHECK(!atlas.atlasDamaged && !atlas.runDamaged);
    CheckAtlas(atlas.atlas, {});
}

POE_TEST("MAPS marks unresolved saved bosses and remaps the surviving references")
{
    auto run = TestRun();
    Bytes bytes;
    POE_CHECK(WriteMapRunSave(Writer(bytes), run));
    const auto resolve = [&run](std::uint32_t raw, std::uint32_t& result) {
        result = raw + 1;
        return raw != run.bossRefFormId && raw != run.returnWorldspaceFormId;
    };
    const auto loaded = ParseMapSave(kMapSaveVersion, bytes, resolve);
    POE_CHECK(loaded.run && loaded.run->unresolvedBossRefFormId == run.bossRefFormId && loaded.run->bossRefFormId == 0);
    POE_CHECK(loaded.run->returnCellFormId == run.returnCellFormId + 1 && loaded.run->returnWorldspaceFormId == 0);
    POE_CHECK(loaded.run->spawnedRefFormIds == std::vector<std::uint32_t>{ run.spawnedRefFormIds.back() + 1 });
    Bytes resaved;
    POE_CHECK(WriteMapRunSave(Writer(resaved), *loaded.run));
    const auto reloaded = ParseMapSave(kMapSaveVersion, resaved, resolve);
    POE_CHECK(reloaded.run && reloaded.run->unresolvedBossRefFormId == run.bossRefFormId);
    for (const auto invalidResolvedId : { 0u, run.returnCellFormId }) {
        const auto invalidBossResolution = [&run, invalidResolvedId](std::uint32_t raw, std::uint32_t& result) {
            result = raw == run.bossRefFormId ? invalidResolvedId : raw;
            return true;
        };
        POE_CHECK(!ParseMapSave(kMapSaveVersion, bytes, invalidBossResolution).run);
    }

    run.bossRefFormId = 0;
    bytes.clear();
    POE_CHECK(WriteMapRunSave(Writer(bytes), run));
    const auto beforeSpawn = ParseMapSave(kMapSaveVersion, bytes, PassThrough);
    POE_CHECK(beforeSpawn.run && beforeSpawn.run->unresolvedBossRefFormId == 0 && beforeSpawn.run->bossRefFormId == 0);
}

POE_TEST("MAPS v3 rejects a forbidden raw boss or spawn even when the raw ID does not resolve")
{
    auto run = TestRun();
    const auto staticId = run.returnCellFormId;
    const auto withoutStatic = [staticId](std::uint32_t raw, std::uint32_t& result) {
        return raw != staticId && PassThrough(raw, result);
    };
    run.bossRefFormId = staticId;
    Bytes bytes;
    POE_CHECK(WriteMapRunSave(Writer(bytes), run));
    POE_CHECK(!ParseMapSave(kMapSaveVersion, bytes, withoutStatic).run);

    run.bossRefFormId = 0;
    run.spawnedRefFormIds.front() = staticId;
    bytes.clear();
    POE_CHECK(WriteMapRunSave(Writer(bytes), run));
    POE_CHECK(!ParseMapSave(kMapSaveVersion, bytes, withoutStatic).run);
}

POE_TEST("MAPS v3 validates complete affix rolls against the loaded item definitions")
{
    ItemData data;
    POE_CHECK(data.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "items_3_30.json"));
    std::vector<RolledAffix> rolls{ { "MapMod_Vulnerability", { 15.0 } }, { "MapMod_ExtraFire", { 60.0 } } };
    POE_CHECK(MapSystem::AreSavedMapRollsValid(data, {}));
    POE_CHECK(MapSystem::AreSavedMapRollsValid(data, rolls));
    rolls.front().values.front() = 14.9;
    POE_CHECK(!MapSystem::AreSavedMapRollsValid(data, rolls));
    rolls.front().values.front() = 25.1;
    POE_CHECK(!MapSystem::AreSavedMapRollsValid(data, rolls));
    rolls.front().values.front() = std::numeric_limits<double>::infinity();
    POE_CHECK(!MapSystem::AreSavedMapRollsValid(data, rolls));
    rolls.front().values.clear();
    POE_CHECK(!MapSystem::AreSavedMapRollsValid(data, rolls));
    rolls.front().values = { 25.0, 25.0 };
    POE_CHECK(!MapSystem::AreSavedMapRollsValid(data, rolls));
    rolls.front().values = { 25.0 };
    POE_CHECK(MapSystem::AreSavedMapRollsValid(data, rolls));
    rolls.front().affixId = "UnknownAffix";
    POE_CHECK(!MapSystem::AreSavedMapRollsValid(data, rolls));
}

POE_TEST("MAPS accepts the final valid run ID and issuance skips reserved values")
{
    auto run = TestRun();
    run.runId = kMapRunIdLimit - 1;
    Bytes bytes;
    POE_CHECK(WriteMapRunSave(Writer(bytes), run));
    const auto loaded = ParseMapSave(kMapSaveVersion, bytes, PassThrough);
    POE_CHECK(loaded.run && loaded.run->runId == run.runId);
    POE_CHECK(NextMapRunId(kMapRunIdLimit - 2) == run.runId);
    POE_CHECK(NextMapRunId(run.runId) == 1);
    POE_CHECK(NextMapRunId(1) == 2);
    POE_CHECK(NextMapRunId(0) == 1);
    POE_CHECK(NextMapRunId(std::numeric_limits<std::uint64_t>::max()) == 1);
}

POE_TEST("Duplicate MAPS selects the latest valid run and keeps a readable legacy prefix on damage")
{
    const auto legacy = LegacyFixture(kLegacyKeyMapSaveVersion, TestRun());
    const auto truncated = std::span(legacy).first(kAtlasBytes + 1);
    const Bytes damaged{ 0, 1 };
    std::optional<MapSaveRecord> selected;
    KeepLatestMapSave(selected, ParseMapSave(kMapSaveVersion, damaged, PassThrough));
    POE_CHECK(selected && !selected->run && !selected->legacyAtlas);
    KeepLatestMapSave(selected, ParseMapSave(kLegacyKeyMapSaveVersion, truncated, PassThrough));
    KeepLatestMapSave(selected, ParseMapSave(kMapSaveVersion, damaged, PassThrough));
    POE_CHECK(selected && !selected->run && selected->legacyAtlas);

    KeepLatestMapSave(selected, ParseMapSave(kLegacyKeyMapSaveVersion, legacy, PassThrough));
    KeepLatestMapSave(selected, ParseMapSave(kMapSaveVersion, damaged, PassThrough));
    POE_CHECK(selected && selected->run && selected->run->active && selected->legacyAtlas);
    CheckAtlas(*selected->legacyAtlas, { 0x0244, 11, 10, true });
    KeepLatestMapSave(selected, ParseMapSave(kMapSaveVersion, Bytes{ 0 }, PassThrough));
    KeepLatestMapSave(selected, ParseMapSave(kLegacyKeyMapSaveVersion, truncated, PassThrough));
    POE_CHECK(selected && selected->run && !selected->run->active && !selected->legacyAtlas);
}

POE_TEST("MAPS bounds arena invulnerability and retains the fixed marker fallback for invalid coordinates")
{
    auto saved = TestRun();
    for (const auto value : { -3.0f, 4.0f, std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity() }) {
        saved.arenaPhaseInvulnSec = value;
        saved.returnPos[0] = std::numeric_limits<float>::infinity();
        saved.returnAngle[1] = std::numeric_limits<float>::quiet_NaN();
        Bytes bytes;
        POE_CHECK(WriteMapRunSave(Writer(bytes), saved));
        const auto loaded = ParseMapSave(kMapSaveVersion, bytes, PassThrough);
        POE_CHECK(loaded.run);
        const auto expected = std::isfinite(value) && value > 0.0f ? kArena4PhaseInvulnSec : 0.0f;
        POE_CHECK(loaded.run->arenaPhaseInvulnSec == expected);
        POE_CHECK(loaded.run->returnCellFormId == 0 && loaded.run->returnWorldspaceFormId == 0);
        POE_CHECK((loaded.run->returnPos == std::array<float, 3>{}));
        POE_CHECK(loaded.run->returnAngle[1] == 0.0f);
    }
}
