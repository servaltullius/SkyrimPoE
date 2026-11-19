#pragma once

#include "core/ItemData.h"
#include "core/ItemTypes.h"

namespace SkyrimPoE::Core
{
    // 롤링된 어픽스 목록을 isLocal 플래그에 따라 로컬/글로벌 스탯 합산치로 변환한다(검수 m1).
    class ItemAffixParser
    {
    public:
        [[nodiscard]] static ItemStatTotals ParseItem(
            const PoEItemRecord& record,
            const ItemData& data);

        // 단일 어픽스의 스탯을 totals에 누적한다.
        static void ApplyAffix(
            const RolledAffix& rolled,
            const AffixDefinition& def,
            ItemStatTotals& totals);
    };
}
