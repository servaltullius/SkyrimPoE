#pragma once

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
    struct TrackedGemEntry
    {
        Core::InventoryItemKey key;
        bool isWorld = false;
        Core::GemInstance gem;
    };

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

    // 결정 59·105~108: 데이터 젬의 (소유자 FormID, ExtraUniqueID) 추적과 인벤토리 반환·회수.
    // TESUniqueIDChangeEvent는 내부 뮤텍스 보호 메모리 갱신만 수행하고, 엔진 변경은 주 갱신 큐로 넘긴다.
    class GemInventory
    {
    public:
        static bool Install(Core::CommandQueue* queue);

        // 주 갱신 스레드 전용: 2단계 9종 젬을 플레이어 인벤토리에 1개씩 순정 지급한다.
        static std::size_t GiveStarterGems(RE::PlayerCharacter* player);

        // 주 갱신 스레드 전용: UI 표시용으로 플레이어 인벤토리의 데이터 젬 + 순정 젬 수량을 조회한다.
        [[nodiscard]] static PlayerGemInventorySnapshot ListPlayerGems(RE::PlayerCharacter* player);

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
