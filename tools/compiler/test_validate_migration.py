import json

import pytest

from compile_tree_poe2 import ROOT
from validate_migration import load_table


@pytest.fixture
def example(tmp_path):
    readme = (ROOT / "data/migrations/README.md").read_text(encoding="utf-8")
    path = tmp_path / "gen1-to-gen2.json"
    path.write_text(readme.split("```json\n")[1].split("```")[0], encoding="utf-8")
    return path


def test_example_table_enumerates_source_and_validates_target(example):
    source = {"cls": {"cls.Witch"}, "node": {"17788"}, "gem": {"gem.OldSpell"}}
    target = {"cls": {"cls.Witch"}, "node": {"17788"}, "gem": set()}
    assert load_table(example, source, target)["toGeneration"] == 2
    source["node"].add("1")
    with pytest.raises(ValueError, match="enumerate exactly"):
        load_table(example, source, target)


@pytest.mark.parametrize(("kind", "identifier", "replacement"), [
    ("node", "17788", {"to": True}),
    ("node", "17788", {"to": 0x100000000}),
    ("cls", "cls.Witch", {"to": "asc.Lich"}),
    ("gem", "gem.OldSpell", {"to": "gem.Other", "retire": True}),
    ("gem", "gem.OldSpell", {"retire": True, "refund": {
        "unit": "item", "destination": "passivePoints", "cap": 1, "oncePer": "sourceInstance",
    }}),
])
def test_invalid_entry_is_rejected(example, kind, identifier, replacement):
    data = json.loads(example.read_bytes())
    data["entries"][kind][identifier] = replacement
    example.write_text(json.dumps(data), encoding="utf-8")
    with pytest.raises(ValueError):
        load_table(example)


def test_duplicate_key_and_generation_gap_are_rejected(example):
    original = example.read_text(encoding="utf-8")
    example.write_text(original.replace('"schemaVersion": 1', '"schemaVersion": 1, "schemaVersion": 1'), encoding="utf-8")
    with pytest.raises(ValueError, match="Duplicate JSON key"):
        load_table(example)
    example.write_text(original.replace('"toGeneration": 2', '"toGeneration": 3'), encoding="utf-8")
    with pytest.raises(ValueError, match="adjacent"):
        load_table(example)
