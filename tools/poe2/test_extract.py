"""Exercise the isolated CLI on scratch packs and the installed PoB fixtures."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))

from extract import InputFiles, translations


REPO = Path(__file__).resolve().parents[2]
SCRATCH = REPO / "build"
TOOLS = REPO / "tools" / "poe2"
POB = Path("G:/poe/PathOfBuilding-PoE2-0.23.1")
KO = Path("G:/poe/POE1 한글 POB (2025.10.31)/Data/Translate/ko-KR")
SKELETAL_SNIPER = "Metadata/Items/Gems/SkillGemSkeletalSniper"


def run_cli(script: str, *args):
    return subprocess.run(
        [sys.executable, "-I", str(TOOLS / script), *map(str, args)], cwd=REPO,
        text=True, encoding="utf-8", errors="replace", capture_output=True, timeout=120,
    )


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


class ScratchTest(unittest.TestCase):
    def setUp(self):
        SCRATCH.mkdir(exist_ok=True)
        directory = tempfile.TemporaryDirectory(prefix="poe2-test-", dir=SCRATCH)
        self.addCleanup(directory.cleanup)
        self.scratch = Path(directory.name)

    def cli(self, script, *args):
        result = run_cli(script, *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result


class ExtractionTests(ScratchTest):
    def test_public_directory_is_refused_by_default(self):
        outside = self.scratch / "bad-output"
        result = run_cli("extract.py", "--pob", self.scratch / "missing", "--tree-version", "0_5",
                         "--out", outside)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("data/poe2/raw/", result.stderr)
        self.assertFalse(outside.exists())

    def test_csv_keeps_non_strict_pairs_and_reports_physical_lines(self):
        input_dir = self.scratch / "pob"
        input_dir.mkdir()
        translation_dir = self.scratch / "ko"
        translation_dir.mkdir()
        translation_file = translation_dir / "POE2 sample.csv"
        translation_file.write_bytes(
            '\ufeff"English","한글"\r\n"multiple\r\nlines","여러 줄"\r\n'
            '"plain"x,"기본"\r\n"one"\r\n\r\n"many","fields","here"\r\n'.encode("utf-8")
        )
        (translation_dir / "PoE1.csv").write_bytes(b"\xff")
        files = InputFiles(input_dir)
        pack = translations(files, translation_dir)
        self.assertEqual(pack["files"], {translation_file.name: [
            ["English", "한글"], ["multiple\r\nlines", "여러 줄"], ["plainx", "기본"],
        ]})
        self.assertEqual(pack["parseWarnings"], [
            {"file": translation_file.name, "line": 4, "reason": "strictCsvError"},
            {"file": translation_file.name, "line": 5, "reason": "columnCount", "columns": 1},
            {"file": translation_file.name, "line": 6, "reason": "columnCount", "columns": 0},
            {"file": translation_file.name, "line": 7, "reason": "columnCount", "columns": 3},
        ])
        self.assertEqual(files.sha256, {
            "../ko/POE2 sample.csv": hashlib.sha256(translation_file.read_bytes()).hexdigest(),
        })

    @unittest.skipUnless(POB.is_dir(), "PoB 0.23.1 fixture is unavailable")
    def test_actual_pack_counts_hashes_and_byte_determinism(self):
        for name in ("first", "second"):
            args = ["--pob", POB, "--tree-version", "0_5", "--out", self.scratch / name,
                    "--summary", self.scratch / f"{name}.json", "--allow-out-anywhere"]
            if KO.is_dir():
                args.extend(("--ko", KO))
            self.cli("extract.py", *args)

        first = self.scratch / "first"
        second = self.scratch / "second"
        filenames = {
            "meta.json", "gems.json", "skills.json", "tree.json", "bases.json", "mods.json",
            "essences.json", "questRewards.json", "misc.json", "uniques.json",
        }
        if KO.is_dir():
            filenames.add("ko.json")
        for directory in (first, second):
            self.assertEqual({path.name for path in directory.iterdir()}, filenames)
        for filename in sorted(filenames):
            with self.subTest(filename=filename):
                self.assertEqual((first / filename).read_bytes(), (second / filename).read_bytes())
        self.assertEqual((self.scratch / "first.json").read_bytes(), (self.scratch / "second.json").read_bytes())

        summary = read_json(self.scratch / "first.json")
        self.assertEqual(summary["gems"], {
            "declarations": 967, "effectiveKeys": 966, "duplicateKeys": [SKELETAL_SNIPER],
            "tierPositive": {"active": 234, "support": 450,
                             "supportNameRanks": {"I": 111, "II": 117, "III": 32, "IV": 1, "none": 189}},
            "tierZero": {"active": 165, "support": 117},
        })
        self.assertEqual(summary["tree"], {"topLevelNodes": 4912})
        self.assertEqual(summary["bases"], {
            "declarations": 1833, "runeforgedDeclarations": 778, "hiddenDeclarations": 283,
            "runeforgedAndHiddenDeclarations": 255, "normalDeclarations": 1027,
            "loadedDeclarations": 1820, "overrides": 65, "effectiveKeys": 1755,
            "effectiveNonHiddenKeys": 1504, "effectiveNormalKeys": 999,
            "unloadedFiles": ["Data/Bases/traptool.lua"],
        })
        self.assertEqual(summary["mods"], {
            "byFile": {"ModItem": 2550, "ModCharm": 51, "ModFlask": 78, "ModJewel": 377,
                       "ModCorrupted": 127, "ModItemExclusive": 5495, "ModVeiled": 388,
                       "ModIncursionLimb": 12, "ModRunes": 287},
            "modItemTypes": {"Prefix": 1395, "Suffix": 1155},
            "modItemWeightValues": [0, 1],
            "modRunesTypes": {"Rune": 213, "Idol": 35, "SoulCore": 34, "AbyssalEye": 4, "CongealedMist": 1},
        })
        self.assertEqual(summary["uniques"], {"blocks": 435, "distinctNames": 433})
        self.assertEqual(summary["monsterLifeTable"], {"length": 100, "65": 6555, "80": 31065})
        definitions = {
            "SkillAssets": 0, "act_dex": 64, "act_int": 154, "act_str": 143, "minion": 53,
            "other": 174, "spectre": 260, "sup_dex": 162, "sup_int": 206, "sup_str": 223,
        }
        functions = {
            "SkillAssets": 0, "act_dex": 2, "act_int": 5, "act_str": 4, "minion": 1,
            "other": 2, "spectre": 0, "sup_dex": 0, "sup_int": 1, "sup_str": 0,
        }
        self.assertEqual(summary["skills"], {
            "definitionsByFile": {f"Data/Skills/{key}.lua": count for key, count in definitions.items()},
            "functionsByFile": {f"Data/Skills/{key}.lua": count for key, count in functions.items()},
        })
        if KO.is_dir():
            self.assertEqual(summary["ko"], {"pairs": 43140, "parseWarnings": 145})
            data = read_json(first / "ko.json")
            self.assertIn({"file": "POE2 0.3.0 번역 추가.csv", "line": 1931, "reason": "strictCsvError"},
                          data["parseWarnings"])
        else:
            self.assertEqual(summary["ko"], {"pairs": 0, "parseWarnings": 0})

        self.assert_public_meta(summary["meta"], read_json(first / "meta.json"))
        public_text = (self.scratch / "first.json").read_text(encoding="utf-8")
        for prohibited in ("increased", "function(", "Bloodstone Amulet", '"The Anvil"'):
            self.assertNotIn(prohibited, public_text)

    def assert_public_meta(self, meta, raw_meta):
        self.assertEqual(meta, raw_meta)
        self.assertEqual({key: value for key, value in meta.items()
                          if key not in ("sourceSha256", "pobManifestSha1")}, {
            "schema": "skyrimpoe.poe2raw", "schemaVersion": 1, "game": "poe2",
            "gameVersion": "0.5", "pobVersion": "0.23.1", "treeVersion": "0_5", "extractorVersion": "1",
        })
        expected_files = {"manifest.xml", "TreeData/0_5/tree.lua"}
        expected_files.update(f"Modules/{name}.lua"
                              for name in ("Data", "CalcDefence", "CalcOffence", "CalcPerform", "CalcSetup"))
        expected_files.update(path.relative_to(POB).as_posix()
                              for dir_name in ("Bases", "Skills", "Uniques")
                              for path in (POB / "Data" / dir_name).rglob("*.lua"))
        expected_files.update(f"Data/{name}.lua" for name in (
            "Gems", "Essence", "QuestRewards", "Misc", "ModItem", "ModCharm", "ModFlask", "ModJewel",
            "ModCorrupted", "ModItemExclusive", "ModVeiled", "ModIncursionLimb", "ModRunes",
        ))
        if KO.is_dir():
            expected_files.update(Path(os.path.relpath(path, POB)).as_posix()
                                  for path in KO.glob("*.csv") if path.name.lower().startswith("poe2"))
        self.assertEqual(set(meta["sourceSha256"]), expected_files)
        for filename, sha256 in meta["sourceSha256"].items():
            self.assertEqual(sha256, hashlib.sha256((POB / filename).read_bytes()).hexdigest(), filename)
        manifest = ET.parse(POB / "manifest.xml")
        expected_sha1 = {record.get("name"): record.get("sha1") for record in manifest.findall("File")
                         if record.get("name") in expected_files and record.get("sha1") is not None}
        self.assertEqual(meta["pobManifestSha1"], expected_sha1)

    @unittest.skipUnless(POB.is_dir(), "PoB 0.23.1 fixture is unavailable")
    def test_actual_tree_diff_identifies_reused_node_10079(self):
        old = self.scratch / "old"
        new = self.scratch / "new"
        for game_version, tree_version, directory in [("0.4", "0_4", old), ("0.5", "0_5", new)]:
            self.cli("extract.py", "--pob", POB, "--tree-version", tree_version,
                     "--game-version", game_version, "--out", directory, "--allow-out-anywhere", "--only", "tree")
            self.assertEqual({path.name for path in directory.iterdir()}, {"meta.json", "tree.json"})
        output = self.scratch / "diff.json"
        markdown = self.scratch / "diff.md"
        self.cli("diffraw.py", "--old", old, "--new", new, "--out", output, "--md", markdown)
        items = read_json(output)["kinds"]["tree"]["items"]
        node = next(item for item in items if item["key"] == 10079)
        self.assertEqual(node["classification"], "meaningChange")
        self.assertIn("/name", node["changedFields"])
        self.assertIn("meaningChange", markdown.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
