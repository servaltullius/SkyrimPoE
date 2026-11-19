"""tools/compiler/compile_stage2.py 산출물 및 변환 로직 단위 테스트."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "compiler"))
import compile_stage2 as compiler  # noqa: E402


class TestStage2Compiler(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out_dir = Path(cls.tmp.name)
        cls.pack = compiler.compile_stage2(compiler.DEFAULT_POB, cls.out_dir)
        cls.ko = json.loads((cls.out_dir / "lang" / "ko-KR.json").read_text(encoding="utf-8"))
        cls.en = json.loads((cls.out_dir / "lang" / "en-US.json").read_text(encoding="utf-8"))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_constants_match_pob_2_68_1(self) -> None:
        c = self.pack["constants"]
        self.assertEqual(c["lifeBase"], 38.0)
        self.assertEqual(c["lifePerLevel"], 12.0)
        self.assertEqual(c["manaBase"], 34.0)
        self.assertEqual(c["manaPerLevel"], 6.0)
        self.assertEqual(c["resistCap"], 75.0)
        self.assertEqual(c["esRechargeDelay"], 2.0)
        self.assertEqual(c["esRechargeRate"], 0.33)
        self.assertAlmostEqual(c["skillDamageBaseEffectiveness"], 3.885209, places=5)
        self.assertAlmostEqual(c["skillDamageIncrementalEffectiveness"], 0.360246, places=5)

    def test_monster_tables_and_expected_defense(self) -> None:
        tables = self.pack["tables"]
        self.assertEqual(len(tables["monsterLife"]), 100)
        self.assertEqual(len(tables["monsterDamage"]), 100)
        self.assertEqual(len(tables["monsterAccuracy"]), 100)
        self.assertEqual(len(tables["expectedDefense"]), 100)
        self.assertEqual(tables["monsterLife"][0], 22.0)
        self.assertGreater(tables["expectedDefense"][99]["bestArmour"], tables["expectedDefense"][0]["bestArmour"])
        self.assertAlmostEqual(tables["expectedDefense"][0]["bestResistTier"], 8.5, places=2)
        self.assertGreaterEqual(tables["expectedDefense"][83]["bestResistTier"], 40.0)

    def test_classes_seven(self) -> None:
        names = {c["name"] for c in self.pack["classes"]}
        self.assertEqual(
            names,
            {"Scion", "Marauder", "Ranger", "Witch", "Duelist", "Templar", "Shadow"},
        )
        marauder = next(c for c in self.pack["classes"] if c["name"] == "Marauder")
        self.assertEqual((marauder["baseStr"], marauder["baseDex"], marauder["baseInt"]), (32, 14, 14))

    def test_stage2_gems_and_interpolation(self) -> None:
        expected_ids = {
            "Cyclone",
            "SupportCastOnCriticalStrike",
            "IceNova",
            "ShockNova",
            "SupportIncreasedCriticalStrikes",
            "SupportMeleePhysicalDamage",
            "SupportBrutality",
            "SupportIncreasedAreaOfEffect",
            "SupportConcentratedEffect",
        }
        self.assertEqual(set(self.pack["gems"].keys()), expected_ids)

        # Cyclone lv1/lv20 확인
        cyc = self.pack["gems"]["Cyclone"]
        self.assertEqual(cyc["levels"]["1"]["manaCost"], 2.0)
        self.assertEqual(cyc["levels"]["20"]["manaCost"], 3.0)
        self.assertEqual(cyc["levels"]["1"]["attackSpeedMultiplier"], 200.0)

        # IceNova / ShockNova statInterpolation == 3 보간 피해가 실제 정수(>10)로 계산됐는지 확인
        ice1 = self.pack["gems"]["IceNova"]["levels"]["1"]["stats"]
        ice20 = self.pack["gems"]["IceNova"]["levels"]["20"]["stats"]
        self.assertGreater(ice1["spell_minimum_base_cold_damage"], 15.0)
        self.assertGreater(ice1["spell_maximum_base_cold_damage"], ice1["spell_minimum_base_cold_damage"])
        self.assertGreater(ice20["spell_minimum_base_cold_damage"], ice1["spell_minimum_base_cold_damage"] * 10)

        shock1 = self.pack["gems"]["ShockNova"]["levels"]["1"]["stats"]
        self.assertGreater(shock1["spell_minimum_base_lightning_damage"], 5.0)
        self.assertGreater(shock1["spell_maximum_base_lightning_damage"], shock1["spell_minimum_base_lightning_damage"])

        # CoC 공격 비용 배수 + triggeredEffect RPN & 감폭 확인
        coc = self.pack["gems"]["SupportCastOnCriticalStrike"]
        self.assertEqual(coc["levels"]["1"]["manaMultiplier"], 20.0)
        trig = coc["triggeredEffect"]
        self.assertEqual(trig["requireSkillTypes"], ["Spell", "Triggerable", "AND"])
        self.assertEqual(trig["levels"]["1"]["cooldown"], 0.15)
        self.assertEqual(trig["levels"]["1"]["stats"]["support_cast_on_crit_spell_damage_+%_final"], -19.0)
        self.assertEqual(trig["levels"]["20"]["stats"]["support_cast_on_crit_spell_damage_+%_final"], -10.0)

    def test_lang_packs_cover_all_gems_and_classes(self) -> None:
        for pack in (self.ko, self.en):
            self.assertEqual(set(pack["gems"].keys()), set(self.pack["gems"].keys()))
            self.assertEqual(set(pack["classes"].keys()), {c["name"] for c in self.pack["classes"]})


if __name__ == "__main__":
    unittest.main()
