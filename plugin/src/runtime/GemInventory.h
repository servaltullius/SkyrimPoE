#pragma once

#include "core/SaveRecordState.h"

#include "core/CommandQueue.h"
#include "core/GemTransaction.h"
#include "core/Types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SkyrimPoE::Runtime
{
    struct PlayerGemInventorySnapshot
    {
        struct PlainStack
        {
            std::string gemId;
            std::int32_t count = 0;
        };
        std::vector<TrackedGemEntry> trackedGems;
        std::vector<PlainStack> plainStacks;
    };

    // 결정 59·105~108·123: 데이터 젬의 (소유자 FormID, ExtraUniqueID) 추적과 인벤토리 반환·회수.
    // - TESUniqueIDChangeEvent는 내부 뮤텍스 보호 메모리 갱신만 수행(예외: 버릴 때 바닥 참조에 스탬프 남기기).
    // - TESContainerChangedEvent(바닥 → 컨테이너)는 스탬프를 대조만 하고, 다시 잇기는 주 갱신 큐로 예약한다.
    class GemInventory
    {
    public:
        static bool Install(Core::CommandQueue* queue);

        // 주 갱신 스레드 전용: 전체 28종 젬을 플레이어 인벤토리에 1개씩 순정 지급한다.
        static std::size_t GiveStarterGems(RE::PlayerCharacter* player);

        // 주 갱신 스레드 전용: 기존 4단계 세이브 업그레이드용으로 5단계 신규 19종 젬만 1개씩 지급한다.
        static std::size_t GiveStage5StarterGems(RE::PlayerCharacter* player);

        // 주 갱신 스레드 전용: UI 표시용으로 플레이어 인벤토리의 데이터 젬 + 순정 젬 수량을 조회한다.
        [[nodiscard]] static PlayerGemInventorySnapshot ListPlayerGems(RE::PlayerCharacter* player);

        // 주 갱신 스레드 전용(계약: g_txMutex 미보유 상태에서만 호출):
        // owner 인벤토리에 (바닥 참조, 스탬프) 묶음으로 남아 있는 추적 젬을 owner의 새 인벤토리 번호로 다시 잇는다.
        // 줍기 예약이 실행되지 못한 경우(예약 전 저장·로드, 로드 세대 폐기)의 보완 경로이기도 하다.
        static std::size_t RelinkPickedUpGems(RE::TESObjectREFR* owner, bool postLoadOwnerNormalized = false);

        // 주 갱신 스레드 전용: 탈착·보관된 데이터 젬을 ExtraUniqueID와 함께 플레이어 인벤토리에 넣는다.
        static std::optional<Core::InventoryItemKey> ReturnGemToPlayerInventory(
            RE::PlayerCharacter* player,
            Core::GemInstance gem);

        // 주 갱신 스레드 전용: 플레이어 인벤토리에서 젬 1개를 꺼낸다(추적 젬이면 데이터 복원, 순정이면 신규 인스턴스).
        static std::optional<Core::GemInstance> TakeGemFromPlayerInventory(
            RE::PlayerCharacter* player,
            const Core::InventoryItemKey& key,
            std::string_view fallbackGemId,
            std::uint8_t defaultSockets,
            std::uint64_t& nextInstanceId);

        // 코세이브 직렬화/복원 (메모리 전용, 엔진 호출 없음)
        [[nodiscard]] static std::vector<TrackedGemEntry> TakeSnapshotForSave();
        static void RestoreFromSave(std::vector<TrackedGemEntry> entries);
        static void Clear();
        [[nodiscard]] static std::size_t CountTracked();
    };
}
