#!/usr/bin/env python3
"""Validate an adjacent-generation content migration table."""

import argparse
import json
from pathlib import Path
import re


KINDS = {"cls", "asc", "node", "mastery", "gem", "sup", "affix", "unique",
         "jewelColor", "weaponTag", "slotGroup", "charm"}
NUMBERED = {"node", "mastery"}
CONTENT_IDS = {"cls", "asc", "gem", "sup", "affix", "unique"}
REFUNDS = {
    "passivePoint": ("passivePoints", "sourceAllocation"),
    "ascendancyPoint": ("ascendancyPoints", "sourceAllocation"),
    "item": ("inventory", "sourceInstance"),
}


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key!r}")
        result[key] = value
    return result


def is_id(kind, value):
    if kind in NUMBERED:
        return (isinstance(value, str) and re.fullmatch(r"[1-9][0-9]{0,9}", value)
                and int(value) <= 0xffffffff)
    if not isinstance(value, str) or len(value) > 64:
        return False
    if kind in CONTENT_IDS:
        return re.fullmatch(rf"{kind}\.[A-Za-z0-9._]+", value)
    return re.fullmatch(r"[A-Za-z0-9_]+", value)


def validate_table(table, filename, source_ids=None, target_ids=None):
    if not isinstance(table, dict) or set(table) != {"schemaVersion", "fromGeneration", "toGeneration", "kinds", "entries"}:
        raise ValueError("Invalid migration header")
    version, source, target = (table[key] for key in ("schemaVersion", "fromGeneration", "toGeneration"))
    if (any(type(value) is not int for value in (version, source, target)) or
        version != 1 or source < 0 or target != source + 1 or target > 0xffffffff):
        raise ValueError("Expected schema version 1 and adjacent non-negative generations")
    if filename != f"gen{source}-to-gen{target}.json":
        raise ValueError("Filename and generations differ")
    kinds = table["kinds"]
    entries = table["entries"]
    if (not isinstance(kinds, list) or not kinds or any(not isinstance(kind, str) or kind not in KINDS for kind in kinds)
        or len(kinds) != len(set(kinds)) or not isinstance(entries, dict) or set(kinds) != set(entries)):
        raise ValueError("Invalid migration kinds")
    if source_ids is not None and set(source_ids) - set(kinds):
        raise ValueError("Migration is missing source kinds")
    for kind, mappings in entries.items():
        if not isinstance(mappings, dict):
            raise ValueError(f"Invalid entries for {kind}")
        if source_ids is not None and kind in source_ids and set(mappings) != set(source_ids[kind]):
            raise ValueError(f"Migration does not enumerate exactly the source IDs for {kind}")
        for old_id, entry in mappings.items():
            if not is_id(kind, old_id) or not isinstance(entry, dict):
                raise ValueError(f"Invalid source entry for {kind} {old_id}")
            if set(entry) == {"to"}:
                new_id = entry["to"]
                valid_type = type(new_id) is int if kind in NUMBERED else isinstance(new_id, str)
                if not valid_type or not is_id(kind, str(new_id)):
                    raise ValueError(f"Invalid target ID for {kind} {old_id}")
                if target_ids is not None and kind in target_ids and str(new_id) not in target_ids[kind]:
                    raise ValueError(f"Unknown target ID for {kind} {old_id}")
            elif entry.get("retire") is True and set(entry) <= {"retire", "refund"}:
                if "refund" not in entry:
                    continue
                refund = entry["refund"]
                if (not isinstance(refund, dict) or set(refund) != {"unit", "destination", "cap", "oncePer"}
                    or not isinstance(refund["unit"], str)
                    or REFUNDS.get(refund["unit"]) != (refund["destination"], refund["oncePer"])
                    or type(refund["cap"]) is not int or not 1 <= refund["cap"] <= 0xffffffff):
                    raise ValueError(f"Invalid refund for {kind} {old_id}")
            else:
                raise ValueError(f"Expected one target or retirement for {kind} {old_id}")


def load_table(path, source_ids=None, target_ids=None):
    table = json.loads(path.read_bytes(), object_pairs_hook=unique_keys)
    validate_table(table, path.name, source_ids, target_ids)
    return table


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path)
    parser.add_argument("--source-baseline", type=Path, help="Check completeness for the baseline's ID kinds")
    args = parser.parse_args()
    try:
        source = json.loads(args.source_baseline.read_bytes()) if args.source_baseline else None
        table = load_table(args.table, source["entries"] if source else None)
        if source and table["fromGeneration"] != source["contentGeneration"]:
            raise ValueError("Source baseline generation does not match migration")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Verified {args.table.name}")


if __name__ == "__main__":
    main()
