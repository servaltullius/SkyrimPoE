#!/usr/bin/env python3
"""Compile PoB 2.68.1 TreeData/3_30/tree.lua into data/packs/tree_3_30.json.

Implements Stage 3 S3-M1 (Decisions 6, 8, 10, 27, 104, 151-155):
- Filters to 7 base classes + 19 base ascendancies (matched by internal id: e.g. Raider -> Warden).
- Excludes root, ungrouped, isProxy (Putrid Seed), isBloodline (147), isBlighted (30),
  non-base ascendancies, and unreachable non-mastery orphan nodes (e.g. 7 Forbidden Flame/Flesh notables).
- Separates visualEdges from rule adjacency; keeps classStart neighbors in classes[*].startNeighbors
  (reading both `out` and `in` so Scion's 6 `in`-only neighbors are preserved, while blocking
  cross-class traversal through other classes' start nodes).
- Preserves isMultipleChoice (7 hubs), isMultipleChoiceOption (20 options), choiceParentId,
  grantedPassivePoints (18 Ascendant nodes), and grantsClassStart (6 Path of the ... nodes).
- Splits multiline `\n` stat strings on both nodes and masteryEffects.
- Normalizes masteryEffects into a top-level table (unique effectId -> stats/statsKo).
- Merges Korean display names and stat descriptions from Korean PoB CSVs when available.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
import re
from collections import deque
from pathlib import Path
from typing import Any

# ---- Constants ----
KIND_NORMAL = 0
KIND_NOTABLE = 1
KIND_KEYSTONE = 2
KIND_MASTERY = 3
KIND_SOCKET = 4
KIND_CLASS_START = 5
KIND_ASCEND_START = 6

ANGLES_16 = [0, 30, 45, 60, 90, 120, 135, 150, 180, 210, 225, 240, 270, 300, 315, 330]
ANGLES_40 = [
    0, 10, 20, 30, 40, 45, 50, 60, 70, 80, 90, 100, 110, 120, 130, 135, 140, 150, 160, 170,
    180, 190, 200, 210, 220, 225, 230, 240, 250, 260, 270, 280, 290, 300, 310, 315, 320, 330, 340, 350,
]

BASE_ASCENDANCY_IDS = (
    "Ascendant",
    "Juggernaut", "Berserker", "Chieftain",
    "Raider", "Deadeye", "Pathfinder",
    "Occultist", "Elementalist", "Necromancer",
    "Slayer", "Gladiator", "Champion",
    "Inquisitor", "Hierophant", "Guardian",
    "Assassin", "Trickster", "Saboteur",
)
BASE_ASCENDANCY_SET = frozenset(BASE_ASCENDANCY_IDS)

CLASS_KO_NAMES = {
    "Scion": "사이온",
    "Marauder": "머라우더",
    "Ranger": "레인저",
    "Witch": "위치",
    "Duelist": "듀얼리스트",
    "Templar": "템플러",
    "Shadow": "섀도우",
}

ASCENDANCY_KO_NAMES = {
    "Ascendant": "어센던트",
    "Juggernaut": "저거넛",
    "Berserker": "버서커",
    "Chieftain": "치프틴",
    "Raider": "워든",
    "Deadeye": "데드아이",
    "Pathfinder": "패스파인더",
    "Occultist": "오컬티스트",
    "Elementalist": "엘리멘탈리스트",
    "Necromancer": "네크로맨서",
    "Slayer": "슬레이어",
    "Gladiator": "글래디에이터",
    "Champion": "챔피언",
    "Inquisitor": "인퀴지터",
    "Hierophant": "하이로펀트",
    "Guardian": "가디언",
    "Assassin": "어쌔신",
    "Trickster": "트릭스터",
    "Saboteur": "사보추어",
}

PATH_START_RE = re.compile(r"Can Allocate Passives from the (\w+)'s starting point", re.IGNORECASE)

DEFERRED_KEYWORDS = (
    "flask", "totem", "minion", "zombie", "spectre", "golem", "skeleton", "raging spirit",
    "trap", "mine", "brand", "warcry", "warcries", "banner", "offering", "link skill",
    "tincture", "graft", "animate", "phantasm", "herald", "aura", "reservation",
    "curse", "hex", "mark", "bleed", "bleeding", "poison", "ignite", "impale", "corrupted blood",
    "leech", "ward", "fortif", "rage", "impaling",
)

SUPPORTED_HINT_KEYWORDS = (
    "strength", "dexterity", "intelligence", "all attributes",
    "maximum life", "maximum mana", "maximum energy shield",
    "energy shield recharge", "armour", "evasion", "global defences",
    "fire resistance", "cold resistance", "lightning resistance", "chaos resistance",
    "elemental resistance", "physical damage reduction",
    "chance to block", "suppress spell damage", "suppressed spell damage",
    "physical damage", "melee damage", "attack damage", "weapon damage",
    "elemental damage", "fire damage", "cold damage", "lightning damage",
    "spell damage", "area damage", "increased damage",
    "attack speed", "cast speed", "critical strike", "area of effect",
    "mana cost", "cost efficiency", "endurance charge", "frenzy charge", "power charge",
    "removes all mana", "chaos damage", "never deal critical", "converts all evasion",
    "cannot evade", "cannot be stunned", "taken from mana before life",
    "pain attunement", "iron will", "can allocate passives from",
    "grants 1 passive skill point", "grants 2 passive skill points",
)


# ---- Lua table parser ----
class LuaTableParser:
    def __init__(self, text: str) -> None:
        self.text = text
        self.pos = 0
        self.length = len(text)

    def parse(self) -> Any:
        self._skip()
        if self.text.startswith("return", self.pos):
            after = self.pos + len("return")
            if after >= self.length or not (self.text[after].isalnum() or self.text[after] == "_"):
                self.pos = after
        val = self._value()
        self._skip()
        if self.pos != self.length:
            raise ValueError(f"Trailing unparsed Lua data at pos {self.pos}")
        return val

    def _skip(self) -> None:
        text, n = self.text, self.length
        while self.pos < n:
            c = text[self.pos]
            if c in " \t\r\n":
                self.pos += 1
            elif text.startswith("--", self.pos):
                end = text.find("\n", self.pos)
                self.pos = n if end < 0 else end + 1
            else:
                return

    def _value(self) -> Any:
        self._skip()
        if self.pos >= self.length:
            raise ValueError("Unexpected EOF while parsing Lua value")
        c = self.text[self.pos]
        if c == "{":
            return self._table()
        if c in ('"', "'"):
            return self._string(c)
        if self.text.startswith("true", self.pos) and self._word_boundary(4):
            self.pos += 4
            return True
        if self.text.startswith("false", self.pos) and self._word_boundary(5):
            self.pos += 5
            return False
        if self.text.startswith("nil", self.pos) and self._word_boundary(3):
            self.pos += 3
            return None
        return self._number()

    def _word_boundary(self, length: int) -> bool:
        nxt = self.pos + length
        if nxt >= self.length:
            return True
        ch = self.text[nxt]
        return not (ch.isalnum() or ch == "_")

    def _string(self, quote: str) -> str:
        text = self.text
        self.pos += 1
        parts: list[str] = []
        while True:
            if self.pos >= self.length:
                raise ValueError("Unterminated Lua string")
            c = text[self.pos]
            if c == quote:
                self.pos += 1
                return "".join(parts)
            if c == "\\":
                self.pos += 1
                if self.pos >= self.length:
                    raise ValueError("Unterminated escape sequence in Lua string")
                e = text[self.pos]
                if e.isdigit():
                    digits = ""
                    while len(digits) < 3 and self.pos < self.length and text[self.pos].isdigit():
                        digits += text[self.pos]
                        self.pos += 1
                    parts.append(chr(int(digits)))
                    continue
                parts.append({"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'"}.get(e, e))
                self.pos += 1
            else:
                end = self.pos
                while end < self.length and text[end] != quote and text[end] != "\\":
                    end += 1
                parts.append(text[self.pos:end])
                self.pos = end

    def _number(self) -> int | float:
        text = self.text
        start = self.pos
        while self.pos < self.length and (text[self.pos].isalnum() or text[self.pos] in "+-."):
            self.pos += 1
        token = text[start:self.pos]
        if not token:
            raise ValueError(f"Unreadable token at {start}: {text[start:start + 20]!r}")
        try:
            return int(token)
        except ValueError:
            return float(token)

    def _table(self) -> Any:
        text = self.text
        self.pos += 1
        keyed: dict[Any, Any] = {}
        positional: list[Any] = []
        while True:
            self._skip()
            if self.pos >= self.length:
                raise ValueError("Unterminated Lua table")
            c = text[self.pos]
            if c == "}":
                self.pos += 1
                break
            if c == "[":
                self.pos += 1
                key = self._value()
                self._skip()
                if text[self.pos] != "]":
                    raise ValueError(f"Expected ']' at pos {self.pos}")
                self.pos += 1
                self._skip()
                if text[self.pos] != "=":
                    raise ValueError(f"Expected '=' at pos {self.pos}")
                self.pos += 1
                keyed[key] = self._value()
            elif c.isalpha() or c == "_":
                end = self.pos
                while end < self.length and (text[end].isalnum() or text[end] == "_"):
                    end += 1
                name = text[self.pos:end]
                look = end
                while look < self.length and text[look] in " \t\r\n":
                    look += 1
                if look < self.length and text[look] == "=" and name not in ("true", "false", "nil"):
                    self.pos = look + 1
                    keyed[name] = self._value()
                else:
                    positional.append(self._value())
            else:
                positional.append(self._value())
            self._skip()
            if self.pos < self.length and text[self.pos] in ",;":
                self.pos += 1
        if keyed and positional:
            for idx, val in enumerate(positional, start=1):
                keyed.setdefault(idx, val)
            return keyed
        return keyed if keyed else positional


def as_list(val: Any) -> list[Any]:
    if val is None:
        return []
    if isinstance(val, dict):
        return [val[k] for k in sorted(val, key=lambda x: (not isinstance(x, int), x))]
    return list(val)


def orbit_angles(count: int) -> list[float]:
    if count == 16:
        degrees = ANGLES_16
    elif count == 40:
        degrees = ANGLES_40
    else:
        degrees = [360.0 * i / count for i in range(count + 1)]
    return [math.radians(d) for d in degrees]


def split_stat_line_groups(raw_stats: list[Any]) -> list[list[str]]:
    """PoB 원문 스탯을 논리적 문장 단위로 묶는다. 묶음 하나는 원문 줄(조각) 목록이다.

    C-31: 한 원문 항목 안에서 문장 중간 줄바꿈(소문자로 이어지는 줄)은 앞 줄과 한 문장으로 묶는다.
    재검수 R-6: 한글 번역은 원문 조각 단위로도 찾아야 하므로 조각을 버리지 않고 남긴다.
    """
    groups: list[list[str]] = []
    for entry in raw_stats:
        if not isinstance(entry, str):
            continue
        parts = [p.strip() for p in entry.replace("\r\n", "\n").replace("\r", "\n").split("\n") if p.strip()]
        entry_groups: list[list[str]] = []
        for part in parts:
            if entry_groups and part[0].islower() and not entry_groups[-1][-1].endswith((".", ")")):
                entry_groups[-1].append(part)
            else:
                entry_groups.append([part])
        groups.extend(entry_groups)
    return groups


def split_stat_lines(raw_stats: list[Any]) -> list[str]:
    return [" ".join(g) for g in split_stat_line_groups(raw_stats)]


def load_korean_maps(
    ko_dir: Path | None, source_sha256: dict[str, str] | None = None,
) -> tuple[dict[str, str], dict[str, str]]:
    """Load Korean node-name and stat-description dictionaries using strict csv.reader."""
    dn_map: dict[str, str] = {}
    sd_map: dict[str, str] = {}
    if not ko_dir or not ko_dir.is_dir():
        return dn_map, sd_map

    ko_dir = ko_dir.resolve()
    repo_root = Path(__file__).resolve().parents[2]
    if ko_dir.is_relative_to(repo_root):
        ko_root = repo_root
    elif ko_dir.parts[-3:] == ("Data", "Translate", "ko-KR"):
        # The default dictionaries live in a separate Korean PoB distribution.
        ko_root = ko_dir.parents[2]
    else:
        ko_root = ko_dir

    def read_csv_into(path: Path, target: dict[str, str]) -> None:
        if not path.is_file():
            return
        data = path.read_bytes()
        if source_sha256 is not None:
            source_sha256[path.relative_to(ko_root).as_posix()] = hashlib.sha256(data).hexdigest()
        with io.StringIO(data.decode("utf-8-sig"), newline="") as f:
            for row in csv.reader(f):
                if len(row) < 2:
                    continue
                en = row[0].strip()
                ko = row[1].strip()
                if en and ko and en not in target:
                    target[en] = ko

    read_csv_into(ko_dir / "tree_dn.csv", dn_map)
    read_csv_into(ko_dir / "tree_sd.csv", sd_map)
    read_csv_into(ko_dir / "passiveTree.csv", sd_map)
    read_csv_into(ko_dir / "3.26_용병리그_번역추가.csv", sd_map)
    read_csv_into(ko_dir / "statDescriptions.csv", sd_map)

    # Also split any " / "-joined entries into individual line fallbacks when lengths match
    extras: dict[str, str] = {}
    for en, ko in sd_map.items():
        if " / " in en and " / " in ko:
            en_parts = [p.strip() for p in en.split(" / ")]
            ko_parts = [p.strip() for p in ko.split(" / ")]
            if len(en_parts) == len(ko_parts):
                for ep, kp in zip(en_parts, ko_parts):
                    if ep and kp and ep not in sd_map and ep not in extras:
                        extras[ep] = kp
    sd_map.update(extras)
    return dn_map, sd_map


def translate_stat_groups(groups: list[list[str]], sd_map: dict[str, str]) -> list[str]:
    """묶음(논리 문장)마다 한글 문장을 하나씩 돌려준다. 결과 길이는 묶음 수와 같다.

    찾는 순서(재검수 R-6: 줄을 합치기 전에 있던 번역을 잃지 않기 위함):
    1. 원문 조각 전체를 " / "로 이은 키(여러 줄 노드 통번역). 한글 조각 수가 원문 조각 수와 같으면 묶음대로 다시 잇는다.
    2. 묶음 문장 전체.
    3. 묶음의 조각마다 찾아 공백으로 잇는다. 하나라도 없으면 영어 문장을 그대로 둔다.
    """
    if not groups:
        return []
    flat = [part for g in groups for part in g]
    if len(flat) > 1:
        joined = " / ".join(flat)
        if joined in sd_map:
            ko_parts = [p.strip() for p in sd_map[joined].split(" / ")]
            if len(ko_parts) == len(flat):
                regrouped: list[str] = []
                idx = 0
                for g in groups:
                    regrouped.append(" ".join(ko_parts[idx:idx + len(g)]))
                    idx += len(g)
                return regrouped
    out: list[str] = []
    for g in groups:
        sentence = " ".join(g)
        if sentence in sd_map:
            out.append(sd_map[sentence])
        elif len(g) > 1 and all(part in sd_map for part in g):
            out.append(" ".join(sd_map[part] for part in g))
        else:
            out.append(sentence)
    return out


def translate_stats(en_lines: list[str], sd_map: dict[str, str]) -> list[str]:
    return translate_stat_groups([[line] for line in en_lines], sd_map)


def classify_icon_category(kind: int, name: str, stats: list[str], asc_name: str) -> str:
    if kind == KIND_KEYSTONE:
        return "keystone"
    if kind == KIND_MASTERY:
        return "mastery"
    if kind == KIND_SOCKET:
        return "socket"
    if kind == KIND_CLASS_START:
        return "class_start"
    if kind == KIND_ASCEND_START or asc_name:
        return "ascendancy"
    blob = (name + " " + " ".join(stats)).lower()
    if "maximum life" in blob or "life regeneration" in blob:
        return "life"
    if "energy shield" in blob:
        return "es"
    if "maximum mana" in blob or "mana regeneration" in blob or "mana cost" in blob:
        return "mana"
    if "fire resistance" in blob or "cold resistance" in blob or "lightning resistance" in blob or "elemental resistance" in blob or "chaos resistance" in blob:
        return "resist"
    if "block" in blob or "suppress" in blob:
        return "block"
    if "armour" in blob:
        return "armour"
    if "evasion" in blob:
        return "evasion"
    if "critical" in blob:
        return "crit"
    if "attack speed" in blob or "cast speed" in blob:
        return "speed"
    if "area of effect" in blob or "area damage" in blob:
        return "area"
    if "endurance charge" in blob or "frenzy charge" in blob or "power charge" in blob:
        return "charge"
    if "spell damage" in blob or "elemental damage" in blob or "cold damage" in blob or "lightning damage" in blob or "fire damage" in blob:
        return "spell"
    if "melee" in blob or "physical damage" in blob or "sword" in blob or "axe" in blob or "mace" in blob or "attack damage" in blob:
        return "melee"
    if "to strength" in blob:
        return "str"
    if "to dexterity" in blob:
        return "dex"
    if "to intelligence" in blob:
        return "int"
    return "generic"


# C-29: C++ PassiveStatParser::ParseLine의 실제 지원 패턴과 1:1 정합(키워드 어림 제거)
SUPPORTED_KEYSTONE_NAMES = {
    "Blood Magic",
    "Chaos Inoculation",
    "Resolute Technique",
    "Iron Reflexes",
    "Unwavering Stance",
    "Acrobatics",
    "Mind Over Matter",
    "Elemental Overload",
    "Pain Attunement",
    "Iron Will",
}

_CPP_EXACT_SPECIAL_LINES = {
    "removes all mana",
    "maximum life becomes 1, immune to chaos damage",
    "your hits can't be evaded",
    "converts all evasion rating to armour. dexterity provides no bonus to evasion rating",
    "cannot evade enemy attacks",
    "cannot be stunned",
    "modifiers to chance to suppress spell damage instead apply to chance to dodge spell hits at 50% of their value",
    "40% of damage is taken from mana before life",
    "skills that have dealt a critical strike in the past 8 seconds deal 40% more elemental damage with hits and ailments",
    "30% more spell damage when on low life",
    "strength's damage bonus applies to all spell damage as well",
    "grants 1 passive skill point",
    "grants 2 passive skill points",
}

_CPP_SUPPORTED_REST_EXACT = {
    "to strength", "to dexterity", "to intelligence", "to all attributes",
    "to strength and dexterity", "to strength and intelligence", "to dexterity and intelligence",
    "increased strength", "increased dexterity", "increased intelligence", "increased attributes",
    "to maximum life", "increased maximum life", "reduced maximum life", "more maximum life",
    "to maximum mana", "increased maximum mana",
    "to maximum energy shield", "increased maximum energy shield",
    "increased energy shield recharge rate", "faster start of energy shield recharge",
    "to armour", "increased armour",
    "to evasion rating", "increased evasion rating",
    "to armour and evasion rating", "to evasion rating and armour",
    "increased armour and evasion", "increased armour and evasion rating", "increased evasion rating and armour",
    "increased armour and energy shield", "increased armour and maximum energy shield",
    "increased evasion rating and energy shield", "increased evasion rating and maximum energy shield",
    "increased global defences",
    "to fire resistance", "to cold resistance", "to lightning resistance", "to chaos resistance",
    "to all elemental resistances",
    "to fire and cold resistances", "to fire and lightning resistances", "to cold and lightning resistances",
    "to maximum fire resistance", "to maximum cold resistance", "to maximum lightning resistance",
    "to maximum chaos resistance", "to maximum all elemental resistances", "to all maximum elemental resistances",
    "additional physical damage reduction",
    "chance to block attack damage", "chance to block spell damage",
    "chance to suppress spell damage", "to amount of suppressed spell damage prevented",
    "to maximum endurance charges", "to maximum frenzy charges", "to maximum power charges",
    "increased damage",
    "increased physical damage", "increased melee damage", "increased melee physical damage",
    "increased attack damage",
    "increased physical attack damage", "increased attack physical damage",
    "increased elemental damage", "increased elemental damage with attack skills",
    "increased fire damage", "increased cold damage", "increased lightning damage",
    "increased chaos damage", "increased spell damage", "increased area damage",
    "increased attack speed", "reduced attack speed", "increased melee attack speed",
    "increased cast speed", "increased attack and cast speed",
    "increased critical strike chance", "increased global critical strike chance",
    "increased critical strike chance for attacks", "increased melee critical strike chance",
    "increased critical strike chance for spells", "increased spell critical strike chance",
    "to critical strike multiplier", "to global critical strike multiplier",
    "to melee critical strike multiplier", "to critical strike multiplier for melee",
    "to critical strike multiplier for spells", "to spell critical strike multiplier",
    "to critical strike multiplier for spell damage",
    "increased area of effect",
    "reduced mana cost of skills", "increased mana cost of skills",
    "increased mana cost efficiency", "increased cost efficiency",
    "increased cost efficiency of attacks", "increased mana cost efficiency of spells",
    "chance to ignite", "chance to freeze", "chance to shock",
    "chance to freeze, shock and ignite",
    "chance to cause bleeding", "chance to bleed", "chance to cause bleeding with attacks",
    "chance to poison on hit", "chance to poison", "chance to poison on hit with attacks",
    "chance to poison on hit with spell damage",
}

# 재검수 R-1: C++ ParseLine의 "Attacks have N% ..." 분기와 같은 꼬리 문구
_CPP_SUPPORTED_ATTACKS_HAVE_REST = {"chance to cause bleeding", "chance to ignite"}
_ATTACKS_HAVE_RE = re.compile(r"^attacks have\s+[+-]?\d+(?:\.\d+)?%\s*(.+)$")  # C++ ParseLeadingNumber처럼 % 뒤 공백은 선택

_LEADING_NUM_RE = re.compile(r"^[+-]?\d+(?:\.\d+)?%?\s+(.+)$")
_PREVENT_SUPP_RE = re.compile(r"^prevent\s+[+-]?\d+(?:\.\d+)?%\s+of suppressed spell damage$")


def is_stat_line_supported_by_cpp(line: str) -> bool:
    low = line.strip().lower()
    if not low:
        return False
    if low in _CPP_EXACT_SPECIAL_LINES:
        return True
    if low.startswith("can allocate passives from the "):
        return True
    if _PREVENT_SUPP_RE.match(low):
        return True
    if low.startswith("attacks have "):
        ma = _ATTACKS_HAVE_RE.match(low)
        return bool(ma) and ma.group(1).strip() in _CPP_SUPPORTED_ATTACKS_HAVE_REST
    m = _LEADING_NUM_RE.match(low)
    if not m:
        return False
    return m.group(1).strip() in _CPP_SUPPORTED_REST_EXACT


def is_node_deferred(kind: int, name: str, stats: list[str], is_mc: bool, grant_pts: int = 0) -> bool:
    # 5단계에서 패시브 주얼 소켓 장착이 구현되었으므로 KIND_SOCKET은 지원됨
    if kind in (KIND_CLASS_START, KIND_ASCEND_START, KIND_MASTERY, KIND_SOCKET) or is_mc or grant_pts > 0:
        return False
    if kind == KIND_KEYSTONE:
        return name not in SUPPORTED_KEYSTONE_NAMES
    if not stats:
        return False
    return not any(is_stat_line_supported_by_cpp(line) for line in stats)


def node_kind(node: dict[str, Any]) -> int:
    if node.get("classStartIndex") is not None:
        return KIND_CLASS_START
    if node.get("isAscendancyStart"):
        return KIND_ASCEND_START
    if node.get("isMastery"):
        return KIND_MASTERY
    if node.get("isJewelSocket"):
        return KIND_SOCKET
    if node.get("isKeystone"):
        return KIND_KEYSTONE
    if node.get("isNotable"):
        return KIND_NOTABLE
    return KIND_NORMAL


def compile_tree(pob_dir: Path, ko_dir: Path | None, version: str = "3_30") -> dict[str, Any]:
    tree_path = pob_dir / "TreeData" / version / "tree.lua"
    if not tree_path.is_file():
        raise FileNotFoundError(f"PoB tree.lua not found: {tree_path}")

    tree_source = tree_path.read_bytes()
    source_sha256 = {tree_path.relative_to(pob_dir).as_posix(): hashlib.sha256(tree_source).hexdigest()}
    tree_text = tree_source.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")
    raw_tree = LuaTableParser(tree_text).parse()
    dn_map, sd_map = load_korean_maps(ko_dir, source_sha256)

    constants = raw_tree["constants"]
    radii = as_list(constants["orbitRadii"])
    angles = [orbit_angles(int(c)) for c in as_list(constants["skillsPerOrbit"])]
    groups = raw_tree["groups"]
    raw_nodes = raw_tree["nodes"]

    # 1. Extract 7 base classes and 19 base ascendancies (matched strictly by ascendancy.id)
    class_name_to_id: dict[str, int] = {}
    classes_out: list[dict[str, Any]] = []
    asc_display_name: dict[str, str] = {}
    asc_class_id: dict[str, int] = {}
    for cid, cls in enumerate(as_list(raw_tree["classes"])):
        cname = str(cls["name"])
        class_name_to_id[cname.lower()] = cid
        asc_list = []
        for a in as_list(cls.get("ascendancies")):
            aid = str(a.get("id") or a.get("name") or "")
            aname = str(a.get("name") or aid)
            if aid not in BASE_ASCENDANCY_SET:
                continue
            asc_display_name[aid] = aname
            asc_class_id[aid] = cid
            asc_list.append({
                "id": aid,
                "name": aname,
                "nameKo": ASCENDANCY_KO_NAMES.get(aid, aname),
                "startNodeId": 0,
            })
        classes_out.append({
            "id": cid,
            "name": cname,
            "nameKo": CLASS_KO_NAMES.get(cname, cname),
            "baseStr": int(cls.get("base_str", 20)),
            "baseDex": int(cls.get("base_dex", 20)),
            "baseInt": int(cls.get("base_int", 20)),
            "startNodeId": 0,
            "startNeighbors": [],
            "ascendancies": asc_list,
        })

    # 2. First filter pass: placed, non-proxy, non-bloodline, non-blighted, main or 19 base ascendancies
    cand_nodes: dict[int, dict[str, Any]] = {}
    for k, n in raw_nodes.items():
        if not isinstance(n, dict) or k == "root":
            continue
        if "group" not in n or n.get("isProxy") or n.get("isBloodline") or n.get("isBlighted"):
            continue
        asc = str(n.get("ascendancyName") or "")
        if asc and asc not in BASE_ASCENDANCY_SET:
            continue
        nid = int(k)
        cand_nodes[nid] = n

    # 3. Build raw symmetric adjacency (from BOTH `out` AND `in`, matching ascendancyName, excluding mastery)
    raw_adj: dict[int, set[int]] = {nid: set() for nid in cand_nodes}
    for nid, n in cand_nodes.items():
        if n.get("isMastery"):
            continue
        asc_a = str(n.get("ascendancyName") or "")
        nbrs = as_list(n.get("out")) + as_list(n.get("in"))
        for raw_m in nbrs:
            try:
                mid = int(raw_m)
            except (TypeError, ValueError):
                continue
            if mid == nid or mid not in cand_nodes:
                continue
            m = cand_nodes[mid]
            if m.get("isMastery"):
                continue
            asc_b = str(m.get("ascendancyName") or "")
            if asc_a != asc_b:
                continue
            raw_adj[nid].add(mid)
            raw_adj[mid].add(nid)

    # 4. Identify classStart nodes, classStartNeighbors, and 19 ascendancyStart nodes
    class_start_ids: set[int] = set()
    asc_start_ids: dict[str, int] = {}
    for nid, n in cand_nodes.items():
        if n.get("classStartIndex") is not None:
            cid = int(n["classStartIndex"])
            if 0 <= cid < len(classes_out):
                classes_out[cid]["startNodeId"] = nid
                class_start_ids.add(nid)
        if n.get("isAscendancyStart"):
            asc = str(n.get("ascendancyName") or "")
            if asc in BASE_ASCENDANCY_SET:
                asc_start_ids[asc] = nid

    for cls in classes_out:
        sid = cls["startNodeId"]
        if sid:
            cls["startNeighbors"] = sorted(
                mid for mid in raw_adj.get(sid, set())
                if mid not in class_start_ids and not cand_nodes[mid].get("ascendancyName")
            )
        for asc in cls["ascendancies"]:
            asc["startNodeId"] = asc_start_ids.get(asc["id"], 0)

    # 5. Reachability BFS: exclude classStart nodes as intermediate vertices!
    reachable: set[int] = set(class_start_ids)
    q: deque[int] = deque()
    for cls in classes_out:
        for nb in cls["startNeighbors"]:
            if nb not in reachable:
                reachable.add(nb)
                q.append(nb)
    for sid in asc_start_ids.values():
        if sid and sid not in reachable:
            reachable.add(sid)
            q.append(sid)

    while q:
        cur = q.popleft()
        for nxt in raw_adj.get(cur, set()):
            if nxt in class_start_ids or nxt in reachable:
                continue
            reachable.add(nxt)
            q.append(nxt)

    # Keep all reachable nodes + mastery nodes whose group has >= 1 reachable main-tree notable
    group_notables: dict[int, list[int]] = {}
    for nid in reachable:
        n = cand_nodes[nid]
        if n.get("isNotable") and not n.get("ascendancyName"):
            g = int(n["group"])
            group_notables.setdefault(g, []).append(nid)
    for g in group_notables:
        group_notables[g].sort()

    kept_ids: list[int] = []
    for nid in sorted(cand_nodes):
        n = cand_nodes[nid]
        if n.get("isMastery"):
            g = int(n["group"])
            if g in group_notables and group_notables[g]:
                kept_ids.append(nid)
        elif nid in reachable:
            kept_ids.append(nid)

    kept_set = set(kept_ids)
    id_to_idx = {nid: idx for idx, nid in enumerate(kept_ids)}

    # 6. Normalize masteryEffects + assemble nodes
    mastery_effects_table: dict[str, dict[str, Any]] = {}
    nodes_out: list[dict[str, Any]] = []
    min_x = float("inf")
    max_x = float("-inf")
    min_y = float("inf")
    max_y = float("-inf")
    ko_name_hits = 0
    stat_lines_total = 0
    stat_lines_ko = 0

    for nid in kept_ids:
        n = cand_nodes[nid]
        g_id = int(n["group"])
        g_obj = groups[g_id]
        orbit = int(n.get("orbit", 0))
        orbit_idx = int(n.get("orbitIndex", 0))
        r = float(radii[orbit]) if orbit < len(radii) else 0.0
        orb_angles = angles[orbit] if orbit < len(angles) else [0.0]
        ang = orb_angles[orbit_idx] if orbit_idx < len(orb_angles) else 0.0
        x = round(float(g_obj["x"]) + r * math.sin(ang), 2)
        y = round(float(g_obj["y"]) - r * math.cos(ang), 2)
        min_x = min(min_x, x)
        max_x = max(max_x, x)
        min_y = min(min_y, y)
        max_y = max(max_y, y)

        kind = node_kind(n)
        asc_id = str(n.get("ascendancyName") or "")
        en_name = str(n.get("name") or "")
        if kind == KIND_CLASS_START:
            cid = int(n["classStartIndex"])
            en_name = classes_out[cid]["name"]
            ko_name = classes_out[cid]["nameKo"]
        elif kind == KIND_ASCEND_START and asc_id in ASCENDANCY_KO_NAMES:
            en_name = asc_display_name.get(asc_id, en_name)
            ko_name = ASCENDANCY_KO_NAMES[asc_id]
        else:
            ko_name = dn_map.get(en_name, en_name)
            if en_name in dn_map:
                ko_name_hits += 1

        stat_groups = split_stat_line_groups(as_list(n.get("stats")))
        en_stats = [" ".join(g) for g in stat_groups]
        ko_stats = translate_stat_groups(stat_groups, sd_map)
        stat_lines_total += len(en_stats)
        stat_lines_ko += sum(1 for e, k in zip(en_stats, ko_stats) if e != k)

        is_mc = bool(n.get("isMultipleChoice", False))
        is_mco = bool(n.get("isMultipleChoiceOption", False))
        choice_parent = 0
        if is_mco:
            for nb in sorted(raw_adj.get(nid, set())):
                if nb in kept_set and cand_nodes[nb].get("isMultipleChoice"):
                    choice_parent = nb
                    break

        grants_class_start = -1
        for line in en_stats:
            m = PATH_START_RE.search(line)
            if m:
                cname_low = m.group(1).lower()
                if cname_low in class_name_to_id:
                    grants_class_start = class_name_to_id[cname_low]
                    break

        granted_pts = int(n.get("grantedPassivePoints", 0) or 0)

        effect_ids: list[int] = []
        if kind == KIND_MASTERY:
            for eff in as_list(n.get("masteryEffects")):
                if not isinstance(eff, dict) or "effect" not in eff:
                    continue
                eid = int(eff["effect"])
                effect_ids.append(eid)
                ekey = str(eid)
                if ekey not in mastery_effects_table:
                    e_groups = split_stat_line_groups(as_list(eff.get("stats")))
                    e_stats = [" ".join(g) for g in e_groups]
                    e_ko = translate_stat_groups(e_groups, sd_map)
                    mastery_effects_table[ekey] = {
                        "id": eid,
                        "stats": e_stats,
                        "statsKo": e_ko,
                    }

        # Rule adjacency excludes classStart and mastery nodes
        rule_nbrs: list[int] = []
        if kind not in (KIND_CLASS_START, KIND_MASTERY):
            rule_nbrs = sorted(
                mid for mid in raw_adj.get(nid, set())
                if mid in kept_set and mid not in class_start_ids and not cand_nodes[mid].get("isMastery")
            )

        icon_cat = classify_icon_category(kind, en_name, en_stats, asc_id)
        deferred = is_node_deferred(kind, en_name, en_stats, is_mc, granted_pts)

        node_entry: dict[str, Any] = {
            "id": nid,
            "k": kind,
            "g": g_id,
            "o": orbit,
            "x": x,
            "y": y,
            "name": en_name,
            "nameKo": ko_name,
            "cat": icon_cat,
            "adj": rule_nbrs,
        }
        if en_stats:
            node_entry["stats"] = en_stats
            node_entry["statsKo"] = ko_stats
        if asc_id:
            node_entry["asc"] = asc_id
        if is_mc:
            node_entry["mc"] = True
        if is_mco:
            node_entry["mco"] = True
            node_entry["mcParent"] = choice_parent
        if granted_pts > 0:
            node_entry["grantPts"] = granted_pts
        if grants_class_start >= 0:
            node_entry["grantStart"] = grants_class_start
        if effect_ids:
            node_entry["effects"] = effect_ids
        if deferred:
            node_entry["deferred"] = True

        nodes_out.append(node_entry)

    # 7. Build visual edges (includes classStart spokes for rendering; deduped unordered pairs)
    # Also compute arc metadata for same-group, same-orbit edges
    visual_edges: list[list[Any]] = []
    seen_pairs: set[tuple[int, int]] = set()
    for nid in kept_ids:
        n = cand_nodes[nid]
        if n.get("isMastery"):
            continue
        asc_a = str(n.get("ascendancyName") or "")
        for mid in sorted(raw_adj.get(nid, set())):
            if mid not in kept_set:
                continue
            pair = (nid, mid) if nid < mid else (mid, nid)
            if pair in seen_pairs:
                continue
            seen_pairs.add(pair)
            m = cand_nodes[mid]
            if m.get("isMastery"):
                continue
            ia = id_to_idx[pair[0]]
            ib = id_to_idx[pair[1]]
            na = cand_nodes[pair[0]]
            nb = cand_nodes[pair[1]]
            is_arc = (
                int(na["group"]) == int(nb["group"])
                and int(na.get("orbit", 0)) == int(nb.get("orbit", 0))
                and int(na.get("orbit", 0)) > 0
                and na.get("classStartIndex") is None
                and nb.get("classStartIndex") is None
            )
            if is_arc:
                g_id = int(na["group"])
                orb = int(na["orbit"])
                g_obj = groups[g_id]
                r = float(radii[orb])
                orb_ang = angles[orb]
                a1 = orb_ang[int(na["orbitIndex"])]
                a2 = orb_ang[int(nb["orbitIndex"])]
                # Canvas arc uses angle from +X axis (PoE orbit 0 is -Y = -pi/2)
                c1 = a1 - math.pi / 2.0
                c2 = a2 - math.pi / 2.0
                diff = (c2 - c1) % (2.0 * math.pi)
                if diff > math.pi:
                    c1, c2 = c2, c1
                    diff = 2.0 * math.pi - diff
                visual_edges.append([
                    ia, ib, 1 if asc_a else 0,
                    round(float(g_obj["x"]), 2),
                    round(float(g_obj["y"]), 2),
                    round(r, 2),
                    round(c1, 5),
                    round(c1 + diff, 5),
                ])
            else:
                visual_edges.append([ia, ib, 1 if asc_a else 0])

    # 8. Group notable mapping (JSON string keys for portability)
    mastery_groups_out = {str(g): nids for g, nids in sorted(group_notables.items())}

    out = {
        "schemaVersion": 1,
        "poeVersion": version,
        "bounds": {
            "minX": round(min_x, 2),
            "maxX": round(max_x, 2),
            "minY": round(min_y, 2),
            "maxY": round(max_y, 2),
        },
        "classes": classes_out,
        "masteryGroups": mastery_groups_out,
        "masteryEffects": dict(sorted(mastery_effects_table.items(), key=lambda kv: int(kv[0]))),
        "nodes": nodes_out,
        "visualEdges": visual_edges,
        "meta": {
            "schema": "skyrimpoe.tree",
            "schemaVersion": 1,
            "game": "poe1",
            "gameVersion": version.replace("_", "."),
            "pobVersion": "2.68.1",
            "contentGeneration": 0,
            "extractorVersion": "compile_tree/1",
            "sourceSha256": dict(sorted(source_sha256.items())),
            "nodeCount": len(nodes_out),
            "visualEdgeCount": len(visual_edges),
            "masteryEffectCount": len(mastery_effects_table),
            "koNameHits": ko_name_hits,
            "statLinesTotal": stat_lines_total,
            "statLinesKo": stat_lines_ko,
        },
    }
    return out


def write_tree_js_wrapper(json_path: Path, js_path: Path) -> None:
    """Wrap compiled tree_3_30.json as `window.POE_TREE = ...;` for Prisma UI."""
    raw = json_path.read_text(encoding="utf-8")
    raw = raw.replace("</", "<\\/").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
    js_path.parent.mkdir(parents=True, exist_ok=True)
    js_path.write_text("window.POE_TREE = " + raw + ";\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Compile PoB 3.30 passive tree to SkyrimPoE JSON/JS.")
    parser.add_argument("--pob-dir", type=Path, default=Path(r"G:\poe\PathOfBuilding-2.68.1"))
    parser.add_argument("--ko-dir", type=Path, default=Path(r"G:\poe\POE1 한글 POB (2025.10.31)\Data\Translate\ko-KR"))
    parser.add_argument("--version", default="3_30")
    parser.add_argument("--out", type=Path, default=Path("data/packs/tree_3_30.json"))
    parser.add_argument("--wrap-only-from", type=Path, default=None,
                        help="Skip PoB compilation and only wrap an existing tree JSON into --js-out.")
    parser.add_argument("--js-out", type=Path, default=None,
                        help="Optional path to write window.POE_TREE = ... JS file.")
    args = parser.parse_args()

    if args.wrap_only_from is not None:
        if args.js_out is None:
            raise SystemExit("--wrap-only-from requires --js-out")
        write_tree_js_wrapper(args.wrap_only_from, args.js_out)
        print(f"Wrapped {args.wrap_only_from} -> {args.js_out}")
        return

    data = compile_tree(args.pob_dir, args.ko_dir, args.version)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
    args.out.write_text(payload, encoding="utf-8")
    if args.js_out is not None:
        write_tree_js_wrapper(args.out, args.js_out)

    meta = data["meta"]
    ko_pct = (100.0 * meta["statLinesKo"] / meta["statLinesTotal"]) if meta["statLinesTotal"] else 0.0
    print(
        f"Compiled {args.out}: nodes={meta['nodeCount']}, visualEdges={meta['visualEdgeCount']}, "
        f"masteryEffects={meta['masteryEffectCount']}, koStats={meta['statLinesKo']}/{meta['statLinesTotal']} ({ko_pct:.1f}%)"
    )


if __name__ == "__main__":
    main()
