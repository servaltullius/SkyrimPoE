"""Extract a local, versioned raw pack from a PoB-PoE2 distribution."""

from __future__ import annotations

import argparse
from collections import Counter
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import xml.etree.ElementTree as ET

# Isolated mode excludes the script directory from sys.path.
if __package__ in ("", None):
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from luadata import Parser, field, long_strings, parse, table_array, table_fields


RAW_ROOT = Path(__file__).resolve().parents[2] / "data" / "poe2" / "raw"
MODULES = ("Data", "CalcDefence", "CalcOffence", "CalcPerform", "CalcSetup")
MODS = (
    "ModItem", "ModCharm", "ModFlask", "ModJewel", "ModCorrupted",
    "ModItemExclusive", "ModVeiled", "ModIncursionLimb", "ModRunes",
)


class InputFiles:
    def __init__(self, pob: Path):
        self.pob = pob.resolve()
        self.sha256 = {}

    def read(self, path: Path) -> bytes:
        path = self.pob / path
        contents = path.read_bytes()
        try:
            key = Path(os.path.relpath(path, self.pob)).as_posix()
        except ValueError:
            # Windows paths on different drives have no relative filesystem path.
            key = f"ko/{path.name}"
        self.sha256[key] = hashlib.sha256(contents).hexdigest()
        return contents

    def lua(self, path: Path) -> str:
        return self.read(path).decode("utf-8-sig")

    def files(self, directory: str, pattern: str) -> list[Path]:
        return sorted(path.relative_to(self.pob) for path in (self.pob / directory).glob(pattern)
                      if path.is_file())


def json_bytes(value, *, indent=None):
    options = {"indent": indent} if indent else {"separators": (",", ":")}
    return (json.dumps(value, ensure_ascii=False, allow_nan=False, **options) + "\n").encode(
        "utf-8", errors="backslashreplace"
    )


def write_atomic(path: Path, contents: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=path.parent) as directory:
        temporary = Path(directory) / path.name
        temporary.write_bytes(contents)
        temporary.replace(path)


def returned(files: InputFiles, filename: str):
    path = Path(filename)
    statements = parse(files.lua(path), filename)
    if len(statements) != 1 or statements[0].kind != "return":
        raise ValueError(f"{filename}: expected a returned table")
    table_fields(statements[0].value)
    return statements[0].value


def assignments(statements, path: Path, target: str) -> list[dict]:
    entries = []
    for statement in statements:
        if statement.kind == "local":
            continue
        if (statement.kind != "assign" or len(statement.target) != 2
                or statement.target[0] != target or not isinstance(statement.target[1], str)):
            raise ValueError(f"{path.as_posix()}:{statement.line}: expected {target} assignment")
        entries.append({"file": path.as_posix(), "key": statement.target[1], "value": statement.value})
    return entries


def gem_entries(gem_table) -> list[dict]:
    result = []
    seen = {}
    for key, value in table_fields(gem_table):
        entry = {"file": "Data/Gems.lua", "key": key, "value": value}
        if key in seen:
            entry["duplicateOf"] = seen[key]
        seen[key] = len(result) + 1
        result.append(entry)
    return result


def skills(files: InputFiles) -> dict:
    entries = []
    returns = {}
    for path in files.files("Data/Skills", "*.lua"):
        name = path.as_posix()
        statements = parse(files.lua(path), name)
        if len(statements) == 1 and statements[0].kind == "return":
            returns[name] = statements[0].value
        else:
            entries.extend(assignments(statements, path, "skills"))
    return {"entries": entries, "fileReturns": returns}


def base_entries(files: InputFiles, module_source: str) -> list[dict]:
    listing = re.search(r"^[ \t]*local[ \t]+itemTypes[ \t]*=[ \t]*", module_source, re.MULTILINE)
    if listing is None:
        raise ValueError("Modules/Data.lua: itemTypes loader list not found")
    load_order = table_array(Parser(module_source, "Modules/Data.lua", listing.end()).expression())
    by_file = {}
    for path in files.files("Data/Bases", "*.lua"):
        source = parse(files.lua(path), path.as_posix())
        by_file[path.stem] = assignments(source, path, "itemBases")
    seen = set()
    result = []
    for name in load_order:
        if name not in by_file:
            raise ValueError(f"Modules/Data.lua: listed base file Data/Bases/{name}.lua is missing")
        for entry in by_file.pop(name):
            key = entry["key"]
            result.append({**entry, "loaded": True, "overrides": key in seen})
            seen.add(key)
    for entries in by_file.values():
        result.extend({**entry, "loaded": False, "overrides": False} for entry in entries)
    return result


def unique_entries(files: InputFiles) -> list[dict]:
    result = []
    for path in files.files("Data/Uniques", "**/*.lua"):
        name = path.as_posix()
        for ordinal, text in enumerate(long_strings(files.lua(path), name), 1):
            lines = text.splitlines()
            if lines and not lines[0]:
                lines = lines[1:]
            if len(lines) < 2:
                raise ValueError(f"{name}: unique block {ordinal} needs a name and a base")
            result.append({
                "file": name, "ordinal": ordinal, "text": text, "name": lines[0], "base": lines[1],
            })
    return result


def translations(files: InputFiles, ko: Path) -> dict:
    pairs_by_file = {}
    warnings = []
    for path in sorted(ko.resolve().iterdir()):
        if not (path.is_file() and path.name.lower().startswith("poe2") and path.suffix.lower() == ".csv"):
            continue
        source = files.read(path).decode("utf-8-sig")
        lines = io.StringIO(source, newline="").readlines()
        reader = csv.reader(lines)
        pairs = []
        previous = 0
        for row in reader:
            start = previous + 1
            spelling = lines[previous:reader.line_num]
            previous = reader.line_num
            strict = csv.reader(spelling, strict=True)
            try:
                list(strict)
            except csv.Error:
                warnings.append({"file": path.name, "line": start + strict.line_num - 1,
                                 "reason": "strictCsvError"})
            if len(row) != 2:
                warnings.append({"file": path.name, "line": start,
                                 "reason": "columnCount", "columns": len(row)})
            else:
                pairs.append(row)
        pairs_by_file[path.name] = pairs
    return {"files": pairs_by_file, "parseWarnings": warnings}


def extract(pob: Path, tree_version: str, game_version: str, ko: Path | None, only: str | None) -> dict:
    if not re.fullmatch(r"\d+(?:_\d+)+", tree_version):
        raise ValueError("tree version must have the form 0_5")
    files = InputFiles(pob)
    manifest = ET.fromstring(files.read(Path("manifest.xml")))
    manifest_hashes = {entry.attrib["name"].replace("\\", "/"): entry.attrib["sha1"]
                       for entry in manifest.findall("File") if "sha1" in entry.attrib}
    meta = {
        "schema": "skyrimpoe.poe2raw", "schemaVersion": 1, "game": "poe2",
        "gameVersion": game_version, "pobVersion": manifest.find("Version").attrib["number"],
        "treeVersion": tree_version, "extractorVersion": "1",
    }
    pack = {"meta": meta, "tree": returned(files, f"TreeData/{tree_version}/tree.lua")}
    if only != "tree":
        module_path = "Modules/Data.lua"
        module_source = files.lua(Path(module_path))
        for name in MODULES[1:]:
            files.read(Path(f"Modules/{name}.lua"))
        misc_path = Path("Data/Misc.lua")
        pack.update({
            "gems": gem_entries(returned(files, "Data/Gems.lua")),
            "skills": skills(files),
            "bases": base_entries(files, module_source),
            "mods": {name: returned(files, f"Data/{name}.lua") for name in MODS},
            "essences": returned(files, "Data/Essence.lua"),
            "questRewards": returned(files, "Data/QuestRewards.lua"),
            "misc": assignments(parse(files.lua(misc_path), misc_path.as_posix()), misc_path, "data"),
            "uniques": unique_entries(files),
        })
        if ko is not None:
            pack["ko"] = translations(files, ko)
    meta["sourceSha256"] = files.sha256
    meta["pobManifestSha1"] = {name: manifest_hashes[name] for name in files.sha256
                              if name in manifest_hashes}
    return pack


def count_functions(value) -> int:
    if isinstance(value, dict):
        if "$function" in value:
            return 1
        return sum(count_functions(child) for child in value.values())
    if isinstance(value, list):
        return sum(count_functions(child) for child in value)
    return 0


def summarize_gems(entries: list[dict]) -> dict:
    effective = {entry["key"]: entry["value"] for entry in entries}
    counts = {"positive": {"active": 0, "support": 0}, "zero": {"active": 0, "support": 0}}
    ranks = dict.fromkeys(("I", "II", "III", "IV", "none"), 0)
    for value in effective.values():
        support = field(value, "gemType") == "Support"
        group = "positive" if field(value, "Tier") > 0 else "zero"
        counts[group]["support" if support else "active"] += 1
        if support and group == "positive":
            rank = re.search(r" (IV|III|II|I)$", field(value, "name"))
            ranks[rank[1] if rank else "none"] += 1
    return {
        "declarations": len(entries), "effectiveKeys": len(effective),
        "duplicateKeys": list(dict.fromkeys(entry["key"] for entry in entries if "duplicateOf" in entry)),
        "tierPositive": {**counts["positive"], "supportNameRanks": ranks}, "tierZero": counts["zero"],
    }


def base_flags(value: dict) -> tuple[bool, bool]:
    tags = field(value, "tags")
    runeforged = tags is not None and field(tags, "runeforged") is True
    return runeforged, field(value, "hidden") is True


def summarize_bases(entries: list[dict]) -> dict:
    flags = [base_flags(entry["value"]) for entry in entries]
    loaded = [entry for entry in entries if entry["loaded"]]
    effective = {entry["key"]: base_flags(entry["value"]) for entry in loaded}
    return {
        "declarations": len(entries),
        "runeforgedDeclarations": sum(rune for rune, _ in flags),
        "hiddenDeclarations": sum(hidden for _, hidden in flags),
        "runeforgedAndHiddenDeclarations": sum(rune and hidden for rune, hidden in flags),
        "normalDeclarations": sum(not (rune or hidden) for rune, hidden in flags),
        "loadedDeclarations": len(loaded),
        "overrides": sum(entry["overrides"] for entry in loaded),
        "effectiveKeys": len(effective),
        "effectiveNonHiddenKeys": sum(not hidden for _, hidden in effective.values()),
        "effectiveNormalKeys": sum(not (rune or hidden) for rune, hidden in effective.values()),
        "unloadedFiles": list(dict.fromkeys(entry["file"] for entry in entries if not entry["loaded"])),
    }


def summarize_mods(mods: dict) -> dict:
    item_values = [value for _, value in table_fields(mods["ModItem"])]
    rune_types = Counter()
    for key, value in table_fields(mods["ModRunes"]):
        types = {field(slot, "type") for _, slot in table_fields(value)}
        if not types or None in types or len(types) != 1:
            raise ValueError(f"Data/ModRunes.lua: expected one type across slots for {key!r}")
        rune_types.update(types)
    return {
        "byFile": {name: len(table_fields(value)) for name, value in mods.items()},
        "modItemTypes": dict(Counter(field(value, "type") for value in item_values)),
        "modItemWeightValues": sorted({weight for value in item_values
                                      for weight in table_array(field(value, "weightVal"))}),
        "modRunesTypes": dict(rune_types),
    }


def summarize(pack: dict) -> dict:
    summary = {"meta": pack["meta"], "tree": {"topLevelNodes": len(table_fields(field(pack["tree"], "nodes")))}}
    if "gems" not in pack:
        return summary
    skill_files = dict.fromkeys(pack["skills"]["fileReturns"], 0)
    function_counts = {file: count_functions(value) for file, value in pack["skills"]["fileReturns"].items()}
    for entry in pack["skills"]["entries"]:
        file = entry["file"]
        skill_files[file] = skill_files.get(file, 0) + 1
        function_counts[file] = function_counts.get(file, 0) + count_functions(entry["value"])
    misc = {entry["key"]: entry["value"] for entry in pack["misc"]}
    life = table_array(misc["monsterLifeTable"])
    ko = pack.get("ko", {"files": {}, "parseWarnings": []})
    summary.update({
        "gems": summarize_gems(pack["gems"]),
        "skills": {"definitionsByFile": skill_files, "functionsByFile": function_counts},
        "bases": summarize_bases(pack["bases"]),
        "mods": summarize_mods(pack["mods"]),
        "uniques": {"blocks": len(pack["uniques"]), "distinctNames": len({row["name"] for row in pack["uniques"]})},
        "monsterLifeTable": {"length": len(life), "65": life[64], "80": life[79]},
        "ko": {"pairs": sum(map(len, ko["files"].values())), "parseWarnings": len(ko["parseWarnings"])},
    })
    return summary


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pob", required=True, type=Path)
    parser.add_argument("--tree-version", required=True)
    parser.add_argument("--game-version", default="0.5")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--ko", type=Path)
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--only", choices=("tree",))
    parser.add_argument("--allow-out-anywhere", action="store_true", help="allow a scratch directory for tests")
    args = parser.parse_args(argv)
    output = args.out.resolve()
    if not args.allow_out_anywhere and (output == RAW_ROOT or not output.is_relative_to(RAW_ROOT)):
        parser.error("--out must be a pack directory below data/poe2/raw/")
    if args.only and args.ko:
        parser.error("--ko requires a full extraction")
    try:
        pack = extract(args.pob, args.tree_version, args.game_version, args.ko, args.only)
        summary = json_bytes(summarize(pack), indent=2) if args.summary is not None else None
        paths = {key: output / f"{key}.json" for key in pack}
        if args.summary is not None and args.summary.resolve() in paths.values():
            raise ValueError("--summary must have a path distinct from the raw pack files")
        for key, value in pack.items():
            write_atomic(paths[key], json_bytes(value))
        if summary is not None:
            write_atomic(args.summary, summary)
    except (OSError, ValueError, KeyError, TypeError, ET.ParseError, csv.Error) as exc:
        parser.exit(1, f"extract: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
