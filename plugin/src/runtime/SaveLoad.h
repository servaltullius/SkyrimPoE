#pragma once

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/ItemData.h"
#include "core/ItemTypes.h"
#include "core/PassiveTreeData.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <array>
#include <cstdint>
#include <functional>
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
        RE::FormID virtualRing2BaseFormId = 0;
        Core::InventoryItemKey virtualBeltKey{};
        RE::FormID virtualBeltBaseFormId = 0;
        std::array<Core::CharmSlotState, Core::kMaxCharmSlots> charmSlots{};
        std::unordered_map<std::uint32_t, Core::PoEItemRecord> socketedJewels;

        PersistentState()
        {
            for (std::size_t i = 0; i < slots.size(); ++i) {
                slots[i].slotIndex = static_cast<std::uint8_t>(i);
            }
        }
    };

    // SKSE 코세이브('SPOE')와 세이브 전환 생명주기 관리(결정 59, 105~108, 117~119, 138, 150 ④, 161~164).
    // 코세이브 콜백(OnSave/OnLoad/OnRevert)은 순수 메모리 작업만 수행하고, 엔진 상태 반영은 CommandQueue로 넘긴다.
    class SaveLoad
    {
    public:
        static constexpr std::uint32_t kCosaveId = 'SPOE';
        static constexpr std::uint32_t kRecordCore = 'CORE';
        static constexpr std::uint32_t kRecordGemKeys = 'GKEY';
        static constexpr std::uint32_t kRecordTree = 'TREE';
        static constexpr std::uint32_t kRecordItems = 'ITEM';
        static constexpr std::uint32_t kRecordWorldDrops = 'WDRP';
        static constexpr std::uint32_t kRecordSpirit = 'SPRT';
        static constexpr std::uint32_t kRecordGear = 'GEAR';
        static constexpr std::uint32_t kRecordSpeedMods = 'SPDM';
        static constexpr std::uint32_t kCoreVersion = 2;
        static constexpr std::uint32_t kGemKeysVersion = 1;
        static constexpr std::uint32_t kTreeVersion = 1;
        static constexpr std::uint32_t kItemsVersion = 2;
        static constexpr std::uint32_t kWorldDropsVersion = 1;
        static constexpr std::uint32_t kSpiritVersion = 1;
        static constexpr std::uint32_t kGearVersion = 1;
        static constexpr std::uint32_t kSpeedModsVersion = 1;

        static bool Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            const Core::DataPack* dataPack,
            const Core::PassiveTreeData* treeData,
            const Core::Config* config,
            const Core::ItemData* itemData = nullptr,
            Core::CharmSystem* charmSystem = nullptr);

        // kNewGame / kPostLoadGame 시 호출: 주 갱신 스레드에 초기화·Variable09 정리·스탯 빌드를 예약한다.
        static void SchedulePostLoadOrNewGameInit(bool isNewGame);

        [[nodiscard]] static PersistentState GetStateCopy();
        static void SetState(const PersistentState& state);
        static void UpdateState(const std::function<void(PersistentState&)>& mutator);
        // 결정 137(ㅁ): 젬·장비·주얼 거래를 코세이브 저장(OnSave)과 직렬화해 절반만 반영된 상태가 저장되지 않도록 보장한다.
        static void RunSerializedTransaction(const std::function<void()>& txFn);
        static void AddRevertCallback(std::function<void()> cb);
        static void SyncStateToStatEngine();
        static void PersistCharmSlotsFromSystem();

        // 재검수 R-9: 원복해야 할 NPC kTemporary SpeedMult(냉각·빙결·가속)를 그 액터를 찾지 못해 지금 되돌릴 수 없을 때 맡긴다.
        // 맡긴 항목은 세이브(SPDM)에 함께 저장되고, 주 갱신에서 주기적으로 다시 시도한다.
        // 실행 중 만든 FF 액터는 찾지 못하면 이미 지워진 것이고 번호가 다른 액터에게 다시 쓰일 수 있으므로 맡기지 않는다.
        static void DeferSpeedRevert(RE::FormID actorId, float appliedDelta);
        // TESResetEvent(부활·셀 리셋)로 엔진이 임시 수정치를 지운 액터의 맡긴 원복을 버린다(주 갱신 스레드).
        static void ForgetSpeedRevert(RE::FormID actorId);
        // 주 갱신 스레드 전용: 맡긴 원복을 다시 시도한다(PostLoadInit과 주기 틱에서 호출).
        static void RetryPendingSpeedReverts(double deltaSec, bool gamePaused);
    };
}
