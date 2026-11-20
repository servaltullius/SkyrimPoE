#pragma once

#include "core/CommandQueue.h"
#include "core/ItemTypes.h"
#include "core/Types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace SkyrimPoE::Runtime
{
    struct TrackedItemEntry
    {
        Core::InventoryItemKey key;
        bool isWorld = false;
        Core::PoEItemRecord record;
    };

    // 결정 59, 123, 검수 C2/M1:
    // 프로토타입 5 검증 방식으로 PoE 어픽스 아이템을 (소유자 FormID, 고유 번호) 키로 추적한다.
    // - 컨테이너 이동/삭제: TESUniqueIDChangeEvent
    // - 바닥 드랍: 결정 123 예외로 TESUniqueIDChangeEvent 안에서 0xC000..0xFFFD 스탬프 즉시 기록
    // - 바닥 줍기: TESContainerChangedEvent 감지 → CommandQueue 주 갱신에서 충돌 회피 인벤토리 UID 재바인딩
    // - 잠금 규칙: 리프 뮤텍스(g_mutex) 안에서 엔진 함수 호출 금지
    class ItemTracker
    {
    public:
        static bool Install(Core::CommandQueue* queue, std::function<void()> onPlayerEquipChanged = {});

        // 주 갱신 스레드 전용: 대상 아이템에 충돌 없는 ExtraUniqueID를 보장한다.
        // extraList==nullptr 또는 count>1 스택이면 정확히 1개만 분리해 어픽스 복제를 차단한다(검수 C2).
        [[nodiscard]] static Core::InventoryItemKey EnsureUniqueId(
            RE::TESObjectREFR* owner,
            RE::TESBoundObject* baseObj,
            RE::ExtraDataList* extraList,
            RE::ExtraDataList** outFinalExtra = nullptr);

        // 주 갱신 스레드 전용: 신규 ExtraDataList에 임시 UID를 붙여 AddObjectToContainer를 호출한 뒤,
        // 엔진이 동기 TESUniqueIDChangeEvent로 재할당한 실제 최종 UID와 ExtraDataList*를 반환한다.
        [[nodiscard]] static Core::InventoryItemKey AddObjectWithTrackedUniqueId(
            RE::TESObjectREFR* owner,
            RE::TESBoundObject* baseObj,
            RE::ExtraDataList* freshExtra,
            RE::ExtraDataList** outFinalExtra = nullptr);

        // 주 갱신 스레드 전용(계약: g_txMutex 미보유 상태에서만 호출, C-34):
        // owner 인벤토리에 (바닥 참조, 스탬프) 묶음으로 남아 있는 추적 아이템을 owner의 새 인벤토리 번호로 다시 잇는다.
        static std::size_t RelinkPickedUpItems(RE::TESObjectREFR* owner, bool postLoadOwnerNormalized = false);

        // 키 조회·갱신·삭제 (스레드 안전)
        [[nodiscard]] static std::optional<Core::PoEItemRecord> FindByKey(const Core::InventoryItemKey& key);
        static void Upsert(const Core::InventoryItemKey& key, const Core::PoEItemRecord& record, bool isWorld = false);
        static void RemoveByKey(const Core::InventoryItemKey& key);

        // 코세이브 직렬화/복원/초기화 (순수 메모리 작업)
        [[nodiscard]] static std::vector<TrackedItemEntry> TakeSnapshotForSave();
        static void RestoreFromSave(std::vector<TrackedItemEntry> entries);
        static void Clear();
        [[nodiscard]] static std::size_t CountTracked();

        // 소유자 인벤토리에서 (owner, uniqueId)에 해당하는 ExtraDataList 조회(주 갱신 스레드 전용)
        [[nodiscard]] static RE::ExtraDataList* FindExtraListByKey(
            RE::TESObjectREFR* owner,
            RE::TESBoundObject* baseObj,
            const Core::InventoryItemKey& key);
    };
}
