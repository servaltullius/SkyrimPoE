#include "test_harness.h"

#include "core/Config.h"
#include "core/DataPack.h"

#include <filesystem>

using namespace SkyrimPoE::Core;

namespace
{
    DataPack LoadStage2Pack()
    {
        DataPack pack;
        std::string err;
        const std::filesystem::path packPath = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "core_3_30.json";
        const bool ok = pack.LoadFromFile(packPath, &err);
        if (!ok) {
            throw std::runtime_error("Failed to load core_3_30.json: " + err);
        }
        return pack;
    }
}

POE_TEST("DataPack: loads core_3_30.json constants, classes, tables, and 28 gems")
{
    const DataPack pack = LoadStage2Pack();
    POE_CHECK(pack.IsLoaded());
    POE_CHECK_NEAR(pack.Constants().lifeBase, 38.0, 1e-6);
    POE_CHECK_NEAR(pack.Constants().lifePerLevel, 12.0, 1e-6);
    POE_CHECK_NEAR(pack.Constants().manaBase, 34.0, 1e-6);
    POE_CHECK_NEAR(pack.Constants().manaPerLevel, 6.0, 1e-6);
    POE_CHECK_NEAR(pack.Constants().resistCap, 75.0, 1e-6);
    POE_CHECK_NEAR(pack.Constants().esRechargeDelay, 2.0, 1e-6);

    POE_CHECK(pack.AllGems().size() == 28);
    POE_CHECK(pack.FindGem("Cyclone") != nullptr);
    POE_CHECK(pack.FindGem("SupportCastOnCriticalStrike") != nullptr);
    POE_CHECK(pack.FindGem("IceNova") != nullptr);
    POE_CHECK(pack.FindGem("ShockNova") != nullptr);
    POE_CHECK(pack.FindGem("SupportIncreasedCriticalStrikes") != nullptr);
    POE_CHECK(pack.FindGem("SupportMeleePhysicalDamage") != nullptr);
    POE_CHECK(pack.FindGem("SupportBrutality") != nullptr);
    POE_CHECK(pack.FindGem("SupportIncreasedAreaOfEffect") != nullptr);
    POE_CHECK(pack.FindGem("SupportConcentratedEffect") != nullptr);
    POE_CHECK(pack.FindGem("Fireball") != nullptr);
    POE_CHECK(pack.FindGem("FreezingPulse") != nullptr);
    POE_CHECK(pack.FindGem("Arc") != nullptr);
    POE_CHECK(pack.FindGem("Spark") != nullptr);
    POE_CHECK(pack.FindGem("Determination") != nullptr && pack.FindGem("Determination")->kind == GemKind::Aura && pack.FindGem("Determination")->spiritCost == 50);
    POE_CHECK(pack.FindGem("Discipline") != nullptr && pack.FindGem("Discipline")->spiritCost == 35);
    POE_CHECK(pack.FindGem("HeraldOfIce") != nullptr && pack.FindGem("HeraldOfIce")->kind == GemKind::Herald && pack.FindGem("HeraldOfIce")->spiritCost == 25);
    POE_CHECK(pack.FindGem("MoltenShell") != nullptr && pack.FindGem("MoltenShell")->kind == GemKind::Guard);

    const auto* scion = pack.FindClass("Scion");
    POE_CHECK(scion != nullptr);
    POE_CHECK(scion->baseStr == 20 && scion->baseDex == 20 && scion->baseInt == 20);

    POE_CHECK(pack.MonsterLife(1) > 10.0);
    POE_CHECK(pack.MonsterLife(100) > pack.MonsterLife(1));
    POE_CHECK(pack.MonsterDamage(68) > pack.MonsterDamage(1));
    POE_CHECK(pack.ExpectedDefense(1).bestArmour > 0.0);
    POE_CHECK(pack.ExpectedDefense(70).bestArmour > pack.ExpectedDefense(1).bestArmour);

    // IceNova lv1 & lv20 interpolated damage sanity
    const auto* iceNova = pack.FindGem("IceNova");
    POE_CHECK(iceNova != nullptr);
    const auto* lv1 = iceNova->GetLevel(1);
    const auto* lv20 = iceNova->GetLevel(20);
    POE_CHECK(lv1 != nullptr && lv20 != nullptr);
    POE_CHECK(lv1->GetStat("spell_minimum_base_cold_damage") > 10.0);
    POE_CHECK(lv20->GetStat("spell_minimum_base_cold_damage") > lv1->GetStat("spell_minimum_base_cold_damage") * 20.0);
}

POE_TEST("DataPack: PoB RPN DoesTypeExpressionMatch logic")
{
    const std::unordered_set<std::string> attackArea{ "Attack", "Melee", "Area", "Damage" };
    const std::unordered_set<std::string> spellArea{ "Spell", "Area", "Damage", "Triggerable", "CanRapidFire" };
    const std::unordered_set<std::string> auraSkill{ "Spell", "Aura", "Area" };

    // 빈 식은 항상 true
    POE_CHECK(DoesTypeExpressionMatch({}, attackArea));

    // 단일 토큰
    POE_CHECK(DoesTypeExpressionMatch({ "Attack" }, attackArea));
    POE_CHECK(!DoesTypeExpressionMatch({ "Spell" }, attackArea));

    // 복수 토큰 암묵적 OR (PoB stack any-true)
    POE_CHECK(DoesTypeExpressionMatch({ "Spell", "Attack" }, attackArea));
    POE_CHECK(!DoesTypeExpressionMatch({ "Minion", "Totem" }, attackArea));

    // AND / OR / NOT
    POE_CHECK(DoesTypeExpressionMatch({ "Attack", "Melee", "AND" }, attackArea));
    POE_CHECK(!DoesTypeExpressionMatch({ "Attack", "Spell", "AND" }, attackArea));
    POE_CHECK(DoesTypeExpressionMatch({ "Attack", "Spell", "OR" }, attackArea));
    POE_CHECK(DoesTypeExpressionMatch({ "Attack", "Spell", "OR" }, spellArea));
    POE_CHECK(!DoesTypeExpressionMatch({ "Totem", "Trap", "OR" }, attackArea));
    POE_CHECK(DoesTypeExpressionMatch({ "Triggerable", "Spell", "AND" }, spellArea));
    POE_CHECK(!DoesTypeExpressionMatch({ "Triggerable", "Spell", "AND" }, attackArea));
    POE_CHECK(DoesTypeExpressionMatch({ "Spell", "Aura", "NOT", "AND" }, spellArea));
    POE_CHECK(!DoesTypeExpressionMatch({ "Spell", "Aura", "NOT", "AND" }, auraSkill));
}

POE_TEST("DataPack: rejects corrupt/incomplete JSON and honors extraSkillTypes")
{
    DataPack bad;
    std::string err;
    POE_CHECK(!bad.LoadFromJsonString("{ not valid json", &err));
    POE_CHECK(!bad.IsLoaded());
    POE_CHECK(!err.empty());

    POE_CHECK(!bad.LoadFromJsonString("{\"constants\":{}}", &err));
    POE_CHECK(!bad.IsLoaded());

    // extraSkillTypes 전달 시 호환 판정에 반영되는지 확인
    const DataPack pack = LoadStage2Pack();
    const std::unordered_set<std::string> extraMelee{ "Melee", "Attack" };
    POE_CHECK(pack.CanSupportApply("SupportMeleePhysicalDamage", "IceNova", &extraMelee));
    POE_CHECK(!pack.CanSupportApply("SupportMeleePhysicalDamage", "IceNova", nullptr));
}

POE_TEST("Config: loads real distribution SkyrimPoE.ini with zero warnings")
{
    const std::filesystem::path iniPath =
        std::filesystem::path(POE_TEST_PLUGIN_DIR) / "dist" / "SKSE" / "Plugins" / "SkyrimPoE.ini";
    const auto res = LoadConfigFromFile(iniPath);
    POE_CHECK(res.fileFound);
    POE_CHECK(res.warnings.empty());
    POE_CHECK_NEAR(res.config.offenseK, 75.0, 1e-6);
    POE_CHECK_NEAR(res.config.defenseDref, 10.7, 1e-6);
    POE_CHECK_NEAR(res.config.baseHitRate, 1.6, 1e-6);
    POE_CHECK_NEAR(res.config.mainUpdateBudgetMs, 2.0, 1e-6);
}

POE_TEST("DataPack: Stage 2 support and CoC compatibility")
{
    const DataPack pack = LoadStage2Pack();

    // Melee Physical Damage -> Cyclone O, IceNova X
    POE_CHECK(pack.CanSupportApply("SupportMeleePhysicalDamage", "Cyclone"));
    POE_CHECK(!pack.CanSupportApply("SupportMeleePhysicalDamage", "IceNova"));

    // Brutality -> Cyclone O, IceNova O (Damage hits; zeroing elemental happens in StatEngine)
    POE_CHECK(pack.CanSupportApply("SupportBrutality", "Cyclone"));

    // Area supports -> Cyclone, IceNova, ShockNova 모두 호환
    POE_CHECK(pack.CanSupportApply("SupportIncreasedAreaOfEffect", "Cyclone"));
    POE_CHECK(pack.CanSupportApply("SupportIncreasedAreaOfEffect", "IceNova"));
    POE_CHECK(pack.CanSupportApply("SupportConcentratedEffect", "ShockNova"));

    // Increased Critical Strikes -> Cyclone, IceNova, ShockNova 모두 호환
    POE_CHECK(pack.CanSupportApply("SupportIncreasedCriticalStrikes", "Cyclone"));
    POE_CHECK(pack.CanSupportApply("SupportIncreasedCriticalStrikes", "IceNova"));

    // CoC 연결 공격 판정: Cyclone(Attack) O, IceNova(Spell) X
    POE_CHECK(pack.CanTriggerLinkAttack("SupportCastOnCriticalStrike", "Cyclone"));
    POE_CHECK(!pack.CanTriggerLinkAttack("SupportCastOnCriticalStrike", "IceNova"));

    // CoC 내부 소켓 주문 판정: IceNova/ShockNova(Triggerable+Spell) O, Cyclone X
    POE_CHECK(pack.CanTriggerSocketSpell("SupportCastOnCriticalStrike", "IceNova"));
    POE_CHECK(pack.CanTriggerSocketSpell("SupportCastOnCriticalStrike", "ShockNova"));
    POE_CHECK(!pack.CanTriggerSocketSpell("SupportCastOnCriticalStrike", "Cyclone"));
}

POE_TEST("Config: parses INI values and handles invalid lines safely")
{
    const char* sample = R"(
[General]
PanelKey = 0x24
YieldToFrenzy = false
MainUpdateBudgetMs = 3.5

[Conversion]
OffenseK = 80.0
OffenseRollback = 0.25

[Cyclone]
PowerAttackInput = leftHand
)";
    const auto res = LoadConfigFromString(sample);
    POE_CHECK(res.config.panelKey == 0x24);
    POE_CHECK(!res.config.yieldToFrenzy);
    POE_CHECK_NEAR(res.config.mainUpdateBudgetMs, 3.5, 1e-6);
    POE_CHECK_NEAR(res.config.offenseK, 80.0, 1e-6);
    POE_CHECK_NEAR(res.config.offenseRollback, 0.25, 1e-6);
    POE_CHECK(res.config.powerAttackInput == "leftHand");
}
