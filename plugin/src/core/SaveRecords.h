#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace SkyrimPoE::Core
{
    inline constexpr std::uint32_t kMaxSaveRecordBytes = 16 * 1024 * 1024;
    inline constexpr std::uint32_t kMaxCosaveBytes = 64 * 1024 * 1024;
    inline constexpr std::uint32_t kSaveRecordHeaderBytes = 3 * sizeof(std::uint32_t);

    struct RawSaveRecord
    {
        std::uint32_t type;
        std::uint32_t version;
        std::vector<std::uint8_t> bytes;

        bool operator==(const RawSaveRecord&) const = default;
    };

    struct SaveRecordBatch
    {
        std::vector<RawSaveRecord> records;
        bool complete = true;
    };

    template <class Interface>
    bool WriteSaveRecord(Interface& output, const RawSaveRecord& record)
    {
        return output.OpenRecord(record.type, record.version) &&
            (record.bytes.empty() || output.WriteRecordData(record.bytes.data(),
                static_cast<std::uint32_t>(record.bytes.size())));
    }

    template <class Write>
    bool ReplaySaveRecords(const SaveRecordBatch& batch, Write write)
    {
        bool complete = true;
        for (const auto& record : batch.records) {
            complete = write(record) && complete;
        }
        return complete;
    }

    // The pre-load snapshot survives revert and incoming records until the game reports the outcome.
    class SaveRecordTransition
    {
    public:
        SaveRecordBatch current;
        bool receivedLoad = false;

        [[nodiscard]] bool IsLoading() const noexcept { return loading_; }
        [[nodiscard]] const SaveRecordBatch& RecordsForSave() const noexcept { return loading_ ? beforeLoad_ : current; }
        void BeginLoad(SaveRecordBatch snapshot) noexcept;
        void Revert() noexcept;
        void FinishLoad(bool succeeded) noexcept;

    private:
        SaveRecordBatch beforeLoad_;
        bool loading_ = false;
    };
    // The next-record operation skips any unread bytes in the previous record.
    template <class NextRecord, class Read>
    SaveRecordBatch CollectSaveRecords(NextRecord next, Read read)
    {
        SaveRecordBatch batch;
        try {
            std::uint32_t type = 0;
            std::uint32_t version = 0;
            std::uint32_t length = 0;
            std::uint32_t consumed = 0;
            std::array<std::uint8_t, 64 * 1024> buffer;
            while (next(type, version, length)) {
                if (kSaveRecordHeaderBytes > kMaxCosaveBytes - consumed) {
                    batch.complete = false;
                    break;
                }
                consumed += kSaveRecordHeaderBytes;
                if (length > kMaxSaveRecordBytes) {
                    batch.complete = false;
                    continue;
                }
                if (length > kMaxCosaveBytes - consumed) {
                    batch.complete = false;
                    break;
                }
                consumed += length;

                RawSaveRecord record{ type, version, {} };
                record.bytes.reserve(length);
                while (record.bytes.size() < length) {
                    const auto remaining = length - static_cast<std::uint32_t>(record.bytes.size());
                    const auto requested = std::min(remaining, static_cast<std::uint32_t>(buffer.size()));
                    // SKSE may report the requested length at physical EOF, which this API cannot detect.
                    buffer.fill(0);
                    if (read(buffer.data(), requested) != requested) {
                        break;
                    }
                    record.bytes.insert(record.bytes.end(), buffer.begin(), buffer.begin() + requested);
                }
                if (record.bytes.size() == length) {
                    batch.records.push_back(std::move(record));
                } else {
                    batch.complete = false;
                }
            }
        } catch (...) {
            batch.complete = false;
        }
        return batch;
    }
}
