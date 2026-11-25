#include "core/SaveRecordFormat.h"
#include "core/Migration.h"
#include "core/SaveCapPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>

namespace SkyrimPoE::Core::SaveRecord
{
    namespace
    {
        // Existing TREE v2 starts the class field with a bounded string length.
        constexpr auto kTreeClassSelectionMarker = std::numeric_limits<std::uint32_t>::max();

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

        bool ReadTreeGeneration(std::uint32_t version, std::span<const std::uint8_t>& bytes, std::uint32_t& generation)
        {
            if (version == kLegacyTreeVersion) {
                generation = 0;
                return true;
            }
            return version == kTreeVersion && ReadPod(bytes, generation);
        }
    }

    bool ReadTreeRecord(const RawSaveRecord& record, PassiveTreeState& out)
    {
        if (record.type != kRecordTree || record.bytes.size() > kMaxSaveRecordBytes) {
            return false;
        }
        std::span<const std::uint8_t> bytes = record.bytes;
        PassiveTreeState tree;
        if (!ReadTreeGeneration(record.version, bytes, tree.generation)) return false;
        if (record.version == kLegacyTreeVersion) {
            std::uint8_t selected = 0;
            if (!ReadPod(bytes, selected) || !ReadPod(bytes, tree.classId)) return false;
            tree.classSelected = selected != 0;
        } else {
            auto classField = bytes;
            std::uint32_t marker = 0;
            if (!ReadPod(classField, marker)) return false;
            if (marker == kTreeClassSelectionMarker) {
                std::uint8_t selected = 0;
                bytes = classField;
                if (!ReadPod(bytes, selected) || selected > 1 || !ReadString(bytes, tree.classContentId)) return false;
                tree.classSelected = selected != 0;
                const bool hasContentId = !tree.classContentId.empty();
                if (tree.classSelected != hasContentId) return false;
            } else {
                if (!ReadString(bytes, tree.classContentId)) return false;
                tree.classSelected = !tree.classContentId.empty();
            }
        }
        if (!ReadString(bytes, tree.ascendancyId)) return false;

        std::uint32_t nodeCount = 0;
        if (!ReadPod(bytes, nodeCount) || nodeCount > kMaxTreeNodes) return false;
        tree.allocatedNodes.resize(nodeCount);
        for (auto& node : tree.allocatedNodes) {
            if (!ReadPod(bytes, node)) return false;
        }
        const auto readChoices = [&](auto& choices, std::uint32_t cap, bool rejectDuplicates) {
            std::uint32_t count = 0;
            if (!ReadPod(bytes, count) || count > cap) return false;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t node = 0, choice = 0;
                if (!ReadPod(bytes, node) || !ReadPod(bytes, choice)) return false;
                auto [entry, inserted] = choices.try_emplace(node, choice);
                if (!inserted) {
                    if (rejectDuplicates) return false;
                    entry->second = choice;
                }
            }
            return true;
        };
        if (record.version == kTreeVersion && !readChoices(tree.attributeSelections, kMaxTreeNodes, true)) return false;
        if (!readChoices(tree.masterySelections, kMaxListLen, false) ||
            !ReadPod(bytes, tree.bossKillCount) || !ReadPod(bytes, tree.dragonKillCount) ||
            !ReadPod(bytes, tree.ascendancyMilestonesBitmask) || !bytes.empty()) {
            return false;
        }
        tree.ascendancyMilestonesBitmask &= 0xFu;
        out = std::move(tree);
        return true;
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

    namespace
    {
        bool ReadCorePayload(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& tmp, bool strict)
        {
            if (!ReadLegacyCoreV2Front(bytes, tmp) || !ReadLegacyCoreV2Slots(bytes, tmp)) {
                return false;
            }
            const bool finiteHealth = std::isfinite(tmp.appliedHealthDelta);
            const bool finiteMagicka = std::isfinite(tmp.appliedMagickaDelta);
            if (strict && (!bytes.empty() || !finiteHealth || !finiteMagicka)) {
                return false;
            }

            tmp.exileLevel = std::clamp(tmp.exileLevel, 1u, kMaxExileLevel);
            tmp.nextGemInstanceId = std::max<std::uint64_t>(tmp.nextGemInstanceId, 1);
            if (!finiteHealth) tmp.appliedHealthDelta = 0.0;
            if (!finiteMagicka) tmp.appliedMagickaDelta = 0.0;
            return true;
        }
    }

    bool ReadCoreV2(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& tmp)
    {
        return ReadCorePayload(bytes, tmp, false);
    }

    bool ReadCoreRecord(const RawSaveRecord& record, Runtime::PersistentState& out)
    {
        if (record.type != kRecordCore ||
            (record.version != kLegacyCoreVersion && record.version != kCoreVersion)) {
            return false;
        }
        std::span<const std::uint8_t> bytes = record.bytes;
        Runtime::PersistentState state;
        if (!ReadCorePayload(bytes, state, record.version == kCoreVersion)) {
            return false;
        }
        out = std::move(state);
        return true;
    }

    bool ReadGearHeader(std::uint32_t version, std::span<const std::uint8_t>& bytes, GearHeader& out)
    {
        if (version != kLegacyGearVersion && version != kGearVersion) {
            return false;
        }
        GearHeader gear;
        if (!ReadPod(bytes, gear.ring2Key.ownerFormId) || !ReadPod(bytes, gear.ring2Key.uniqueId) ||
            !ReadPod(bytes, gear.ring2BaseFormId) || !ReadPod(bytes, gear.beltKey.ownerFormId) ||
            !ReadPod(bytes, gear.beltKey.uniqueId) || !ReadPod(bytes, gear.beltBaseFormId) ||
            !ReadPod(bytes, gear.charmCount) || gear.charmCount > gear.charms.size()) {
            return false;
        }
        for (std::uint32_t index = 0; index < gear.charmCount; ++index) {
            auto& charm = gear.charms[index];
            if (!ReadPod(bytes, charm.occupied) || !ReadPod(bytes, charm.kind) || !ReadPod(bytes, charm.charges)) {
                return false;
            }
        }
        if (!ReadPod(bytes, gear.jewelCount) || gear.jewelCount > kMaxSocketedJewels ||
            (version == kGearVersion && !ReadPod(bytes, gear.jewelGeneration))) {
            return false;
        }
        out = gear;
        return true;
    }

    bool AreValidPlayerRateDeltas(PlayerRateDeltas deltas) noexcept
    {
        const auto valid = [](double value) {
            return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
        };
        return valid(deltas.speedMult) && valid(deltas.magickaRateMult);
    }

    std::optional<PlayerRateDeltas> ReadPlayerRateRecord(const RawSaveRecord& record) noexcept
    {
        if (record.type != kRecordPlayerRates || record.version != kPlayerRatesVersion) {
            return std::nullopt;
        }
        std::span<const std::uint8_t> bytes = record.bytes;
        PlayerRateDeltas deltas;
        if (!ReadPod(bytes, deltas.speedMult) || !ReadPod(bytes, deltas.magickaRateMult) ||
            !bytes.empty() || !AreValidPlayerRateDeltas(deltas)) {
            return std::nullopt;
        }
        return deltas;
    }

    std::optional<PlayerRateDeltas> ReadPlayerRates(const SaveRecordBatch& batch) noexcept
    {
        const auto record = std::ranges::find(batch.records, kRecordPlayerRates, &RawSaveRecord::type);
        if (record == batch.records.end()) {
            return PlayerRateDeltas{};
        }
        if (std::find_if(std::next(record), batch.records.end(), [](const auto& next) {
            return next.type == kRecordPlayerRates;
        }) != batch.records.end()) {
            return std::nullopt;
        }
        return ReadPlayerRateRecord(*record);
    }

    float ComputePlayerRateDelta(double observed, double applied, double multiplier) noexcept
    {
        if (!std::isfinite(observed) || !std::isfinite(applied) || !std::isfinite(multiplier)) {
            return 0.0f;
        }
        const double baseline = std::max(0.0, observed - applied);
        const double change = baseline * (std::max(0.0, multiplier) - 1.0) - applied;
        return std::abs(change) <= std::numeric_limits<float>::max() ? static_cast<float>(change) : 0.0f;
    }

    PlayerRateReconciliation ReconcilePlayerRates(double speedMult, double magickaRateMult,
        PlayerRateDeltas applied, double speedFactor, double magickaRateFactor) noexcept
    {
        constexpr double tolerance = 0.0001;
        const auto change = [](double observed, double previous, double factor) {
            const float next = ComputePlayerRateDelta(observed, previous, factor);
            return std::abs(next) > tolerance ? next : 0.0f;
        };
        const float speed = change(speedMult, applied.speedMult, speedFactor);
        const float magicka = change(magickaRateMult, applied.magickaRateMult, magickaRateFactor);
        return { speed, magicka, { applied.speedMult + speed, applied.magickaRateMult + magicka } };
    }

    SaveDecision DecideSave(const SaveRecordBatch& batch, bool isNewGame, const SaveGeneration& pack,
        std::optional<std::uint32_t> activeTreeGeneration)
    {
        if (!batch.complete) {
            return { SaveDisposition::Preserve, "모드 기록 수집이 불완전합니다." };
        }
        if (!isNewGame && !ReadPlayerRates(batch)) {
            return { SaveDisposition::Preserve, "PRAT 레코드의 플레이어 속도·매지카 재생 보정치를 복원할 수 없습니다." };
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
        if (!isNewGame && input.generation.status == GenerationRecordStatus::Missing &&
            std::ranges::any_of(batch.records, [](const auto& record) {
                return record.type == kRecordCore && record.version > kLegacyCoreVersion;
            })) {
            return { SaveDisposition::Preserve, "세대 기록(GENR)이 없는 CORE v3 이상 세이브입니다." };
        }
        const auto decision = DecideSaveGeneration(input);
        if (isNewGame) {
            return decision;
        }
        const bool normal = decision.disposition == SaveDisposition::Normal;
        const bool legacyCleanup = decision.disposition == SaveDisposition::CleanupFromLegacy;
        const bool needsGenerationRecords = normal && pack.contentGeneration != 0;
        if (needsGenerationRecords) {
            if (std::ranges::find(batch.records, kRecordPlayerRates, &RawSaveRecord::type) == batch.records.end()) {
                return { SaveDisposition::Preserve, "세대 1 이상 세이브에 필수 PRAT 레코드가 없습니다." };
            }
            const auto core = std::ranges::find(batch.records, kRecordCore, &RawSaveRecord::type);
            Runtime::PersistentState decoded;
            if (core == batch.records.end() || core->version != kCoreVersion ||
                std::ranges::find_if(std::next(core), batch.records.end(), [](const auto& record) {
                    return record.type == kRecordCore;
                }) != batch.records.end() || !ReadCoreRecord(*core, decoded)) {
                return { SaveDisposition::Preserve, "세대 1 이상 세이브에 온전한 CORE v3 레코드가 하나 있어야 합니다." };
            }
        }
        if (normal || legacyCleanup) {
            const auto expected = legacyCleanup ? 0u : activeTreeGeneration.value_or(pack.contentGeneration);
            bool sawTree = false;
            for (const auto& record : batch.records) {
                if (record.type == kRecordTree) {
                    PassiveTreeState tree;
                    if (sawTree || (needsGenerationRecords && record.version != kTreeVersion) ||
                        !ReadTreeRecord(record, tree)) {
                        return { SaveDisposition::Preserve, "TREE 레코드가 중복되었거나 온전히 읽을 수 없습니다." };
                    }
                    if (tree.generation != expected) {
                        return { SaveDisposition::Preserve, legacyCleanup
                            ? "PoE1 정리 대상의 TREE 레코드가 세대 0이 아닙니다."
                            : "TREE 레코드와 활성 패시브 트리 팩의 세대가 다릅니다." };
                    }
                    sawTree = true;
                } else if (record.type == kRecordGear) {
                    std::span<const std::uint8_t> bytes = record.bytes;
                    GearHeader gear;
                    if (ReadGearHeader(record.version, bytes, gear) && gear.jewelCount != 0 &&
                        gear.jewelGeneration != expected) {
                        return { SaveDisposition::Preserve, "GEAR 소켓 주얼과 패시브 트리의 세대가 다릅니다." };
                    }
                } else if (!needsGenerationRecords && record.type == kRecordCore &&
                    record.version >= kCoreVersion) {
                    return { SaveDisposition::Preserve, "세대 0 세이브에 세대 1 이상의 CORE 레코드가 있습니다." };
                }
            }
            if (needsGenerationRecords && !sawTree) {
                return { SaveDisposition::Preserve, "세대 1 이상 세이브에 필수 TREE v2 레코드가 없습니다." };
            }
        }
        return decision;
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
        const auto coreVersion = generation.contentGeneration == 0 ? kLegacyCoreVersion : kCoreVersion;
        encode(kRecordCore, coreVersion, [&](RecordBuffer* intfc) {
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
        encode(kRecordPlayerRates, kPlayerRatesVersion, [&](RecordBuffer* intfc) {
            const PlayerRateDeltas deltas{ state.appliedSpeedMultDelta, state.appliedMagickaRateMultDelta };
            return AreValidPlayerRateDeltas(deltas) &&
                intfc->WriteRecordData(&deltas.speedMult, sizeof(deltas.speedMult)) &&
                intfc->WriteRecordData(&deltas.magickaRateMult, sizeof(deltas.magickaRateMult));
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
            if (tr.classSelected && tr.classContentId.empty() && reportWarning) {
                reportWarning("SaveLoad: selected class has no content ID; saving TREE with no class selected.");
            }
            const std::uint8_t selected = tr.classSelected && !tr.classContentId.empty() ? 1 : 0;
            const auto ascendancyId = tr.ascendancyId.empty() || tr.ascendancyId.starts_with("asc.")
                ? tr.ascendancyId : "asc." + tr.ascendancyId;
            bool treeOk = intfc->WriteRecordData(&tr.generation, sizeof(tr.generation));
            treeOk = treeOk && intfc->WriteRecordData(&kTreeClassSelectionMarker, sizeof(kTreeClassSelectionMarker));
            treeOk = treeOk && intfc->WriteRecordData(&selected, sizeof(selected));
            treeOk = treeOk && WriteString(intfc, selected ? tr.classContentId : std::string{});
            treeOk = treeOk && WriteString(intfc, ascendancyId);

            const auto nodeCount = static_cast<std::uint32_t>(std::min<std::size_t>(tr.allocatedNodes.size(), kMaxTreeNodes));
            treeOk = treeOk && intfc->WriteRecordData(&nodeCount, sizeof(nodeCount));
            for (std::uint32_t i = 0; i < nodeCount && treeOk; ++i) {
                treeOk = treeOk && intfc->WriteRecordData(&tr.allocatedNodes[i], sizeof(std::uint32_t));
            }

            const auto writeChoices = [&](const auto& choices, std::uint32_t cap) {
                std::vector<std::pair<std::uint32_t, std::uint32_t>> sorted(choices.begin(), choices.end());
                std::sort(sorted.begin(), sorted.end());
                const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(sorted.size(), cap));
                if (!intfc->WriteRecordData(&count, sizeof(count))) return false;
                for (std::uint32_t i = 0; i < count; ++i) {
                    if (!intfc->WriteRecordData(&sorted[i].first, sizeof(std::uint32_t)) ||
                        !intfc->WriteRecordData(&sorted[i].second, sizeof(std::uint32_t))) return false;
                }
                return true;
            };
            treeOk = treeOk && writeChoices(tr.attributeSelections, kMaxTreeNodes);
            treeOk = treeOk && writeChoices(tr.masterySelections, kMaxListLen);

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
            const auto jewelGeneration = state.tree.generation;
            gearOk = gearOk && intfc->WriteRecordData(&jewelGeneration, sizeof(jewelGeneration));
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
