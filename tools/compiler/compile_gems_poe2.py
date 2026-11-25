#!/usr/bin/env python3
"""Regenerate the generation-1 gem pack, effect report and ID baseline.

    python -I tools/compiler/compile_gems_poe2.py
    python -I tools/compiler/compile_gems_poe2.py --check

Use the PoE2 0.5 raw pack from tools/poe2/extract.py and the same PoB distribution
for the stat description templates and Lua source locations. Cyclone's values
come from the repository's generation-0 core pack.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import sys
from typing import Any
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "poe2"))
from extract import InputFiles, json_bytes, returned, write_atomic  # noqa: E402
from luadata import field, table_array, table_fields  # noqa: E402

DEFAULT_RAW = ROOT / "data/poe2/raw/0.5"
DEFAULT_POB = Path("G:/poe/PathOfBuilding-PoE2-0.23.1")
BUNDLE_PATH = ROOT / "data/poe2/gem-bundle.json"
CORE_PATH = ROOT / "data/packs/core_3_30.json"
PACK_PATH = ROOT / "data/packs/gen1/gems.json"
REPORT_PATH = ROOT / "data/poe2/reports/gem-table.json"
BASELINE_PATH = ROOT / "data/poe2/id-baseline/gen1-gems.json"
GEMS_FILE = "Data/Gems.lua"
STAT_ENGINE = "plugin/src/core/StatEngine.cpp"
DATA_PACK = "plugin/src/core/DataPack.cpp"
SOURCE_KEYS = ("gameId", "variantId", "grantedEffectId", "Tier", "gemFamily")
ATTRIBUTES = ("reqStr", "reqDex", "reqInt")
REF_FIELD = re.compile(r"additional(GrantedEffectId|StatSet)\d*")
CONTENT_ID = re.compile(r"(?:gem|sup)\.[A-Za-z0-9._]{1,60}")
NO_INPUT = {
    "S8": "Spirit 예약 후 지속 또는 자동 반응",
    "S9": "Spirit 예약 후 처치 또는 후속 적중 조건으로 발동",
    "S10": "Spirit 예약 후 에너지가 차면 자동 발동",
    "S11": "연결한 스킬에 적용되는 보조 젬",
}
STATUSES = ("implemented", "partial", "unsupported")

# Existing generation-0 mechanics cover only part of these source keys. The
# anchor names the related live engine input and the reason names the missing part.
PARTIAL_STATS = {
    "base_skill_effect_duration": ("base_skill_effect_duration", "현재 조회는 Molten Shell의 지속 시간에만 적용"),
    "active_skill_base_area_of_effect_radius": ("base_skill_area_of_effect_+%", "범위 보정은 있으나 원천의 기본 반경을 읽지 않음"),
    "support_multiple_damage_+%_final": ("support_multiple_projectile_damage_+%_final", "세대 0 투사체 전용 배율만 읽음"),
    "support_fork_forked_projectile_damage_+%_final": ("support_fork_projectile_damage_+%_final", "세대 0 배율은 분기 후 투사체로 적용을 한정하지 않음"),
    "support_pierce_projectile_damage_+%_final_if_pierced_enemy": ("support_pierce_projectile_damage_+%_final", "세대 0 배율은 관통한 적 조건을 적용하지 않음"),
    "support_pinpoint_critical_strike_chance_+%_final": ("critical_strike_chance_+%", "세대 0은 증가 연산을 지원하며 PoE2 최종 배율을 읽지 않음"),
    "display_herald_of_ice_behaviour": ("secondary_minimum_base_cold_damage", "전령 폭발 피해 계산은 있으나 비전령 공격의 산산조각 조건을 구현하지 않음"),
}


def lua_value(value: Any) -> Any:
    """Decode ordered Lua data, retaining symbolic expressions without evaluating Lua."""
    if not isinstance(value, dict):
        return value
    if "$number" in value:
        return float(value["$number"])
    if "$table" not in value:
        return value
    array = [lua_value(item) for item in table_array(value)]
    fields = table_fields(value)
    if not fields:
        return array
    if not array and all(type(key) is int for key, _ in fields):
        if [key for key, _ in fields] == list(range(1, len(fields) + 1)):
            return [lua_value(item) for _, item in fields]
    if not array and all(isinstance(key, str) for key, _ in fields):
        return {key: lua_value(item) for key, item in fields}
    return {"array": array, "fields": [[lua_value(key), lua_value(item)] for key, item in fields]}


def properties(value: dict, exclude: tuple[str, ...] = ()) -> dict:
    return {key: lua_value(item) for key, item in table_fields(value) if isinstance(key, str) and key not in exclude}


def numbered_fields(value: dict) -> list[tuple[int, Any]]:
    numbered = dict(enumerate(table_array(value), 1))
    for key, item in table_fields(value):
        if type(key) is int:
            numbered[key] = item
    return sorted(numbered.items())


def skill_types(value: dict) -> list[str]:
    keys = table_array(value) + [key for key, enabled in table_fields(value) if enabled]
    return [key["$name"].removeprefix("SkillType.") for key in keys]


def gem_requirement(level_requirement: int, multiplier: int, support: bool) -> int:
    # CalcTools.lua:getGemStatRequirement and Common.lua:round.
    if not multiplier or support:
        return 0
    requirement = math.floor((5 + (level_requirement - 3) * 1.7) * (multiplier / 100) ** 0.9 + 0.5) + 4
    return requirement if requirement >= 8 else 0


def compile_stat_set(index: int, source: dict) -> dict:
    stat_set = properties(source, ("levels",))
    stat_set["index"] = index
    stat_set["levels"] = {
        str(level): {**properties(row), "values": [lua_value(value) for value in table_array(row)]}
        for level, row in numbered_fields(field(source, "levels"))
    }
    return stat_set


def compile_effect(source: dict, gem: dict) -> dict:
    raw = source["value"]
    effect = properties(raw, ("levels", "statSets", "icon"))
    support = effect.get("support", False)
    levels = {}
    for level, raw_level in numbered_fields(field(raw, "levels")):
        row = properties(raw_level)
        row["requirements"] = {
            key: gem_requirement(row["levelRequirement"], gem[key], support) for key in ATTRIBUTES
        }
        if support:
            row["costMultiplier"] = 1 + row.get("manaMultiplier", 0) / 100
        levels[str(level)] = row
    for key in ("skillTypes", "requireSkillTypes", "excludeSkillTypes", "addSkillTypes"):
        effect[key] = skill_types(field(raw, key)) if key in effect else []
    effect.update({
        "grantedEffectId": source["key"],
        "source": {"file": source["file"], "key": source["key"]},
        "support": support,
        "weaponTypes": [weapon for weapon, enabled in effect.get("weaponTypes", {}).items() if enabled],
        "levels": levels,
        "statSets": [compile_stat_set(index, value) for index, value in numbered_fields(field(raw, "statSets"))],
    })
    return effect


def gem_source_location(text: str, key: str, ref_field: str) -> str:
    declaration = rf'^\s*\["{re.escape(key)}"\]\s*=\s*\{{[^\n]*\n'
    for start in re.finditer(declaration, text, re.MULTILINE):
        end = re.search(r'^\t\},', text[start.end():], re.MULTILINE)
        body = text[start.end():start.end() + end.start()] if end else ""
        match = re.search(rf'^\s*{re.escape(ref_field)}\s*=', body, re.MULTILINE)
        if match:
            line = text.count("\n", 0, start.end() + match.start()) + 1
            return f"{GEMS_FILE}:{line} {ref_field}"
    return f"{GEMS_FILE} [{key}] {ref_field}"


def compile_poe2_gem(row: dict, source: dict, skills: dict, gems_lua: str) -> dict:
    raw = dict(table_fields(source["value"]))
    for key in SOURCE_KEYS:
        if row[key] != raw.get(key):
            raise ValueError(f"{row['contentId']}: bundle {key} differs from {source['key']}")
    primary = skills.get(row["grantedEffectId"])
    if primary is None:
        raise ValueError(f"{row['contentId']}: primary effect {row['grantedEffectId']} is missing")
    effect = compile_effect(primary, raw)
    effects = [effect]
    references = []
    missing = []
    bindings = row.get("additionalEffects", {})
    source_refs = [(key, ref) for key, ref in raw.items() if REF_FIELD.fullmatch(key)]
    if set(bindings) - {ref for _, ref in source_refs}:
        raise ValueError(f"{row['contentId']}: bundle lists a reference absent from {GEMS_FILE}")
    stat_sets = {stat_set["label"]: stat_set for stat_set in effect["statSets"]}
    if len(stat_sets) != len(effect["statSets"]):
        raise ValueError(f"{row['contentId']}: duplicate stat set label")
    bound_indices = set()
    for key, ref in source_refs:
        binding = bindings.get(ref, {})
        located = gem_source_location(gems_lua, source["key"], key)
        reference = {"field": key, "ref": ref, "where": located}
        if "condition" in binding:
            reference.update(condition=binding["condition"], conditionSource={**effect["source"], "field": "description"})
        if key.startswith("additionalGrantedEffectId"):
            if ref in skills:
                effects.append(compile_effect(skills[ref], raw))
                reference["grantedEffectId"] = ref
        else:
            # PoB's numbered stat sets have labels and scopes, but omit their game IDs.
            # The bundle records the reviewed association, including any absent set.
            stat_set = stat_sets.get(binding.get("statSet"))
            if stat_set is not None:
                stat_set_index = stat_set["index"]
                if stat_set_index <= 1 or stat_set_index in bound_indices:
                    raise ValueError(f"{row['contentId']}: ambiguous association for {ref}")
                bound_indices.add(stat_set_index)
                reference.update(grantedEffectId=effect["grantedEffectId"], statSet=stat_set_index)
        if "grantedEffectId" not in reference:
            missing.append({"ref": ref, "where": located, "reason": "PoB 0.23.1 Skills에 정의 없음"})
        references.append(reference)
    if {stat_set["index"] for stat_set in stat_sets.values()} - {1} - bound_indices:
        raise ValueError(f"{row['contentId']}: some primary stat sets have no source reference")
    return {
        "name": raw["name"],
        "source": {
            "file": source["file"], "key": source["key"], **{key: raw.get(key) for key in SOURCE_KEYS},
        },
        "tags": [tag for tag, enabled in table_fields(raw["tags"]) if enabled],
        "tagString": raw["tagString"],
        "gemType": raw["gemType"],
        "attributeMultipliers": {key: raw[key] for key in ATTRIBUTES},
        "naturalMaxLevel": raw["naturalMaxLevel"],
        "calculationLevelCount": len(effect["levels"]),
        "effects": effects,
        "references": references,
        "missingInSource": missing,
    }


def compile_borrowed_gem(row: dict, core: dict) -> dict:
    source = core["gems"][row["grantedEffectId"]]
    levels = {
        level: {
            **{key: value for key, value in entry.items() if key not in (*ATTRIBUTES, "manaCost", "spiritCost", "stats")},
            "cost": {"Mana": entry["manaCost"]}, "spiritReservationFlat": entry["spiritCost"],
            "requirements": {key: entry[key] for key in ATTRIBUTES},
        }
        for level, entry in source["levels"].items()
    }
    # The legacy pack has already resolved PoE1 stat rows. Retain every value,
    # including where the pack has the same key in the constant and level tables.
    stats = list(dict.fromkeys(key for entry in source["levels"].values() for key in entry["stats"]))
    effect = {
        **{key: value for key, value in source.items() if key not in ("id", "levels", "constantStats")},
        "grantedEffectId": source["id"], "levels": levels,
        "source": {"file": "data/packs/core_3_30.json", "key": source["id"]},
        "statSets": [{
            "index": 1, "label": source["name"], "stats": stats, "baseFlags": [],
            "constantStats": list(source["constantStats"].items()),
            "levels": {level: {"values": [entry["stats"].get(key) for key in stats]}
                       for level, entry in source["levels"].items()},
            "levelValuesIncludeConstants": True,
        }],
    }
    return {
        "name": source["name"], "borrowedFrom": row["borrowedFrom"],
        "source": {"file": GEMS_FILE, "key": row["sourceKey"], **{key: row[key] for key in SOURCE_KEYS}},
        "tags": row["tags"], "gemType": "Attack", "naturalMaxLevel": row["naturalMaxLevel"],
        "calculationLevelCount": len(levels), "effects": [effect], "references": [], "missingInSource": [],
    }


def stat_keys(stat_set: dict) -> list[str]:
    return list(dict.fromkeys([*stat_set["stats"], *stat_set["baseFlags"],
                               *(pair[0] for pair in stat_set.get("constantStats", []))]))


class StatDescriptions:
    """Read PoB's description scope and its ancestors for the selected stats."""

    def __init__(self, files: InputFiles):
        self.files = files
        self.scopes: dict[str, tuple[str | None, list[tuple[list, dict]]]] = {}

    def matching(self, scope: str | None, stats: set[str]) -> list[dict]:
        if not scope:
            return []
        if scope not in self.scopes:
            name = scope.removeprefix("specific_skill_stat_descriptions/") + ".lua"
            directory = Path("Data/StatDescriptions")
            path = directory / "Specific_Skill_Stat_Descriptions" / name
            if not (self.files.pob / path).is_file():
                path = directory / name
            raw = returned(self.files, path.as_posix())
            self.scopes[scope] = (field(raw, "parent"), [
                (lua_value(field(entry, "stats")), lua_value(entry)) for _, entry in numbered_fields(raw)
            ])
        parent, entries = self.scopes[scope]
        selected = [entry for keys, entry in entries if stats.intersection(keys)]
        return selected + self.matching(parent, stats)


def fingerprint(gem: dict, descriptions: StatDescriptions) -> str:
    effects = []
    for effect in gem["effects"]:
        sets = []
        quality_keys = {stat[0] for key in ("qualityStats", "altQualityStats") for stat in effect.get(key, [])}
        for stat_set in effect["statSets"]:
            # Ordered stat IDs and PoB's formatting rules carry the units, value
            # order, display ranges and conditions independently of magnitudes.
            scope = stat_set.get("statDescriptionScope")
            sets.append({
                "index": stat_set["index"], "label": stat_set["label"], "scope": scope,
                "stats": stat_set["stats"], "baseFlags": stat_set["baseFlags"],
                "constants": [pair[0] for pair in stat_set.get("constantStats", [])],
                "levelFields": sorted({key for level in stat_set["levels"].values() for key in level}),
                "interpolation": sorted({tuple(level.get("statInterpolation", []))
                                         for level in stat_set["levels"].values()}),
                "modifiers": {key: stat_set[key] for key in ("statMap", "baseMods", "notMinionStat") if key in stat_set},
                "descriptions": descriptions.matching(scope, set(stat_keys(stat_set)) | quality_keys),
            })
        effects.append({
            **{key: value for key, value in effect.items() if key not in (
                "levels", "statSets", "castTime", "qualityStats", "altQualityStats", "color", "spiritCost", "source",
            )},
            # 품질 스탯의 적용 대상(PoB 셋째 원소 statSetIndexes)은 행동 필드라 지문에 넣고, 수치(둘째 원소)는 뺀다.
            "qualityStats": {key: [[stat[0], stat[2] if len(stat) > 2 else []] for stat in effect[key]]
                             for key in ("qualityStats", "altQualityStats") if key in effect},
            "levelFields": sorted({key for level in effect["levels"].values() for key in level}),
            "costUnits": sorted({unit for level in effect["levels"].values() for unit in level.get("cost", {})}),
            "statSets": sets,
        })
    meaning = {
        **{key: gem[key] for key in ("name", "tags", "gemType", "shape", "defaultInput", "naturalMaxLevel")},
        "effects": effects,
        "references": [{key: value for key, value in reference.items() if key != "where"}
                       for reference in gem["references"]],
    }
    return hashlib.sha256(json_bytes(meaning)).hexdigest()


def engine_stat_reads() -> dict[str, list[dict]]:
    source = (ROOT / STAT_ENGINE).read_text(encoding="utf-8")
    keys = set(re.findall(r'\bGetStat\("([^"\n]+)"', source))
    return {key: [{"file": STAT_ENGINE, "key": key}] for key in keys}


def effect_rows(gem: dict, engine_reads: dict) -> list[dict]:
    rows = []
    for effect_position, effect in enumerate(gem["effects"]):
        for set_position, stat_set in enumerate(effect["statSets"]):
            for key in stat_keys(stat_set):
                if effect_position or set_position:
                    # DataPack은 첫 효과의 첫 stat set만 읽는다. 조건부·추가 stat set의 키는 같은 GetStat이 있어도 쓰이지 않는다.
                    status = "unsupported"
                    evidence = {"file": DATA_PACK, "key": key, "reason": "DLL은 첫 효과의 첫 stat set만 읽음"}
                elif key in PARTIAL_STATS and PARTIAL_STATS[key][0] in engine_reads:
                    anchor, reason = PARTIAL_STATS[key]
                    status, evidence = "partial", {"reads": engine_reads[anchor], "reason": reason}
                elif key in engine_reads:
                    status, evidence = "implemented", {"reads": engine_reads[key]}
                else:
                    status = "unsupported"
                    evidence = {"file": STAT_ENGINE, "key": key, "reason": "이 키를 읽는 GetStat 호출 없음"}
                rows.append({
                    "grantedEffectId": effect["grantedEffectId"], "statSet": stat_set["index"],
                    "key": key, "status": status, "evidence": evidence,
                })
    for missing in gem["missingInSource"]:
        rows.append({"ref": missing["ref"], "status": "unsupported", "reason": "원천 누락", "evidence": missing})
    return rows


def report_cost(gem: dict) -> list[dict]:
    costs = []
    for effect in gem["effects"]:
        levels = effect["levels"]
        first, twentieth = levels["1"], levels.get("20", {})
        unavailable = "해당 없음"
        units = sorted({unit for level in levels.values() for unit in level.get("cost", {})})
        values = [{"unit": unit, "level1": first.get("cost", {}).get(unit, unavailable),
                   "level20": twentieth.get("cost", {}).get(unit, unavailable)} for unit in units]
        if any("spiritReservationFlat" in level for level in levels.values()):
            values.append({"unit": "Spirit 가산" if effect["support"] else "Spirit 예약",
                           "level1": first.get("spiritReservationFlat", unavailable),
                           "level20": twentieth.get("spiritReservationFlat", unavailable)})
        if effect["support"]:
            # PoB's MORE modifier represents a multiplier via a percent delta.
            values.append({"unit": "비용 배수 증감 (%)", "level1": first.get("manaMultiplier", 0),
                           "level20": twentieth.get("manaMultiplier", 0) if twentieth else unavailable})
        costs.append({"grantedEffectId": effect["grantedEffectId"],
                      "values": values or [{"unit": unavailable, "level1": unavailable, "level20": unavailable}]})
    return costs


def report_requirements(gem: dict) -> dict:
    levels = gem["effects"][0]["levels"]
    unavailable = "해당 없음"
    return {label: levels[level]["requirements"] if level in levels else unavailable
            for label, level in (("level1", "1"), ("level20", "20"))}


def report_row(gem: dict, row: dict, engine_reads: dict) -> dict:
    effects = effect_rows(gem, engine_reads)
    counts = Counter(effect["status"] for effect in effects)
    control = ({"reason": NO_INPUT[gem["shape"]]} if gem["defaultInput"] == "none"
               else {"default": gem["defaultInput"]})
    return {
        "contentId": gem["contentId"], "name": gem["nameKo"],
        "acquisition": {"dropCandidate": row["dropCandidate"], "readyDrop": row["dropCandidate"] and gem["ready"]},
        "form": row["form"], "input": control, "cost": report_cost(gem), "requirements": report_requirements(gem),
        "shape": gem["shape"],
        "effects": effects, "effectCounts": {status: counts[status] for status in STATUSES},
        "missingInSource": gem["missingInSource"], "ready": gem["ready"],
        "storage": {"inventory": "폼 아이템", "GENR": 1, "CORE": "v3 콘텐츠 ID", "GKEY": "v2 콘텐츠 ID"},
    }


def rebuild(raw: Path = DEFAULT_RAW, pob: Path = DEFAULT_POB, bundle: Path = BUNDLE_PATH) -> dict[Path, bytes]:
    def load(path: Path, logical_path: Path) -> Any:
        contents = path.read_bytes()
        input_hashes[logical_path.relative_to(ROOT).as_posix()] = hashlib.sha256(contents).hexdigest()
        return json.loads(contents)

    input_hashes: dict[str, str] = {}
    raw_meta = json.loads((raw / "meta.json").read_bytes())
    if (raw_meta["game"], raw_meta["gameVersion"], raw_meta["pobVersion"]) != ("poe2", "0.5", "0.23.1"):
        raise ValueError("PoE2 0.5 raw pack from PoB 0.23.1 required")
    gems = {entry["key"]: entry for entry in load(raw / "gems.json", DEFAULT_RAW / "gems.json")}
    skills = {entry["key"]: entry for entry in load(raw / "skills.json", DEFAULT_RAW / "skills.json")["entries"]}
    core = load(CORE_PATH, CORE_PATH)
    rows = load(bundle, BUNDLE_PATH)
    files = InputFiles(pob)
    manifest = ET.fromstring(files.read(Path("manifest.xml")))
    if manifest.find("Version").attrib["number"] != raw_meta["pobVersion"]:
        raise ValueError("PoB distribution and raw pack versions differ")
    manifest_hashes = {entry.attrib["name"].replace("\\", "/"): entry.attrib["sha1"]
                       for entry in manifest.findall("File") if "sha1" in entry.attrib}
    gems_lua = files.lua(Path(GEMS_FILE))
    files.read(Path("Modules/CalcTools.lua"))
    files.read(Path("Modules/Common.lua"))
    descriptions = StatDescriptions(files)
    engine_reads = engine_stat_reads()
    catalog = {}
    table = []
    ids: dict[str, dict[str, list[str]]] = {"gem": {}, "sup": {}}
    for row in rows:
        content_id = row["contentId"]
        if not CONTENT_ID.fullmatch(content_id) or content_id in catalog:
            raise ValueError(f"Invalid or duplicate content ID: {content_id}")
        if row.get("borrowedFrom") == "poe1-3.30":
            gem = compile_borrowed_gem(row, core)
        else:
            gem = compile_poe2_gem(row, gems[row["sourceKey"]], skills, gems_lua)
        gem.update({key: row[key] for key in ("contentId", "nameKo", "displayGrade", "shape", "defaultInput", "ready")})
        catalog[content_id] = gem
        table.append(report_row(gem, row, engine_reads))
        ids[content_id.split(".", 1)[0]][content_id] = [gem["name"], fingerprint(gem, descriptions)]
    for path, actual_hash in files.sha256.items():
        expected_hash = raw_meta["sourceSha256"].get(path)
        if expected_hash is not None and actual_hash != expected_hash:
            raise ValueError(f"PoB file differs from raw pack provenance: {path}")
    used_sources = {GEMS_FILE, *(effect["source"]["file"] for gem in catalog.values() for effect in gem["effects"])}
    source_hashes = {key: value for key, value in raw_meta["sourceSha256"].items() if key in used_sources}
    source_hashes.update(files.sha256)
    source_hashes.update(input_hashes)
    meta = {
        "schema": "skyrimpoe.gems", "schemaVersion": 1, "game": "poe2", "contentGeneration": 1,
        "gameVersion": raw_meta["gameVersion"], "pobVersion": raw_meta["pobVersion"],
        "extractorVersion": "compile_gems_poe2/1", "sourceSha256": dict(sorted(source_hashes.items())),
        "pobManifestSha1": {key: manifest_hashes[key] for key in sorted(source_hashes) if key in manifest_hashes},
    }
    pack = {"meta": meta, "gems": catalog}
    report = {
        "contentGeneration": 1, "gameVersion": meta["gameVersion"], "pobVersion": meta["pobVersion"],
        "effectCounts": {status: sum(row["effectCounts"][status] for row in table) for status in STATUSES},
        "gems": table,
    }
    baseline = {"contentGeneration": 1, "entries": ids}
    return {PACK_PATH: json_bytes(pack), REPORT_PATH: json_bytes(report, indent=2),
            BASELINE_PATH: json_bytes(baseline, indent=2)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path, default=DEFAULT_RAW)
    parser.add_argument("--pob", type=Path, default=DEFAULT_POB)
    parser.add_argument("--check", action="store_true", help="Compare regenerated bytes with the checked-in files")
    args = parser.parse_args()
    try:
        for path, contents in rebuild(args.raw.resolve(), args.pob).items():
            if args.check:
                if contents != path.read_bytes():
                    raise ValueError(f"Regenerated file differs: {path.relative_to(ROOT)}")
            else:
                write_atomic(path, contents)
            print(f"{'Verified' if args.check else 'Wrote'} {path.relative_to(ROOT)} ({len(contents)} bytes)")
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
