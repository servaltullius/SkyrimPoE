"""PoE2 젬 카탈로그, 기대 방어표, 상수를 세대 1 core 팩으로 조립합니다."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

if __package__ in ("", None):
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from extract import json_bytes, write_atomic


ROOT = Path(__file__).resolve().parents[2]
GEMS = "data/packs/gen1/gems.json"
DEFENSE = "data/poe2/expected-defense-0.5.json"
CONSTANTS = "data/poe2/gen1-core-constants.json"
OUTPUT = "data/packs/gen1/core.json"


def check_meta(pack: dict, path: str, **expected) -> None:
    meta = pack.get("meta")
    if not isinstance(meta, dict):
        raise ValueError(f"{path}의 meta는 객체여야 합니다.")
    for field, value in expected.items():
        actual = meta.get(field)
        if type(actual) is not type(value) or actual != value:
            raise ValueError(f"{path}의 meta.{field} 값은 {value!r}이어야 합니다.")


def assemble_core(root: Path) -> bytes:
    inputs = {}
    hashes = {}
    for path in (GEMS, DEFENSE, CONSTANTS):
        raw = (root / path).read_bytes()
        pack = json.loads(raw)
        if not isinstance(pack, dict):
            raise ValueError(f"{path}의 최상위 값은 객체여야 합니다.")
        inputs[path] = pack
        hashes[path] = hashlib.sha256(raw).hexdigest()

    gem_pack = inputs[GEMS]
    check_meta(gem_pack, GEMS, schema="skyrimpoe.gems", schemaVersion=1,
               game="poe2", gameVersion="0.5", contentGeneration=1)
    gems = gem_pack.get("gems")
    if not isinstance(gems, dict):
        raise ValueError(f"{GEMS}의 gems는 객체여야 합니다.")

    defense_pack = inputs[DEFENSE]
    check_meta(defense_pack, DEFENSE, schema="skyrimpoe.expectedDefense", game="poe2", gameVersion="0.5")
    rows = defense_pack.get("rows")
    if not isinstance(rows, list) or len(rows) != 100:
        raise ValueError(f"{DEFENSE}의 rows는 정확히 100행인 배열이어야 합니다.")

    constants = inputs[CONSTANTS].get("constants")
    if not isinstance(constants, dict) or constants.keys() != {"monsterDamageMult"}:
        raise ValueError(f"{CONSTANTS}의 constants에는 monsterDamageMult만 있어야 합니다.")
    multiplier = constants["monsterDamageMult"]
    if type(multiplier) not in (int, float) or not 0 < multiplier <= sys.float_info.max:
        raise ValueError(f"{CONSTANTS}의 monsterDamageMult는 유한한 양수여야 합니다.")

    return json_bytes({
        "meta": {
            "schema": "skyrimpoe.core",
            "schemaVersion": 2,
            "game": "poe2",
            "gameVersion": "0.5",
            "contentGeneration": 1,
            "pobVersion": "0.23.1",
            "extractorVersion": "assemble_core/1",
            "sourceSha256": hashes,
        },
        "constants": constants,
        "tables": {"expectedDefense": rows},
        "classes": [],
        "gems": gems,
    }, indent=2)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="다시 조립한 결과와 저장된 파일을 바이트 단위로 비교합니다.")
    args = parser.parse_args(argv)
    output = ROOT / OUTPUT
    try:
        contents = assemble_core(ROOT)
        if args.check:
            if output.read_bytes() != contents:
                raise ValueError(f"{OUTPUT}이 다시 조립한 결과와 다릅니다.")
        else:
            write_atomic(output, contents)
    except (OSError, ValueError) as error:
        print(f"세대 1 core 조립 실패: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
