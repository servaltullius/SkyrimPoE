"""세대 1 core 팩의 본문, 출처 해시, 재생성 및 입력 검사를 검증합니다."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from assemble_core import CONSTANTS, DEFENSE, GEMS, OUTPUT, ROOT, assemble_core


def test_core_contents_and_reproducibility():
    raw = {path: (ROOT / path).read_bytes() for path in (GEMS, DEFENSE, CONSTANTS)}
    assembled = assemble_core(ROOT)
    assert assembled == assemble_core(ROOT) == (ROOT / OUTPUT).read_bytes()

    core = json.loads(assembled)
    assert core.keys() == {"meta", "constants", "tables", "classes", "gems"}
    assert core["meta"] == {
        "schema": "skyrimpoe.core",
        "schemaVersion": 2,
        "game": "poe2",
        "gameVersion": "0.5",
        "contentGeneration": 1,
        "pobVersion": "0.23.1",
        "extractorVersion": "assemble_core/1",
        "sourceSha256": {path: hashlib.sha256(contents).hexdigest() for path, contents in raw.items()},
    }
    assert core["constants"] == {"monsterDamageMult": 1.5}
    assert core["classes"] == []
    assert core["tables"] == {"expectedDefense": json.loads(raw[DEFENSE])["rows"]}
    assert core["gems"] == json.loads(raw[GEMS])["gems"]
    assert assembled == (json.dumps(core, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def test_check_command():
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/poe2/assemble_core.py"), "--check"], check=True)


@pytest.fixture
def inputs(tmp_path):
    packs = {
        GEMS: {"meta": {"schema": "skyrimpoe.gems", "schemaVersion": 1,
                        "game": "poe2", "gameVersion": "0.5", "contentGeneration": 1}, "gems": {}},
        DEFENSE: {"meta": {"schema": "skyrimpoe.expectedDefense", "game": "poe2", "gameVersion": "0.5"},
                  "rows": [{"level": level} for level in range(1, 101)]},
        CONSTANTS: {"constants": {"monsterDamageMult": 1.5}},
    }

    def write_input(path, field, value):
        for relative, pack in packs.items():
            target = tmp_path / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if relative == path:
                target_object = pack["meta"] if field in pack.get("meta", {}) else pack
                target_object[field] = value
            target.write_text(json.dumps(pack), encoding="utf-8")

    return write_input


@pytest.mark.parametrize(("path", "field", "value"), [
    (GEMS, "schema", "skyrimpoe.core"),
    (GEMS, "schemaVersion", True),
    (GEMS, "game", "poe1"),
    (GEMS, "gameVersion", "0.4"),
    (GEMS, "contentGeneration", 0),
    (DEFENSE, "schema", "skyrimpoe.gems"),
    (DEFENSE, "game", "poe1"),
    (DEFENSE, "gameVersion", "0.4"),
    (DEFENSE, "rows", []),
    (CONSTANTS, "constants", {"monsterDamageMult": 1.5, "resistCap": 75}),
    (CONSTANTS, "constants", {"monsterDamageMult": 0}),
    (CONSTANTS, "constants", {"monsterDamageMult": True}),
    (CONSTANTS, "constants", {"monsterDamageMult": float("inf")}),
])
def test_rejects_invalid_input(tmp_path, inputs, path, field, value):
    inputs(path, field, value)
    with pytest.raises(ValueError, match=field):
        assemble_core(tmp_path)
