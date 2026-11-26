"""PoE2 기대 방어표를 독립 계산값과 합성 Lua 자료로 검증합니다."""

import hashlib
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from expected_defense import DEFAULT_POB, OUTPUT, build_expected_defense, read_resist_tiers
from extract import InputFiles, json_bytes


COLUMNS = ("bestArmour", "bestEvasion", "bestEnergyShield", "bestResistTier")


@pytest.fixture(scope="module")
def pob_pack():
    if not DEFAULT_POB.is_dir():
        pytest.skip("PoB-PoE2 0.23.1 자료 폴더가 없습니다.")
    return build_expected_defense(DEFAULT_POB)


def test_matches_independent_answers(pob_pack):
    rows = pob_pack["rows"]
    assert [row["level"] for row in rows] == list(range(1, 101))
    for row in rows:
        assert tuple(row) == ("level", *COLUMNS)
        assert all(type(row[column]) is float for column in COLUMNS)
    for column in COLUMNS:
        values = [row[column] for row in rows]
        assert values == sorted(values)

    answers = {
        1: (132.0, 93.0, 70.0, 8.0),
        2: (132.0, 93.0, 70.0, 8.0),
        11: (239.0, 232.0, 105.0, 8.0),
        12: (290.0, 232.0, 111.0, 13.0),
        24: (400.0, 357.0, 144.0, 18.0),
        38: (711.0, 636.0, 205.0, 23.0),
        50: (926.0, 832.0, 268.0, 28.0),
        65: (1301.0, 1117.0, 374.0, 33.0),
        80: (1436.0, 1284.0, 417.0, 38.0),
        100: (1436.0, 1284.0, 417.0, 43.0),
    }
    actual = {level: tuple(rows[level - 1][column] for column in COLUMNS) for level in answers}
    assert actual == answers
    assert read_resist_tiers(InputFiles(DEFAULT_POB)) == [
        (1, 8.0), (12, 13.0), (24, 18.0), (36, 23.0),
        (48, 28.0), (60, 33.0), (71, 38.0), (82, 43.0),
    ]


def test_hidden_bases_change_level_38_armour(pob_pack):
    with_hidden = build_expected_defense(DEFAULT_POB, include_hidden=True)
    assert pob_pack["rows"][37]["bestArmour"] == 711.0
    assert with_hidden["rows"][37]["bestArmour"] == 903.0


def test_source_hashes_and_deterministic_bytes(pob_pack):
    paths = (
        "manifest.xml", "Data/ModItem.lua", "Data/Bases/body.lua", "Data/Bases/helmet.lua",
        "Data/Bases/gloves.lua", "Data/Bases/boots.lua",
    )
    assert pob_pack["meta"] == {
        "schema": "skyrimpoe.expectedDefense", "schemaVersion": 1, "game": "poe2",
        "gameVersion": "0.5", "pobVersion": "0.23.1", "extractorVersion": "expected_defense/1",
        "sourceSha256": {path: hashlib.sha256((DEFAULT_POB / path).read_bytes()).hexdigest() for path in paths},
    }
    first = json_bytes(pob_pack, indent=2)
    second = json_bytes(build_expected_defense(DEFAULT_POB), indent=2)
    assert first.endswith(b"\n")
    assert first == second == OUTPUT.read_bytes()


@pytest.mark.skipif(not DEFAULT_POB.is_dir(), reason="PoB-PoE2 0.23.1 자료 폴더가 없습니다.")
def test_isolated_cli_check():
    script = Path(__file__).with_name("expected_defense.py").resolve()
    result = subprocess.run(
        [sys.executable, "-I", str(script), "--pob", str(DEFAULT_POB), "--check"],
        capture_output=True, timeout=120,
    )
    assert result.returncode == 0, result.stderr.decode(errors="replace")


def test_synthetic_bases_and_affixes(tmp_path):
    (tmp_path / "manifest.xml").write_text('<Manifest><Version number="0.23.1"/></Manifest>', encoding="utf-8")
    bases = tmp_path / "Data/Bases"
    bases.mkdir(parents=True)
    (bases / "body.lua").write_text('''
local itemBases = ...
itemBases["Plain"] = { subType = "Armour", armour = { Armour = 10 } }
itemBases["Step"] = { subType = "Armour", armour = { Armour = 500 }, req = { level = 3 } }
itemBases["Step"] = { subType = "Armour", armour = { Armour = 16 }, req = { level = 3 } }
itemBases["Hidden"] = { subType = "Armour", armour = { Armour = 500 } }
itemBases["Hidden"] = { subType = "Armour", armour = { Armour = 500 }, hidden = true }
itemBases["Not for sale"] = { subType = "Armour", armour = { Armour = 500 }, tags = { not_for_sale = true } }
itemBases["Demigods"] = { subType = "Armour", armour = { Armour = 500 }, tags = { demigods = true } }
itemBases["Hybrid"] = { subType = "Armour/Evasion", armour = { Armour = 500, Evasion = 500 } }
itemBases["Ward"] = { subType = "Armour", armour = { Ward = 500 } }
''', encoding="utf-8")
    (bases / "helmet.lua").write_text('''
local itemBases = ...
itemBases["Helm"] = { subType = "Armour", armour = { Armour = 3 }, req = { str = 12 } }
itemBases["Hood"] = { subType = "Evasion", armour = { Evasion = 7 }, req = { level = 2 } }
itemBases["Crown"] = { subType = "Energy Shield", armour = { EnergyShield = 4 } }
''', encoding="utf-8")
    for slot in ("gloves", "boots"):
        (bases / f"{slot}.lua").write_text("local itemBases = ...\n", encoding="utf-8")
    (tmp_path / "Data/ModItem.lua").write_text('''
return {
    FireResist1 = { type = "Suffix", "+(6-10)% to Fire Resistance", level = 1 },
    FireResist2 = { type = "Suffix", "+(2-4)% to Fire Resistance", level = 2 },
    FireResist3 = { type = "Suffix", "+(11-15)% to Fire Resistance", level = 3 },
    FireResist4 = { type = "Prefix", "+(100-200)% to Fire Resistance", level = 1 },
    FireResist5 = { type = "Suffix", "+(100-200)% to Maximum Fire Resistance", level = 1 },
    HandWrapsFireResist1 = { type = "Suffix", "+(100-200)% to Fire Resistance", level = 1 },
}
''', encoding="utf-8")

    rows = build_expected_defense(tmp_path)["rows"]
    assert [tuple(row[column] for column in COLUMNS) for row in rows[:3]] == [
        (13.0, 0.0, 4.0, 8.0), (13.0, 7.0, 4.0, 8.0), (19.0, 7.0, 4.0, 13.0),
    ]


def test_refuses_unpinned_pob_version(tmp_path):
    (tmp_path / "manifest.xml").write_text('<Manifest><Version number="0.22.0"/></Manifest>', encoding="utf-8")
    with pytest.raises(ValueError, match=r"0\.23\.1.*0\.22\.0"):
        build_expected_defense(tmp_path)
