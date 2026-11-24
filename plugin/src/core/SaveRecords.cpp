#include "core/SaveRecords.h"

namespace SkyrimPoE::Core
{
    void SaveRecordTransition::BeginLoad(SaveRecordBatch snapshot) noexcept
    {
        beforeLoad_ = std::move(snapshot);
        receivedLoad = false;
        loading_ = true;
    }

    void SaveRecordTransition::Revert() noexcept
    {
        current = {};
        receivedLoad = false;
    }

    void SaveRecordTransition::FinishLoad(bool succeeded) noexcept
    {
        if (loading_ && !succeeded) {
            current = std::move(beforeLoad_);
            receivedLoad = false;
        }
        beforeLoad_ = {};
        loading_ = false;
    }
}
