"""Test numeric extraction, provenance, and failure on source wording changes."""
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_rules import CONFIG, DATA, KEYWORDS, MISC, ROOT, SETUP, RulesSource


@pytest.fixture
def fixture_source(tmp_path):
    (tmp_path / "manifest.xml").write_text(
        '<Manifest><Version number="0.23.1"/><File name="Data/Misc.lua" sha1="abcd"/></Manifest>',
        encoding="utf-8",
    )
    data = tmp_path / MISC
    data.parent.mkdir()
    data.write_text('data.constants = {\n\t["Test"] = 750.5,\n}\n', encoding="utf-8")
    formula = tmp_path / DATA
    formula.parent.mkdir()
    formula.write_text("other_expression = 0\nlocal chance = value * 0.12 + 50\n", encoding="utf-8")
    keywords = tmp_path / KEYWORDS
    keywords.parent.mkdir(parents=True)
    keywords.write_text(json.dumps([{"Id": "Test", "Definition": "Before. Exact wording. After."}], indent=2),
                        encoding="utf-8")
    return RulesSource(tmp_path, tmp_path)


def test_lua_values_and_formula_coefficients_are_parsed(fixture_source):
    source = fixture_source
    assert source.assignments(MISC, "section", {"rate": "Test"}) == {"rate": 750.5}
    assert source.formula("section", DATA, "local chance = value * {} + {}", ("scale", "offset")) == {
        "scale": 0.12, "offset": 50
    }
    assert source.provenance["section.scale"] == {
        "grade": "pob", "references": [{"file": DATA, "line": 2}]
    }
    assert list(source.meta()["pobManifestSha1"]) == [MISC]
    assert set(source.meta()["sourceSha256"]) == {"manifest.xml", MISC, DATA, KEYWORDS}


def test_source_formula_and_exact_sentence_fail_on_drift(fixture_source):
    source = fixture_source
    assert source.compare_sentence("Test", "Exact wording.") == {
        "file": KEYWORDS, "line": 4, "id": "Test"
    }
    assert source.excerpts["Test", "Exact wording."]["excerpt"] == "Exact wording."
    source.keywords["Test"] = "Before. A prefix before Exact wording. After."
    with pytest.raises(ValueError, match="source wording changed"):
        source.compare_sentence("Test", "Exact wording.")
    source.keywords["Test"] = "Before. Exact wording, unless something changes. After."
    with pytest.raises(ValueError, match="source wording changed"):
        source.compare_sentence("Test", "Exact wording.")
    with pytest.raises(ValueError, match="source wording changed"):
        source.compare_sentence("Test", "Different wording.")
    with pytest.raises(ValueError, match="expected one row matching"):
        source.formula("section", DATA, "local chance = value / {} + {}", ("scale", "offset"))


def test_committed_rules_cite_each_value_and_level_table():
    rules = json.loads((ROOT / "data/packs/rules.json").read_bytes())
    excerpts = json.loads((ROOT / "data/poe2/rules-provenance.json").read_bytes())
    assert '"excerpt"' not in json.dumps(rules)
    assert excerpts["gameDataVersion"] == rules["supplementalGameData"]["version"]
    assert excerpts["excerpts"] and all(entry["excerpt"] for entry in excerpts["excerpts"])
    provenance = rules["provenance"]
    for group, keys in rules.items():
        if group in {"meta", "rulesSource", "supplementalGameData", "project", "provenance"}:
            continue
        for name in keys:
            origin = provenance[f"{group}.{name}"]
            assert origin["grade"] in {"pob", "gameData", "official"}
            for ref in origin["references"] + origin.get("pobReferences", []):
                assert ref["file"] in rules["meta"]["sourceSha256"]
            if origin.get("pobDiffers"):
                assert origin["pobReferences"]
    assert all(len(table) == 100 for table in rules["levelTables"].values())
    assert rules["armour"]["physicalReductionCapPercent"] == 90
    assert provenance["armour.physicalReductionCapPercent"]["references"] == [
        {"file": MISC, "line": 149}, {"file": DATA, "line": 240}
    ]
    assert "Data/SkillStatMap.lua" in rules["meta"]["sourceSha256"]
    assert "skillStatMap" not in rules
    assert rules["poison"]["baseMaxStacks"] == 1
    assert provenance["poison.baseMaxStacks"]["references"] == [{"file": SETUP, "line": 55}]
    assert rules["poise"]["uniqueThresholdMorePercent"] == 500
    assert rules["poise"]["mapBossThresholdMorePercent"] == 213
    assert rules["poise"]["fullBuildupPercent"] == 100
    assert provenance["poise.fullBuildupPercent"]["grade"] == "gameData"
    assert provenance["poise.uniqueThresholdMorePercent"]["references"][0] == {"file": CONFIG, "line": 2006}
    assert provenance["poise.mapBossThresholdMorePercent"]["references"][0] == {"file": CONFIG, "line": 2007}
    assert rules["levelTables"]["mapLevelLifeMult"] == [1] * 100
