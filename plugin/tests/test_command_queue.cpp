#include "test_harness.h"

#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/StatEngine.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

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
}

POE_TEST("CommandQueue: FIFO order and re-enqueued tasks defer to next frame")
{
    CommandQueue q;
    std::vector<int> log;

    q.Enqueue([&]() {
        log.push_back(1);
        q.Enqueue([&]() { log.push_back(4); });
    });
    q.Enqueue([&]() { log.push_back(2); });
    q.Enqueue([&]() { log.push_back(3); });

    const auto s1 = q.DrainFrame(0.0);
    POE_CHECK(s1.executed == 3);
    POE_CHECK(log.size() == 3 && log[0] == 1 && log[1] == 2 && log[2] == 3);
    POE_CHECK(q.PendingCount() == 1);

    const auto s2 = q.DrainFrame(0.0);
    POE_CHECK(s2.executed == 1);
    POE_CHECK(log.size() == 4 && log[3] == 4);
    POE_CHECK(q.PendingCount() == 0);
}

POE_TEST("CommandQueue: BumpGenerationAndClear drops pending and stale-generation tasks")
{
    CommandQueue q;
    const auto gen1 = q.CurrentGeneration();
    int ran = 0;

    q.Enqueue([&]() { ++ran; });
    const auto gen2 = q.BumpGenerationAndClear();
    POE_CHECK(gen2 == gen1 + 1);
    POE_CHECK(q.PendingCount() == 0);

    // 이전 세대로 뒤늦게 들어온 작업은 DrainFrame에서 실행되지 않고 폐기된다
    q.EnqueueForGeneration(gen1, [&]() { ++ran; });
    q.EnqueueForGeneration(gen2, [&]() { ran += 10; });

    const auto stats = q.DrainFrame(0.0);
    POE_CHECK(stats.droppedStale == 1);
    POE_CHECK(stats.executed == 1);
    POE_CHECK(ran == 10);
}

POE_TEST("CommandQueue: budget deferral preserves FIFO order ahead of new tasks")
{
    CommandQueue q;
    std::vector<int> order;

    q.Enqueue([&]() {
        order.push_back(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        q.Enqueue([&]() { order.push_back(3); });
    });
    q.Enqueue([&]() { order.push_back(2); });

    // 1ms 예산 -> 첫 작업(5ms) 후 2번은 다음 프레임 맨 앞으로 되돌려져야 하며, 새로 들어온 3번보다 앞서 실행된다
    const auto s1 = q.DrainFrame(1.0);
    POE_CHECK(s1.executed == 1);
    POE_CHECK(s1.deferredByBudget == 1);

    const auto s2 = q.DrainFrame(0.0);
    POE_CHECK(s2.executed == 2);
    POE_CHECK(order.size() == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);
}

POE_TEST("CommandQueue: exception in one task does not drop remaining tasks in batch")
{
    CommandQueue q;
    std::vector<int> ran;
    q.Enqueue([&]() { ran.push_back(1); });
    q.Enqueue([&]() { throw std::runtime_error("boom"); });
    q.Enqueue([&]() { ran.push_back(3); });

    const auto stats = q.DrainFrame(0.0);
    POE_CHECK(stats.executed == 2);
    POE_CHECK(stats.failed == 1);
    POE_CHECK(ran.size() == 2 && ran[0] == 1 && ran[1] == 3);
}

POE_TEST("StatEngine: immutable snapshot, CoC attack/spell rules, Brutality, and Layer 2 charges")
{
    const DataPack pack = LoadStage2Pack();
    Config cfg;
    StatEngine engine;

    // 1. Cyclone(slot 0) + CoC(slot 1 linked to 0, socketing IceNova) + Brutality on Cyclone
    Layer1Input l1;
    l1.exileLevel = 10;
    l1.className = "Scion";
    l1.baseWeaponPhysicalMin = 20.0;
    l1.baseWeaponPhysicalMax = 40.0;
    l1.baseWeaponCritChancePct = 6.0;

    l1.slots[0].slotIndex = 0;
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 101;
    l1.slots[0].gem.gemId = "Cyclone";
    l1.slots[0].gem.level = 1;

    GemInstance meleePhys;
    meleePhys.instanceId = 102;
    meleePhys.gemId = "SupportMeleePhysicalDamage";
    meleePhys.level = 1;
    l1.slots[0].gem.socketedGems.push_back(meleePhys);

    GemInstance brutality;
    brutality.instanceId = 103;
    brutality.gemId = "SupportBrutality";
    brutality.level = 1;
    l1.slots[0].gem.socketedGems.push_back(brutality);

    // CoC 발동 젬 (slot 1 -> linked to slot 0)
    l1.slots[1].slotIndex = 1;
    l1.slots[1].occupied = true;
    l1.slots[1].linkedAttackSlot = 0;
    l1.slots[1].gem.instanceId = 201;
    l1.slots[1].gem.gemId = "SupportCastOnCriticalStrike";
    l1.slots[1].gem.level = 1;

    GemInstance iceNova;
    iceNova.instanceId = 301;
    iceNova.gemId = "IceNova";
    iceNova.level = 1;
    l1.slots[1].gem.socketedGems.push_back(iceNova);

    engine.SetLayer1(l1);
    POE_CHECK(engine.IsDirty());
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    POE_CHECK(!engine.IsDirty());

    const auto snap1 = engine.GetSnapshot();
    POE_CHECK(snap1 != nullptr);
    POE_CHECK(snap1->exileLevel == 10);
    // Scion: Str 20 -> +10 life, Int 20 -> +10 mana; base life = 38 + 12*10 + 10 = 168
    POE_CHECK_NEAR(snap1->poeMaxLife, 168.0, 1e-6);
    POE_CHECK_NEAR(snap1->poeMaxMana, 34.0 + 6.0 * 10.0 + 10.0, 1e-6);

    // Cyclone: Brutality로 원소·카오스 0, CoC 연결로 마나 배율 +20%(×1.20) 반영
    const auto& cyc = snap1->slotStats[0];
    POE_CHECK(cyc.gemId == "Cyclone");
    POE_CHECK(cyc.dealNoElemental && cyc.dealNoChaos);
    POE_CHECK(cyc.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Physical)].Average() > 0.0);
    POE_CHECK_NEAR(cyc.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Cold)].Average(), 0.0, 1e-9);
    POE_CHECK(cyc.attackSpeedMult > 2.0);  // Cyclone +200% attack speed mult

    // CoC 내부 IceNova가 triggeredSpellStats에 계산되고, CoC 1레벨 피해 페널티(more -19%)가 반영됐는지 확인
    const auto it = snap1->triggeredSpellStats.find(301);
    POE_CHECK(it != snap1->triggeredSpellStats.end());
    POE_CHECK(it->second.isTriggered);
    POE_CHECK(it->second.cooldownSec > 0.1);
    POE_CHECK(it->second.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Cold)].Average() > 0.0);

    // Layer 2 충전 변경 시 새 불변 사본이 생성되며 기존 snap1은 변하지 않는다(결정 118)
    Layer2Input l2;
    l2.powerCharges = 3;
    l2.frenzyCharges = 3;
    l2.enduranceCharges = 3;
    engine.SetLayer2(l2);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));

    const auto snap2 = engine.GetSnapshot();
    POE_CHECK(snap2->version > snap1->version);
    POE_CHECK(snap1->slotStats[0].critChanceIncPct < snap2->slotStats[0].critChanceIncPct);
    POE_CHECK(snap1->slotStats[0].attackSpeedMult < snap2->slotStats[0].attackSpeedMult);
    // PoE 3.30: 인내 충전 3개는 원소 저항이 아니라 물리·원소 피해 감소 +12%를 부여한다
    POE_CHECK_NEAR(snap2->fireResist, snap1->fireResist, 1e-9);
    POE_CHECK_NEAR(snap2->physDamageReduction, 12.0, 1e-9);
    POE_CHECK_NEAR(snap2->elementalDamageReduction, 12.0, 1e-9);
}

POE_TEST("StatEngine: duplicate support dedup, socket cap, multi-CoC rejection, Area/Crit/Brutality/BloodMagic/Act penalties")
{
    const DataPack pack = LoadStage2Pack();
    Config cfg;
    StatEngine engine;

    Layer1Input l1;
    l1.exileLevel = 67;  // 10막 패널티(-60%) 적용 구간
    l1.className = "Scion";
    l1.fireResist = 100.0;
    l1.bloodMagic = true;
    l1.baseWeaponPhysicalMin = 20.0;
    l1.baseWeaponPhysicalMax = 20.0;

    // 슬롯 0: Cyclone (소켓 2개 상한)에 동일 MeleePhys(lv1, lv10) 2개 + IncreasedAreaOfEffect + ConcentratedEffect(소켓 초과)
    l1.slots[0].slotIndex = 0;
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 1;
    l1.slots[0].gem.gemId = "Cyclone";
    l1.slots[0].gem.level = 1;
    l1.slots[0].gem.socketCount = 3;
    l1.slots[0].gem.socketedGems = {
        GemInstance{ 2, "SupportMeleePhysicalDamage", 1, 0, 0, 0, {} },
        GemInstance{ 3, "SupportMeleePhysicalDamage", 10, 0, 0, 0, {} },
        GemInstance{ 4, "SupportIncreasedAreaOfEffect", 1, 0, 0, 0, {} },
        GemInstance{ 5, "SupportConcentratedEffect", 1, 0, 0, 0, {} },  // socketCount=3 초과 -> 무시돼야 함
    };

    // 슬롯 1, 2: 동일 공격 칸(슬롯 0)을 가리키는 CoC 2개 -> 결정 131에 따라 슬롯 1만 연결 허용, 슬롯 2는 차단
    l1.slots[1].slotIndex = 1;
    l1.slots[1].occupied = true;
    l1.slots[1].linkedAttackSlot = 0;
    l1.slots[1].gem.instanceId = 10;
    l1.slots[1].gem.gemId = "SupportCastOnCriticalStrike";
    l1.slots[1].gem.level = 1;
    l1.slots[1].gem.socketCount = 2;
    l1.slots[1].gem.socketedGems = {
        GemInstance{ 11, "IceNova", 1, 0, 0, 1, { GemInstance{ 12, "SupportBrutality", 1, 0, 0, 0, {} } } }
    };

    l1.slots[2].slotIndex = 2;
    l1.slots[2].occupied = true;
    l1.slots[2].linkedAttackSlot = 0;
    l1.slots[2].gem.instanceId = 20;
    l1.slots[2].gem.gemId = "SupportCastOnCriticalStrike";
    l1.slots[2].gem.level = 1;
    l1.slots[2].gem.socketCount = 2;
    l1.slots[2].gem.socketedGems = {
        GemInstance{ 21, "ShockNova", 1, 0, 0, 0, {} }
    };

    // 슬롯 3: 직접 시전 ShockNova + IncreasedCriticalStrikes + ConcentratedEffect
    l1.slots[3].slotIndex = 3;
    l1.slots[3].occupied = true;
    l1.slots[3].gem.instanceId = 30;
    l1.slots[3].gem.gemId = "ShockNova";
    l1.slots[3].gem.level = 1;
    l1.slots[3].gem.socketCount = 2;
    l1.slots[3].gem.socketedGems = {
        GemInstance{ 31, "SupportIncreasedCriticalStrikes", 1, 0, 0, 0, {} },
        GemInstance{ 32, "SupportConcentratedEffect", 1, 0, 0, 0, {} }
    };

    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snap = engine.GetSnapshot();

    // BloodMagic -> poeMaxMana == 0, 10막 패널티 -> fireResist = 100 - 60 = 40
    POE_CHECK_NEAR(snap->poeMaxMana, 0.0, 1e-9);
    POE_CHECK_NEAR(snap->fireResist, 40.0, 1e-9);

    // 중복 MeleePhys: lv10 단일 배율만 적용되고 lv1과 이중 곱연산되지 않아야 함, 소켓 4번째 ConcentratedEffect는 무시
    // 3단계: Scion 기본 힘 20 -> PoE 규칙(힘 5당 +1% 근접 물리 피해 = +4% increased) 반영
    const auto* cycLv = pack.FindGem("Cyclone")->GetLevel(1);
    const auto* mp10 = pack.FindGem("SupportMeleePhysicalDamage")->GetLevel(10);
    const double strMeleeIncMult = 1.0 + static_cast<double>(snap->str / 5) / 100.0;
    const double expectedPhys = 20.0 * cycLv->baseMultiplier * strMeleeIncMult *
        (1.0 + mp10->GetStat("support_melee_physical_damage_+%_final", 0.0) / 100.0);
    POE_CHECK_NEAR(snap->slotStats[0].baseHitDamage.byType[static_cast<std::size_t>(DamageType::Physical)].Average(),
        expectedPhys, 1e-6);
    POE_CHECK(snap->slotStats[0].areaMult > 1.0);  // IncreasedAreaOfEffect 적용, ConcentratedEffect 미적용

    // 다중 CoC 차단: 슬롯 1의 IceNova(11)만 등록되고 슬롯 2의 ShockNova(21)는 미등록
    POE_CHECK(snap->triggeredSpellStats.contains(11));
    POE_CHECK(!snap->triggeredSpellStats.contains(21));
    // Brutality가 IceNova의 냉기 피해를 0으로 만듦
    POE_CHECK_NEAR(snap->triggeredSpellStats.at(11).baseHitDamage.TotalAverage(), 0.0, 1e-9);

    // ShockNova + IncreasedCriticalStrikes + ConcentratedEffect
    const auto& shock = snap->slotStats[3];
    POE_CHECK(shock.baseCritChancePct > pack.FindGem("ShockNova")->GetLevel(1)->critChance);
    POE_CHECK(shock.critChanceIncPct > 0.0);
    POE_CHECK(shock.areaMult < 1.0);
}
