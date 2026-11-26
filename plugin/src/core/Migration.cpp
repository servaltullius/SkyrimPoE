#include "core/Migration.h"
#include "core/SaveRecordFormat.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace SkyrimPoE::Core
{
    namespace
    {
        using namespace SaveRecord;

        static_assert(std::endian::native == std::endian::little);
        constexpr std::uint8_t kMaxJewelColor = 2;
        constexpr std::uint32_t kMinReturnBytes = 9 + sizeof(ReturnEntry::instanceId);
        // GKEY can hold 1024 trees, each with several children. The record byte cap bounds the flattened list.
        constexpr auto kMaxReturns = kMaxSaveRecordBytes / kMinReturnBytes;

        struct Reader
        {
            std::span<const std::uint8_t> bytes;

            bool Skip(std::size_t size)
            {
                if (size > bytes.size()) {
                    return false;
                }
                bytes = bytes.subspan(size);
                return true;
            }

            bool Read(void* data, std::size_t size)
            {
                if (size > bytes.size()) {
                    return false;
                }
                if (size != 0) {
                    std::memcpy(data, bytes.data(), size);
                    bytes = bytes.subspan(size);
                }
                return true;
            }

            template <class T>
            bool Pod(T& value) { return Read(&value, sizeof(value)); }

            bool Flag(bool& value)
            {
                std::uint8_t byte = 0;
                if (!Pod(byte) || byte > 1) {
                    return false;
                }
                value = byte != 0;
                return true;
            }

            template <class E>
            bool Enum(E& value, E last)
            {
                std::uint8_t byte = 0;
                if (!Pod(byte) || byte > static_cast<std::uint8_t>(last)) {
                    return false;
                }
                value = static_cast<E>(byte);
                return true;
            }

            bool String(std::string& value)
            {
                return ReadSaveString([this](void* data, std::uint32_t size) { return Read(data, size); }, value);
            }

            bool Blob(std::vector<std::uint8_t>& value)
            {
                std::uint32_t size = 0;
                if (!Pod(size) || size > bytes.size()) {
                    return false;
                }
                value.assign(bytes.begin(), bytes.begin() + size);
                bytes = bytes.subspan(size);
                return true;
            }

            template <class T, class ReadEntry>
            bool List(std::vector<T>& entries, std::uint32_t limit, std::size_t minEntryBytes, ReadEntry readEntry)
            {
                std::uint32_t count = 0;
                if (!Pod(count) || count > limit || count > bytes.size() / minEntryBytes) {
                    return false;
                }
                entries.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    if (!readEntry(entries.emplace_back())) {
                        return false;
                    }
                }
                return true;
            }
        };

        struct Writer
        {
            std::vector<std::uint8_t> bytes;

            template <class It>
            void Append(It first, std::size_t size)
            {
                if (size > kMaxSaveRecordBytes - bytes.size()) {
                    throw std::length_error("MIGR exceeds the record byte limit");
                }
                bytes.insert(bytes.end(), first, first + size);
            }

            template <class T>
            void Pod(T value)
            {
                const auto data = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
                Append(data.begin(), data.size());
            }

            template <class T>
            void Byte(T value) { Pod(static_cast<std::uint8_t>(value)); }

            void Size(std::size_t size, std::uint32_t limit)
            {
                if (size > limit) {
                    throw std::length_error("MIGR field exceeds its size limit");
                }
                Pod(static_cast<std::uint32_t>(size));
            }

            void String(const std::string& value)
            {
                Size(value.size(), kMaxSaveStringBytes);
                Append(value.begin(), value.size());
            }

            template <class T, class WriteEntry>
            void List(const std::vector<T>& entries, std::uint32_t limit, WriteEntry writeEntry)
            {
                Size(entries.size(), limit);
                for (const auto& entry : entries) {
                    writeEntry(entry);
                }
            }
        };

        void WriteReturn(Writer& writer, const ReturnEntry& entry)
        {
            writer.Byte(entry.kind);
            switch (entry.kind) {
            case ReturnKind::SlottedGem:
            case ReturnKind::BagSupport:
                writer.String(entry.gemId);
                break;
            case ReturnKind::Jewel:
                writer.Pod(entry.baseFormId);
                writer.Pod(entry.jewelColor);
                break;
            case ReturnKind::Charm:
                writer.Byte(entry.charmKind);
                break;
            }
            writer.Pod(entry.instanceId);
            writer.Pod(entry.recipientFormId);
            writer.Pod(entry.depth);
            writer.Byte(entry.status);
            writer.Pod(entry.attempts);
        }

        bool ReadReturn(Reader& reader, ReturnEntry& entry)
        {
            if (!reader.Enum(entry.kind, ReturnKind::Charm)) {
                return false;
            }
            switch (entry.kind) {
            case ReturnKind::SlottedGem:
            case ReturnKind::BagSupport:
                if (!reader.String(entry.gemId)) return false;
                break;
            case ReturnKind::Jewel:
                if (!reader.Pod(entry.baseFormId) || !reader.Pod(entry.jewelColor) || entry.jewelColor > kMaxJewelColor) {
                    return false;
                }
                break;
            case ReturnKind::Charm:
                if (!reader.Enum(entry.charmKind, CharmKind::Bastion)) return false;
                break;
            }
            return reader.Pod(entry.instanceId) && reader.Pod(entry.recipientFormId) &&
                reader.Pod(entry.depth) && entry.depth <= kMaxGemDepth &&
                reader.Enum(entry.status, ReturnStatus::Lost) && reader.Pod(entry.attempts) &&
                entry.attempts <= kMaxReturnAttempts;
        }

        void CountLoss(MigrationState& state)
        {
            if (state.lostCount < std::numeric_limits<std::uint32_t>::max()) {
                ++state.lostCount;
            }
        }

        bool ResolveForm(std::uint32_t& id, const FormIdResolver& resolver)
        {
            if (id == 0 || !resolver) {
                return true;
            }
            std::uint32_t resolved = 0;
            const bool ok = resolver(id, resolved) && resolved != 0;
            id = ok ? resolved : 0;
            return ok;
        }

        std::uint32_t ResolveMigrationForms(MigrationState& state, const FormIdResolver& resolver, bool forRuntimeCleanup = false)
        {
            std::uint32_t unresolved = 0;
            for (auto& entry : state.returns) {
                const bool recipientOk = forRuntimeCleanup || ResolveForm(entry.recipientFormId, resolver);
                const bool baseOk = ResolveForm(entry.baseFormId, resolver);
                unresolved += !recipientOk || !baseOk;
                if (entry.recipientFormId == 0 && entry.status == ReturnStatus::Pending) {
                    entry.status = ReturnStatus::Lost;
                    CountLoss(state);
                }
            }
            for (auto& refund : state.refunds) {
                if (!ResolveForm(refund.baseFormId, resolver)) {
                    ++unresolved;
                    if (!refund.done) CountLoss(state);
                }
            }
            for (auto& ref : state.virtualRefs) {
                const bool ownerOk = ResolveForm(ref.key.ownerFormId, resolver);
                const bool baseOk = ResolveForm(ref.baseFormId, resolver);
                unresolved += !ownerOk || !baseOk;
            }
            std::erase_if(state.speedMods, [&](MigrationSpeedMod& speedMod) {
                if (ResolveForm(speedMod.actorFormId, resolver)) {
                    return false;
                }
                ++unresolved;
                CountLoss(state);
                return true;
            });
            for (auto& drop : state.worldDrops) {
                const bool dropOk = ResolveForm(drop.dropRefFormId, resolver);
                const bool corpseOk = ResolveForm(drop.corpseRefFormId, resolver);
                const bool beamOk = ResolveForm(drop.beamRefFormId, resolver);
                unresolved += !dropOk || !corpseOk || !beamOk;
                // Leave a surviving beam for cleanup even if the corpse/drop was lost.
                if (!forRuntimeCleanup && (!dropOk || !corpseOk) &&
                    (drop.status == WorldDropStatus::Pending || drop.status == WorldDropStatus::Retry)) {
                    drop.status = WorldDropStatus::CorpseGone;
                    CountLoss(state);
                }
            }
            return unresolved;
        }

        void AppendGemTree(const GemInstance& gem, ReturnKind kind, std::uint32_t recipient,
            std::uint8_t depth, std::vector<ReturnEntry>& entries)
        {
            // ReadGemInstance already enforces kMaxGemDepth, including for GKEY.
            for (const auto& child : gem.socketedGems) {
                AppendGemTree(child, kind, recipient, depth + 1, entries);
            }
            entries.push_back({ .kind = kind, .gemId = gem.gemId, .instanceId = gem.instanceId,
                .recipientFormId = recipient, .depth = depth });
        }

        void SortDeepestFirst(std::vector<ReturnEntry>& entries)
        {
            std::ranges::stable_sort(entries, std::greater{}, &ReturnEntry::depth);
        }

        bool ReadLegacyGemKeys(Reader& reader, std::vector<ReturnEntry>& entries, std::uint32_t& count)
        {
            if (!reader.Pod(count) || count > kMaxListLen) {
                return false;
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                InventoryItemKey key;
                bool isWorld = false;
                GemInstance gem;
                if (!reader.Pod(key.ownerFormId) || !reader.Pod(key.uniqueId) || !reader.Flag(isWorld) ||
                    !ReadGemInstance(reader.bytes, gem)) {
                    return false;
                }
                for (const auto& child : gem.socketedGems) {
                    AppendGemTree(child, ReturnKind::BagSupport, key.ownerFormId, 1, entries);
                }
            }
            return reader.bytes.empty();
        }

        bool ReadLegacyItem(Reader& reader, std::uint32_t version, std::uint32_t& baseFormId, std::uint8_t& jewelColor)
        {
            const auto read = [&](void* data, std::uint32_t size) { return reader.Read(data, size); };
            std::string weaponTag, uniqueId;
            std::vector<RolledAffix> affixes;
            if (!reader.Pod(baseFormId) || !reader.Skip(sizeof(std::uint32_t) + 4 * sizeof(std::uint8_t)) ||
                !reader.String(weaponTag) || !ReadSaveAffixList(read, affixes)) {
                return false;
            }
            return version == 1 ||
                (reader.String(uniqueId) && reader.Pod(jewelColor) && ReadSaveAffixList(read, affixes));
        }

        bool ReadLegacyItems(Reader& reader, std::uint32_t version, std::uint32_t& count)
        {
            if ((version != 1 && version != kItemsVersion) || !reader.Skip(1) || !reader.Pod(count) || count > kMaxItemRecords) {
                return false;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t baseFormId = 0;
                std::uint8_t jewelColor = 0;
                if (!reader.Skip(sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint8_t)) ||
                    !ReadLegacyItem(reader, version, baseFormId, jewelColor)) {
                    return false;
                }
            }
            return reader.bytes.empty();
        }

        bool ReadLegacyGear(Reader& reader, std::uint32_t version, std::vector<ReturnEntry>& entries,
            std::vector<MigrationVirtualRef>& refs)
        {
            GearHeader gear;
            if (!ReadGearHeader(version, reader.bytes, gear) || (gear.jewelCount != 0 && gear.jewelGeneration != 0)) {
                return false;
            }
            for (const auto& ref : { MigrationVirtualRef{ VirtualSlot::Ring2, gear.ring2Key, gear.ring2BaseFormId },
                     MigrationVirtualRef{ VirtualSlot::Belt, gear.beltKey, gear.beltBaseFormId } }) {
                if (ref.baseFormId != 0 || ref.key.ownerFormId != 0 || ref.key.uniqueId != 0) {
                    refs.push_back(ref);
                }
            }

            for (std::uint32_t index = 0; index < gear.charmCount; ++index) {
                const auto& charm = gear.charms[index];
                if (charm.occupied > 1 || charm.kind > static_cast<std::uint8_t>(CharmKind::Bastion)) {
                    return false;
                }
                if (charm.occupied != 0) {
                    entries.push_back({ .kind = ReturnKind::Charm, .gemId = {}, .charmKind = static_cast<CharmKind>(charm.kind),
                        .recipientFormId = kPlayerRefFormId });
                }
            }
            for (std::uint32_t index = 0; index < gear.jewelCount; ++index) {
                ReturnEntry entry{ .kind = ReturnKind::Jewel, .gemId = {}, .recipientFormId = kPlayerRefFormId };
                if (!reader.Skip(sizeof(std::uint32_t)) ||
                    !ReadLegacyItem(reader, kGearItemVersion, entry.baseFormId, entry.jewelColor) || entry.jewelColor > kMaxJewelColor) {
                    return false;
                }
                entries.push_back(std::move(entry));
            }
            return reader.bytes.empty();
        }

        std::optional<bool> ReadLegacyMapActive(const RawSaveRecord& record)
        {
            constexpr auto legacyAtlasSize = sizeof(AtlasState::completedTierMask) + sizeof(AtlasState::totalMapsCleared) +
                sizeof(AtlasState::highestTierCleared) + sizeof(std::uint8_t);
            const bool legacy = record.version == kLegacyIndexMapSaveVersion || record.version == kLegacyKeyMapSaveVersion;
            std::size_t activeOffset = 0;
            if (legacy) {
                activeOffset = legacyAtlasSize;
            } else if (record.version != kMapsVersion) {
                return std::nullopt;
            }
            if (activeOffset >= record.bytes.size() || (!legacy && record.bytes[activeOffset] > 1)) {
                return std::nullopt;
            }
            return record.bytes[activeOffset] != 0;
        }

        bool ReadLegacyMapRun(const RawSaveRecord& record, MigrationMapRun& map)
        {
            const auto active = ReadLegacyMapActive(record);
            if (!active) {
                return false;
            }
            map = { *active, record.version, record.bytes };
            return true;
        }

        template <class Decode>
        LegacyRecordStatus ReadLegacyRecord(const SaveRecordBatch& batch, std::uint32_t type,
            std::uint32_t version, Decode decode)
        {
            const auto record = std::ranges::find(batch.records, type, &RawSaveRecord::type);
            if (record == batch.records.end()) {
                return LegacyRecordStatus::Missing;
            }
            if ((version != 0 && record->version != version) || record->bytes.size() > kMaxSaveRecordBytes ||
                std::find_if(std::next(record), batch.records.end(),
                    [type](const auto& other) { return other.type == type; }) != batch.records.end()) {
                return LegacyRecordStatus::Damaged;
            }
            return decode(*record) ? LegacyRecordStatus::Valid : LegacyRecordStatus::Damaged;
        }

        bool HasMapWork(const MigrationState& state)
        {
            return (state.mapRun.active && !state.mapRunClosed) ||
                std::ranges::any_of(state.refunds, [](const auto& refund) { return refund.baseFormId != 0 && !refund.done; });
        }

        bool HasStepSixWork(const MigrationState& state)
        {
            return !state.speedMods.empty() || std::ranges::any_of(state.worldDrops, [](const auto& drop) {
                return drop.status == WorldDropStatus::Pending || drop.status == WorldDropStatus::Retry ||
                    (drop.status == WorldDropStatus::CorpseGone && drop.beamRefFormId != 0);
            });
        }

        bool IsReturnForStep(CleanupStep step, const ReturnEntry& entry)
        {
            switch (step) {
            case CleanupStep::ReturnSlottedGems: return entry.kind == ReturnKind::SlottedGem;
            case CleanupStep::DetachBagSupports: return entry.kind == ReturnKind::BagSupport;
            case CleanupStep::ReturnJewelsAndCharms: return entry.kind == ReturnKind::Jewel || entry.kind == ReturnKind::Charm;
            default: return false;
            }
        }

        StepOutcome AdvanceReturns(MigrationState& state, CleanupOps& ops, StepOutcome (CleanupOps::*operation)(MigrationState&))
        {
            const auto pending = [&](const ReturnEntry& entry) {
                return entry.status == ReturnStatus::Pending && IsReturnForStep(state.step, entry);
            };
            const auto refsPending = [&]() {
                return state.step == CleanupStep::ReturnJewelsAndCharms &&
                    std::ranges::any_of(state.virtualRefs, [](const auto& ref) { return !ref.released; });
            };
            if (!std::ranges::any_of(state.returns, pending) && !refsPending()) {
                return StepOutcome::Done;
            }
            const auto outcome = (ops.*operation)(state);
            if (outcome != StepOutcome::Retry) {
                return outcome;
            }
            bool retry = refsPending();
            for (auto& entry : state.returns) {
                if (pending(entry)) {
                    if (entry.attempts < kMaxReturnAttempts) {
                        ++entry.attempts;
                    }
                    if (entry.attempts == kMaxReturnAttempts) {
                        entry.status = ReturnStatus::Lost;
                        CountLoss(state);
                    } else {
                        retry = true;
                    }
                }
            }
            return retry ? StepOutcome::Retry : StepOutcome::Done;
        }
    }

    std::vector<std::uint8_t> WriteMigrationRecord(const MigrationState& state)
    {
        if (!SaveRecord::AreValidPlayerRateDeltas({ state.speedMultDelta, state.magickaRateMultDelta })) {
            throw std::length_error("Invalid player rate deltas in MIGR");
        }
        Writer writer;
        writer.String(state.sourceGame);
        writer.Pod(state.sourceGeneration);
        writer.String(state.targetGame);
        writer.Pod(state.targetGeneration);
        writer.Byte(state.step);
        writer.Pod(state.healthDelta);
        writer.Pod(state.magickaDelta);
        writer.Byte(state.deltasReverted);
        writer.Byte(state.mapRun.active);
        writer.Pod(state.mapRun.version);
        writer.Size(state.mapRun.bytes.size(), kMaxSaveRecordBytes);
        writer.Append(state.mapRun.bytes.begin(), state.mapRun.bytes.size());
        writer.Byte(state.mapRunClosed);
        writer.List(state.refunds, kMaxListLen, [&](const MigrationRefund& refund) {
            writer.Byte(refund.kind);
            writer.Pod(refund.baseFormId);
            writer.Pod(refund.amount);
            writer.Byte(refund.done);
        });
        writer.List(state.returns, kMaxReturns, [&](const ReturnEntry& entry) { WriteReturn(writer, entry); });
        writer.List(state.virtualRefs, 2, [&](const MigrationVirtualRef& ref) {
            writer.Byte(ref.slot);
            writer.Pod(ref.key.ownerFormId);
            writer.Pod(ref.key.uniqueId);
            writer.Pod(ref.baseFormId);
            writer.Byte(ref.released);
        });
        writer.List(state.speedMods, kMaxListLen, [&](const MigrationSpeedMod& speedMod) {
            writer.Pod(speedMod.actorFormId);
            writer.Pod(speedMod.delta);
        });
        writer.List(state.worldDrops, kMaxWorldDropRecords, [&](const MigrationWorldDrop& drop) {
            writer.Pod(drop.dropRefFormId);
            writer.Pod(drop.corpseRefFormId);
            writer.Pod(drop.beamRefFormId);
            writer.Pod(drop.worldDropTier);
            writer.Byte(drop.status);
        });
        writer.Byte(state.starterGranted);
        writer.Pod(state.lostCount);
        writer.Pod(state.speedMultDelta);
        writer.Pod(state.magickaRateMultDelta);
        writer.Pod(SaveRecordCrc32(writer.bytes));
        return std::move(writer.bytes);
    }

    MigrationReadResult ReadMigrationRecord(
        std::uint32_t version, std::span<const std::uint8_t> bytes, const FormIdResolver& resolver)
    {
        constexpr auto crcSize = sizeof(std::uint32_t);
        if ((version != kPreviousMigrationVersion && version != kMigrationVersion) ||
            bytes.size() < crcSize || bytes.size() > kMaxSaveRecordBytes) {
            return {};
        }
        Reader reader{ bytes.first(bytes.size() - crcSize) };
        Reader checksum{ bytes.last(crcSize) };
        std::uint32_t expected = 0;
        if (!checksum.Pod(expected) || expected != SaveRecordCrc32(reader.bytes)) {
            return {};
        }

        MigrationState state;
        if (!reader.String(state.sourceGame) || state.sourceGame.empty() || !reader.Pod(state.sourceGeneration) ||
            !reader.String(state.targetGame) || state.targetGame.empty() || !reader.Pod(state.targetGeneration) ||
            !reader.Enum(state.step, CleanupStep::Complete) || !reader.Pod(state.healthDelta) ||
            !reader.Pod(state.magickaDelta) || !reader.Flag(state.deltasReverted) ||
            !reader.Flag(state.mapRun.active) || !reader.Pod(state.mapRun.version) ||
            !reader.Blob(state.mapRun.bytes) || !reader.Flag(state.mapRunClosed)) {
            return {};
        }
        const bool listsOk =
            reader.List(state.refunds, kMaxListLen, 10, [&](MigrationRefund& refund) {
                return reader.Enum(refund.kind, RefundKind::Tribute) && reader.Pod(refund.baseFormId) &&
                    reader.Pod(refund.amount) && reader.Flag(refund.done);
            }) &&
            reader.List(state.returns, kMaxReturns, kMinReturnBytes, [&](ReturnEntry& entry) {
                return ReadReturn(reader, entry);
            }) &&
            reader.List(state.virtualRefs, 2, 12, [&](MigrationVirtualRef& ref) {
                return reader.Enum(ref.slot, VirtualSlot::Belt) && reader.Pod(ref.key.ownerFormId) &&
                    reader.Pod(ref.key.uniqueId) && reader.Pod(ref.baseFormId) && reader.Flag(ref.released);
            }) &&
            reader.List(state.speedMods, kMaxListLen, 12, [&](MigrationSpeedMod& speedMod) {
                return reader.Pod(speedMod.actorFormId) && reader.Pod(speedMod.delta);
            }) &&
            reader.List(state.worldDrops, kMaxWorldDropRecords, 14, [&](MigrationWorldDrop& drop) {
                return reader.Pod(drop.dropRefFormId) && reader.Pod(drop.corpseRefFormId) &&
                    reader.Pod(drop.beamRefFormId) && reader.Pod(drop.worldDropTier) &&
                    reader.Enum(drop.status, WorldDropStatus::CorpseGone);
            });
        if (!listsOk || !reader.Flag(state.starterGranted) || !reader.Pod(state.lostCount)) {
            return {};
        }
        if (version == kMigrationVersion &&
            (!reader.Pod(state.speedMultDelta) || !reader.Pod(state.magickaRateMultDelta) ||
             !SaveRecord::AreValidPlayerRateDeltas({ state.speedMultDelta, state.magickaRateMultDelta }))) {
            return {};
        }
        if (!reader.bytes.empty()) {
            return {};
        }
        const auto unresolved = ResolveMigrationForms(state, resolver);
        return { MigrationRecordStatus::Valid, std::move(state), unresolved };
    }

    LegacyMigrationResult BuildLegacyMigration(const SaveRecordBatch& batch, std::string_view targetGame,
        std::uint32_t targetGeneration, const FormIdResolver& resolver, LegacyReturnDestination destination)
    {
        LegacyMigrationResult result;
        auto& [state, report] = result;
        Runtime::PersistentState core;
        std::span<const std::uint8_t> slots;
        const auto coreCount = std::ranges::count(batch.records, kRecordCore, &RawSaveRecord::type);
        report.coreFront = ReadLegacyRecord(batch, kRecordCore, kLegacyCoreVersion, [&](const auto& record) {
            slots = record.bytes;
            return ReadLegacyCoreV2Front(slots, core);
        });
        if (!report.Succeeded()) {
            if (coreCount > 1) {
                report.failureReason = "CORE 레코드가 중복됩니다.";
            } else {
                report.failureReason = report.coreFront == LegacyRecordStatus::Missing
                    ? "CORE v2 기록이 없습니다." : "CORE v2 앞부분이 손상되었거나 지원하지 않는 판입니다.";
            }
            return result;
        }

        state.sourceGame = "poe1";
        state.targetGame = targetGame;
        state.targetGeneration = targetGeneration;
        state.step = CleanupStep::CloseMapRun;
        state.healthDelta = core.appliedHealthDelta;
        state.magickaDelta = core.appliedMagickaDelta;
        report.playerRates = ReadLegacyRecord(batch, kRecordPlayerRates, kPlayerRatesVersion, [&](const auto& record) {
            const auto deltas = ReadPlayerRateRecord(record);
            if (!deltas) return false;
            state.speedMultDelta = deltas->speedMult;
            state.magickaRateMultDelta = deltas->magickaRateMult;
            return true;
        });
        if (!report.Succeeded()) {
            report.failureReason = "PRAT 기록이 중복되었거나 플레이어 속도·매지카 재생 보정치가 손상되었습니다.";
            return result;
        }
        report.slotsDamaged = !ReadLegacyCoreV2Slots(slots, core) || !slots.empty();
        if (!report.slotsDamaged) {
            for (const auto& slot : core.slots) {
                if (slot.occupied) {
                    AppendGemTree(slot.gem, ReturnKind::SlottedGem, kPlayerRefFormId, 0, state.returns);
                }
            }
            SortDeepestFirst(state.returns);
        }
        report.gemKeys = ReadLegacyRecord(batch, kRecordGemKeys, kLegacyGemKeysVersion, [&](const auto& record) {
            Reader reader{ record.bytes };
            std::vector<ReturnEntry> entries;
            if (!ReadLegacyGemKeys(reader, entries, report.gemKeyCount)) return false;
            SortDeepestFirst(entries);
            std::ranges::move(entries, std::back_inserter(state.returns));
            return true;
        });
        // CORE precedes GKEY so a gem shared by both is kept with its slotted entry.
        std::unordered_set<std::uint64_t> returnedGemIds;
        report.duplicateReturns = static_cast<std::uint32_t>(std::erase_if(state.returns, [&](const auto& entry) {
            return entry.instanceId != 0 && !returnedGemIds.insert(entry.instanceId).second;
        }));
        report.tree = ReadLegacyRecord(batch, kRecordTree, 0, [&](const auto& record) {
            PassiveTreeState tree;
            if (!ReadTreeRecord(record, tree) || tree.generation != 0) {
                return false;
            }
            report.treeNodeCount = static_cast<std::uint32_t>(tree.allocatedNodes.size());
            return true;
        });
        report.items = ReadLegacyRecord(batch, kRecordItems, 0, [&](const auto& record) {
            Reader reader{ record.bytes };
            return ReadLegacyItems(reader, record.version, report.itemCount);
        });
        // SPRT stores the aura mask and starter flag. GEAR stores the actual charm slots.
        report.spirit = ReadLegacyRecord(batch, kRecordSpirit, kSpiritVersion, [&](const auto& record) {
            Reader reader{ record.bytes };
            std::uint16_t auraMask = 0;
            bool starter = false;
            if (!reader.Pod(auraMask) || !reader.Flag(starter) || !reader.bytes.empty()) {
                return false;
            }
            report.auraCount = std::popcount(auraMask);
            return true;
        });
        report.gear = ReadLegacyRecord(batch, kRecordGear, 0, [&](const auto& record) {
            Reader reader{ record.bytes };
            std::vector<ReturnEntry> entries;
            std::vector<MigrationVirtualRef> refs;
            if (!ReadLegacyGear(reader, record.version, entries, refs)) return false;
            std::ranges::move(entries, std::back_inserter(state.returns));
            state.virtualRefs = std::move(refs);
            return true;
        });
        report.maps = ReadLegacyRecord(batch, kRecordMaps, 0, [&](const auto& record) {
            return ReadLegacyMapRun(record, state.mapRun);
        });
        if (report.maps == LegacyRecordStatus::Damaged) {
            state.mapRun.active = std::ranges::any_of(batch.records, [](const auto& record) {
                return record.type == kRecordMaps && ReadLegacyMapActive(record).value_or(false);
            });
        }
        report.atlas = ReadLegacyRecord(batch, kRecordAtlas, kAtlasVersion, [&](const auto& record) {
            Reader reader{ record.bytes };
            const auto atlas = ReadAtlasSave(record.version, record.bytes.size(),
                [&](void* out, std::size_t size) { return reader.Read(out, size); });
            if (!atlas) {
                return false;
            }
            report.atlasClears = atlas->totalMapsCleared;
            return true;
        });
        report.speedMods = ReadLegacyRecord(batch, kRecordSpeedMods, kSpeedModsVersion, [&](const auto& record) {
            Reader reader{ record.bytes };
            std::vector<MigrationSpeedMod> entries;
            if (!reader.List(entries, kMaxListLen, 8, [&](MigrationSpeedMod& speedMod) {
                float delta = 0.0f;
                if (!reader.Pod(speedMod.actorFormId) || !reader.Pod(delta)) return false;
                speedMod.delta = delta;
                return true;
            }) || !reader.bytes.empty()) {
                return false;
            }
            report.speedModCount = static_cast<std::uint32_t>(entries.size());
            std::erase_if(entries, [](const auto& entry) {
                return entry.actorFormId == 0 || !std::isfinite(entry.delta) || std::abs(entry.delta) < 0.01f;
            });
            state.speedMods = std::move(entries);
            return true;
        });
        report.worldDrops = ReadLegacyRecord(batch, kRecordWorldDrops, kWorldDropsVersion, [&](const auto& record) {
            Reader reader{ record.bytes };
            std::vector<MigrationWorldDrop> entries;
            if (!reader.List(entries, kMaxWorldDropRecords, 13, [&](MigrationWorldDrop& drop) {
                return reader.Pod(drop.dropRefFormId) && reader.Pod(drop.corpseRefFormId) &&
                    reader.Pod(drop.beamRefFormId) && reader.Pod(drop.worldDropTier);
            }) || !reader.bytes.empty()) {
                return false;
            }
            state.worldDrops = std::move(entries);
            return true;
        });
        if (destination == LegacyReturnDestination::Player) {
            for (auto& entry : state.returns) {
                entry.recipientFormId = kPlayerRefFormId;
            }
        }
        const auto speedModsBeforeResolve = static_cast<std::uint32_t>(state.speedMods.size());
        report.unresolvedCount = ResolveMigrationForms(state, resolver, destination == LegacyReturnDestination::Player);
        report.speedModsUnresolved = speedModsBeforeResolve - static_cast<std::uint32_t>(state.speedMods.size());
        report.speedModsFiltered = report.speedModCount - static_cast<std::uint32_t>(state.speedMods.size());
        return result;
    }

    DeltaTrust JudgeLegacyDelta(double delta, double modifier) noexcept
    {
        if (!std::isfinite(delta)) {
            return DeltaTrust::Untrusted;
        }
        if (delta == 0.0) {
            return DeltaTrust::Trusted;
        }
        if (std::isfinite(modifier) && modifier != 0.0 &&
            std::signbit(delta) == std::signbit(modifier) &&
            std::abs(delta) <= std::abs(modifier) + kLegacyDeltaTolerance) {
            return DeltaTrust::Trusted;
        }
        return DeltaTrust::Untrusted;
    }

    std::string_view CheckLegacyActorDeltas(const MigrationState& state, double permanentHealth, double permanentMagicka,
        double temporarySpeedMult, double temporaryMagickaRateMult) noexcept
    {
        if (JudgeLegacyDelta(state.healthDelta, permanentHealth) != DeltaTrust::Trusted ||
            JudgeLegacyDelta(state.magickaDelta, permanentMagicka) != DeltaTrust::Trusted) {
            return "옛 체력·매지카 보정값을 확인할 수 없어 정리하지 않았습니다.";
        }
        if (JudgeLegacyDelta(state.speedMultDelta, temporarySpeedMult) != DeltaTrust::Trusted ||
            JudgeLegacyDelta(state.magickaRateMultDelta, temporaryMagickaRateMult) != DeltaTrust::Trusted) {
            return "옛 이동 속도·매지카 재생 보정값을 확인할 수 없어 정리하지 않았습니다.";
        }
        return {};
    }

    StepOutcome AdvanceCleanup(MigrationState& state, CleanupOps& ops)
    {
        using enum CleanupStep;
        auto outcome = StepOutcome::Done;
        switch (state.step) {
        case Begin:
            break;
        case CloseMapRun:
            if (HasMapWork(state)) {
                outcome = ops.CloseMapRun(state);
                if (outcome == StepOutcome::Retry && !HasMapWork(state)) outcome = StepOutcome::Done;
            }
            break;
        case ReturnSlottedGems:
            outcome = AdvanceReturns(state, ops, &CleanupOps::ReturnSlottedGems);
            break;
        case DetachBagSupports:
            outcome = AdvanceReturns(state, ops, &CleanupOps::DetachBagSupports);
            break;
        case ReturnJewelsAndCharms:
            outcome = AdvanceReturns(state, ops, &CleanupOps::ReturnJewelsAndCharms);
            break;
        case RevertEngineState:
            outcome = ops.RevertEngineState(state);
            break;
        case SpeedModsAndWorldDrops:
            if (HasStepSixWork(state)) {
                outcome = ops.SpeedModsAndWorldDrops(state);
                if (outcome == StepOutcome::Retry && !HasStepSixWork(state)) outcome = StepOutcome::Done;
            }
            break;
        case ClearCosave:
            outcome = ops.ClearCosave(state);
            break;
        case GrantStarter:
            outcome = ops.GrantStarter(state);
            break;
        case Finish:
            outcome = ops.Finish(state);
            break;
        case Complete:
            return StepOutcome::Done;
        }
        if (outcome == StepOutcome::Done) {
            state.step = static_cast<CleanupStep>(static_cast<std::uint8_t>(state.step) + 1);
        }
        return outcome;
    }

    StepOutcome CompleteCleanup(MigrationState& state, CleanupOps& ops)
    {
        while (state.step != CleanupStep::Complete) {
            const auto outcome = AdvanceCleanup(state, ops);
            if (outcome != StepOutcome::Done) {
                return outcome;
            }
        }
        return StepOutcome::Done;
    }
}
