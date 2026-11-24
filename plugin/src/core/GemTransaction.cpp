#include "core/GemTransaction.h"

#include <algorithm>
#include <unordered_set>

namespace SkyrimPoE::Core
{
    namespace
    {
        bool HasDuplicateGemId(const std::vector<GemInstance>& list, std::string_view gemId) noexcept
        {
            for (const auto& g : list) {
                if (g.gemId == gemId) {
                    return true;
                }
            }
            return false;
        }

        std::unordered_set<std::string> CollectAddedSkillTypes(
            const GemInstance& parentGem,
            const DataPack& pack,
            const SkillEffectDefinition* triggerParentDef = nullptr)
        {
            std::unordered_set<std::string> extra;
            if (triggerParentDef) {
                for (const auto& t : triggerParentDef->triggeredAddSkillTypes) {
                    extra.insert(t);
                }
            }
            for (const auto& sub : parentGem.socketedGems) {
                if (const auto* def = pack.FindGem(sub.gemId); def && def->isSupport) {
                    for (const auto& t : def->addSkillTypes) {
                        extra.insert(t);
                    }
                }
            }
            return extra;
        }

        [[nodiscard]] bool IsMainSlotEquipable(GemKind kind) noexcept
        {
            return kind == GemKind::AttackChannel ||
                   kind == GemKind::Trigger ||
                   kind == GemKind::Spell ||
                   kind == GemKind::Aura ||
                   kind == GemKind::Herald ||
                   kind == GemKind::Guard;
        }

        [[nodiscard]] bool SupportsSubSockets(GemKind kind) noexcept
        {
            return kind == GemKind::AttackChannel ||
                   kind == GemKind::Trigger ||
                   kind == GemKind::Spell ||
                   kind == GemKind::Guard;
        }

        void UnlinkTriggersPointingTo(std::array<SkillSlot, kSkillSlotCount>& slots, std::uint8_t attackSlotIndex) noexcept
        {
            for (auto& slot : slots) {
                if (slot.occupied && slot.linkedAttackSlot == static_cast<std::int8_t>(attackSlotIndex)) {
                    slot.linkedAttackSlot = -1;
                }
            }
        }

        void ClearSkillSlot(std::array<SkillSlot, kSkillSlotCount>& slots, std::uint8_t slotIndex)
        {
            UnlinkTriggersPointingTo(slots, slotIndex);
            auto& slot = slots[slotIndex];
            slot.occupied = false;
            slot.gem = GemInstance{};
            slot.linkedAttackSlot = -1;
        }

        bool ReturnGemTree(GemInstance& gem, const std::function<bool(const GemInstance&)>& returnOrDiscard)
        {
            for (auto it = gem.socketedGems.begin(); it != gem.socketedGems.end();) {
                if (ReturnGemTree(*it, returnOrDiscard)) {
                    it = gem.socketedGems.erase(it);
                } else {
                    ++it;
                }
            }
            return gem.socketedGems.empty() && returnOrDiscard(gem);
        }
    }

    GemTransactionResult GemTransaction::EquipSkillGem(
        std::array<SkillSlot, kSkillSlotCount>& slots,
        std::uint64_t& nextInstanceId,
        std::uint8_t slotIndex,
        const InventoryItemKey& itemKey,
        std::string_view gemId,
        std::uint8_t defaultSockets,
        const DataPack& pack,
        IGemInventoryAdapter& inventory)
    {
        if (slotIndex >= kSkillSlotCount) {
            return { false, "잘못된 스킬 칸 번호입니다.", std::nullopt };
        }
        const auto* def = pack.FindGem(std::string(gemId));
        if (!def) {
            return { false, "알 수 없는 젬입니다.", std::nullopt };
        }
        if (!IsMainSlotEquipable(def->kind)) {
            return { false, "보조 젬은 스킬 젬 소켓에만 장착할 수 있습니다.", std::nullopt };
        }

        // 1. 인벤토리에서 새 젬 1개 회수 시도(실패하면 기존 칸을 전혀 건드리지 않음)
        const std::uint8_t sockets = SupportsSubSockets(def->kind)
            ? static_cast<std::uint8_t>(std::min<std::uint32_t>(defaultSockets, kMaxGemSockets))
            : static_cast<std::uint8_t>(0);
        auto taken = inventory.TakeGem(itemKey, gemId, sockets, nextInstanceId);
        if (!taken) {
            return { false, "인벤토리에 해당 젬이 남아 있지 않습니다.", std::nullopt };
        }

        // 회수된 데이터 젬의 실제 ID와 종류를 다시 검증(위조/불일치 요청 방어)
        const auto* takenDef = pack.FindGem(taken->gemId);
        if (!takenDef || taken->gemId != def->id || !IsMainSlotEquipable(takenDef->kind)) {
            (void)inventory.ReturnGem(std::move(*taken));
            return { false, "인벤토리 젬 정보가 요청과 일치하지 않거나 장착할 수 없는 종류입니다.", std::nullopt };
        }
        if (!SupportsSubSockets(takenDef->kind)) {
            taken->socketCount = 0;
        }

        // 2. 기존 칸이 차 있으면 기존 젬을 인벤토리로 반환(반환 실패 시 새 젬을 되돌리고 롤백)
        auto& slot = slots[slotIndex];
        std::optional<InventoryItemKey> returnedKey;
        if (slot.occupied) {
            GemInstance oldGem = std::move(slot.gem);
            returnedKey = inventory.ReturnGem(oldGem);
            if (!returnedKey) {
                slot.gem = std::move(oldGem);
                (void)inventory.ReturnGem(std::move(*taken));
                return { false, "기존 젬을 인벤토리로 반환하지 못해 장착을 취소했습니다.", std::nullopt };
            }
            UnlinkTriggersPointingTo(slots, slotIndex);
        }

        slot.slotIndex = slotIndex;
        slot.occupied = true;
        slot.gem = std::move(*taken);
        slot.linkedAttackSlot = -1;
        return { true, "스킬 젬을 장착했습니다.", returnedKey };
    }

    GemTransactionResult GemTransaction::UnequipSkillGem(
        std::array<SkillSlot, kSkillSlotCount>& slots,
        std::uint8_t slotIndex,
        IGemInventoryAdapter& inventory)
    {
        if (slotIndex >= kSkillSlotCount) {
            return { false, "잘못된 스킬 칸 번호입니다.", std::nullopt };
        }
        auto& slot = slots[slotIndex];
        if (!slot.occupied) {
            return { false, "비어 있는 스킬 칸입니다.", std::nullopt };
        }

        // 결정 106, 107, 145: 장착된 보조 젬·발동 안 주문/보조를 그대로 보존해 인벤토리로 반환
        GemInstance copyForReturn = slot.gem;
        auto key = inventory.ReturnGem(std::move(copyForReturn));
        if (!key) {
            return { false, "인벤토리로 젬을 반환하지 못했습니다.", std::nullopt };
        }

        ClearSkillSlot(slots, slotIndex);
        return { true, "스킬 젬을 해제해 인벤토리에 보관했습니다.", key };
    }

    void GemTransaction::ReturnSlottedGemsForUninstall(
        std::array<SkillSlot, kSkillSlotCount>& slots, const std::function<bool(const GemInstance&)>& returnOrDiscard)
    {
        for (std::uint8_t index = 0; index < slots.size(); ++index) {
            auto& slot = slots[index];
            if (slot.occupied && ReturnGemTree(slot.gem, returnOrDiscard)) {
                ClearSkillSlot(slots, index);
            }
        }
    }

    GemTransactionResult GemTransaction::SocketSubGem(
        std::array<SkillSlot, kSkillSlotCount>& slots,
        std::uint64_t& nextInstanceId,
        std::uint8_t slotIndex,
        std::int32_t triggeredSpellIndex,
        const InventoryItemKey& itemKey,
        std::string_view gemId,
        std::uint8_t defaultSockets,
        const DataPack& pack,
        IGemInventoryAdapter& inventory)
    {
        if (slotIndex >= kSkillSlotCount || !slots[slotIndex].occupied) {
            return { false, "대상 스킬 칸이 비어 있습니다.", std::nullopt };
        }

        auto& rootGem = slots[slotIndex].gem;
        const auto* rootDef = pack.FindGem(rootGem.gemId);
        if (!rootDef) {
            return { false, "대상 스킬 젬 정보를 찾지 못했습니다.", std::nullopt };
        }

        GemInstance* targetParent = nullptr;
        const SkillEffectDefinition* targetParentDef = nullptr;
        bool expectingSpell = false;
        const SkillEffectDefinition* triggerParentDef = nullptr;

        if (triggeredSpellIndex < 0) {
            if (!SupportsSubSockets(rootDef->kind)) {
                return { false, "오라·전령 젬에는 보조 소켓을 장착할 수 없습니다.", std::nullopt };
            }
            targetParent = &rootGem;
            targetParentDef = rootDef;
            expectingSpell = (rootDef->kind == GemKind::Trigger);
        } else {
            if (rootDef->kind != GemKind::Trigger ||
                static_cast<std::size_t>(triggeredSpellIndex) >= rootGem.socketedGems.size()) {
                return { false, "발동 젬 안의 주문 젬을 찾지 못했습니다.", std::nullopt };
            }
            targetParent = &rootGem.socketedGems[static_cast<std::size_t>(triggeredSpellIndex)];
            targetParentDef = pack.FindGem(targetParent->gemId);
            if (!targetParentDef) {
                return { false, "주문 젬 정보를 찾지 못했습니다.", std::nullopt };
            }
            expectingSpell = false;
            triggerParentDef = rootDef;
        }

        if (targetParent->socketedGems.size() >= targetParent->socketCount) {
            return { false, "남은 보조 소켓이 없습니다.", std::nullopt };
        }

        const auto* subDef = pack.FindGem(std::string(gemId));
        if (!subDef) {
            return { false, "알 수 없는 젬입니다.", std::nullopt };
        }

        if (expectingSpell) {
            if (!pack.CanTriggerSocketSpell(targetParentDef->id, subDef->id)) {
                return { false, "이 발동 젬에 담을 수 없는 주문 젬입니다.", std::nullopt };
            }
        } else {
            if (subDef->kind != GemKind::Support) {
                return { false, "이 소켓에는 보조 젬만 장착할 수 있습니다.", std::nullopt };
            }
            if (HasDuplicateGemId(targetParent->socketedGems, subDef->id)) {
                return { false, "같은 종류의 보조 젬은 하나만 장착할 수 있습니다.", std::nullopt };
            }
            const auto extraTypes = CollectAddedSkillTypes(*targetParent, pack, triggerParentDef);
            if (!pack.CanSupportApply(subDef->id, targetParentDef->id, extraTypes.empty() ? nullptr : &extraTypes)) {
                return { false, "대상 스킬과 호환되지 않는 보조 젬입니다.", std::nullopt };
            }
        }

        // 인벤토리에서 1개 회수(주문 젬이면 defaultSockets, 보조 젬이면 0소켓)
        const std::uint8_t childSockets = expectingSpell
            ? static_cast<std::uint8_t>(std::min<std::uint32_t>(defaultSockets, kMaxGemSockets))
            : 0;
        auto taken = inventory.TakeGem(itemKey, gemId, childSockets, nextInstanceId);
        if (!taken) {
            return { false, "인벤토리에 해당 젬이 남아 있지 않습니다.", std::nullopt };
        }

        // 회수된 데이터 젬 ID 재검증(불일치 시 롤백)
        if (taken->gemId != subDef->id) {
            (void)inventory.ReturnGem(std::move(*taken));
            return { false, "인벤토리 젬 정보가 요청과 일치하지 않아 취소했습니다.", std::nullopt };
        }

        targetParent->socketedGems.push_back(std::move(*taken));
        return { true, expectingSpell ? "발동 주문을 장착했습니다." : "보조 젬을 장착했습니다.", std::nullopt };
    }

    GemTransactionResult GemTransaction::UnsocketSubGem(
        std::array<SkillSlot, kSkillSlotCount>& slots,
        std::uint8_t slotIndex,
        std::int32_t triggeredSpellIndex,
        std::size_t socketIndex,
        IGemInventoryAdapter& inventory)
    {
        if (slotIndex >= kSkillSlotCount || !slots[slotIndex].occupied) {
            return { false, "대상 스킬 칸이 비어 있습니다.", std::nullopt };
        }

        auto& rootGem = slots[slotIndex].gem;
        GemInstance* targetParent = nullptr;
        if (triggeredSpellIndex < 0) {
            targetParent = &rootGem;
        } else {
            if (static_cast<std::size_t>(triggeredSpellIndex) >= rootGem.socketedGems.size()) {
                return { false, "발동 젬 안의 주문 젬을 찾지 못했습니다.", std::nullopt };
            }
            targetParent = &rootGem.socketedGems[static_cast<std::size_t>(triggeredSpellIndex)];
        }

        if (socketIndex >= targetParent->socketedGems.size()) {
            return { false, "잘못된 소켓 번호입니다.", std::nullopt };
        }

        GemInstance copyForReturn = targetParent->socketedGems[socketIndex];
        auto key = inventory.ReturnGem(std::move(copyForReturn));
        if (!key) {
            return { false, "인벤토리로 젬을 반환하지 못했습니다.", std::nullopt };
        }

        targetParent->socketedGems.erase(
            targetParent->socketedGems.begin() + static_cast<std::ptrdiff_t>(socketIndex));
        return { true, "소켓 젬을 해제해 인벤토리에 보관했습니다.", key };
    }

    GemTransactionResult GemTransaction::LinkTriggerAttack(
        std::array<SkillSlot, kSkillSlotCount>& slots,
        std::uint8_t triggerSlotIndex,
        std::int8_t attackSlotIndex,
        const DataPack& pack)
    {
        if (triggerSlotIndex >= kSkillSlotCount || !slots[triggerSlotIndex].occupied) {
            return { false, "발동 젬 칸이 비어 있습니다.", std::nullopt };
        }
        auto& trigSlot = slots[triggerSlotIndex];
        const auto* trigDef = pack.FindGem(trigSlot.gem.gemId);
        if (!trigDef || trigDef->kind != GemKind::Trigger) {
            return { false, "발동 젬이 아닌 칸입니다.", std::nullopt };
        }

        if (attackSlotIndex < 0) {
            trigSlot.linkedAttackSlot = -1;
            return { true, "공격 연결을 해제했습니다.", std::nullopt };
        }

        const auto atkIdx = static_cast<std::size_t>(attackSlotIndex);
        if (atkIdx >= kSkillSlotCount || atkIdx == triggerSlotIndex || !slots[atkIdx].occupied) {
            return { false, "연결할 공격 스킬 칸이 유효하지 않습니다.", std::nullopt };
        }
        if (!pack.CanTriggerLinkAttack(trigSlot.gem.gemId, slots[atkIdx].gem.gemId)) {
            return { false, "이 발동 젬과 호환되지 않는 공격 스킬입니다.", std::nullopt };
        }

        // 결정 131: 공격 1칸에는 발동 젬 1개만 연결 허용
        for (std::size_t i = 0; i < kSkillSlotCount; ++i) {
            if (i != triggerSlotIndex && slots[i].occupied && slots[i].linkedAttackSlot == attackSlotIndex) {
                return { false, "해당 공격 칸에는 이미 다른 발동 젬이 연결되어 있습니다.", std::nullopt };
            }
        }

        trigSlot.linkedAttackSlot = attackSlotIndex;
        return { true, "발동 젬을 공격 칸에 연결했습니다.", std::nullopt };
    }
}
