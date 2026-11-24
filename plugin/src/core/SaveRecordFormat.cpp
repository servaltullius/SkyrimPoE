#include "core/SaveRecordFormat.h"
#include "core/Migration.h"
#include "core/SaveCapPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace SkyrimPoE::Core::SaveRecord
{
    namespace
    {
        struct RecordBuffer
        {
            RawSaveRecord record;

            bool WriteRecordData(const void* data, std::uint32_t size)
            {
                if (size > kMaxSaveRecordBytes - record.bytes.size()) {
                    return false;
                }
                const auto* bytes = static_cast<const std::uint8_t*>(data);
                if (size != 0) {
                    record.bytes.insert(record.bytes.end(), bytes, bytes + size);
                }
                return true;
            }
        };

        // 재검수 R-3: 상한을 넘으면 플레이어 소지품 → 바닥 → 그 밖 순으로 남긴다(Core::SelectEntriesWithinSaveCap).
        template <class Entry>
        [[nodiscard]] std::vector<std::size_t> SelectForSave(const std::vector<Entry>& entries, std::size_t cap)
        {
            std::vector<Core::SaveCapCandidate> cands;
            cands.reserve(entries.size());
            for (const auto& e : entries) {
                cands.push_back(Core::SaveCapCandidate{ e.key.ownerFormId, e.isWorld });
            }
            return Core::SelectEntriesWithinSaveCap(cands, cap, kPlayerRefFormId);
        }

        bool WriteString(RecordBuffer* intfc, const std::string& str)
        {
            return Core::WriteSaveString(
                [intfc](const void* data, std::uint32_t size) { return intfc->WriteRecordData(data, size); }, str);
        }

        bool WriteGemInstance(RecordBuffer* intfc, const Core::GemInstance& gem, std::uint32_t depth = 0)
        {
            if (depth > kMaxGemDepth) {
                return false;
            }
            if (!intfc->WriteRecordData(&gem.instanceId, sizeof(gem.instanceId))) return false;
            if (!WriteString(intfc, gem.gemId)) return false;
            if (!intfc->WriteRecordData(&gem.level, sizeof(gem.level))) return false;
            if (!intfc->WriteRecordData(&gem.quality, sizeof(gem.quality))) return false;
            if (!intfc->WriteRecordData(&gem.xp, sizeof(gem.xp))) return false;
            if (!intfc->WriteRecordData(&gem.socketCount, sizeof(gem.socketCount))) return false;

            const auto childCount = static_cast<std::uint32_t>(std::min<std::size_t>(gem.socketedGems.size(), kMaxListLen));
            if (!intfc->WriteRecordData(&childCount, sizeof(childCount))) return false;
            for (std::uint32_t i = 0; i < childCount; ++i) {
                if (!WriteGemInstance(intfc, gem.socketedGems[i], depth + 1)) {
                    return false;
                }
            }
            return true;
        }

        bool WriteAffixList(RecordBuffer* intfc, const std::vector<Core::RolledAffix>& list)
        {
            return Core::WriteSaveAffixList(
                [intfc](const void* data, std::uint32_t size) { return intfc->WriteRecordData(data, size); }, list);
        }

        bool WritePoEItemRecordV2(RecordBuffer* intfc, const Core::PoEItemRecord& rec)
        {
            const auto rarityByte = static_cast<std::uint8_t>(rec.rarity);
            const auto slotGroupByte = static_cast<std::uint8_t>(rec.slotGroup);
            const auto defSubByte = static_cast<std::uint8_t>(rec.defenseSubtype);
            const std::uint8_t amuletByte = rec.isAmulet ? 1 : 0;
            bool ok = true;
            ok = ok && intfc->WriteRecordData(&rec.baseFormId, sizeof(rec.baseFormId));
            ok = ok && intfc->WriteRecordData(&rarityByte, sizeof(rarityByte));
            ok = ok && intfc->WriteRecordData(&rec.itemLevel, sizeof(rec.itemLevel));
            ok = ok && intfc->WriteRecordData(&slotGroupByte, sizeof(slotGroupByte));
            ok = ok && intfc->WriteRecordData(&defSubByte, sizeof(defSubByte));
            ok = ok && intfc->WriteRecordData(&amuletByte, sizeof(amuletByte));
            ok = ok && WriteString(intfc, rec.weaponTag);
            ok = ok && WriteAffixList(intfc, rec.affixes);
            // v2 확장: 고유 아이템 ID, 주얼 색상, 고정 임플리싯 어픽스
            ok = ok && WriteString(intfc, rec.uniqueId);
            ok = ok && intfc->WriteRecordData(&rec.jewelColor, sizeof(rec.jewelColor));
            ok = ok && WriteAffixList(intfc, rec.implicitAffixes);
            return ok;
        }

        template <class T>
        bool ReadPod(std::span<const std::uint8_t>& bytes, T& out)
        {
            if (bytes.size() < sizeof(T)) {
                return false;
            }
            std::memcpy(&out, bytes.data(), sizeof(T));
            bytes = bytes.subspan(sizeof(T));
            return true;
        }

        bool ReadString(std::span<const std::uint8_t>& bytes, std::string& out)
        {
            return ReadSaveString([&](void* data, std::uint32_t size) {
                if (bytes.size() < size) {
                    return false;
                }
                std::memcpy(data, bytes.data(), size);
                bytes = bytes.subspan(size);
                return true;
            }, out);
        }
    }

    bool ReadGemInstance(std::span<const std::uint8_t>& bytes, Core::GemInstance& out, std::uint32_t depth)
    {
        if (depth > kMaxGemDepth) {
            return false;
        }
        if (!ReadPod(bytes, out.instanceId)) return false;
        if (!ReadString(bytes, out.gemId)) return false;
        if (!ReadPod(bytes, out.level)) return false;
        if (!ReadPod(bytes, out.quality)) return false;
        if (!ReadPod(bytes, out.xp)) return false;
        if (!ReadPod(bytes, out.socketCount)) return false;

        out.level = static_cast<std::uint8_t>(std::clamp<std::uint32_t>(out.level, 1, Core::kMaxGemLevel));
        out.socketCount = static_cast<std::uint8_t>(std::min<std::uint32_t>(out.socketCount, Core::kMaxGemSockets));

        std::uint32_t childCount = 0;
        if (!ReadPod(bytes, childCount) || childCount > kMaxListLen) {
            return false;
        }
        out.socketedGems.clear();
        out.socketedGems.reserve(childCount);
        for (std::uint32_t i = 0; i < childCount; ++i) {
            Core::GemInstance child;
            if (!ReadGemInstance(bytes, child, depth + 1)) {
                return false;
            }
            out.socketedGems.push_back(std::move(child));
        }
        return true;
    }

    bool ReadLegacyCoreV2Front(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& tmp)
    {
        std::uint8_t starterByte = 0;
        if (!ReadPod(bytes, tmp.exileLevel) || !ReadPod(bytes, tmp.exileXp) ||
            !ReadPod(bytes, tmp.unallocatedPassivePoints) || !ReadString(bytes, tmp.className) ||
            !ReadPod(bytes, starterByte) || !ReadPod(bytes, tmp.nextGemInstanceId) ||
            !ReadPod(bytes, tmp.appliedHealthDelta) || !ReadPod(bytes, tmp.appliedMagickaDelta)) {
            return false;
        }
        tmp.starterGemsGiven = starterByte != 0;
        return true;
    }

    bool ReadLegacyCoreV2Slots(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& tmp)
    {
        std::uint32_t slotCount = 0;
        if (!ReadPod(bytes, slotCount) || slotCount > kSkillSlotCount) {
            return false;
        }
        for (std::uint32_t i = 0; i < slotCount; ++i) {
            std::uint8_t occupiedByte = 0;
            std::int8_t linked = -1;
            if (!ReadPod(bytes, occupiedByte) || !ReadPod(bytes, linked)) {
                return false;
            }
            auto& slot = tmp.slots[i];
            slot.slotIndex = static_cast<std::uint8_t>(i);
            slot.occupied = occupiedByte != 0;
            slot.linkedAttackSlot = (linked >= 0 && linked < static_cast<std::int8_t>(kSkillSlotCount)) ? linked : -1;
            if (slot.occupied && !ReadGemInstance(bytes, slot.gem)) {
                return false;
            }
        }
        return true;
    }

    bool ReadCoreV2(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& tmp)
    {
        if (!ReadLegacyCoreV2Front(bytes, tmp) || !ReadLegacyCoreV2Slots(bytes, tmp)) {
            return false;
        }

        tmp.exileLevel = std::clamp(tmp.exileLevel, 1u, kMaxExileLevel);
        tmp.nextGemInstanceId = std::max<std::uint64_t>(tmp.nextGemInstanceId, 1);
        if (!std::isfinite(tmp.appliedHealthDelta)) tmp.appliedHealthDelta = 0.0;
        if (!std::isfinite(tmp.appliedMagickaDelta)) tmp.appliedMagickaDelta = 0.0;
        return true;
    }

    SaveDecision DecideSave(const SaveRecordBatch& batch, bool isNewGame, const SaveGeneration& pack)
    {
        if (!batch.complete) {
            return { SaveDisposition::Preserve, "모드 기록 수집이 불완전합니다." };
        }
        SaveGenerationInput input{
            .isNewGame = isNewGame,
            .generation = {},
            .migration = {},
            .packGame = pack.game,
            .packGeneration = pack.contentGeneration
        };
        for (const auto& record : batch.records) {
            if (record.type == kRecordGeneration) {
                if (input.generation.status == GenerationRecordStatus::Missing) {
                    input.generation = ReadGenerationRecord(record.version, record.bytes);
                } else {
                    input.generation.status = GenerationRecordStatus::Damaged;
                }
                continue;
            }
            if (record.type == kRecordMigration) {
                if (input.migration.status != MigrationRecordStatus::Missing) {
                    input.migration.status = MigrationRecordStatus::Damaged;
                } else {
                    auto migration = ReadMigrationRecord(record.version, record.bytes);
                    auto& state = migration.state;
                    input.migration = { migration.status, std::move(state.sourceGame), state.sourceGeneration,
                        std::move(state.targetGame), state.targetGeneration };
                }
                continue;
            }
            input.hasOtherRecords = true;
            if (input.packGeneration != 0 && !input.hasIntactCoreV2Front &&
                record.type == kRecordCore && record.version == kLegacyCoreVersion) {
                std::span<const std::uint8_t> bytes = record.bytes;
                Runtime::PersistentState core;
                input.hasIntactCoreV2Front = ReadLegacyCoreV2Front(bytes, core);
            }
        }
        return DecideSaveGeneration(input);
    }

    bool WriteSnapshot(const Snapshot& snapshot, const SaveGeneration& generation, const RecordSink& sink,
        const Report& reportWarning)
    {
        const auto& [state, tracked, trackedItems, savedWorldDrops, savedMapState, savedSpeedMods] = snapshot;
        bool allWritten = true;
        std::size_t emittedBytes = 0;
        const auto emit = [&](RawSaveRecord&& record) {
            const auto bytes = record.bytes.size() + kSaveRecordHeaderBytes;
            if (record.bytes.size() > kMaxSaveRecordBytes || bytes > kMaxCosaveBytes - emittedBytes ||
                !sink(std::move(record))) {
                return false;
            }
            emittedBytes += bytes;
            return true;
        };
        const auto encode = [&](std::uint32_t type, std::uint32_t version, auto writePayload) {
            RecordBuffer buffer{ { type, version, {} } };
            if (writePayload(&buffer) && emit(std::move(buffer.record))) {
                return;
            }
            allWritten = false;
            if (reportWarning) {
                reportWarning(std::format("failed to write record 0x{:08X} v{}.", type, version));
            }
        };
        encode(kRecordCore, kCoreVersion, [&](RecordBuffer* intfc) {
            bool ok = intfc->WriteRecordData(&state.exileLevel, sizeof(state.exileLevel));
            ok = ok && intfc->WriteRecordData(&state.exileXp, sizeof(state.exileXp));
            ok = ok && intfc->WriteRecordData(&state.unallocatedPassivePoints, sizeof(state.unallocatedPassivePoints));
            ok = ok && WriteString(intfc, state.className);
            const std::uint8_t starterByte = state.starterGemsGiven ? 1 : 0;
            ok = ok && intfc->WriteRecordData(&starterByte, sizeof(starterByte));
            ok = ok && intfc->WriteRecordData(&state.nextGemInstanceId, sizeof(state.nextGemInstanceId));
            ok = ok && intfc->WriteRecordData(&state.appliedHealthDelta, sizeof(state.appliedHealthDelta));
            ok = ok && intfc->WriteRecordData(&state.appliedMagickaDelta, sizeof(state.appliedMagickaDelta));

            const auto slotCount = static_cast<std::uint32_t>(state.slots.size());
            ok = ok && intfc->WriteRecordData(&slotCount, sizeof(slotCount));
            for (const auto& slot : state.slots) {
                const std::uint8_t occupiedByte = slot.occupied ? 1 : 0;
                ok = ok && intfc->WriteRecordData(&occupiedByte, sizeof(occupiedByte));
                ok = ok && intfc->WriteRecordData(&slot.linkedAttackSlot, sizeof(slot.linkedAttackSlot));
                if (slot.occupied) {
                    ok = ok && WriteGemInstance(intfc, slot.gem);
                }
            }
            return ok;
        });

        encode(kRecordGemKeys, kGemKeysVersion, [&](RecordBuffer* intfc) {
            // C-4, 재검수 R-3: 상한 초과 시 플레이어 소지품 → 바닥 → 그 밖 순으로 남긴다.
            const auto keep = SelectForSave(tracked, kMaxListLen);
            if (reportWarning && keep.size() < tracked.size()) {
                reportWarning(std::format("SaveLoad: GKEY count {} exceeded cap {}; dropped {} entries not held by the player first.",
                    tracked.size(), kMaxListLen, tracked.size() - keep.size()));
            }
            const auto trackedCount = static_cast<std::uint32_t>(keep.size());
            bool gkeyOk = intfc->WriteRecordData(&trackedCount, sizeof(trackedCount));
            for (std::size_t k = 0; k < keep.size() && gkeyOk; ++k) {
                const auto& entry = tracked[keep[k]];
                const std::uint8_t worldByte = entry.isWorld ? 1 : 0;
                gkeyOk = gkeyOk && intfc->WriteRecordData(&entry.key.ownerFormId, sizeof(entry.key.ownerFormId));
                gkeyOk = gkeyOk && intfc->WriteRecordData(&entry.key.uniqueId, sizeof(entry.key.uniqueId));
                gkeyOk = gkeyOk && intfc->WriteRecordData(&worldByte, sizeof(worldByte));
                gkeyOk = gkeyOk && WriteGemInstance(intfc, entry.gem);
            }
            return gkeyOk;
        });

        encode(kRecordTree, kTreeVersion, [&](RecordBuffer* intfc) {
            const auto& tr = state.tree;
            const std::uint8_t clsSelByte = tr.classSelected ? 1 : 0;
            bool treeOk = intfc->WriteRecordData(&clsSelByte, sizeof(clsSelByte));
            treeOk = treeOk && intfc->WriteRecordData(&tr.classId, sizeof(tr.classId));
            treeOk = treeOk && WriteString(intfc, tr.ascendancyId);

            const auto nodeCount = static_cast<std::uint32_t>(std::min<std::size_t>(tr.allocatedNodes.size(), kMaxTreeNodes));
            treeOk = treeOk && intfc->WriteRecordData(&nodeCount, sizeof(nodeCount));
            for (std::uint32_t i = 0; i < nodeCount && treeOk; ++i) {
                treeOk = treeOk && intfc->WriteRecordData(&tr.allocatedNodes[i], sizeof(std::uint32_t));
            }

            std::vector<std::pair<std::uint32_t, std::uint32_t>> sortedMasteries(
                tr.masterySelections.begin(), tr.masterySelections.end());
            std::sort(sortedMasteries.begin(), sortedMasteries.end());
            const auto masteryCount = static_cast<std::uint32_t>(std::min<std::size_t>(sortedMasteries.size(), kMaxListLen));
            treeOk = treeOk && intfc->WriteRecordData(&masteryCount, sizeof(masteryCount));
            for (std::uint32_t i = 0; i < masteryCount && treeOk; ++i) {
                treeOk = treeOk && intfc->WriteRecordData(&sortedMasteries[i].first, sizeof(std::uint32_t));
                treeOk = treeOk && intfc->WriteRecordData(&sortedMasteries[i].second, sizeof(std::uint32_t));
            }

            treeOk = treeOk && intfc->WriteRecordData(&tr.bossKillCount, sizeof(tr.bossKillCount));
            treeOk = treeOk && intfc->WriteRecordData(&tr.dragonKillCount, sizeof(tr.dragonKillCount));
            treeOk = treeOk && intfc->WriteRecordData(&tr.ascendancyMilestonesBitmask, sizeof(tr.ascendancyMilestonesBitmask));
            return treeOk;
        });

        encode(kRecordItems, kItemsVersion, [&](RecordBuffer* intfc) {
            const std::uint8_t starterCurrByte = state.starterCurrencyGiven ? 1 : 0;
            // C-4, 재검수 R-3: GKEY와 같은 순위로 남긴다(장비 칸 GEAR가 가리키는 플레이어 소지품이 빠지지 않게).
            const auto keep = SelectForSave(trackedItems, kMaxItemRecords);
            if (reportWarning && keep.size() < trackedItems.size()) {
                reportWarning(std::format("SaveLoad: ITEM count {} exceeded cap {}; dropped {} entries not held by the player first.",
                    trackedItems.size(), kMaxItemRecords, trackedItems.size() - keep.size()));
            }
            const auto itemCount = static_cast<std::uint32_t>(keep.size());
            bool itemOk = intfc->WriteRecordData(&starterCurrByte, sizeof(starterCurrByte));
            itemOk = itemOk && intfc->WriteRecordData(&itemCount, sizeof(itemCount));
            for (std::size_t k = 0; k < keep.size() && itemOk; ++k) {
                const auto& entry = trackedItems[keep[k]];
                const std::uint8_t worldByte = entry.isWorld ? 1 : 0;
                itemOk = itemOk && intfc->WriteRecordData(&entry.key.ownerFormId, sizeof(entry.key.ownerFormId));
                itemOk = itemOk && intfc->WriteRecordData(&entry.key.uniqueId, sizeof(entry.key.uniqueId));
                itemOk = itemOk && intfc->WriteRecordData(&worldByte, sizeof(worldByte));
                itemOk = itemOk && WritePoEItemRecordV2(intfc, entry.record);
            }
            return itemOk;
        });

        encode(kRecordWorldDrops, kWorldDropsVersion, [&](RecordBuffer* intfc) {
            const std::size_t startIdx = (savedWorldDrops.size() > kMaxWorldDropRecords)
                ? (savedWorldDrops.size() - kMaxWorldDropRecords) : 0;
            const auto wdrpCount = static_cast<std::uint32_t>(savedWorldDrops.size() - startIdx);
            bool wdrpOk = intfc->WriteRecordData(&wdrpCount, sizeof(wdrpCount));
            for (std::size_t i = startIdx; i < savedWorldDrops.size() && wdrpOk; ++i) {
                const auto& w = savedWorldDrops[i];
                wdrpOk = wdrpOk && intfc->WriteRecordData(&w.dropRefFormId, sizeof(w.dropRefFormId));
                wdrpOk = wdrpOk && intfc->WriteRecordData(&w.corpseRefFormId, sizeof(w.corpseRefFormId));
                wdrpOk = wdrpOk && intfc->WriteRecordData(&w.beamRefFormId, sizeof(w.beamRefFormId));
                wdrpOk = wdrpOk && intfc->WriteRecordData(&w.worldDropTier, sizeof(w.worldDropTier));
            }
            return wdrpOk;
        });

        encode(kRecordSpirit, kSpiritVersion, [&](RecordBuffer* intfc) {
            const std::uint8_t starterS5Byte = state.starterStage5GemsGiven ? 1 : 0;
            bool sprtOk = intfc->WriteRecordData(&state.activeAuraMask, sizeof(state.activeAuraMask));
            sprtOk = sprtOk && intfc->WriteRecordData(&starterS5Byte, sizeof(starterS5Byte));
            return sprtOk;
        });

        encode(kRecordGear, kGearVersion, [&](RecordBuffer* intfc) {
            bool gearOk = intfc->WriteRecordData(&state.virtualRing2Key.ownerFormId, sizeof(state.virtualRing2Key.ownerFormId));
            gearOk = gearOk && intfc->WriteRecordData(&state.virtualRing2Key.uniqueId, sizeof(state.virtualRing2Key.uniqueId));
            gearOk = gearOk && intfc->WriteRecordData(&state.virtualRing2BaseFormId, sizeof(state.virtualRing2BaseFormId));
            gearOk = gearOk && intfc->WriteRecordData(&state.virtualBeltKey.ownerFormId, sizeof(state.virtualBeltKey.ownerFormId));
            gearOk = gearOk && intfc->WriteRecordData(&state.virtualBeltKey.uniqueId, sizeof(state.virtualBeltKey.uniqueId));
            gearOk = gearOk && intfc->WriteRecordData(&state.virtualBeltBaseFormId, sizeof(state.virtualBeltBaseFormId));

            const auto charmCount = static_cast<std::uint32_t>(state.charmSlots.size());
            gearOk = gearOk && intfc->WriteRecordData(&charmCount, sizeof(charmCount));
            for (std::uint32_t c = 0; c < charmCount && gearOk; ++c) {
                const auto& cs = state.charmSlots[c];
                const std::uint8_t occ = cs.occupied ? 1 : 0;
                const auto kindByte = static_cast<std::uint8_t>(cs.kind);
                gearOk = gearOk && intfc->WriteRecordData(&occ, sizeof(occ));
                gearOk = gearOk && intfc->WriteRecordData(&kindByte, sizeof(kindByte));
                gearOk = gearOk && intfc->WriteRecordData(&cs.charges, sizeof(cs.charges));
            }

            std::vector<std::pair<std::uint32_t, Core::PoEItemRecord>> sortedJewels(
                state.socketedJewels.begin(), state.socketedJewels.end());
            std::sort(sortedJewels.begin(), sortedJewels.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            if (reportWarning && sortedJewels.size() > kMaxSocketedJewels) {
                reportWarning(std::format("SaveLoad: socketed jewel count {} exceeded cap {}.", sortedJewels.size(), kMaxSocketedJewels));
            }
            const auto jewelCount = static_cast<std::uint32_t>(std::min<std::size_t>(sortedJewels.size(), kMaxSocketedJewels));
            gearOk = gearOk && intfc->WriteRecordData(&jewelCount, sizeof(jewelCount));
            for (std::uint32_t j = 0; j < jewelCount && gearOk; ++j) {
                gearOk = gearOk && intfc->WriteRecordData(&sortedJewels[j].first, sizeof(std::uint32_t));
                gearOk = gearOk && WritePoEItemRecordV2(intfc, sortedJewels[j].second);
            }
            return gearOk;
        });

        if (!savedSpeedMods.empty()) {
            encode(kRecordSpeedMods, kSpeedModsVersion, [&](RecordBuffer* intfc) {
                const auto spdmCount = static_cast<std::uint32_t>(std::min<std::size_t>(savedSpeedMods.size(), kMaxListLen));
                if (reportWarning && savedSpeedMods.size() > kMaxListLen) {
                    // Codex 지적 13: 상한을 넘은 액터의 원복 근거는 이 세이브에 남지 않는다. 조용히 버리지 않고 알린다.
                    reportWarning(std::format("SaveLoad: SPDM has {} actors over cap {}; {} SpeedMult revert(s) will not be saved.",
                        savedSpeedMods.size(), kMaxListLen, savedSpeedMods.size() - kMaxListLen));
                }
                bool spdmOk = intfc->WriteRecordData(&spdmCount, sizeof(spdmCount));
                for (std::uint32_t i = 0; i < spdmCount && spdmOk; ++i) {
                    spdmOk = spdmOk && intfc->WriteRecordData(&savedSpeedMods[i].first, sizeof(std::uint32_t));
                    spdmOk = spdmOk && intfc->WriteRecordData(&savedSpeedMods[i].second, sizeof(float));
                }
                return spdmOk;
            });
        }

        // ATLS and MAPS are independent records. An inactive MAPS payload contains only active=0.
        encode(kRecordAtlas, kAtlasVersion, [&](RecordBuffer* intfc) {
            return WriteAtlasSave([&](const void* data, std::uint32_t size) {
                return intfc->WriteRecordData(data, size);
            }, state.atlas);
        });
        encode(kRecordMaps, kMapsVersion, [&](RecordBuffer* intfc) {
            return WriteMapRunSave([&](const void* data, std::uint32_t size) {
                return intfc->WriteRecordData(data, size);
            }, savedMapState);
        });

        if (allWritten) {
            allWritten = emit({ kRecordGeneration, kGenerationRecordVersion, WriteGenerationRecord(generation) });
        }
        return allWritten;
    }
}
