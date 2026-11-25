#!/usr/bin/env python3
"""Compile the PoE2 0.5 passive tree and its P3 coverage report."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from collections.abc import Callable
from functools import lru_cache
import hashlib
import json
import math
from pathlib import Path
import re
import sys

from compile_tree import (
    KIND_ASCEND_START, KIND_CLASS_START, KIND_KEYSTONE, KIND_NORMAL, KIND_NOTABLE,
    KIND_SOCKET, classify_icon_category, split_stat_lines,
)

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "poe2"))
from diffraw import number_value
from extract import extract, json_bytes, write_atomic
from luadata import table_array, table_fields
from validate_migration import load_table


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_POB = Path(r"G:\poe\PathOfBuilding-PoE2-0.23.1")
DEFAULT_GAME = Path(r"G:\poe\ggpk\out")
GAME_VERSION = "0.5"
POB_VERSION = "0.23.1"
GAME_DATA_VERSION = "0.5.5h"
TREE_FILE = "TreeData/0_5/tree.lua"
PACK_PATH = ROOT / "data/packs/gen1/tree.json"
REPORT_PATH = ROOT / "data/poe2/reports/tree-coverage.json"
BASELINE_PATH = ROOT / "data/poe2/id-baseline/gen1.json"
SCHEMA_VERSION = 2
KIND_ATTRIBUTE = 7
STRAIGHT_ORBIT = 2147483647
PASSIVE_POINTS = re.compile(r"Grants ([0-9]+) Passive Skill Points?")
PASSIVE_START = re.compile(r"Can Allocate Passive Skills from the (.+)'s starting point")
MAIN_KINDS = {
    KIND_NORMAL: "normal", KIND_NOTABLE: "notable", KIND_ATTRIBUTE: "attribute",
    KIND_KEYSTONE: "keystone", KIND_SOCKET: "socket",
}
STAT_LEVELS = ("full", "partial", "unsupported", "mappingOnly")
NODE_LEVELS = ("full", "partial", "unsupported", "deferred")



_GAME_LINK = re.compile(r"\[([^\[\]|]+)(?:\|([^\[\]]+))?\]")
_CSD_ROW = re.compile(r'([^"]+) "((?:[^"\\]|\\.)*)"(?: (.*))?')
_CSD_FIELD = re.compile(r"\{([0-9]*)(:[+\-]?d?)?\}")
_CSD_VALUE = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]+)?|\.[0-9]+)")
_CSD_LITERAL_NUMBER = re.compile(r"(?<![\w.])[+-]?[0-9]+(?:\.[0-9]+)?")
_CSD_DISPLAY_FLAGS = re.compile(r"(?:^|\s)(?:canonical_line|canonical_stat(?: \d+)?)(?=\s|$)")
_CSD_NUMBER_WORDS = ("zero", "one", "two", "three", "four", "five", "six",
                     "seven", "eight", "nine", "ten", "eleven", "twelve")
_CSD_NUMBER_WITH_UNIT = re.compile(
    rf"\b({'|'.join(_CSD_NUMBER_WORDS)})(?= seconds?\b| metres?\b)", re.IGNORECASE)


def game_display_text(text: str) -> str:
    text = _GAME_LINK.sub(lambda match: match[2] or match[1], text)
    return " ".join(text.split())


@lru_cache(maxsize=4096)
def _csd_fields(template: str) -> tuple[tuple[int, str], ...]:
    return tuple((int(match[1]) if match[1] else order, match[2] or "")
                 for order, match in enumerate(_CSD_FIELD.finditer(template)))


def _csd_shape(line: str) -> str:
    return _CSD_VALUE.sub("#", line).casefold()


def _csd_read_rows(rows: list[str]) -> dict[tuple, list[str]]:
    groups = defaultdict(list)
    for row in rows:
        match = _CSD_ROW.fullmatch(row)
        if not match:
            continue
        conditions, template, directives = match.groups()
        directives = " ".join(_CSD_DISPLAY_FLAGS.sub("", directives or "").split())
        template = template.replace(r"\n", " ").replace(r'\"', '"').replace(r"\'", "'")
        groups[tuple(conditions.split()), directives].append(template)
    return groups


def _csd_constants(template: str, shared_links: set[str]) -> list[str]:
    # Glossary terms can spell out an implicit game constant in one locale,
    # such as [Recently] / [Recently|최근 4초 이내].
    literal = _CSD_FIELD.sub("", template)
    literal = _GAME_LINK.sub(lambda match: " " if match[1] in shared_links else match[2] or match[1],
                             literal)
    literal = _CSD_NUMBER_WITH_UNIT.sub(
        lambda match: str(_CSD_NUMBER_WORDS.index(match[1].casefold())), literal)
    # "One" and "per second" can be implicit in translation.
    return sorted(value for value in _CSD_LITERAL_NUMBER.findall(literal) if value != "1")


def _csd_templates(data: bytes) -> dict[str, list[tuple]]:
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    text = data.decode(encoding).replace("\r\n", "\n")
    translations = defaultdict(list)
    for block in re.split(r"(?m)^description\s*$", text):
        lines = [line.strip() for line in block.splitlines() if line.strip()]
        if len(lines) < 3 or not lines[1].isdigit() or 'lang "Korean"' not in lines:
            continue
        header = lines[0].split()
        stat_ids = frozenset(header[1:])
        if not header[0].isdigit() or int(header[0]) != len(header) - 1:
            continue
        english_rows = _csd_read_rows(lines[2:2 + int(lines[1])])
        ko_start = lines.index('lang "Korean"') + 1
        korean_rows = _csd_read_rows(lines[ko_start + 1:ko_start + 1 + int(lines[ko_start])])
        for key, english in english_rows.items():
            korean = korean_rows.get(key, [])
            if len(english) != 1 or len(korean) != 1:
                continue
            en, ko = english[0], korean[0]
            if {index for index, _ in _csd_fields(en)} != {index for index, _ in _csd_fields(ko)}:
                continue
            # Reject stat text whose values differ between the two locale sections.
            glossary = {match[1] for match in _GAME_LINK.finditer(en)}
            glossary &= {match[1] for match in _GAME_LINK.finditer(ko)}
            if _csd_constants(en, glossary) != _csd_constants(ko, glossary):
                continue
            en, ko = game_display_text(en), game_display_text(ko)
            translations[_csd_shape(_CSD_FIELD.sub("#", en))].append((stat_ids, en, ko))
    return translations


@lru_cache(maxsize=4096)
def _csd_pattern(template: str) -> re.Pattern:
    position = 0
    parts = []
    for match in _CSD_FIELD.finditer(template):
        parts.append(re.escape(template[position:match.start()]))
        sign = r"[+-]" if match[2] in (":+d", ":+") else r"[+-]?"
        parts.append(f"({sign}(?:[0-9]+(?:\\.[0-9]+)?|\\.[0-9]+))")
        position = match.end()
    parts.append(re.escape(template[position:]))
    return re.compile("".join(parts), re.IGNORECASE)


def _csd_substitute(line: str, english: str, korean: str) -> str | None:
    match = _csd_pattern(english).fullmatch(line)
    if not match:
        return None
    values = {}
    for (index, _), value in zip(_csd_fields(english), match.groups(), strict=True):
        if values.setdefault(index, value) != value:
            return None

    replacements = []
    for index, fmt in _csd_fields(korean):
        value = values[index]
        # CSD number transforms ran before substitution in PoB. Even a d
        # placeholder can display a fraction, as on Melee Strike Range.
        if fmt in (":+d", ":+") and value[0] not in "+-":
            value = "+" + value
        if fmt in ("", ":d", ":-d"):
            value = value.removeprefix("+")
        replacements.append(value)
    count = iter(replacements)
    return _CSD_FIELD.sub(lambda _match: next(count), korean)


class KoreanTables:
    """Resolve names through the game tables, and stats through their bilingual CSD entries."""

    def __init__(self, balance: Path, stat_descriptions: Path | None,
                 read_input: Callable[[str, Path], bytes]):
        def read_table(name: str, locale: str) -> dict:
            directory = balance / "korean" if locale == "ko" else balance
            filename = f"{name}.datc64.json"
            return {row["Id"]: row for row in json.loads(read_input(f"ggpk/{locale}/{filename}",
                                                                    directory / filename))}

        self.passive = {row["PassiveSkillGraphId"]: row
                        for row in read_table("passiveskills", "en").values()}
        self.passive_ko = read_table("passiveskills", "ko")
        self.classes = {row["IntegerId"]: row["Id"] for row in read_table("characters", "en").values()}
        self.classes_ko = read_table("characters", "ko")
        self.asc = read_table("ascendancy", "en")
        self.asc_ko = read_table("ascendancy", "ko")
        self.translations = (_csd_templates(read_input("ggpk/stat_descriptions.csd", stat_descriptions))
                             if stat_descriptions is not None else {})
        self.popup_translations = defaultdict(set)
        self.popup_hits = 0
        popup_file = "keywordpopups.datc64.json"
        if (balance / popup_file).is_file() and (balance / "korean" / popup_file).is_file():
            english_popups = read_table("keywordpopups", "en")
            korean_popups = read_table("keywordpopups", "ko")
            for key, row in english_popups.items():
                en = row.get("Definition")
                ko = korean_popups.get(key, {}).get("Definition")
                if en and ko:
                    self.popup_translations[game_display_text(en).removesuffix(".")].add(game_display_text(ko))

    @staticmethod
    def _name(rows: dict, key: str | None, english: str) -> str:
        name = rows.get(key, {}).get("Name")
        if not name or name == "WIP" or name.startswith("[DNT-UNUSED]"):
            return english
        return game_display_text(name)

    def node_name(self, graph_id: int, name: str) -> str:
        return self._name(self.passive_ko, self.passive.get(graph_id, {}).get("Id"), name)

    def class_name(self, integer_id: int, name: str) -> str:
        return self._name(self.classes_ko, self.classes.get(integer_id), name)

    def asc_name(self, internal_id: str, name: str) -> str:
        return self._name(self.asc_ko, internal_id if internal_id in self.asc else None, name)

    def stats(self, graph_id: int, english_lines: list[str]) -> list[str]:
        source = self.passive.get(graph_id, {}).get("Stats")
        source_ids = frozenset(source["Ids"]) if source else frozenset()
        localized = []
        for line in english_lines:
            normalized = game_display_text(line)
            matches = defaultdict(set)
            for stat_ids, english, korean in self.translations.get(_csd_shape(normalized), []):
                candidate = _csd_substitute(normalized, english, korean)
                if candidate is not None:
                    matches[stat_ids <= source_ids].add(candidate)
            candidates = matches[True] or matches[False]
            if len(candidates) == 1:
                localized.append(next(iter(candidates)))
                continue
            popup = self.popup_translations.get(normalized.removesuffix("."), set()) if not candidates else set()
            if len(popup) == 1 and (translated := next(iter(popup))) != line:
                self.popup_hits += 1
                localized.append(translated)
            else:
                localized.append(line)
        return localized


def lua_value(value):
    if isinstance(value, dict) and "$table" in value:
        fields = {index: lua_value(item) for index, item in enumerate(table_array(value), 1)}
        fields.update((key, lua_value(item)) for key, item in table_fields(value))
        return fields
    if isinstance(value, dict) and "$number" in value:
        return float(number_value(value))
    return value


def content_id(kind: str, key: str) -> str:
    result = kind + "." + re.sub(r"[^A-Za-z0-9._]", "", key)
    if not re.fullmatch(r"[a-z]+\.[A-Za-z0-9._]+", result) or len(result) > 64:
        raise ValueError(f"Invalid content ID for {key!r}: {result!r}")
    return result


def source_ref(key, **details) -> dict:
    return {"version": GAME_VERSION, "file": TREE_FILE, "key": key, **details}


def compile_classes(classes: dict, nodes: dict, adj: dict, korean: KoreanTables):
    starts = {}
    for node_id, node in nodes.items():
        for name in node.get("classesStart", {}).values():
            starts["cls", name] = node_id
        if node.get("isAscendancyStart"):
            starts["asc", node["ascendancyName"]] = node_id
            for name in node.get("options", {}):
                starts["asc", name] = node_id

    classes_out = []
    asc_by_name = {}
    class_start_display = {}
    conditions = {}
    used_ids = set()

    def register(kind, name, key):
        identifier = content_id(kind, key)
        if identifier in used_ids:
            raise ValueError(f"Duplicate content ID: {identifier}")
        used_ids.add(identifier)
        conditions[name] = identifier
        return identifier

    for index, klass in enumerate(classes.values()):
        name, integer_id = klass["name"], klass["integerId"]
        start = starts["cls", name]
        name_ko = korean.class_name(integer_id, name)
        class_start_display.setdefault(start, (name, name_ko))
        result = {
            "id": index, "contentId": register("cls", name, name),
            "source": source_ref(name, integerId=integer_id),
            "name": name, "nameKo": name_ko,
            "baseStr": klass["base_str"], "baseDex": klass["base_dex"], "baseInt": klass["base_int"],
            "startNodeId": start, "startNeighbors": sorted(adj[start]), "ascendancies": [],
        }
        ascendants = {}
        replacements = []
        for asc in klass["ascendancies"].values():
            key, display, internal_id = asc["id"], asc["name"], asc["internalId"]
            entry = {
                "id": register("asc", display, key), "source": source_ref(key, internalId=internal_id),
                "name": display, "nameKo": korean.asc_name(internal_id, display),
                "startNodeId": starts["asc", key],
            }
            asc_by_name[key] = entry
            if "replace" in asc:
                replacements.append((asc["replace"], entry))
            else:
                ascendants[key] = entry
                result["ascendancies"].append(entry)
        for base, replacement in replacements:
            parent = ascendants[base]
            if replacement["startNodeId"] != parent["startNodeId"]:
                raise ValueError(f"Replacement start differs from {parent['id']}")
            parent.setdefault("replacements", []).append(replacement)
        classes_out.append(result)
    return classes_out, class_start_display, asc_by_name, conditions


def build_connections(raw_nodes: dict, nodes: dict):
    """Keep the PoB connector direction for its signed radius."""
    adj = {node_id: set() for node_id in nodes}
    edges = {}
    excluded = Counter()
    for node_id, node in sorted(raw_nodes.items()):
        for connection in node["connections"].values():
            other_id = connection["id"]
            if other_id not in raw_nodes:
                excluded["missing"] += 1
            elif node_id not in nodes or other_id not in nodes:
                excluded["onlyImage"] += 1
            elif node_id == other_id:
                excluded["self"] += 1
            elif node.get("ascendancyName") != nodes[other_id].get("ascendancyName"):
                excluded["crossTree"] += 1
            else:
                adj[node_id].add(other_id)
                adj[other_id].add(node_id)
                if node.get("classesStart") or nodes[other_id].get("classesStart"):
                    excluded["classStartConnector"] += 1
                    continue
                pair = tuple(sorted((node_id, other_id)))
                # Opposite raw directions encode the same curve with opposite signs.
                orbit = connection["orbit"]
                previous = edges.get(pair)
                if previous:
                    old_id, _, old_orbit = previous
                    if old_id != node_id and old_orbit not in (0, STRAIGHT_ORBIT):
                        old_orbit = -old_orbit
                    if orbit != old_orbit:
                        raise ValueError(f"Conflicting connector geometry for {pair}")
                else:
                    edges[pair] = (node_id, other_id, orbit)
    return adj, edges, dict(sorted(excluded.items()))


def connector_arc(first: dict, second: dict, orbit: int, groups: dict, radii: dict, positions: dict):
    """Return [cx, cy, r, start, end] in canvas angles, or [] for a straight line."""
    a_x, a_y, a_angle = positions[first["skill"]]
    b_x, b_y, b_angle = positions[second["skill"]]
    radius = radii.get(abs(orbit) + 1)
    if orbit and radius and 0 < (distance := math.hypot(b_x - a_x, b_y - a_y)) < 2 * radius:
        dx, dy = b_x - a_x, b_y - a_y
        perpendicular = math.copysign(math.sqrt(radius ** 2 - distance ** 2 / 4), orbit)
        cx = a_x + dx / 2 + perpendicular * dy / distance
        cy = a_y + dy / 2 - perpendicular * dx / distance
        start = math.atan2(a_y - cy, a_x - cx)
        end = math.atan2(b_y - cy, b_x - cx)
    elif orbit == 0 and first["group"] == second["group"] and first["orbit"] == second["orbit"]:
        group = groups[first["group"]]
        cx, cy = group["x"], group["y"]
        radius = radii[first["orbit"] + 1]
        start, end = a_angle - math.pi / 2, b_angle - math.pi / 2
    else:
        return []
    start, end = sorted((start, end))
    if end - start >= math.pi:
        start, end = end, start + math.tau
    return [round(cx, 6), round(cy, 6), radius, round(start, 9), round(end, 9)]


def kind_of(node: dict) -> int:
    for flag, kind in (
        ("classesStart", KIND_CLASS_START), ("isAscendancyStart", KIND_ASCEND_START),
        ("isJewelSocket", KIND_SOCKET), ("isKeystone", KIND_KEYSTONE),
        ("isNotable", KIND_NOTABLE), ("isAttribute", KIND_ATTRIBUTE),
    ):
        if node.get(flag):
            return kind
    return KIND_NORMAL


def display_stats(raw: dict, node_id: int, korean: KoreanTables) -> dict:
    stats = []
    for line in split_stat_lines(list(raw["stats"].values())):
        line = game_display_text(line)
        # PoB also wraps individual effects across consecutive table entries.
        if stats and line[0].islower() and not stats[-1].endswith((".", ")")):
            stats[-1] += " " + line
        else:
            stats.append(line)
    return {"stats": stats, "statsKo": korean.stats(node_id, stats)}


def node_entry(raw, positions, adj, class_starts, asc_by_name, conditions, korean):
    node_id = raw["skill"]
    kind = kind_of(raw)
    asc_name = raw.get("ascendancyName")
    asc = asc_by_name.get(asc_name)
    name = game_display_text(raw["name"])
    if kind == KIND_CLASS_START:
        name, name_ko = class_starts[node_id]
    elif kind == KIND_ASCEND_START:
        name, name_ko = asc["name"], asc["nameKo"]
    else:
        name_ko = korean.node_name(node_id, name)
    x, y, _ = positions[node_id]
    result = {
        "id": node_id, "k": kind, "g": raw["group"], "o": raw["orbit"],
        "x": round(x, 6), "y": round(y, 6), "name": name, "nameKo": name_ko,
        "adj": [] if kind == KIND_CLASS_START else sorted(adj[node_id] - class_starts.keys()),
        **display_stats(raw, node_id, korean),
    }
    result["cat"] = classify_icon_category(kind, name, result["stats"], asc_name or "")
    if asc:
        result["asc"] = asc["id"]
    if raw.get("isMultipleChoice"):
        result["mc"] = True
    if raw.get("isMultipleChoiceOption"):
        result["mco"] = True
    if raw.get("isFreeAllocate"):
        result["freeAlloc"] = True
    if raw.get("isJewelSocket") or raw.get("containJewelSocket"):
        result["jewelSocket"] = True
    if raw.get("aliasPassiveSocket"):
        result["alias"] = raw["aliasPassiveSocket"]
    elif not asc and kind == KIND_NOTABLE and not adj[node_id] and raw.get("recipe"):
        result["recipeOnly"] = True
    if constraint := raw.get("unlockConstraint"):
        result["unlock"] = list(constraint["nodes"].values())
    if kind == KIND_ATTRIBUTE:
        result["attrOptions"] = [
            {"id": option["id"], "name": option["name"],
             "nameKo": korean.node_name(option["id"], option["name"]),
             **display_stats(option, option["id"], korean)}
            for option in raw["options"].values()
        ]
    if raw.get("isSwitchable"):
        variants = {}
        for condition, option in raw["options"].items():
            variant = {key: result[key] for key in ("name", "nameKo", "stats", "statsKo")}
            if kind == KIND_ASCEND_START:
                alternate = asc_by_name[option["ascendancyName"]]
                variant.update(name=alternate["name"], nameKo=alternate["nameKo"])
            if "id" in option:
                variant["sourceId"] = option["id"]
            if "name" in option:
                variant["name"] = game_display_text(option["name"])
                variant["nameKo"] = korean.node_name(option["id"], variant["name"])
            if "stats" in option:
                variant.update(display_stats(option, option["id"], korean))
            if option.get("ascendancyName"):
                variant["asc"] = asc_by_name[option["ascendancyName"]]["id"]
            variants[conditions[condition]] = variant
        result["variants"] = variants
    return result


def attach_passive_grants(nodes: dict, raw_nodes: dict, classes: list) -> None:
    classes_by_name = {klass["name"]: klass for klass in classes}
    for node in nodes.values():
        if not node.get("asc"):
            continue
        for line in node["stats"]:
            if match := PASSIVE_POINTS.fullmatch(line):
                node["grantPts"] = int(match[1])
            if match := PASSIVE_START.fullmatch(line):
                target = classes_by_name[match[1]]
                links = {edge["id"] for edge in raw_nodes[node["id"]]["connections"].values()}
                if not set(target["startNeighbors"]) <= links:
                    raise ValueError(f"Node {node['id']} has incomplete granted start links")
                # PoB can use this option as a start, but cannot traverse into it from the main tree.
                node["grantStart"] = target["id"]


def reachable_main(nodes: dict, classes: list, *, include_locked=False) -> set[int]:
    allowed = {
        node_id for node_id, node in nodes.items()
        if node["k"] in MAIN_KINDS and not node.get("asc") and (include_locked or not node.get("unlock"))
    }
    reached = {node_id for klass in classes for node_id in klass["startNeighbors"]} & allowed
    pending = list(reached)
    while pending:
        fresh = (set(nodes[pending.pop()]["adj"]) & allowed) - reached
        reached.update(fresh)
        pending.extend(fresh)
    return reached


def graph_counts(nodes: dict, classes: list, ordinary: set, conditional: set) -> dict:
    by_kind = Counter(MAIN_KINDS[nodes[node_id]["k"]] for node_id in ordinary)
    disconnected = [node for node in nodes.values()
                    if node["k"] in MAIN_KINDS and not node.get("asc") and not node["adj"]]
    ascendancies = [asc for klass in classes for asc in klass["ascendancies"]]
    return {
        "classCount": len(classes),
        "classStartCount": len({klass["startNodeId"] for klass in classes}),
        "ascendancyCount": len(ascendancies),
        "ascendancyReplacementCount": sum(len(asc.get("replacements", [])) for asc in ascendancies),
        "ordinaryReachable": len(ordinary), "ordinaryByKind": dict(sorted(by_kind.items())),
        "reachableWithUnlocks": len(conditional),
        "disconnectedMain": len(disconnected),
        "recipeOnlyMain": sum(bool(node.get("recipeOnly")) for node in disconnected),
        "aliasSocketsMain": sum(bool(node.get("alias")) for node in disconnected),
    }


# PassiveStatParser::ParseLine compares numerical suffixes only within the
# matching unit branch. The PoE1 compiler's legacy support heuristic combines them.
_STAT_CATEGORIES = {
    category: re.compile(r"\b(?:" + terms + r")\b", re.IGNORECASE)
    for category, terms in {
        "attributes": r"strength|dexterity|intelligence|attributes?",
        "life": r"life|lifetap",
        "mana": r"mana",
        "energyShield": r"energy shield",
        "defences": r"armou?r|eva(?:sion|ded?|ding)|(?:global )?defences?|damage (?:reduction|taken)|less damage from hits",
        "resistances": r"resistances?",
        "block": r"block(?:s|ed|ing)?",
        "deflection": r"deflect(?:s|ed|ion|ing)?",
        "damage": r"damage|hits (?:deal|cannot deal)|hits? (?:cannot|can't) be evaded",
        "speed": r"speed",
        "critical": r"critical|non-critical",
        "ailments": (
            r"ailments?|bleed(?:s|ing)?|poisons?|poisoned|ignit(?:e|es|ed|ion|ing)|"
            r"flammability|chill(?:s|ed)?|freez(?:e|es|ing)|frozen|shock(?:s|ed)?|"
            r"electrocut(?:e|ion|ed)|pin(?:ned|ning)?|stun(?:s|ned|ning)?"
        ),
        "charges": r"(?:endurance|frenzy|power) charges?|charges instead",
        "areaOfEffect": r"area of effect",
        "cost": r"cost(?:s| efficiency)?|reserve|reservation",
        "spirit": r"spirit",
        "duration": r"(?:skill effect|buff) duration",
        "cooldown": r"cooldowns?|cooldown recovery",
        "projectiles": r"projectiles?|pierc(?:e|es|ed|ing)|arrows?",
        "charms": r"charms?",
    }.items()
}
_STAT_NUMBER = re.compile(r"[0-9]+(?:\.[0-9]+)?|(?<![0-9])\.[0-9]+")


PARSE_PATH = ROOT / "data/poe2/reports/tree-parse.json"
MIXED_PATH = ROOT / "data/poe2/reports/tree-mixed-nodes.json"


def stat_categories(line: str) -> tuple[str, ...]:
    damage_line = re.sub(r"\bcritical (?:\w+ )?damage bonus\b", "", line, flags=re.IGNORECASE)
    return tuple(
        category for category, pattern in _STAT_CATEGORIES.items()
        if pattern.search(damage_line if category == "damage" else line)
    )


_MECHANIC_REASONS = tuple(
    (re.compile(pattern), reason)
    for pattern, reason in (
        (r"\bminions?\b|puppet master|manifest weapon|infernal hound", "소환수 생성·스탯 계산 체계 없음"),
        (r"\btotems?\b", "토템 설치·배달·스탯 계산 체계 없음"),
        (r"\ballies\b|\bally\b|\bpresence\b|\bsurrounded\b", "주변 아군·적 수와 존재감 판정 체계 없음"),
        (r"\bcompanions?\b|tamed beasts?|tame beast|beast companion", "동료 소환·포획·스탯 계산 체계 없음"),
        (r"\b(?:traps?|mines?|ballistas?|brands?|hazards?|concoctions?)\b", "설치형·제조 투척 스킬 배달 체계 없음"),
        (r"\bflasks?\b", "PoE 플라스크 보유·사용·회복 체계 없음"),
        (r"\bcharms?\b", "호신부 작동 여부·충전·지속시간을 패시브로 수정하는 체계 없음"),
        (r"\brage\b", "격노 축적·소비와 효과 체계 없음"),
        (r"\bglory\b", "위세 축적·소비 체계 없음"),
        (r"\bvalour\b", "용맹 축적·소비 체계 없음"),
        (r"\bvolatility\b", "불안정성 축적·소멸 체계 없음"),
        (r"\bthorns\b", "피격 시 가시 피해를 반격하는 체계 없음"),
        (r"\b(?:curses?|cursed)\b|elemental weakness", "저주의 적용과 상태 추적 체계 없음"),
        (r"\bmarks?\b", "징표 스킬의 적용과 소비 체계 없음"),
        (r"\bofferings?\b", "공물 스킬의 적용과 지속 체계 없음"),
        (r"\bexposure\b", "원소별 노출의 적용과 상태 추적 체계 없음"),
        (r"\bwarcr(?:y|ies)\b|\bbanners?\b", "함성·깃발 스킬과 위세 판정 체계 없음"),
        (r"\barchon\b", "아르콘 상태 및 회복 기간 체계 없음"),
        (r"\bshapeshift\w*\b|\bdemon(?: form|flame)\b|bear spirit|embrace of the wild", "변신 상태·스킬·전용 자원 체계 없음"),
        (r"\b(?:remnants?|infusions?|elemental surge|unbound avatar)\b", "잔해·원소 주입 생성·획득·소비 체계 없음"),
        (r"\b(?:endurance|frenzy|power) charges?\b|\bconsuming (?:them|charges)\b", "충전 획득·소비·만료 체계 없음"),
        (r"\bstun (?:buildup|threshold|recovery)\b|heavy stun", "강한 기절 누적 런타임 입력 및 문턱 판정 없음"),
        (r"\b(?:pin|electrocut)\w*\b", "고정·전기 처형 누적 런타임 입력 없음"),
        (r"\blight stun", "가벼운 기절 상태·면역 판정 체계 없음"),
        (r"\b(?:maim|hinder|impale|incision|daze|blind|wither)\w*\b|corrupted blood",
         "해당 종류의 절단·방해·꿰뚫기·절개·멍해짐·실명·위축·타락한 피 적용 체계 없음"),
        (r"\b(?:leech\w*|recoup\w*)\b", "흡수·피해 회생의 발생량과 지속 회복 원장 없음"),
        (r"\b(?:parry|parried|parrying)\b", "쳐내기 타이밍·성공 사건 체계 없음"),
        (r"\bdodge roll\w*\b", "구르기 타이밍과 회피 판정 체계 없음"),
        (r"\b(?:invocation|invocat(?:ed|ing)|meta skills?|energy spent)\b", "기원·메타 스킬의 에너지 저장 및 비용 체계 없음"),
        (r"\b(?:empowered|ancestrally boosted)\b", "강화된 타격과 선대 증강 표식 체계 없음"),
        (r"\b(?:suppress\w*)\b", "결정 206으로 주문 억제 제거"),
        (r"\baccuracy\b|(?:base |excess )?chance to hit", "결정 207로 플레이어 명중 판정 미사용"),
        (r"\b(?:attribute requirements?|requirements of equipped)\b", "요구치 자체를 퍼센트로 바꾸거나 재사용하는 장비 요구치 계산 없음"),
        (r"\b(?:focus|flails?|spears?|crossbows?|bows?|quarterstaff|quarterstaves|quivers?)\b",
         "PoE 무기·보조장비 종류에 대응하는 장비 또는 공격 배달 없음"),
        (r"\b(?:arrows?|bolts?|projectile attacks?|attack projectile)\b", "플레이어 화살·볼트 투사체 공격의 패시브 배달 없음"),
        (r"\b(?:grenades?|detonator|reload|combo)\b", "수류탄·폭파·재장전·연계 스킬 상태 체계 없음"),
        (r"\b(?:plants?|fissures?|slams?|aftershocks?)\b", "식물·균열·강타·여진 스킬 배달과 타격 분류 없음"),
        (r"\b(?:weapon sets?|weapon swap(?:ping)?|talisman)\b", "무기 세트 교체·부적 장착 체계 없음"),
        (r"\b(?:darkness|infernal flame|jade|owl feathers?|vivid wisp|flames of chayula|sands of time)\b",
         "전직 전용 자원의 생성·예약·최대치·소비 체계 없음"),
        (r"\b(?:adaptations?|adapt to)\b", "피격 원소에 따라 대상을 정하는 적응 버프의 상태·효과 체계 없음"),
        (r"\b(?:arcane surge|onslaught|tailwind|primal bounty)\b", "해당 명명 버프의 획득·지속·효과 체계 없음"),
        (r"\bculling strike\b|decimating strike|crushing blows", "특수 마무리 타격·타격 변형 판정 체계 없음"),
        (r"\b(?:seals?|sealed|sacrifice|sacrificing)\b", "봉인 누적·소비 또는 별도 희생 자원 체계 없음"),
        (r"\b(?:terrain chain|chain an additional time from terrain)\b", "지형 충돌 지점에서 연쇄 대상을 선택해 재발사하는 체계 없음"),
        (r"\b(?:chilled|ignited|shocked|soaring|jagged) ground\b", "바닥 효과 생성·체류 및 상태 판정 체계 없음"),
        (r"\b(?:break armour|break enemy armour|armour break|broken armour)\b|\bbreak [\d.]+% (?:increased|reduced) armour", "방어구 파괴 누적·만료의 런타임 입력 없음"),
        (r"\bconcentration\b", "PoE 적 집중 자원과 타격 시 집중 붕괴 체계 없음"),
        (r"loft|knock\s*back|\bknock (?:back|enemies back)\b", "타격별 넉백 발동·방향·거리와 차단 반응 체계 없음"),
        (r"\b(?:aggravate|aggravated|aggravation)\b", "일반 타격의 조건별 출혈 악화 판정과 기존 출혈 갱신 연결 없음"),
        (r"\blight radius\b", "캐릭터 광원 반경을 변경하고 되돌리는 체계 없음"),
        (r"\b(?:ice crystal|vivid stags?)\b", "스킬로 생성된 얼음 결정·사슴 개체의 스탯 및 행동 체계 없음"),
        (r"\binventory slots?\b|\bcarry a chest\b", "칸 기반 인벤토리 용량 체계 없음"),
        (r"\b(?:quality|level of all)\b.*(?:skills?|gems?)|gem quality", "패시브로 스킬 젬의 유효 레벨·품질을 바꾸는 연결 없음"),
        (r"\bgrants skill:|\btrigger (?:elemental storm|elemental expression)\b|\bgrants (?:unravelling|thaumaturgical)",
         "전직 패시브로 개별 PoE2 스킬을 등록하고 실행하는 체계 없음"),
        (r"\bskill slots?\b", "패시브가 스킬 슬롯 수를 변경하는 저장·입력 체계 없음"),
        (r"\b(?:runes?|idols?|socketed jewel|phylactery|time-lost jewels?|sinister jewel|body armour sockets|helmet socket|gloves socket|boots socket)\b",
         "룬·특수 주얼 장착과 소켓별 스탯 변경 체계 없음"),
        (r"\bring slot\b|bonuses gained from equipped rings", "반지 슬롯 확장과 장신구 어픽스 강도 변경 체계 없음"),
        (r"\b(?:attribute passive skills can instead|connected notable|walk the paths not taken|non-keystone passive skills in|effect of small passive skills|to any attribute)\b",
         "노드 선택지·반경 할당 또는 연결된 주요 패시브 스탯 조회 체계 없음"),
        (r"\b(?:cascadable|echoed|repeatable)\b|different non-instant", "스킬 반복 단계·연쇄 시전 유형 또는 서로 다른 최근 스킬 이력 추적 없음"),
        (r"\b(?:bleeding|freeze|shock|ignite|poison) duration on you\b|\b(?:non-damaging ailments|bleeding) on you\b",
         "플레이어가 받는 상태이상 종류별 지속시간·피해 계수의 독립 수정 연결 없음"),
        (r"\breduced effect of chill on you\b|slowing potency of debuffs|unaffected by slows",
         "플레이어 감속 원인과 적용 강도를 개별 추적하는 체계 없음"),
        (r"\b(?:debuffs you inflict|your chills can slow|damage from hits contributes to chill|two of your chills)\b",
         "대상 감속 종류별 크기·상한 또는 다중 냉각 효과 합산 체계 없음"),
        (r"\b(?:debuffs|buffs) on you expire\b", "플레이어의 모든 버프·디버프 공통 만료 속도 제어 체계 없음"),
        (r"\b(?:multiple|same time|number of your poisons)\b.*(?:poison|shock)|\b(?:poisons?|shocks?)\b.*same time",
         "대상별 패시브 중독·감전 동시 적용 상한을 바꾸는 연결 없음"),
        (r"\b(?:reapply it|consume(?:[sd]|ing)? (?:a )?shock|also shock enemies)\b",
         "대상 감전 소비·재적용 및 주변 대상 전파 체계 없음"),
        (r"\bimmune to poison\b", "플레이어 피격 중독 적용·면역 판정 자체가 없음"),
        (r"\b(?:lightning damage|damage (?:of enemies hitting you|with hits)) (?:to be|is) (?:un)?lucky\b",
         "타격 피해량을 종류별로 두 번 굴려 하나를 선택하는 피해 굴림 체계 없음"),
        (r"\bdamage from blocked hits\b|\bblock recovery\b",
         "성공한 방패 방어가 막는 피해율 및 방어 후 회복 시점을 별도 조절하는 판정 없음"),
        (r"\b(?:chance to defend with|defend with)\b", "개별 타격의 유효 방어도 재굴림 판정 체계 없음"),
        (r"\b(?:damage taken bypasses energy shield|damage taken as|damage from hits taken as|life loss from hits is prevented)\b",
         "받는 피해의 전환·지연 손실 또는 일반 ES 우회 비율을 전달하는 배분 체계 없음"),
        (r"\b(?:evasion rating also grants elemental damage reduction|current energy shield is added|combined armour and evasion)\b",
         "다른 방어 자원을 개별 피격 방어 공식에 합산하는 변환 체계 없음"),
        (r"\b(?:reduced movement speed penalty|less movement speed penalty|movement penalties from armour)\b",
         "스킬 사용·능동 방어·방어구가 만든 개별 이동 감속량의 추적 및 제거 체계 없음"),
        (r"\b(?:using cold skills|using fire skills)\b", "스킬 속성별 사용 중 이동 감속량을 별도로 추적하지 않음"),
        (r"\b(?:chance to avoid death|life loss from hits)\b", "치명 피격 직후 피해 취소·생명 감소 지연을 처리하는 체계 없음"),
        (r"\brecover\b.*\bon kill(?:ing)?\b|\bgain .* (?:life|mana) per enemy killed\b|\bkill.*explode\b",
         "적 처치 사건에 패시브 자원 회복·폭발 효과를 연결하는 분배 체계 없음"),
        (r"\brecover\b.*\b(?:when you block|on block)\b|\b(?:life|mana) gained when you block\b",
         "막기 성공 사건을 주 갱신의 플레이어 자원 회복으로 넘기는 체계 없음"),
        (r"\b(?:base chance to block from equipped shield|max(?:imum)? block chance|maximum chance to evade|maximum physical damage reduction)\b",
         "개별 빌드가 막기 기본값이나 절대 방어 확률 상한을 덮어쓰는 스탯 없음"),
        (r"\b(?:critical hits ignore|hits ignore|treat enemy monster elemental resistance values as inverted)\b",
         "관통과 구별되는 대상 원소 저항 무시·반전 판정 없음"),
        (r"\b(?:of elemental damage is taken from mana|mana before life)\b",
         "특정 피해 종류만 매지카 우선 차감으로 보내는 피해 배분 없음"),
        (r"\b(?:maximum life converted|reserves .* life|life reserve)\b", "최대 생명력 전환과 생명력 예약 계산 체계 없음"),
        (r"\b(?:inherent bonuses gained from attributes|inherent life granted by strength)\b", "능력치의 고유 보너스 배율을 바꾸는 계산 연결 없음"),
        (r"\b(?:physical damage reduction from armour|of armour also applies to chaos damage)\b",
         "카오스를 경감하거나 다른 방어 자원을 합산하는 피격 방어 계산 없음"),
        (r"\b(?:base energy shield recharge delay|recharge start|energy shield recharge delay)\b",
         "ES 재충전 시작 시간에 초 단위 기본값을 더하거나 빼는 계산 연결 없음"),
        (r"\b(?:body armour grants|(?:from|on) equipped (?:body armour|helmet)|normal body armour|boots|gloves|helmet)\b",
         "지정한 방어구 칸의 개별 방어·능력치 또는 조건부 보너스 집계 연결 없음"),
        (r"\b(?:reserve|reserves|reservation|no spirit|spirit from equipment)\b",
         "스킬 분류별 정신력 예약 또는 장비 정신력 차단 계산 없음"),
        (r"\b(?:herald skills|herald of)\b", "전령의 처치 효과에 패시브 피해·범위·치명타를 전달하는 연결 없음"),
        (r"\b(?:non-negative elemental resistances|cold resistance|fire resistance|lightning resistance)\b",
         "대상 상태별 저항 감소 또는 저항 전파·무시 계산 없음"),
        (r"\b(?:gain deflection rating|ailment threshold)\b", "다른 능력치를 편향 또는 상태이상 문턱으로 이전하는 계산 연결 없음"),
        (r"\b(?:chaos resistance is doubled|modifiers to maximum|modifiers to fire)\b",
         "저항 수치 배율·종류 간 전파 규칙 없음"),
        (r"\b(?:chance to inflict bleeding on critical|magnitude of .* with critical hits|critical hits with daggers)\b",
         "치명타와 공격 출처를 상태이상별 적용 확률·크기로 전달하는 연결 없음"),
        (r"\b(?:bleeding magnitude|damage from hits contributes to shock|ignite.*reflected)\b",
         "원소 피해를 출혈·감전에 이전하거나 점화를 반사하는 상태이상 판정 없음"),
        (r"\b(?:at close range|metres to melee strike range)\b", "근접 타격 거리·리치 배율을 받는 판정 없음"),
        (r"\b(?:maximum lightning damage|lightning damage to attacks)\b",
         "패시브가 공격의 기본 번개 피해 범위 또는 번개 상한만 바꾸는 계산 연결 없음"),
        (r"\b(?:base critical hit chance|more critical damage bonus|inevitable critical hits)\b",
         "스킬 치명타 기본값 덮어쓰기 또는 별도 보장·증폭 규칙 없음"),
        (r"\b(?:less armour|less attributes|less evasion|less maximum|less poison duration|less magnitude of)\b",
         "해당 방어·능력치·자원·상태이상에 독립 감폭 배율을 합성하는 스탯 없음"),
        (r"\b(?:less elemental damage taken|take .* less damage)\b",
         "저항·방어도를 거친 뒤 플레이어 피격 피해를 독립 감폭하는 연결 없음"),
        (r"\b(?:spell damage with spells that cost life|skill.*cost|mana costs?|life costs?|cost efficiency)\b",
         "이 스킬 하위 분류 또는 비용 원천을 판별하는 입력 없음"),
        (r"\b(?:penetra\w*|as extra damage|damage as extra)\b",
         "이 효과의 무작위·조건부 피해 원천 또는 관통 적용 유형 판정 없음"),
        (r"\b(?:travel|further than|within .* distance of you|within [+-]?[\d.]+m)\b",
         "투사체 비행 거리 또는 플레이어와 적 사이 거리 문턱을 적중 스탯에 공급하지 않음"),
        (r"\b(?:immobilisation|light stun(?:ned)?)\b", "이동 불가 누적 및 효과 판정 입력 없음"),
        (r"\b(?:strikes deal splash|strike skills|orb skills|limit for elemental skills)\b",
         "타격 스킬 주변 확산 또는 원소 설치물 상한 체계 없음"),
        (r"\bon targets that are not poisoned\b|\benemies you inflict bleeding on\b",
         "대상의 중독 여부 또는 나의 출혈 효과를 후속 피해·회복 제한의 조건으로 전달하는 체계 없음"),
        (r"\benemies frozen by you\b|\b(?:beasts?)\b", "빙결을 부여한 출처 또는 대상 괴물 유형에 따른 스탯 판정 연결 없음"),
        (r"\bsorcery ward\b", "마법 보호막의 전용 재생과 보호 피해 유형 계산 체계 없음"),
        (r"\baura skills have\b", "오라 스킬의 적용 수치에 별도의 스킬 한정 강도 변경 전달 없음"),
        (r"\bno inherent mana regeneration\b", "고유 마나 재생을 차단하고 다른 재생원을 유지하는 자원 규칙 없음"),
        (r"\b(?:attack and cast speed with|cast speed with|attacks with one-handed)\b",
         "스킬 속성·무기 유형 한정의 공격·시전 속도 또는 피격 상태이상 판정 연결 없음"),
    )
)


def unsupported_reason(line: str) -> str:
    low = line.lower()
    if PASSIVE_POINTS.fullmatch(line) or PASSIVE_START.fullmatch(line):
        return "패시브 할당 규칙에 반영되는 문구"
    if "if on low mana" in low:
        return "현재 매지카 비율을 적중 시점의 낮은 마나 조건으로 전달하는 판정 없음"
    for pattern, reason in _MECHANIC_REASONS:
        if pattern.search(low):
            return reason
    if re.search(r"\b(?:recently|in the past|in the last|in the previous)\b", low):
        event = re.search(r"\b(?:if|while|for each|since|when|after|per)\b .+", stat_shape(low))
        return f"최근 전투 사건 이력 미추적 ({event.group() if event else stat_shape(low)})"
    condition = re.search(r"\b(?:while|when|if|against|within|after|per|nearby|for each|for every|on hit)\b.*", stat_shape(low))
    if condition:
        return f"패시브 판정용 조건 입력 없음 ({condition.group()})"
    raise ValueError(f"Missing concrete unsupported stat reason: {line!r}")


def validate_parse_display(displays: dict, kind: str, key: str, entry: dict) -> list:
    if kind == "node":
        parsed = displays.get("node")
    else:
        parsed = displays.get(kind, {}).get(key)
    if parsed is None or [line["line"] for line in parsed] != entry["stats"]:
        raise ValueError(f"tree-parse.json has stale stat text for node display {entry.get('id', key)!r}")
    for line in parsed:
        if line["status"] not in STAT_LEVELS or not isinstance(line["stats"], dict):
            raise ValueError(f"Invalid parser result for {line['line']!r}")
        if line["status"] == "unsupported" and line["stats"]:
            raise ValueError(f"Unexpected mapping for unsupported line {line['line']!r}")
        charge_mapping = len(line["stats"]) == 1 and next(iter(line["stats"])).startswith("chargeGain.")
        if (line["status"] == "mappingOnly") != charge_mapping:
            raise ValueError(f"Charge mapping status differs from parser result for {line['line']!r}")
    return parsed


def stat_shape(line: str) -> str:
    """Replace numeric magnitudes, retaining signs, units and conditions."""
    return _STAT_NUMBER.sub("#", " ".join(line.split()))


def make_id_baseline(pack: dict, known_entries=None) -> dict:
    entries = {kind: {} for kind in ("cls", "asc", "node")}

    def register(kind, identifier, name, meaning):
        if identifier in entries[kind]:
            raise ValueError(f"Duplicate {kind} ID: {identifier}")
        entries[kind][identifier] = [name, hashlib.sha256(json_bytes(meaning)).hexdigest()]

    for klass in pack["classes"]:
        classes = klass["ascendancies"]
        register("cls", klass["contentId"], klass["name"], [
            klass["source"]["key"], klass["startNodeId"], klass["startNeighbors"],
            sorted(asc["id"] for asc in classes if known_entries is None or asc["id"] in known_entries["asc"]),
        ])
        for base in classes:
            for asc in (base, *base.get("replacements", [])):
                register("asc", asc["id"], asc["name"], [
                    klass["contentId"], asc["source"]["key"], asc["startNodeId"], base["id"],
                ])
    class_ids = {klass["id"]: klass["contentId"] for klass in pack["classes"]}
    for node in pack["nodes"]:
        meaning = {field: node[field] for field in (
            "k", "asc", "jewelSocket", "freeAlloc", "unlock", "mc", "mco", "mcParent", "alias", "recipeOnly",
        ) if field in node}
        if "grantStart" in node:
            meaning["grantStart"] = class_ids[node["grantStart"]]
        if "grantPts" in node:
            meaning["grantPts"] = node["grantPts"]
        meaning["adj"] = [ref for ref in node["adj"] if known_entries is None or str(ref) in known_entries["node"]]
        meaning["displays"] = [
            [kind, key, entry["name"], entry.get("asc"), entry.get("sourceId"),
             [stat_shape(line) for line in entry["stats"]]]
            for kind, key, entry in node_displays(node)
        ]
        register("node", str(node["id"]), node["name"], meaning)
    return {"contentGeneration": pack["meta"]["contentGeneration"], "entries": entries}


def validate_id_baseline(pack: dict, baseline: dict, migrations: Path = ROOT / "data/migrations") -> None:
    original = baseline["entries"]
    rebuilt = make_id_baseline(pack, original)
    source = baseline["contentGeneration"]
    target = rebuilt["contentGeneration"]
    current = rebuilt["entries"]
    if target == source + 1:
        load_table(migrations / f"gen{source}-to-gen{target}.json", original, current)
        return
    if target != source:
        raise ValueError(f"Expected generation {source} or {source + 1}, got {target}")
    for kind, entries in original.items():
        for identifier, signature in entries.items():
            if current[kind].get(identifier) != signature:
                raise ValueError(f"Generation {source} {kind} ID {identifier} was removed or changed meaning; "
                                 "preserve the ID or publish a migration to the next generation")


# Decision 215 requires every line of each selected keystone to map together.
_KEYSTONE_REQUIRED = {
    "Avatar of Fire": "스킬 타격의 75% 화염 변환 및 남은 비화염 피해 제거.",
    "Blackflame Covenant": "화염 주문의 화염을 카오스로 변환하고 가연성·점화 크기 및 점화 피해 유형에 연결.",
    "Blood Magic": "최대 PoE 마나 0 및 기존 생명력 비용 경로 사용.",
    "Chaos Inoculation": "최대 생명력 1, 카오스 피해와 출혈 면역.",
    "Dance with Death": "한 손 무기와 빈 보조 손 조건에서 공격·시전 속도 25% 증폭.",
    "Eldritch Battery": "최대 에너지 보호막을 마나로 변환하고 마나 비용을 두 배로 계산.",
    "Glancing Blows": "회피 불리 및 편향 유리 판정.",
    "Iron Reflexes": "회피를 방어도로 전부 변환.",
    "Mind Over Matter": "PoE 피해를 마나에서 먼저 차감하고 PoE 마나 재생과 스카이림 매지카 재생에 50% 감소 적용.",
    "Pain Attunement": "충만 및 낮은 생명력에서 각각 치명타 피해 보너스 30% 감폭 및 증폭.",
    "Resonance": "권능·격분·인내의 획득 종류 매핑을 구현함. 게임 내 충전 획득 경로가 없어 실제 획득 효과는 없음.",
    "Crimson Assault": "출혈 악화, 기본 지속시간 및 크기 계산에 연결함.",
    "Zealot's Oath": "PoE 생명 재생 초과분만 에너지 보호막으로 회복하며 재충전을 끔.",
}
_KEYSTONE_DEFERRED = {
    "Ancestral Bond": "토템 수·설치와 토템별 정신력 예약 체계가 필요함.",
    "Bulwark": "구르기 중 피해 회피와 경감 판정이 필요함.",
    "Conduit": "존재 범위 안 아군에게 충전을 넘기는 체계가 필요함.",
    "Elemental Equilibrium": "원소 주입 잔해의 생성·유형 전환 체계가 필요함.",
    "Eternal Youth": "생명력 재충전 경로와 플라스크 회복 체계가 필요함.",
    "Giant's Blood": "한 손 양손 무기와 무기별 능력치 요구 조건 판정이 필요함.",
    "Heartstopper": "플레이어가 지속 피해를 받기 시작한 시점을 추적하는 체계가 필요함.",
    "Hollow Palm Technique": "비무장 시 육척봉 스킬 허용, 스킬 레벨로 비무장 피해를 대체하는 체계가 필요함.",
    "Lord of the Wilds": "부적과 셉터 동시 장착, 소환수 기준 예약 효율 체계가 필요함.",
    "Necromantic Talisman": "소환수에 장착한 목걸이의 보너스를 이전하는 체계가 필요함.",
    "Oasis": "호신부는 있으나 플라스크의 PoE2 회복 체계가 필요함.",
    "Primal Hunger": "격노의 최대치·소비 기록·재생 및 효과 체계가 필요함.",
    "Resolute Technique": "치명타 제한은 있으나 결정 207로 플레이어 명중 수치가 쓰이지 않음.",
    "Ritual Cadence": "기원 스킬의 에너지 소비와 주기적인 발동 체계가 필요함.",
    "Scarred Faith": "방지한 물리 피해와 적 위세 기반 에너지 보호막 피해 흡수 회복이 필요함.",
    "Trusted Kinship": "동료 유형 제한과 동료 스킬의 예약 효율 체계가 필요함.",
    "Unwavering Stance": "가벼운 기절과 구르기·질주를 이 효과로 제어하는 체계가 필요함.",
    "Vaal Pact": "생명력 흡수량·속도·유지 및 회복원 제한 체계가 필요함.",
    "Whispers of Doom": "저주 수 상한과 활성화 지연 체계가 필요함.",
    "Wildsurge Incantation": "폭풍과 식물 주문 계열을 식별해 피해·비용·지속시간을 바꾸는 체계가 필요함.",
}


def keystone_review(node: dict, statuses: set[str]) -> tuple[str, str]:
    name = node["name"]
    if name in _KEYSTONE_REQUIRED:
        required = "mappingOnly" if name == "Resonance" else "full"
        if statuses != {required}:
            raise ValueError(f"Required keystone not mapped: {name}")
        return ("mappingOnly" if required == "mappingOnly" else "implemented"), _KEYSTONE_REQUIRED[name]
    if statuses != {"unsupported"}:
        raise ValueError(f"Deferred keystone has a gameplay effect: {name}")
    return "deferred", _KEYSTONE_DEFERRED[name]


def node_displays(node: dict):
    yield "node", "", node
    for option in node.get("attrOptions", []):
        yield "attributeOption", str(option["id"]), option
    for key, variant in node.get("variants", {}).items():
        yield "variant", key, variant


def implemented_structure(node, line, unlock_sources):
    return ("grantPts" in node and PASSIVE_POINTS.fullmatch(line) or
            "grantStart" in node and PASSIVE_START.fullmatch(line) or
            node["id"] in unlock_sources and line == "Walk the Paths Not Taken")


def build_report(nodes: dict, classes: list, ordinary: set, parser_report: dict,
                 mixed_report: dict) -> tuple[dict, dict]:
    if parser_report.get("contentGeneration") != 1 or set(parser_report["nodes"]) != {str(i) for i in nodes}:
        raise ValueError("tree-parse.json generation or node IDs differ from the compiled tree")
    mixed = {entry["id"]: entry for entry in mixed_report["nodes"]}
    if mixed_report["contentGeneration"] != 1 or len(mixed) != len(mixed_report["nodes"]) or mixed.keys() - nodes.keys():
        raise ValueError("tree-mixed-nodes.json generation or node IDs differ from the compiled tree")
    all_lines, ordinary_lines, selected_lines, selected_ordinary = (Counter() for _ in range(4))
    by_category = defaultdict(Counter)
    by_category_ordinary = defaultdict(Counter)
    incomplete_shapes = {status: {} for status in STAT_LEVELS if status != "full"}
    ordinary_nodes = Counter()
    selected_ordinary_nodes = Counter()
    ordinary_nodes_by_kind = defaultdict(Counter)
    keystones = []
    name_counts = defaultdict(Counter)
    missing_names = []
    missing_stats = Counter()
    stat_total = 0
    attribute_display_lines = 0
    unlock_sources = {required for node in nodes.values() for required in node.get("unlock", [])}

    def count_name(kind, entry, identifier):
        name_counts[kind]["total"] += 1
        if entry["nameKo"] == entry["name"]:
            name_counts[kind]["missing"] += 1
            missing_names.append({"kind": kind, "id": identifier, "name": entry["name"]})

    for klass in classes:
        count_name("class", klass, klass["contentId"])
        for base in klass["ascendancies"]:
            for asc in (base, *base.get("replacements", [])):
                count_name("ascendancy", asc, asc["id"])

    for node_id, node in nodes.items():
        node_statuses = set()
        node_deferred = []
        node_category_levels = defaultdict(list)
        mixed_entry = mixed.get(node_id, {})
        mixed_causes = {*mixed_entry.get("unsupportedDrawbacks", []),
                        *mixed_entry.get("unsupportedManaRegenReplacement", [])}
        found_causes = set()
        parsed_node = parser_report["nodes"][str(node_id)]
        if set(parsed_node.get("attributeOptions", {})) != {str(o["id"]) for o in node.get("attrOptions", [])} or (
            set(parsed_node.get("variants", {})) != set(node.get("variants", {}))
        ):
            raise ValueError(f"tree-parse.json has stale choices or variants for node {node_id}")
        for entry_kind, key, entry in node_displays(node):
            parsed = validate_parse_display(
                parsed_node, {"attributeOption": "attributeOptions", "variant": "variants"}.get(entry_kind, entry_kind),
                key, entry,
            )
            count_name(entry_kind, entry, node_id)
            for line, translated in zip(entry["stats"], entry["statsKo"], strict=True):
                stat_total += 1
                if line == translated:
                    missing_stats[line] += 1
            if entry_kind == "node" and node["k"] == KIND_ATTRIBUTE:
                attribute_display_lines += len(entry["stats"])
                continue
            display_causes = mixed_causes.intersection(entry["stats"])
            found_causes.update(display_causes)
            withheld = mixed_entry.get("treatment") == "deferred" and bool(display_causes)
            support = []
            for parsed_line in parsed:
                line = parsed_line["line"]
                status = parsed_line["status"]
                reason = None
                if withheld and line not in display_causes:
                    status = "unsupported"
                    reason = "미구현 대가와 함께 노드 효과 보류 (" + " / ".join(sorted(display_causes)) + ")"
                elif status == "unsupported" and implemented_structure(node, line, unlock_sources):
                    status = "full"
                elif status == "mappingOnly":
                    reason = _KEYSTONE_REQUIRED["Resonance"]
                elif status != "full":
                    reason = _KEYSTONE_DEFERRED.get(node["name"]) or unsupported_reason(line)
                support.append({"status": status, **({"reason": reason} if reason else {})})
            display_statuses = {line["status"] for line in support}
            deferred = withheld or (bool(display_statuses) and
                display_statuses <= {"unsupported", "mappingOnly"} and not node.get("jewelSocket"))
            if entry_kind == "variant" or deferred:
                entry["deferred"] = deferred
            if support:
                entry["statSupport"] = support
            if entry_kind == "variant" and entry["stats"] == node["stats"] and support == node.get("statSupport", []):
                continue
            node_deferred.append(deferred)
            for parsed_line, effect in zip(parsed, support, strict=True):
                line, status = parsed_line["line"], effect["status"]
                categories = stat_categories(line)
                node_statuses.add(parsed_line["status"])
                all_lines[status] += 1
                if node_id in ordinary:
                    ordinary_lines[status] += 1
                if categories:
                    selected_lines[status] += 1
                    if node_id in ordinary:
                        selected_ordinary[status] += 1
                for category in categories:
                    by_category[category][status] += 1
                    node_category_levels[category].append((status, deferred))
                    if node_id in ordinary:
                        by_category_ordinary[category][status] += 1
                if status != "full":
                    shape = (stat_shape(line), effect["reason"])
                    shapes = incomplete_shapes[status]
                    if shape not in shapes:
                        shapes[shape] = {
                            "shape": shape[0], "example": line, "statKinds": set(),
                            "nodes": set(), "lineCount": 0, "ordinaryLineCount": 0,
                            "reason": effect["reason"],
                        }
                    pattern = shapes[shape]
                    pattern["statKinds"].update(categories)
                    pattern["nodes"].add(node_id)
                    pattern["lineCount"] += 1
                    if node_id in ordinary:
                        pattern["ordinaryLineCount"] += 1
        if found_causes != mixed_causes:
            raise ValueError(f"tree-mixed-nodes.json has stale drawback text for node {node_id}")
        if node_id in ordinary:
            effects = [line["status"] for _, _, display in node_displays(node)
                       for line in display.get("statSupport", [])]
            ordinary_nodes[node_level(effects, node_deferred)] += 1
            selected_effects = [effect for examples in node_category_levels.values() for effect in examples]
            if selected_effects:
                statuses, deferred_flags = zip(*selected_effects)
                selected_ordinary_nodes[node_level(statuses, deferred_flags)] += 1
            for category, examples in node_category_levels.items():
                statuses, deferred_flags = zip(*examples)
                ordinary_nodes_by_kind[category][node_level(statuses, deferred_flags)] += 1
        if node["k"] == KIND_KEYSTONE and node_id in ordinary:
            review, reason = keystone_review(node, node_statuses)
            keystones.append({
                "id": node_id, "name": node["name"], "nameKo": node["nameKo"],
                "stats": node["stats"], "statsKo": node["statsKo"],
                "review": review, "reason": reason,
            })

    def levels(counts):
        return {status: counts[status] for status in STAT_LEVELS}

    def shapes_for(status):
        shapes = [
            {"shape": pattern["shape"], "example": pattern["example"], "reason": pattern["reason"],
             "statKinds": sorted(pattern["statKinds"]), "lineCount": pattern["lineCount"],
             "ordinaryLineCount": pattern["ordinaryLineCount"],
             "nodeCount": len(pattern["nodes"]), "ordinaryNodeCount": len(ordinary & pattern["nodes"])}
            for pattern in incomplete_shapes[status].values()
        ]
        return sorted(shapes, key=lambda p: (-p["nodeCount"], -p["lineCount"], p["shape"], p["reason"]))

    missing_count = sum(missing_stats.values())
    unsupported = shapes_for("unsupported")
    by_reason = defaultdict(Counter)
    for entry in unsupported:
        by_reason[entry["reason"]]["lines"] += entry["lineCount"]
        by_reason[entry["reason"]]["ordinaryLines"] += entry["ordinaryLineCount"]
        by_reason[entry["reason"]]["shapes"] += 1
    coverage = {
        "countUnit": "Effect line occurrences in base nodes, attribute choices and changed variants",
        "statKindsOverlap": True,
        "attributeDisplayLinesExcluded": attribute_display_lines,
        "all": levels(all_lines), "ordinaryPath": levels(ordinary_lines),
        "decision213": levels(selected_lines), "decision213OrdinaryPath": levels(selected_ordinary),
        "byStatKind": {category: levels(counts) for category, counts in sorted(by_category.items())},
        "byStatKindOrdinaryPath": {category: levels(by_category_ordinary[category]) for category in sorted(by_category)},
        "nodes": {
            "rule": "All selectable displays must apply for full. Nodes with no effect lines are counted as full only when they provide structural functionality. Stat kinds may overlap.",
            "ordinaryPath": {level: ordinary_nodes[level] for level in NODE_LEVELS},
            "decision213OrdinaryPath": {level: selected_ordinary_nodes[level] for level in NODE_LEVELS},
            "byStatKindOrdinaryPath": {
                category: {level: counts[level] for level in NODE_LEVELS}
                for category, counts in sorted(ordinary_nodes_by_kind.items())
            },
        },
        "deferredNodeCount": sum(bool(node.get("deferred")) for node in nodes.values()),
        "deferredVariantCount": sum(variant["deferred"] for node in nodes.values()
                                    for variant in node.get("variants", {}).values()),
        "mixedNodes": mixed_report["nodes"],
        "partialShapes": shapes_for("partial"),
        "mappingOnlyShapes": shapes_for("mappingOnly"),
        "unsupportedShapes": unsupported,
        "unsupportedByReason": dict(sorted(by_reason.items())),
        "keystoneReviewStatus": "decision215",
        "keystones": sorted(keystones, key=lambda ks: ks["name"]),
    }
    localization = {
        "names": {
            kind: {"total": counts["total"], "translated": counts["total"] - counts["missing"],
                   "missing": counts["missing"]}
            for kind, counts in sorted(name_counts.items())
        },
        "stats": {"total": stat_total, "translated": stat_total - missing_count, "missing": missing_count},
        "missingNames": missing_names,
        "missingStats": [{"line": line, "count": count}
                         for line, count in sorted(missing_stats.items(), key=lambda item: (-item[1], item[0]))],
    }
    return coverage, localization


def node_level(statuses, deferred_flags):
    if deferred_flags and all(deferred_flags):
        return "deferred"
    if not statuses or all(status == "full" for status in statuses):
        return "full"
    if any(status in {"full", "partial"} for status in statuses):
        return "partial"
    return "unsupported"


def compile_source(pob: Path, game: Path) -> dict:
    raw = extract(pob, "0_5", GAME_VERSION, None, "tree")
    if raw["meta"]["pobVersion"] != POB_VERSION:
        raise ValueError(f"Expected PoB {POB_VERSION}, found {raw['meta']['pobVersion']}")
    source_sha = raw["meta"]["sourceSha256"]

    def read_input(key: str, path: Path) -> bytes:
        contents = path.read_bytes()
        source_sha[key] = hashlib.sha256(contents).hexdigest()
        return contents

    description = game / "stat_descriptions.csd"
    korean = KoreanTables(game / "tables/data/balance", description if description.is_file() else None,
                          read_input)
    tree = lua_value(raw["tree"])
    raw_nodes = {key: value for key, value in tree["nodes"].items() if key != "root"}
    nodes = {key: value for key, value in sorted(raw_nodes.items()) if not value.get("isOnlyImage")}
    groups = tree["groups"]
    radii, angles = (tree["constants"][key] for key in ("orbitRadii", "orbitAnglesByOrbit"))
    positions = {}
    for node_id, node in nodes.items():
        group = groups[node["group"]]
        orbit = node["orbit"] + 1
        angle = angles[orbit][node["orbitIndex"] + 1]
        radius = radii[orbit]
        positions[node_id] = (group["x"] + math.sin(angle) * radius,
                              group["y"] - math.cos(angle) * radius, angle)

    adj, edges, excluded = build_connections(raw_nodes, nodes)
    classes, class_starts, asc_by_name, conditions = compile_classes(tree["classes"], nodes, adj, korean)
    packed_nodes = {
        node_id: node_entry(node, positions, adj, class_starts, asc_by_name, conditions, korean)
        for node_id, node in nodes.items()
    }
    attach_passive_grants(packed_nodes, nodes, classes)
    for node in packed_nodes.values():
        if node.get("mco"):
            parents = [other for other in node["adj"] if packed_nodes[other].get("mc")]
            if len(parents) != 1:
                raise ValueError(f"Choice option {node['id']} has {len(parents)} parents")
            node["mcParent"] = parents[0]
        for prerequisite in node.get("unlock", []):
            if prerequisite not in packed_nodes:
                raise ValueError(f"Node {node['id']} has missing unlock prerequisite {prerequisite}")

    visual_edges = []
    indices = {node_id: index for index, node_id in enumerate(nodes)}
    for (a, b), (first, second, orbit) in sorted(edges.items()):
        visual_edges.append([
            indices[a], indices[b], int("ascendancyName" in nodes[first]),
            *connector_arc(nodes[first], nodes[second], orbit, groups, radii, positions),
        ])
    ordinary = reachable_main(packed_nodes, classes)
    conditional = reachable_main(packed_nodes, classes, include_locked=True)
    counts = graph_counts(packed_nodes, classes, ordinary, conditional)
    bounds = {key: round(fun(node[axis] for node in packed_nodes.values()), 6)
              for key, axis, fun in (("minX", "x", min), ("maxX", "x", max),
                                     ("minY", "y", min), ("maxY", "y", max))}
    meta = {
        "schema": "skyrimpoe.tree", "schemaVersion": SCHEMA_VERSION, "game": "poe2",
        "gameVersion": GAME_VERSION, "contentGeneration": 1, "pobVersion": POB_VERSION,
        "extractorVersion": "compile_tree_poe2/2", "gameDataVersion": GAME_DATA_VERSION,
        "sourceSha256": dict(sorted(source_sha.items())), "pobManifestSha1": raw["meta"]["pobManifestSha1"],
        **counts, "nodeCount": len(nodes), "onlyImageExcluded": len(raw_nodes) - len(nodes),
        "visualEdgeCount": len(visual_edges), "masteryEffectCount": 0,
    }
    pack = {
        "schemaVersion": SCHEMA_VERSION, "poeVersion": GAME_VERSION, "bounds": bounds,
        "classes": classes, "masteryGroups": {}, "masteryEffects": {},
        "nodes": list(packed_nodes.values()), "visualEdges": visual_edges, "meta": meta,
    }
    return {"tree": pack, "graph": {**counts, "excludedConnections": excluded},
            "keywordPopupTranslations": korean.popup_hits}


def finalize_tree(source: dict, parser_report: dict, mixed_report: dict) -> tuple[dict, dict]:
    pack = source["tree"]
    validate_id_baseline(pack, json.loads(BASELINE_PATH.read_bytes()))
    classes = pack["classes"]
    nodes = {node["id"]: node for node in pack["nodes"]}
    coverage, localization = build_report(nodes, classes, reachable_main(nodes, classes), parser_report, mixed_report)
    meta = pack["meta"]
    meta.update(
        koNameHits=localization["names"]["node"]["translated"],
        statLinesTotal=localization["stats"]["total"],
        statLinesKo=localization["stats"]["translated"],
    )
    popup_count = source["keywordPopupTranslations"]
    localization["fallback"] = {
        "source": "0.5.5h keywordpopups (en and ko)",
        "matching": "Pair rows by Id. Match the complete normalized English definition after CSD misses; ignore link markup, whitespace and a trailing period. Accept only one distinct Korean result.",
        "translated": popup_count,
        "missingWithoutFallback": localization["stats"]["missing"] + popup_count,
    }
    report = {
        "meta": {**meta, "schema": "skyrimpoe.treeCoverage", "schemaVersion": 1},
        "graph": source["graph"],
        "coverage": coverage, "korean": localization,
    }
    return pack, report


def compile_tree(pob: Path, game: Path, parse_report: Path = PARSE_PATH,
                 mixed_report: Path = MIXED_PATH) -> tuple[dict, dict]:
    return finalize_tree(compile_source(pob, game), json.loads(parse_report.read_bytes()),
                         json.loads(mixed_report.read_bytes()))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pob", type=Path, default=DEFAULT_POB)
    parser.add_argument("--game", type=Path, default=DEFAULT_GAME, help="Extracted Korean client data root")
    parser.add_argument("--parse-report", type=Path, default=PARSE_PATH)
    parser.add_argument("--mixed-report", type=Path, default=MIXED_PATH)
    pass_mode = parser.add_mutually_exclusive_group()
    pass_mode.add_argument("--emit-source", type=Path)
    pass_mode.add_argument("--source", type=Path)
    parser.add_argument("--out", type=Path, default=PACK_PATH)
    parser.add_argument("--report", type=Path, default=REPORT_PATH)
    args = parser.parse_args()
    if args.out.resolve() == args.report.resolve():
        parser.error("--out and --report must be different files")
    try:
        if args.emit_source:
            write_atomic(args.emit_source, json_bytes(compile_source(args.pob, args.game)))
            return
        source = json.loads(args.source.read_bytes()) if args.source else compile_source(args.pob, args.game)
        pack, report = finalize_tree(source, json.loads(args.parse_report.read_bytes()),
                                     json.loads(args.mixed_report.read_bytes()))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    pack_contents = json_bytes(pack)
    write_atomic(args.out, pack_contents)
    write_atomic(args.report, json_bytes(report, indent=2))
    print(f"Compiled {len(pack['nodes'])} nodes, {len(pack_contents)} bytes. Report: {args.report}")


if __name__ == "__main__":
    main()
