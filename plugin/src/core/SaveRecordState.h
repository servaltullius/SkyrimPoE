#pragma once

#include "core/CharmSystem.h"
#include "core/ItemTypes.h"
#include "core/MapTypes.h"
#include "core/PassiveTreeData.h"
#include "core/Types.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace SkyrimPoE::Runtime
{
    struct PersistentState
    {
        std::uint32_t exileLevel = 1;
        std::uint64_t exileXp = 0;
        std::uint32_t unallocatedPassivePoints = 0;
        std::string className = "Scion";
        bool starterGemsGiven = false;
        bool starterCurrencyGiven = false;  // 4단계: 'ITEM' 헤더에 저장(CORE v2 하위 호환 유지)
        bool starterStage5GemsGiven = false; // 5단계: 'SPRT' v1에 저장(4단계 세이브 업그레이드용)
        std::uint64_t nextGemInstanceId = 1;
        double appliedHealthDelta = 0.0;   // kPermanent 체력 보정 누적치(세이브 왕복 드리프트 방지)
        double appliedMagickaDelta = 0.0;  // kPermanent 매지카 보정 누적치(세이브 왕복 드리프트 방지)
        std::array<Core::SkillSlot, Core::kSkillSlotCount> slots{};
        Core::PassiveTreeState tree{};

        // 5단계(결정 161~164): 정신력 오라 토글, 논리 10칸 가상 반지2/벨트, 벨트 참, 패시브 주얼 소켓
        std::uint16_t activeAuraMask = 0;
        Core::InventoryItemKey virtualRing2Key{};
        std::uint32_t virtualRing2BaseFormId = 0;
        Core::InventoryItemKey virtualBeltKey{};
        std::uint32_t virtualBeltBaseFormId = 0;
        std::array<Core::CharmSlotState, Core::kMaxCharmSlots> charmSlots{};
        std::unordered_map<std::uint32_t, Core::PoEItemRecord> socketedJewels;

        // 6단계(결정 166): 아틀라스 영구 진행 상태(T1~T16 첫 클리어 +16 패시브, 첫 공물 지급)
        Core::AtlasState atlas{};

        PersistentState()
        {
            for (std::size_t i = 0; i < slots.size(); ++i) {
                slots[i].slotIndex = static_cast<std::uint8_t>(i);
            }
        }
    };

    struct TrackedGemEntry
    {
        Core::InventoryItemKey key;
        bool isWorld = false;
        Core::GemInstance gem;
        // C-35: TESUniqueIDChangeEvent 싱크에서 폼 조회 없이 event->objectID를 대조하기 위한 런타임 베이스 폼 ID 캐시
        std::uint32_t baseFormId = 0;
    };

    struct TrackedItemEntry
    {
        Core::InventoryItemKey key;
        bool isWorld = false;
        Core::PoEItemRecord record;
    };

    struct SavedWorldDropEntry
    {
        std::uint32_t dropRefFormId = 0;
        std::uint32_t corpseRefFormId = 0;
        std::uint32_t beamRefFormId = 0;
        std::uint8_t worldDropTier = 2;
    };
}
