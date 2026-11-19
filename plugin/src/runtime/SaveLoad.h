#pragma once

#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/PassiveTreeData.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace SkyrimPoE::Runtime
{
    struct PersistentState
    {
        std::uint32_t exileLevel = 1;
        std::uint64_t exileXp = 0;
        std::uint32_t unallocatedPassivePoints = 0;
        std::string className = "Scion";
        bool starterGemsGiven = false;
        std::uint64_t nextGemInstanceId = 1;
        double appliedHealthDelta = 0.0;   // kPermanent 체력 보정 누적치(세이브 왕복 드리프트 방지)
        double appliedMagickaDelta = 0.0;  // kPermanent 매지카 보정 누적치(세이브 왕복 드리프트 방지)
        std::array<Core::SkillSlot, Core::kSkillSlotCount> slots{};
        Core::PassiveTreeState tree{};

        PersistentState()
        {
            for (std::size_t i = 0; i < slots.size(); ++i) {
                slots[i].slotIndex = static_cast<std::uint8_t>(i);
            }
        }
    };

    // SKSE 코세이브('SPOE')와 세이브 전환 생명주기 관리(결정 59, 105~108, 117~119, 138, 150 ④).
    // 코세이브 콜백(OnSave/OnLoad/OnRevert)은 순수 메모리 작업만 수행하고, 엔진 상태 반영은 CommandQueue로 넘긴다.
    class SaveLoad
    {
    public:
        static constexpr std::uint32_t kCosaveId = 'SPOE';
        static constexpr std::uint32_t kRecordCore = 'CORE';
        static constexpr std::uint32_t kRecordGemKeys = 'GKEY';
        static constexpr std::uint32_t kRecordTree = 'TREE';
        static constexpr std::uint32_t kCoreVersion = 2;
        static constexpr std::uint32_t kGemKeysVersion = 1;
        static constexpr std::uint32_t kTreeVersion = 1;

        static bool Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            const Core::DataPack* dataPack,
            const Core::PassiveTreeData* treeData,
            const Core::Config* config);

        // kNewGame / kPostLoadGame 시 호출: 주 갱신 스레드에 초기화·Variable09 정리·스탯 빌드를 예약한다.
        static void SchedulePostLoadOrNewGameInit(bool isNewGame);

        [[nodiscard]] static PersistentState GetStateCopy();
        static void SetState(const PersistentState& state);
        static void UpdateState(const std::function<void(PersistentState&)>& mutator);
        // 결정 137(ㅁ): 젬 장착/탈착 거래를 코세이브 저장(OnSave)과 직렬화해 절반만 반영된 상태가 저장되지 않도록 보장한다.
        static void RunSerializedTransaction(const std::function<void()>& txFn);
        static void AddRevertCallback(std::function<void()> cb);
        static void SyncStateToStatEngine();
    };
}
