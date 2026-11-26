"""PoB-PoE2 0.23.1에서 레벨별 기대 방어표를 추출합니다."""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

if __package__ in ("", None):
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from extract import InputFiles, assignments, json_bytes, returned, write_atomic
from luadata import field, numeric_key, parse, table_array, table_fields


POB_VERSION = "0.23.1"
DEFAULT_POB = Path(r"G:\poe\PathOfBuilding-PoE2-0.23.1")
OUTPUT = Path(__file__).resolve().parents[2] / "data/poe2/expected-defense-0.5.json"
SLOTS = ("body", "helmet", "gloves", "boots")
DEFENSE_KEYS = {"Armour": "Armour", "Evasion": "Evasion", "Energy Shield": "EnergyShield"}
RESIST_TEXT = re.compile(r"\+\(([0-9]+)-([0-9]+)\)% to Fire Resistance")


def is_eligible_base(base: dict, *, include_hidden: bool = False) -> bool:
    if not include_hidden and field(base, "hidden") is True:
        return False
    tags = field(base, "tags")
    return tags is None or all(field(tags, tag) is None for tag in ("not_for_sale", "demigods"))


def read_defense_bases(
    files: InputFiles, *, include_hidden: bool = False,
) -> dict[tuple[str, str], list[tuple[int, float]]]:
    bases = {}
    for slot in SLOTS:
        path = Path(f"Data/Bases/{slot}.lua")
        entries = assignments(parse(files.lua(path), path.as_posix()), path, "itemBases")
        # Lua에서 나중에 대입한 베이스가 앞선 선언 전체를 덮습니다.
        effective = {entry["key"]: entry["value"] for entry in entries}
        for name, base in effective.items():
            kind = field(base, "subType")
            if kind not in DEFENSE_KEYS or not is_eligible_base(base, include_hidden=include_hidden):
                continue
            armour = field(base, "armour")
            value = None if armour is None else field(armour, DEFENSE_KEYS[kind])
            if value is None:
                continue
            req = field(base, "req")
            level = 1 if req is None else field(req, "level", 1)
            value = numeric_key(value)
            if type(level) is not int:
                raise ValueError(f"{path.as_posix()}의 {name} 요구 레벨은 정수여야 합니다.")
            if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
                raise ValueError(f"{path.as_posix()}의 {name} 방어값은 0 이상의 유한한 수여야 합니다.")
            bases.setdefault((slot, kind), []).append((level, float(value)))
    return bases


def read_resist_tiers(files: InputFiles) -> list[tuple[int, float]]:
    path = "Data/ModItem.lua"
    tiers = []
    for key, mod in dict(table_fields(returned(files, path))).items():
        if not isinstance(key, str) or not re.fullmatch(r"FireResist[0-9]+", key):
            continue
        if field(mod, "type") != "Suffix":
            continue
        for line in table_array(mod):
            match = RESIST_TEXT.fullmatch(line) if isinstance(line, str) else None
            if match is not None:
                level = field(mod, "level")
                if type(level) is not int:
                    raise ValueError(f"{path}의 {key} 요구 레벨은 정수여야 합니다.")
                low, high = map(int, match.groups())
                tiers.append((level, (low + high) / 2.0))
    if not tiers:
        raise ValueError(f"{path}에서 화염 저항 등급을 찾지 못했습니다.")
    return sorted(tiers)


def best_at(entries, level: int) -> float:
    return max((value for required, value in entries if required <= level), default=0.0)


def build_expected_defense(pob: Path, *, include_hidden: bool = False) -> dict:
    files = InputFiles(pob)
    manifest = ET.fromstring(files.read(Path("manifest.xml")))
    version = manifest.find("Version")
    if version is None or version.get("number") != POB_VERSION:
        found = version.get("number") if version is not None else None
        raise ValueError(f"PoB 판은 {POB_VERSION}이어야 합니다. 확인된 판은 {found or '없음'}입니다.")

    bases = read_defense_bases(files, include_hidden=include_hidden)
    tiers = read_resist_tiers(files)

    def best_sum(kind: str, level: int) -> float:
        return sum(best_at(bases.get((slot, kind), ()), level) for slot in SLOTS)

    rows = [
        {
            "level": level,
            "bestArmour": best_sum("Armour", level),
            "bestEvasion": best_sum("Evasion", level),
            "bestEnergyShield": best_sum("Energy Shield", level),
            "bestResistTier": best_at(tiers, level),
        }
        for level in range(1, 101)
    ]
    return {
        "meta": {
            "schema": "skyrimpoe.expectedDefense",
            "schemaVersion": 1,
            "game": "poe2",
            "gameVersion": "0.5",
            "pobVersion": POB_VERSION,
            "extractorVersion": "expected_defense/1",
            "sourceSha256": dict(sorted(files.sha256.items())),
        },
        "rows": rows,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pob", type=Path, default=DEFAULT_POB, help="PoB-PoE2 폴더 (기본값: %(default)s)")
    parser.add_argument("--check", action="store_true", help="재추출한 바이트를 저장소의 기대 방어표와 비교합니다.")
    args = parser.parse_args(argv)
    try:
        payload = json_bytes(build_expected_defense(args.pob), indent=2)
        if args.check:
            if payload != OUTPUT.read_bytes():
                raise ValueError(f"재추출한 결과가 {OUTPUT.name}과 일치하지 않습니다.")
        else:
            write_atomic(OUTPUT, payload)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, ET.ParseError) as error:
        parser.exit(1, f"기대 방어표 추출 실패: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
