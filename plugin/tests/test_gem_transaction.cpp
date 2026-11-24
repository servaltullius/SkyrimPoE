#include "test_harness.h"

#include "core/DataPack.h"
#include "core/GemTransaction.h"

#include <filesystem>
#include <unordered_map>

using namespace SkyrimPoE::Core;

namespace
{
    DataPack LoadStage2Pack()
    {
        DataPack pack;
        std::string err;
        const std::filesystem::path packPath = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "core_3_30.json";
        if (!pack.LoadFromFile(packPath, &err)) {
            throw std::runtime_error("Failed to load core_3_30.json: " + err);
        }
        return pack;
    }

    class MockGemInventory final : public IGemInventoryAdapter
    {
    public:
        std::unordered_map<std::string, int> plainCounts;
        std::unordered_map<std::uint16_t, GemInstance> trackedByUid;
        std::uint16_t nextUid = 0x7001;
        bool failNextReturn = false;

        std::optional<GemInstance> TakeGem(
            const InventoryItemKey& key,
            std::string_view fallbackGemId,
            std::uint8_t defaultSockets,
            std::uint64_t& nextInstanceId) override
        {
            if (key.IsValid()) {
                const auto it = trackedByUid.find(key.uniqueId);
                if (it != trackedByUid.end()) {
                    GemInstance out = std::move(it->second);
                    trackedByUid.erase(it);
                    return out;
                }
                return std::nullopt;
            }
            const std::string gid(fallbackGemId);
            if (plainCounts[gid] > 0) {
                --plainCounts[gid];
                GemInstance fresh;
                fresh.instanceId = nextInstanceId++;
                fresh.gemId = gid;
                fresh.level = 1;
                fresh.socketCount = defaultSockets;
                return fresh;
            }
            return std::nullopt;
        }

        std::optional<InventoryItemKey> ReturnGem(GemInstance gem) override
        {
            if (failNextReturn) {
                failNextReturn = false;
                return std::nullopt;
            }
            const std::uint16_t uid = nextUid++;
            trackedByUid[uid] = std::move(gem);
            return InventoryItemKey{ 0x14, uid };
        }
    };
}

POE_TEST("GemTransaction: duplicate request on last gem, rollback on failure, and data-gem round-trip preservation")
{
    const DataPack pack = LoadStage2Pack();
    std::array<SkillSlot, kSkillSlotCount> slots{};
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i].slotIndex = static_cast<std::uint8_t>(i);
    }
    std::uint64_t nextId = 1;

    MockGemInventory inv;
    inv.plainCounts["Cyclone"] = 1;
    inv.plainCounts["SupportCastOnCriticalStrike"] = 2;
    inv.plainCounts["IceNova"] = 1;
    inv.plainCounts["SupportMeleePhysicalDamage"] = 2;
    inv.plainCounts["SupportConcentratedEffect"] = 1;

    // 1. 마지막 1개 Cyclone 중복 장착 요청 -> 첫 요청만 성공, 둘째 거절(결정 137(마))
    const auto eq1 = GemTransaction::EquipSkillGem(slots, nextId, 0, {}, "Cyclone", 3, pack, inv);
    POE_CHECK(eq1.ok && slots[0].occupied && slots[0].gem.gemId == "Cyclone");
    POE_CHECK(inv.plainCounts["Cyclone"] == 0);

    const auto eqDup = GemTransaction::EquipSkillGem(slots, nextId, 1, {}, "Cyclone", 3, pack, inv);
    POE_CHECK(!eqDup.ok && !slots[1].occupied);

    // 2. 보조 젬 장착 + 동일 보조 젬 중복 차단
    const auto sup1 = GemTransaction::SocketSubGem(slots, nextId, 0, -1, {}, "SupportMeleePhysicalDamage", 3, pack, inv);
    POE_CHECK(sup1.ok && slots[0].gem.socketedGems.size() == 1);
    const auto supDup = GemTransaction::SocketSubGem(slots, nextId, 0, -1, {}, "SupportMeleePhysicalDamage", 3, pack, inv);
    POE_CHECK(!supDup.ok && slots[0].gem.socketedGems.size() == 1);
    POE_CHECK(inv.plainCounts["SupportMeleePhysicalDamage"] == 1);  // 소비되지 않아야 함

    // 3. CoC 장착 + 공격 연결 + IceNova + IceNova 내부 ConcentratedEffect 장착
    POE_CHECK(GemTransaction::EquipSkillGem(slots, nextId, 1, {}, "SupportCastOnCriticalStrike", 3, pack, inv).ok);
    POE_CHECK(GemTransaction::LinkTriggerAttack(slots, 1, 0, pack).ok);
    POE_CHECK(slots[1].linkedAttackSlot == 0);

    // 결정 131: 두 번째 CoC가 같은 공격 칸(0)에 연결 시도 -> 차단
    POE_CHECK(GemTransaction::EquipSkillGem(slots, nextId, 2, {}, "SupportCastOnCriticalStrike", 3, pack, inv).ok);
    POE_CHECK(!GemTransaction::LinkTriggerAttack(slots, 2, 0, pack).ok);
    POE_CHECK(slots[2].linkedAttackSlot == -1);

    // IceNova는 CoC 소켓에 장착 + 내부 보조 장착 가능
    POE_CHECK(GemTransaction::SocketSubGem(slots, nextId, 1, -1, {}, "IceNova", 3, pack, inv).ok);
    POE_CHECK(GemTransaction::SocketSubGem(slots, nextId, 1, 0, {}, "SupportConcentratedEffect", 3, pack, inv).ok);
    // 비호환 보조(MeleePhys)는 IceNova 내부 소켓 거부
    POE_CHECK(!GemTransaction::SocketSubGem(slots, nextId, 1, 0, {}, "SupportMeleePhysicalDamage", 3, pack, inv).ok);

    // CoC와 담긴 IceNova 레벨을 올린 뒤 탈착 → 재장착 시 레벨·주문·보조 모두 보존 확인(결정 106, 107, 145)
    slots[1].gem.level = 5;
    slots[1].gem.socketedGems[0].level = 7;
    slots[1].gem.socketedGems[0].socketedGems[0].level = 4;

    const auto uneqCoc = GemTransaction::UnequipSkillGem(slots, 1, inv);
    POE_CHECK(uneqCoc.ok && uneqCoc.returnedKey.has_value() && !slots[1].occupied);

    const auto reeqCoc = GemTransaction::EquipSkillGem(slots, nextId, 4, *uneqCoc.returnedKey, "SupportCastOnCriticalStrike", 3, pack, inv);
    POE_CHECK(reeqCoc.ok && slots[4].occupied);
    POE_CHECK(slots[4].gem.level == 5);
    POE_CHECK(slots[4].gem.socketedGems.size() == 1 && slots[4].gem.socketedGems[0].level == 7);
    POE_CHECK(slots[4].gem.socketedGems[0].socketedGems.size() == 1 && slots[4].gem.socketedGems[0].socketedGems[0].level == 4);

    // 4. 중간 실패 롤백: 슬롯 0 교체 시 기존 Cyclone 반환이 실패하면 슬롯 0 유지 + 새 젬도 인벤토리로 복구
    inv.plainCounts["SupportCastOnCriticalStrike"] = 1;
    inv.failNextReturn = true;
    const auto failSwap = GemTransaction::EquipSkillGem(slots, nextId, 0, {}, "SupportCastOnCriticalStrike", 3, pack, inv);
    POE_CHECK(!failSwap.ok);
    POE_CHECK(slots[0].occupied && slots[0].gem.gemId == "Cyclone");
    POE_CHECK(inv.trackedByUid.size() == 1);  // 새 젬이 유실되지 않고 인벤토리로 되돌려짐

    // 5. 추적 키 미존재 요청 시 순정 젬 스택을 오소비하지 않음(결정 137(마))
    inv.plainCounts["Cyclone"] = 1;
    const auto staleKeyReq = GemTransaction::EquipSkillGem(
        slots, nextId, 5, InventoryItemKey{ 0x14, 0x7FFF }, "Cyclone", 3, pack, inv);
    POE_CHECK(!staleKeyReq.ok && !slots[5].occupied);
    POE_CHECK(inv.plainCounts["Cyclone"] == 1);

    // 6. UnsocketSubGem (중첩 보조·상위 소켓·범위 초과·ReturnGem 실패 롤백) 및 소켓 상한 초과 거절
    inv.failNextReturn = true;
    POE_CHECK(!GemTransaction::UnsocketSubGem(slots, 4, 0, 0, inv).ok);
    POE_CHECK(slots[4].gem.socketedGems[0].socketedGems.size() == 1);  // 롤백 보존

    const auto unsockNested = GemTransaction::UnsocketSubGem(slots, 4, 0, 0, inv);
    POE_CHECK(unsockNested.ok && unsockNested.returnedKey.has_value());
    POE_CHECK(slots[4].gem.socketedGems[0].socketedGems.empty());
    POE_CHECK(!GemTransaction::UnsocketSubGem(slots, 4, 0, 99, inv).ok);  // 범위 초과

    // 소켓 상한(1칸) 초과 거절
    slots[0].gem.socketCount = 1;  // 이미 MeleePhys 1개 장착됨
    inv.plainCounts["SupportBrutality"] = 1;
    POE_CHECK(!GemTransaction::SocketSubGem(slots, nextId, 0, -1, {}, "SupportBrutality", 3, pack, inv).ok);
    POE_CHECK(inv.plainCounts["SupportBrutality"] == 1);

    // 7. LinkTriggerAttack 해제(-1) 및 연결된 공격 칸 탈착 시 자동 연결 해제
    POE_CHECK(GemTransaction::LinkTriggerAttack(slots, 4, 0, pack).ok && slots[4].linkedAttackSlot == 0);
    POE_CHECK(GemTransaction::LinkTriggerAttack(slots, 4, -1, pack).ok && slots[4].linkedAttackSlot == -1);
    POE_CHECK(GemTransaction::LinkTriggerAttack(slots, 4, 0, pack).ok && slots[4].linkedAttackSlot == 0);
    POE_CHECK(GemTransaction::UnequipSkillGem(slots, 0, inv).ok);
    POE_CHECK(!slots[0].occupied && slots[4].linkedAttackSlot == -1);

    // 8. 5단계(결정 161·163): 손 시전 주문·오라·가드 젬 메인 칸 장착 + 오라 소켓 0칸/보조 거부 검증
    inv.plainCounts["Fireball"] = 1;
    inv.plainCounts["Determination"] = 1;
    inv.plainCounts["MoltenShell"] = 1;
    inv.plainCounts["SupportConcentratedEffect"] = 1;
    POE_CHECK(!GemTransaction::EquipSkillGem(slots, nextId, 3, {}, "SupportConcentratedEffect", 3, pack, inv).ok);
    POE_CHECK(GemTransaction::EquipSkillGem(slots, nextId, 3, {}, "Fireball", 3, pack, inv).ok);
    POE_CHECK(slots[3].occupied && slots[3].gem.gemId == "Fireball" && slots[3].gem.socketCount == 3);
    POE_CHECK(GemTransaction::UnequipSkillGem(slots, 3, inv).ok);
    POE_CHECK(GemTransaction::EquipSkillGem(slots, nextId, 3, {}, "Determination", 3, pack, inv).ok);
    POE_CHECK(slots[3].occupied && slots[3].gem.socketCount == 0);
    POE_CHECK(!GemTransaction::SocketSubGem(slots, nextId, 3, -1, {}, "SupportConcentratedEffect", 3, pack, inv).ok);
    POE_CHECK(GemTransaction::UnequipSkillGem(slots, 3, inv).ok);
    POE_CHECK(GemTransaction::EquipSkillGem(slots, nextId, 3, {}, "MoltenShell", 3, pack, inv).ok);
    POE_CHECK(slots[3].occupied && slots[3].gem.gemId == "MoltenShell" && slots[3].gem.socketCount == 3);
    POE_CHECK(GemTransaction::UnequipSkillGem(slots, 3, inv).ok);
}

POE_TEST("Uninstall clears links and retries only gems that could not be returned")
{
    const auto gem = [](std::string id) {
        GemInstance instance;
        instance.gemId = std::move(id);
        return instance;
    };
    std::array<SkillSlot, kSkillSlotCount> slots{};
    slots[0].occupied = true;
    slots[0].gem = gem("attack");
    slots[1].occupied = true;
    slots[1].linkedAttackSlot = 0;
    slots[1].gem = gem("trigger");
    auto spell = gem("spell");
    spell.socketedGems = { gem("available support"), gem("retry support") };
    slots[1].gem.socketedGems = { std::move(spell), gem("missing form") };

    std::vector<std::string> attempted;
    GemTransaction::ReturnSlottedGemsForUninstall(slots, [&](const GemInstance& instance) {
        attempted.push_back(instance.gemId);
        // A missing form is removed, but an inventory failure keeps that gem for the next attempt.
        return instance.gemId != "retry support";
    });
    const std::vector<std::string> firstAttempt{ "attack", "available support", "retry support", "missing form" };
    POE_CHECK(attempted == firstAttempt);
    POE_CHECK(!slots[0].occupied);
    POE_CHECK(slots[1].occupied && slots[1].linkedAttackSlot == -1);
    POE_CHECK(slots[1].gem.socketedGems.size() == 1);
    POE_CHECK(slots[1].gem.socketedGems.front().socketedGems.size() == 1);
    POE_CHECK(slots[1].gem.socketedGems.front().socketedGems.front().gemId == "retry support");

    attempted.clear();
    GemTransaction::ReturnSlottedGemsForUninstall(slots, [&](const GemInstance& instance) {
        attempted.push_back(instance.gemId);
        return true;
    });
    const std::vector<std::string> secondAttempt{ "retry support", "spell", "trigger" };
    POE_CHECK(attempted == secondAttempt);
    POE_CHECK(!slots[1].occupied && slots[1].gem.gemId.empty());
}
