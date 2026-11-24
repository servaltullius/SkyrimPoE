#pragma once

#include "core/MapSaveIdentity.h"
#include "core/MapSystem.h"
#include "core/SaveAffixList.h"

#include <bit>
#include <cstring>
#include <span>

namespace SkyrimPoE::Core
{
    inline constexpr std::uint32_t kLegacyIndexMapSaveVersion = 1;
    inline constexpr std::uint32_t kLegacyKeyMapSaveVersion = 2;

    struct MapSaveRecord
    {
        std::optional<AtlasState> legacyAtlas;
        std::optional<ActiveMapState> run;
        std::uint8_t legacyDungeonIndex = 0;
    };

    // 정상 회차를 우선하고, 회차가 전부 손상이면 읽을 수 있었던 레거시 아틀라스를 보존한다.
    inline void KeepLatestMapSave(std::optional<MapSaveRecord>& selected, MapSaveRecord candidate)
    {
        if (candidate.run || !selected || (!selected->run && candidate.legacyAtlas)) {
            selected = std::move(candidate);
        }
    }

    struct MapSaveStatus
    {
        AtlasState atlas;
        bool atlasDamaged = false;
        bool runDamaged = false;
    };

    // ATLS가 존재하면 저장 순서와 관계없이 레거시 접두부보다 우선한다.
    inline MapSaveStatus GetMapSaveStatus(
        bool sawAtlasRecord, const std::optional<AtlasState>& atlas, const std::optional<MapSaveRecord>& maps)
    {
        const bool seen = sawAtlasRecord || maps.has_value();
        const bool runDamaged = seen && (!maps || !maps->run);
        if (sawAtlasRecord && atlas) {
            return { *atlas, false, runDamaged };
        }
        if (!sawAtlasRecord && maps && maps->legacyAtlas) {
            return { *maps->legacyAtlas, false, runDamaged };
        }
        AtlasState fallback;
        fallback.firstTributeGiven = seen;
        return { fallback, seen, runDamaged };
    }

    template <class Write>
    bool WriteAtlasSave(Write&& write, const AtlasState& atlas)
    {
        const std::uint8_t tribute = atlas.firstTributeGiven ? 1 : 0;
        return write(&atlas.completedTierMask, sizeof(atlas.completedTierMask)) &&
               write(&atlas.totalMapsCleared, sizeof(atlas.totalMapsCleared)) &&
               write(&atlas.highestTierCleared, sizeof(atlas.highestTierCleared)) &&
               write(&tribute, sizeof(tribute));
    }

    namespace Detail
    {
        template <class Read>
        std::optional<AtlasState> ReadMapAtlasFields(Read& read, bool legacy)
        {
            AtlasState atlas;
            std::uint8_t tribute = 0;
            if (!read(&atlas.completedTierMask, sizeof(atlas.completedTierMask)) ||
                !read(&atlas.totalMapsCleared, sizeof(atlas.totalMapsCleared)) ||
                !read(&atlas.highestTierCleared, sizeof(atlas.highestTierCleared)) ||
                !read(&tribute, sizeof(tribute))) {
                return std::nullopt;
            }
            if (legacy) {
                atlas.highestTierCleared = std::min(atlas.highestTierCleared, kMaxMapTier);
            } else if (atlas.highestTierCleared != std::bit_width(atlas.completedTierMask) ||
                atlas.totalMapsCleared < static_cast<std::uint32_t>(std::popcount(atlas.completedTierMask)) ||
                (atlas.completedTierMask == 0 && atlas.totalMapsCleared != 0) || tribute > 1) {
                return std::nullopt;
            }
            atlas.firstTributeGiven = tribute != 0;
            return atlas;
        }
    }

    template <class Read>
    std::optional<AtlasState> ReadAtlasSave(std::uint32_t version, std::size_t length, Read&& read)
    {
        constexpr auto atlasBytes = sizeof(AtlasState::completedTierMask) + sizeof(AtlasState::totalMapsCleared) +
            sizeof(AtlasState::highestTierCleared) + sizeof(std::uint8_t);
        if (version != kAtlasSaveVersion || length != atlasBytes) {
            return std::nullopt;
        }
        return Detail::ReadMapAtlasFields(read, false);
    }

    template <class Write>
    bool WriteMapRunSave(Write&& write, const ActiveMapState& run)
    {
        const auto pod = [&write](const auto& value) { return write(&value, sizeof(value)); };
        const auto flag = [&pod](bool value) { return pod(static_cast<std::uint8_t>(value)); };
        if (!flag(run.active)) {
            return false;
        }
        if (!run.active) {
            return true;
        }
        const auto spawnCount = static_cast<std::uint32_t>(
            std::min<std::size_t>(run.spawnedRefFormIds.size(), kMaxMapSpawnedActors));
        const auto boss = run.bossRefFormId != 0 ? run.bossRefFormId : run.unresolvedBossRefFormId;
        if (!flag(run.isArena4) || !flag(run.bossKilled) || !flag(run.failed) ||
            !flag(run.playerAuthorized) || !flag(run.endRequested) ||
            !pod(run.runId) || !pod(run.tier) || !WriteMapSaveIdentity(write, run) ||
            !pod(run.portalsRemaining) || !pod(run.arenaPhase) || !WriteSaveAffixList(write, run.mods) ||
            !pod(run.returnCellFormId) || !pod(run.returnWorldspaceFormId) ||
            !write(run.returnPos.data(), sizeof(run.returnPos)) ||
            !write(run.returnAngle.data(), sizeof(run.returnAngle)) ||
            !pod(boss) || !pod(spawnCount)) {
            return false;
        }
        for (std::uint32_t i = 0; i < spawnCount; ++i) {
            if (!pod(run.spawnedRefFormIds[i])) {
                return false;
            }
        }
        return pod(run.arenaPhaseInvulnSec) && flag(run.spawnsDisabled);
    }

    template <class Read, class Resolve>
    MapSaveRecord ReadMapSave(std::uint32_t version, std::size_t length, Read&& readBytes, Resolve&& resolve)
    {
        MapSaveRecord record;
        const bool legacyIndex = version == kLegacyIndexMapSaveVersion;
        const bool legacy = legacyIndex || version == kLegacyKeyMapSaveVersion;
        if (!legacy && version != kMapSaveVersion) {
            return record;
        }
        auto read = [&length, &readBytes](void* data, std::size_t size) {
            if (size > length || !readBytes(data, size)) {
                return false;
            }
            length -= size;
            return true;
        };
        if (legacy) {
            // 회차를 읽지 못해도 온전한 아틀라스 접두부는 보존한다.
            record.legacyAtlas = Detail::ReadMapAtlasFields(read, true);
        }

        ActiveMapState run;
        const auto pod = [&read](auto& value) { return read(&value, sizeof(value)); };
        const auto flag = [&pod, legacy](bool& value) {
            std::uint8_t byte = 0;
            if (!pod(byte) || (!legacy && byte > 1)) {
                return false;
            }
            value = byte != 0;
            return true;
        };
        if (!flag(run.active)) {
            return record;
        }
        if (!legacy && !run.active) {
            if (length == 0) {
                record.run = std::move(run);
            }
            return record;
        }

        std::uint32_t rawCell = 0, rawWorldspace = 0, rawBoss = 0, spawnCount = 0;
        if (!flag(run.isArena4) || !flag(run.bossKilled) || !flag(run.failed) ||
            (!legacyIndex && (!flag(run.playerAuthorized) || !flag(run.endRequested))) ||
            !pod(run.runId) || !pod(run.tier) ||
            (legacyIndex ? !pod(record.legacyDungeonIndex) : !ReadMapSaveIdentity(read, run)) ||
            !pod(run.portalsRemaining) || !pod(run.arenaPhase) || !ReadSaveAffixList(read, run.mods, !legacy) ||
            !pod(rawCell) || !pod(rawWorldspace) ||
            !read(run.returnPos.data(), sizeof(run.returnPos)) ||
            !read(run.returnAngle.data(), sizeof(run.returnAngle)) ||
            !pod(rawBoss) || !pod(spawnCount) || spawnCount > kMaxMapSpawnedActors) {
            return record;
        }
        const auto maxPortals = run.isArena4 ? kArena4MaxPortals : kNormalMapMaxPortals;
        if (legacy) {
            run.tier = std::clamp(run.tier, kMinMapTier, kMaxMapTier);
            run.portalsRemaining = std::min(run.portalsRemaining, maxPortals);
            run.arenaPhase = std::clamp(run.arenaPhase, kArena4FirstPhase, kArena4FinalPhase);
        } else if (run.tier < kMinMapTier || run.tier > kMaxMapTier ||
            run.portalsRemaining > maxPortals ||
            run.arenaPhase < kArena4FirstPhase || run.arenaPhase > kArena4FinalPhase ||
            !IsValidMapRunId(run.runId)) {
            return record;
        }

        const auto resolveForm = [&resolve](std::uint32_t raw) {
            std::uint32_t resolved = 0;
            return raw != 0 && resolve(raw, resolved) ? resolved : 0u;
        };
        const auto isDynamicForm = [](std::uint32_t id) {
            constexpr std::uint32_t dynamicFormMask = 0xFF000000u;
            return (id & dynamicFormMask) == dynamicFormMask;
        };
        if (!legacy && rawBoss != 0 && !isDynamicForm(rawBoss)) {
            return record;
        }
        run.spawnedRefFormIds.reserve(spawnCount);
        for (std::uint32_t i = 0; i < spawnCount; ++i) {
            std::uint32_t rawSpawn = 0;
            if (!pod(rawSpawn) || (!legacy && !isDynamicForm(rawSpawn))) {
                return record;
            }
            if (const auto resolved = resolveForm(rawSpawn); isDynamicForm(resolved)) {
                run.spawnedRefFormIds.push_back(resolved);
            }
        }
        if (!legacy) {
            if (!pod(run.arenaPhaseInvulnSec) || !flag(run.spawnsDisabled) || length != 0) {
                return record;
            }
            run.arenaPhaseInvulnSec = std::isfinite(run.arenaPhaseInvulnSec)
                ? std::clamp(run.arenaPhaseInvulnSec, 0.0f, kArena4PhaseInvulnSec)
                : 0.0f;
        }

        // 좌표를 신뢰할 수 없으면 기존과 같이 고정 귀환 마커를 쓴다.
        if (std::ranges::all_of(run.returnPos, [](float value) { return std::isfinite(value); })) {
            run.returnCellFormId = resolveForm(rawCell);
            run.returnWorldspaceFormId = resolveForm(rawWorldspace);
        } else {
            run.returnPos = {};
        }
        for (float& angle : run.returnAngle) {
            if (!std::isfinite(angle)) {
                angle = 0.0f;
            }
        }
        std::uint32_t resolvedBoss = 0;
        const bool bossResolved = rawBoss != 0 && resolve(rawBoss, resolvedBoss);
        if (bossResolved && isDynamicForm(resolvedBoss)) {
            run.bossRefFormId = resolvedBoss;
        } else if (!legacy && bossResolved) {
            return record;
        } else if (run.active && isDynamicForm(rawBoss) && (!bossResolved || resolvedBoss == 0)) {
            run.unresolvedBossRefFormId = rawBoss;
        }
        run.summary = MapSystem::AggregateMapMods(run.mods);
        record.run = legacyIndex && !run.active ? ActiveMapState{} : std::move(run);
        return record;
    }

    template <class Resolve>
    MapSaveRecord ReadMapSave(std::uint32_t version, std::span<const std::uint8_t> bytes, Resolve&& resolve)
    {
        return ReadMapSave(version, bytes.size(), [&bytes](void* out, std::size_t size) {
            if (size > bytes.size()) {
                return false;
            }
            if (size != 0) {
                std::memcpy(out, bytes.data(), size);
                bytes = bytes.subspan(size);
            }
            return true;
        }, std::forward<Resolve>(resolve));
    }
}
