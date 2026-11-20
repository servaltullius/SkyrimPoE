"""4단계 아이템 컴파일러(compile_items.py) 및 젬 요구 능력치(compile_stage2.py) 검증 테스트."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import compile_items  # noqa: E402
import compile_stage2  # noqa: E402


def resolve_weight(weights: list[list], item_tags: set[str]) -> int:
    for k, v in weights:
        if k in item_tags:
            return int(v)
    return 0


class TestItemsCompiler(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        # T-8: 시험 실행이 저장소 data/packs/*.json을 덮어쓰지 않도록 임시 디렉터리로 출력
        cls._tmp = tempfile.TemporaryDirectory()
        cls.out_dir = Path(cls._tmp.name)
        cls.pack = compile_items.compile_items(compile_items.DEFAULT_POB, cls.out_dir)
        cls.by_id = {a["id"]: a for a in cls.pack["affixes"]}

    @classmethod
    def tearDownClass(cls) -> None:
        cls._tmp.cleanup()

    def test_meta_and_currencies(self) -> None:
        m = self.pack["meta"]
        self.assertEqual(m["schemaVersion"], 2)
        self.assertEqual(m["poeVersion"], "3.30")
        self.assertGreater(m["totalAffixes"], 1000)
        self.assertGreater(m["rollableAffixes"], 750)
        self.assertGreaterEqual(m["jewelAffixes"], 30)
        self.assertEqual(m["uniqueCount"], 16)
        self.assertEqual(len(self.pack["tiers"]), 5)
        self.assertEqual(len(self.pack["currencies"]), 12)
        self.assertEqual(len(self.pack["beltBases"]), 5)
        self.assertEqual(len(self.pack["charms"]), 6)
        self.assertEqual(len(self.pack["monsterMods"]), 12)
        self.assertEqual(len(self.pack["uniques"]), 16)

    def test_stage5_negative_ranges_jewels_and_uniques(self) -> None:
        # 검수 C3: 음수 범위 파싱 검증 (Ventor's Gamble)
        neg = compile_items.parse_stat_line("+(-25-50)% to Fire Resistance")
        self.assertEqual(neg["template"], "+{0}% to Fire Resistance")
        self.assertEqual(neg["ranges"], [[-25, 50, False]])

        # 주얼 어픽스는 일반 방어구/무기에 롤링되지 않고 jewel 태그에만 매칭되어야 함
        jewel_affs = [a for a in self.pack["affixes"] if a["rollable"] and any(k == "jewel" for k, _ in a["weights"])]
        self.assertGreaterEqual(len(jewel_affs), 30)
        armor_tags = {"body_armour", "str_armour", "armour", "default"}
        crimson_tags = {"jewel", "not_dex", "not_int"}
        for a in jewel_affs:
            self.assertEqual(resolve_weight(a["weights"], armor_tags), 0, f"Jewel mod leaked to armor: {a['id']}")
        self.assertTrue(any(resolve_weight(a["weights"], crimson_tags) > 0 for a in jewel_affs))

        # 16종 고유 아이템 어픽스가 비롤러블(rollable=False)로 affixes에 등록되어 Divine 재롤 가능
        uniques_by_id = {u["id"]: u for u in self.pack["uniques"]}
        self.assertIn("Headhunter", uniques_by_id)
        self.assertIn("VentorsGamble", uniques_by_id)
        self.assertIn("Starforge", uniques_by_id)
        self.assertIn("ShavronnesWrappings", uniques_by_id)
        for u in self.pack["uniques"]:
            for aid in u["implicitAffixIds"] + u["explicitAffixIds"]:
                self.assertIn(aid, self.by_id)
                self.assertFalse(self.by_id[aid]["rollable"])

    def test_first_match_wins_and_zero_overrides_preserved(self) -> None:
        # IncreasedLife0: weapon=0 before default=1000 -> weapon must resolve to 0, helmet to 1000
        life0 = self.by_id["IncreasedLife0"]
        self.assertEqual(resolve_weight(life0["weights"], {"sword", "one_hand_weapon", "weapon", "default"}), 0)
        self.assertEqual(resolve_weight(life0["weights"], {"helmet", "str_armour", "armour", "default"}), 1000)

        # SpellDamageOnTwoHandWeapon1: attack_staff=0 before staff=1500
        sd_staff = self.by_id["SpellDamageOnTwoHandWeapon1"]
        self.assertEqual(resolve_weight(sd_staff["weights"], {"attack_staff", "staff", "weapon", "default"}), 0)
        self.assertGreater(resolve_weight(sd_staff["weights"], {"staff", "weapon", "default"}), 0)

        # SpellDamageOnWeapon1: attack_dagger=0 before dagger=1500, focus > 0
        sd_weap = self.by_id["SpellDamageOnWeapon1"]
        self.assertEqual(resolve_weight(sd_weap["weights"], {"attack_dagger", "dagger", "weapon", "default"}), 0)
        self.assertGreater(resolve_weight(sd_weap["weights"], {"focus", "shield", "int_armour", "default"}), 0)

    def test_shield_and_ranged_tags_rollable(self) -> None:
        # str_shield block chance mod must be present & rollable
        blk = self.by_id["LocalIncreasedBlockPercentage1"]
        self.assertTrue(blk["rollable"])
        self.assertTrue(blk["isLocal"])
        self.assertGreater(resolve_weight(blk["weights"], {"str_shield", "shield", "str_armour", "default"}), 0)

        # ranged high-tier attack speed must be present & rollable
        ias4 = self.by_id["LocalIncreasedAttackSpeed4"]
        self.assertTrue(ias4["rollable"])
        self.assertTrue(ias4["isLocal"])
        self.assertGreater(resolve_weight(ias4["weights"], {"bow", "ranged", "weapon", "default"}), 0)

    def test_no_influence_tags_leak(self) -> None:
        for a in self.pack["affixes"]:
            for k, _ in a["weights"]:
                for suf in compile_items.INFLUENCE_SUFFIXES:
                    self.assertFalse(k.endswith(suf), f"Influence tag leaked in {a['id']}: {k}")

    def test_range_normalization_and_float_support(self) -> None:
        # Reversed range normalization unit check
        parsed_rev = compile_items.parse_stat_line("-(8-6) to Total Mana Cost of Skills")
        self.assertEqual(parsed_rev["ranges"], [[6, 8, False]])
        self.assertEqual(parsed_rev["template"], "-{0} to Total Mana Cost of Skills")

        # Float range check
        parsed_flt = compile_items.parse_stat_line("(0.2-0.4)% of Physical Attack Damage Leeched as Life")
        self.assertEqual(parsed_flt["ranges"], [[0.2, 0.4, True]])

        # Mixed fixed + range check
        parsed_mix = compile_items.parse_stat_line("Adds 1 to (2-3) Physical Damage")
        self.assertEqual(parsed_mix["ranges"], [[1, 1, False], [2, 3, False]])
        self.assertEqual(parsed_mix["template"], "Adds {0} to {1} Physical Damage")

        # Every affix in compiled pack has min <= max
        for a in self.pack["affixes"]:
            for ln in a["lines"]:
                for lo, hi, _ in ln["ranges"]:
                    self.assertLessEqual(lo, hi, f"Reversed range in {a['id']}")

    def test_local_vs_global_classification(self) -> None:
        self.assertTrue(self.by_id["LocalIncreasedPhysicalDamagePercent1"]["isLocal"])
        self.assertTrue(self.by_id["LocalAddedFireDamage1"]["isLocal"])
        self.assertFalse(self.by_id["IncreasedLife1"]["isLocal"])
        self.assertFalse(self.by_id["AddedFireDamage1"]["isLocal"])
        self.assertFalse(self.by_id["Strength1"]["isLocal"])
        # 검수 C1: LocalCriticalMultiplier1..6은 Global 치명타 배율이므로 isLocal=False
        self.assertFalse(self.by_id["LocalCriticalMultiplier1"]["isLocal"])
        self.assertTrue(self.by_id["LocalCriticalMultiplier1"]["rollable"])
        # 검수 M2: 패밀리별 티어 산정(ChanceToSuppressSpells 최고 티어가 T1이어야 함)
        supp_low = [a for a in self.pack["affixes"] if a["id"].startswith("ChanceToSuppressSpells") and "High" not in a["id"]]
        self.assertEqual(min(a["tier"] for a in supp_low), 1)
        self.assertLessEqual(max(a["tier"] for a in supp_low), 6)

    def test_base_curves_complete_and_sorted(self) -> None:
        armor = self.pack["baseArmorCurves"]
        for slot in ("Helmet", "Body", "Gloves", "Boots", "Shield"):
            for sub in ("Armour", "Evasion", "EnergyShield"):
                key = f"{slot}:{sub}"
                self.assertIn(key, armor)
                rows = armor[key]
                self.assertGreaterEqual(len(rows), 5, key)
                levels = [r["reqLevel"] for r in rows]
                self.assertEqual(levels, sorted(levels))

        weaps = self.pack["baseWeaponCurves"]
        for wkey in ("OneHandSword", "TwoHandSword", "OneHandAxe", "TwoHandAxe", "OneHandMace", "TwoHandMace", "Dagger", "Bow", "Staff"):
            self.assertIn(wkey, weaps)
            rows = weaps[wkey]
            self.assertGreaterEqual(len(rows), 5, wkey)
            levels = [r["reqLevel"] for r in rows]
            self.assertEqual(levels, sorted(levels))

    def test_stage2_gem_attribute_requirements(self) -> None:
        core_pack = compile_stage2.compile_stage2(compile_stage2.DEFAULT_POB, self.out_dir)
        cyc_lv1 = core_pack["gems"]["Cyclone"]["levels"]["1"]
        self.assertEqual(cyc_lv1["levelRequirement"], 28)
        self.assertGreater(cyc_lv1["reqStr"], 0)
        self.assertGreater(cyc_lv1["reqDex"], cyc_lv1["reqStr"])
        self.assertEqual(cyc_lv1["reqInt"], 0)

        ice_lv1 = core_pack["gems"]["IceNova"]["levels"]["1"]
        self.assertEqual(ice_lv1["levelRequirement"], 12)
        self.assertEqual(ice_lv1["reqStr"], 0)
        self.assertGreater(ice_lv1["reqInt"], 0)

        # C-16 검증: Lifesprig은 1H 검 곡선, Stygian Vise는 PoB 원본 reqLevel 1 + Abyssal Socket
        uniques_by_id = {u["id"]: u for u in self.pack["uniques"]}
        self.assertEqual(uniques_by_id["Lifesprig"]["weaponTag"], "OneHandSword")
        belts_by_id = {b["id"]: b for b in self.pack["beltBases"]}
        self.assertEqual(belts_by_id["StygianVise"]["reqLevel"], 1)
        self.assertTrue(belts_by_id["StygianVise"]["bonusCharmSlot"])

        # Also ensure JSON on disk matches freshly compiled pack
        disk = json.loads((ROOT / "data" / "packs" / "items_3_30.json").read_text(encoding="utf-8"))
        self.assertEqual(disk["meta"]["rollableAffixes"], self.pack["meta"]["rollableAffixes"])


if __name__ == "__main__":
    unittest.main()
