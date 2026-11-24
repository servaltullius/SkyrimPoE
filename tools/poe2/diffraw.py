"""Compare two PoE2 raw packs by source key and meaning."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from decimal import Decimal
from fractions import Fraction
import json
from pathlib import Path
import re
import sys

if __package__ in ("", None):
    sys.path.insert(0, str(Path(__file__).resolve().parent))

from extract import json_bytes, write_atomic
from luadata import field, table_array, table_fields


CLASSES = ("added", "removed", "unchanged", "numericChange", "meaningChange", "layoutChange")
TREE_LAYOUT = frozenset(("x", "y", "group", "orbit", "orbitIndex", "$connectionOrbits"))
IDENTITY_FIELDS = frozenset(("connections", "statOrder", "weightKey", "tags", "grantedEffectId"))
KINDS = ("gems", "skills", "tree", "bases", "mods", "uniques", "misc", "essences")


def number_value(value):
    if type(value) in (int, float):
        return Decimal(str(value))
    if isinstance(value, dict) and "$number" in value:
        spelling = value["$number"].lower()
        if not spelling.lstrip("-").startswith("0x"):
            return Decimal(spelling)
        mantissa, _, exponent = spelling.lstrip("-")[2:].partition("p")
        whole, _, fractional = mantissa.partition(".")
        scaled = Fraction(int(whole + fractional, 16), 16 ** len(fractional))
        return (-scaled if spelling.startswith("-") else scaled) * Fraction(2) ** int(exponent or 0)
    return None


def atomic(value):
    numeric = number_value(value)
    if numeric is not None:
        return ("number", numeric)
    return (type(value).__name__, value)


def branches(value):
    """Return ordered children as identity, path segment, value."""
    if isinstance(value, list):
        return "array", [(index, str(index), child) for index, child in enumerate(value, 1)]
    if isinstance(value, dict):
        if "$table" in value:
            children = []
            if table_array(value):
                children.append((("array",), "$array", table_array(value)))
            occurrences = Counter()
            for key, child in table_fields(value):
                identity = fingerprint(key, False)
                occurrences[identity] += 1
                position = occurrences[identity]
                label = key if isinstance(key, str) else display_key(key)
                if position > 1:
                    label = f"{label}#{position}"
                children.append(((identity, position), label, child))
            return "table", children
        if "$number" not in value:
            keys = sorted(key for key in value if key != "line" or "$function" not in value)
            return "object", [(key, key, value[key]) for key in keys]
    return None, ()


def identity_field(label: str) -> bool:
    return label in IDENTITY_FIELDS or label.startswith("additionalStatSet")


def fingerprint(value, mask_numbers: bool, *, tree=False):
    family, children = branches(value)
    if family is None:
        tag, atom = atomic(value)
        return (tag, None if mask_numbers and tag == "number" else atom)
    return family, tuple(
        (identity, fingerprint(child, mask_numbers and not identity_field(label)))
        for identity, label, child in children if not (tree and label in TREE_LAYOUT)
    )


def reordered_numbers(old_children, new_children, family: str) -> bool:
    if family not in ("array", "table") or len(old_children) < 2:
        return False
    # An exact permutation preserves the multiset but changes which position has which value.
    old_values = [fingerprint(child, False) for _, _, child in old_children]
    new_values = [fingerprint(child, False) for _, _, child in new_children]
    return old_values != new_values and Counter(old_values) == Counter(new_values)


def changed_fields(old, new, path=(), *, identity_value=False):
    if old is new:
        return {}
    old_family, old_children = branches(old)
    new_family, new_children = branches(new)
    if old_family != new_family:
        return {path: "meaningChange"}
    if old_family is None:
        left = atomic(old)
        right = atomic(new)
        if left == right:
            return {}
        numeric = not identity_value and left[0] == right[0] == "number"
        return {path: "numericChange" if numeric else "meaningChange"}
    changes = {}
    left = {key: (label, value) for key, label, value in old_children}
    right = {key: (label, value) for key, label, value in new_children}
    if [key for key in left if key in right] != [key for key in right if key in left]:
        changes[path] = "meaningChange"
    for key in dict.fromkeys((*left, *right)):
        label = (left[key] if key in left else right[key])[0]
        child_path = (*path, label)
        if key not in left or key not in right:
            changes[child_path] = "meaningChange"
        else:
            changes.update(changed_fields(
                left[key][1], right[key][1], child_path,
                identity_value=identity_value or identity_field(label),
            ))
    if changes and set(changes.values()) == {"numericChange"}:
        if reordered_numbers(old_children, new_children, old_family):
            changes = dict.fromkeys(changes, "meaningChange")
    return changes


def json_pointer(path: tuple) -> str:
    return "".join("/" + segment.replace("~", "~0").replace("/", "~1") for segment in path)


def display_key(key) -> str:
    if isinstance(key, str):
        return key
    return json.dumps(key, ensure_ascii=False, separators=(",", ":"))


def tree_value(node: dict) -> dict:
    fields = [[key, value] for key, value in table_fields(node) if key != "connections"]
    connections = field(node, "connections")
    if connections is None:
        return {"$table": {"array": table_array(node), "fields": fields}}
    targets = {}
    orbits = defaultdict(dict)
    for connection in [*table_array(connections), *(value for _, value in table_fields(connections))]:
        if isinstance(connection, dict) and "$table" in connection:
            target = field(connection, "id")
            orbit = field(connection, "orbit")
        else:
            target, orbit = connection, None
        identity = fingerprint(target, False)
        targets[identity] = target
        orbits[identity][fingerprint(orbit, False)] = orbit
    if targets:
        ordered = sorted(targets, key=repr)
        fields.append(["connections", [targets[identity] for identity in ordered]])
        fields.append(["$connectionOrbits", [sorted(orbits[identity].values(), key=repr) for identity in ordered]])
    return {"$table": {"array": table_array(node), "fields": fields}}


def kind_entries(name: str, value):
    if name == "tree":
        return [(key, tree_value(node)) for key, node in table_fields(field(value, "nodes"))]
    if name == "essences":
        return table_fields(value)
    if name == "uniques":
        return [([row["name"], row["base"]], row["text"]) for row in value]
    if name == "skills":
        value = value["entries"]
    if name == "bases":
        return [(row["key"], {"$table": {"array": table_array(row["value"]), "fields": [
            *table_fields(row["value"]), ["$loaded", row["loaded"]],
        ]}}) for row in value]
    return [(row["key"], row["value"]) for row in value]


def index_entries(entries) -> dict:
    # The last declaration is the effective one. Earlier duplicates are numbered from the end.
    occurrences = Counter()
    indexed = {}
    for key, value in reversed(entries):
        identity = fingerprint(key, False)
        occurrences[identity] += 1
        indexed[(identity, occurrences[identity])] = (key, value)
    return dict(reversed(list(indexed.items())))


def item_key(indexed, identity) -> dict:
    key, _ = indexed[identity]
    result = {"key": key}
    if identity[1] > 1:
        result["occurrenceFromEnd"] = identity[1]
    return result


def compare_entries(old_entries, new_entries, *, tree=False) -> dict:
    old = index_entries(old_entries)
    new = index_entries(new_entries)
    counts = dict.fromkeys(CLASSES, 0)
    items = []
    removed = defaultdict(list)
    added = defaultdict(list)
    for identity in dict.fromkeys((*old, *new)):
        changes = {}
        if identity not in old:
            classification = "added"
            key = item_key(new, identity)
            added[fingerprint(new[identity][1], True, tree=tree)].append(key)
        elif identity not in new:
            classification = "removed"
            key = item_key(old, identity)
            removed[fingerprint(old[identity][1], True, tree=tree)].append(key)
        else:
            key = item_key(old, identity)
            changes = changed_fields(old[identity][1], new[identity][1])
            if tree:
                changes = {path: "layoutChange" if path and path[0] in TREE_LAYOUT else change
                           for path, change in changes.items()}
            classes = set(changes.values())
            classification = next((name for name in ("meaningChange", "numericChange", "layoutChange")
                                   if name in classes), "unchanged")
        counts[classification] += 1
        items.append({**key, "classification": classification,
                      "changedFields": [json_pointer(path) for path in changes]})
    candidates = [{"removed": before, "added": after}
                  for signature, keys in removed.items()
                  for before in keys for after in added.get(signature, ())]
    return {"counts": counts, "items": items, "possibleRename": candidates}


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def compare_folders(old: Path, new: Path) -> dict:
    report = {"kinds": {}}
    kinds = report["kinds"]
    for name in KINDS:
        filename = f"{name}.json"
        old_file, new_file = old / filename, new / filename
        if not old_file.is_file() and not new_file.is_file():
            continue
        if old_file.is_file() != new_file.is_file():
            raise ValueError(f"{filename} is present in only one pack")
        left, right = read_json(old_file), read_json(new_file)
        if name == "mods":
            for mod in dict.fromkeys((*left, *right)):
                kinds[mod] = compare_entries(table_fields(left[mod]) if mod in left else [],
                                             table_fields(right[mod]) if mod in right else [])
        else:
            kinds[name] = compare_entries(kind_entries(name, left), kind_entries(name, right), tree=name == "tree")
    if not kinds:
        raise ValueError("no comparable raw pack files")
    return report


def markdown(report: dict) -> str:
    headings = ("종류", *CLASSES, "possibleRename")
    lines = ["# PoE2 원천 팩 판 비교", "", "| " + " | ".join(headings) + " |",
             "| " + " | ".join("---" for _ in headings) + " |"]
    for kind, data in report["kinds"].items():
        cells = [kind, *(str(data["counts"][name]) for name in CLASSES), str(len(data["possibleRename"]))]
        lines.append("| " + " | ".join(cells) + " |")
    lines.extend(("", "## 의미 변경 (종류별 최대 10개)", ""))
    for kind, data in report["kinds"].items():
        changed = [item for item in data["items"] if item["classification"] == "meaningChange"]
        for item in changed[:10]:
            key = re.sub(r"[\r\n`]", " ", display_key(item["key"]))
            paths = ", ".join(path or "/" for path in item["changedFields"])
            lines.append(f"- {kind} `{key}` · {paths}")
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old", required=True, type=Path)
    parser.add_argument("--new", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--md", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.md is not None and args.out.resolve() == args.md.resolve():
            raise ValueError("--md and --out must be distinct")
        report = compare_folders(args.old, args.new)
        write_atomic(args.out, json_bytes(report, indent=2))
        if args.md is not None:
            write_atomic(args.md, markdown(report).encode("utf-8", errors="backslashreplace"))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(1, f"diffraw: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
