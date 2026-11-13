#!/usr/bin/env python3
"""PoE Proto 7: 측정 기록(measure-*.tsv)을 읽어 보고서를 낸다.

무엇을 재서 값을 어떻게 정하는지, 도구가 맞는지의 기준(V1~V4)은 ../README.md에 재기 전에 적어 두었다.
이 스크립트는 그 규칙을 그대로 계산할 뿐이고, 값을 정하는 것은 보고서를 본 사람이다.

    python analyze.py                      기본 폴더(문서의 SKSE/PoEProtoMeasure)의 모든 기록
    python analyze.py <폴더나 파일> [...]   지정한 기록
    python analyze.py --selftest            꾸민 기록으로 스크립트가 도는지 확인
"""
from __future__ import annotations

import argparse
import bisect
import math
import os
import re
import statistics
import sys
import tempfile
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

PLAYER = 0x14
PAIR_WINDOW_US = 50_000       # 적중 이벤트와 피해 기록을 잇는 시각 차이(V4)
SWING_WINDOW_US = 80_000      # 이 안에 여러 대상에 맞은 것은 한 번의 휘두름으로 센다
STREAK_GAP_US = 2_000_000     # 타격이 이보다 길게 끊기면 다른 구간
LOW_LEVEL = 10
MIN_LOW_ENEMIES = 30
MIN_STREAKS = 20
LEVEL_BINS = [(1, 5), (6, 10), (11, 15), (16, 20), (21, 30), (31, 40), (41, 50), (51, 75), (76, 9999)]
MELEE_WEAPON_TYPES = {0, 1, 2, 3, 4, 5, 6}
WEAPON_TYPE_NAMES = {0: "맨손", 1: "한손검", 2: "단검", 3: "한손도끼", 4: "한손둔기", 5: "양손검", 6: "양손도끼·둔기", 7: "활", 8: "지팡이", 9: "석궁"}
DEFAULT_POB_MISC = Path(r"G:\poe\PathOfBuilding-2.67.2\Data\Misc.lua")  # 이 프로젝트의 기준 PoB(설계-현황 6장). --pob로 바꾼다
# PoB를 찾지 못했을 때 쓰는 값(설계-현황 5장의 표, PoB 3.29의 monsterLifeTable에서 옮긴 것). 사이는 로그 보간한다.
LIFE_FALLBACK = {1: 22, 10: 78, 20: 207, 30: 467, 40: 972, 50: 1927, 60: 3698, 68: 6127, 75: 9436, 84: 16265, 90: 23250}


# ---------------------------------------------------------------- 읽기

@dataclass(eq=False)
class Instance:
    """한 액터. 같은 폼 ID라도 기본 폼이 바뀌거나 죽은 뒤 산 채로 다시 보이면 다른 액터로 본다."""
    session: int
    segment: int
    ref: int
    base: int
    root: int
    t_first: int
    level: int = 0
    max_health: float = 0.0
    first_health: float = 0.0
    flags: set = field(default_factory=set)
    name: str = "-"
    race: str = "-"
    plugin: str = "-"
    last_dead: bool = False
    readings: list = field(default_factory=list)   # 훑기가 읽은 (시각, 체력)
    damage_in: list = field(default_factory=list)  # 이 액터가 받은 피해 기록
    death_t: int | None = None
    killer: int = 0

    @property
    def is_party(self) -> bool:
        return self.ref == PLAYER or bool(self.flags & {"T", "C", "S"})


@dataclass(eq=False)
class Damage:
    session: int
    t: int
    target: Instance | None
    attacker_ref: int
    attacker: Instance | None
    arg: float
    before: float
    after: float
    max_health: float
    amount: float = 0.0
    hit: "Hit | None" = None


@dataclass(eq=False)
class Hit:
    session: int
    t: int
    target: Instance | None
    cause_ref: int
    cause: Instance | None
    source: int
    projectile: int
    flags: int
    damages: list = field(default_factory=list)

    @property
    def amount(self) -> float:
        return sum(d.amount for d in self.damages)


@dataclass(eq=False)
class Context:
    t: int
    level: int
    max_health: float
    difficulty: int
    mult_by: float
    mult_to: float
    right: int
    left: int
    cell_name: str
    location_name: str


class Data:
    def __init__(self):
        self.files: list[Path] = []
        self.instances: list[Instance] = []
        self.damages: list[Damage] = []
        self.hits: list[Hit] = []
        self.contexts: dict[int, list[Context]] = defaultdict(list)
        self.context_times: dict[int, list[int]] = {}
        self.forms: dict[int, tuple[str, int, int, str]] = {}
        self.row_counts: Counter = Counter()
        self.session_span: dict[int, list[int]] = {}
        self.loads = 0
        self.bad_rows = 0
        self.headers: list[str] = []
        self.hook_configs: list[str] = []

    def context_at(self, session: int, t: int) -> Context | None:
        items = self.contexts.get(session)
        if not items:
            return None
        times = self.context_times.get(session)
        if times is None or len(times) != len(items):
            times = self.context_times[session] = [c.t for c in items]
        return items[max(bisect.bisect_right(times, t) - 1, 0)]


def fnum(text: str) -> float:
    try:
        return float(text)
    except ValueError:
        return math.nan


def load(paths: list[Path]) -> Data:
    data = Data()
    for session, path in enumerate(paths):
        data.files.append(path)
        segment = 0
        current: dict[int, Instance] = {}  # 이 구간에서 폼 ID가 지금 가리키는 액터

        def resolve(ref: int) -> Instance | None:
            return current.get(ref) if ref else None

        with open(path, encoding="utf-8", errors="replace", newline="") as handle:
            for line in handle:
                line = line.rstrip("\r\n")
                if not line:
                    continue
                if line.startswith("#"):
                    if line.startswith("# PoEProtoMeasure"):
                        data.headers.append(f"{path.name}: {line[2:]}")
                    continue
                cols = line.split("\t")
                kind = cols[0]
                try:
                    t = int(cols[1])
                    span = data.session_span.setdefault(session, [t, t])
                    span[1] = max(span[1], t)
                    data.row_counts[kind] += 1
                    if kind == "S":
                        if cols[2] in ("load", "newgame"):
                            segment += 1
                            current = {}
                            data.loads += 1
                        elif cols[2] == "config":
                            data.hook_configs.append(f"{path.name}: {cols[3]}")
                    elif kind == "C":
                        data.contexts[session].append(Context(t, int(cols[2]), fnum(cols[3]), int(cols[4]), fnum(cols[5]), fnum(cols[6]),
                                                               int(cols[7], 16), int(cols[8], 16), cols[12], cols[13]))
                    elif kind == "A":
                        ref, base, root = int(cols[2], 16), int(cols[3], 16), int(cols[4], 16)
                        flags = set(cols[9]) - {"-"}
                        dead = "D" in flags
                        instance = current.get(ref)
                        if instance is None or instance.base != base or (instance.last_dead and not dead):
                            instance = Instance(session, segment, ref, base, root, t, first_health=fnum(cols[8]))
                            current[ref] = instance
                            data.instances.append(instance)
                        instance.last_dead = dead
                        if not dead:
                            instance.level = int(cols[5])
                            instance.max_health = fnum(cols[6])
                        elif instance.max_health == 0.0:
                            instance.level = int(cols[5])
                            instance.max_health = fnum(cols[6])
                        instance.flags |= flags - {"D", "X", "?"}
                        instance.name, instance.race, instance.plugin = cols[12], cols[13], cols[11]
                        if "?" not in flags:  # 훑기가 적은 줄만 독립된 체력 읽기로 쓴다
                            instance.readings.append((t, fnum(cols[8])))
                    elif kind == "P":
                        instance = resolve(int(cols[2], 16))
                        if instance:
                            instance.readings.append((t, fnum(cols[3])))
                    elif kind == "D":
                        target_ref, attacker_ref = int(cols[3], 16), int(cols[4], 16)
                        damage = Damage(session, t, resolve(target_ref), attacker_ref, resolve(attacker_ref), fnum(cols[5]), fnum(cols[6]),
                                        fnum(cols[7]), fnum(cols[8]))
                        data.damages.append(damage)
                        if damage.target:
                            damage.target.damage_in.append(damage)
                    elif kind == "H":
                        target_ref, cause_ref = int(cols[3], 16), int(cols[4], 16)
                        data.hits.append(Hit(session, t, resolve(target_ref), cause_ref, resolve(cause_ref), int(cols[5], 16), int(cols[6], 16),
                                             int(cols[7])))
                    elif kind == "K":
                        instance = resolve(int(cols[3], 16))
                        if instance:
                            if instance.death_t is None:
                                instance.death_t = t
                                instance.killer = int(cols[4], 16)
                            if cols[5] == "1":
                                instance.last_dead = True
                    elif kind == "F":
                        data.forms[int(cols[2], 16)] = (cols[3], int(cols[4]), int(cols[5]), cols[6])
                except (ValueError, IndexError):
                    data.bad_rows += 1
    return data


# ---------------------------------------------------------------- 셈

def pct(values: list[float], q: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * q
    low = int(math.floor(position))
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def fmt(value: float, digits: int = 1) -> str:
    return "-" if value is None or (isinstance(value, float) and math.isnan(value)) else f"{value:.{digits}f}"


def share(part: int, whole: int) -> str:
    return "-" if whole == 0 else f"{100.0 * part / whole:.0f}% ({part}/{whole})"


def bin_of(level: int) -> tuple[int, int]:
    for low, high in LEVEL_BINS:
        if low <= level <= high:
            return (low, high)
    return LEVEL_BINS[0]


def bin_name(level_bin: tuple[int, int]) -> str:
    return f"{level_bin[0]}+" if level_bin[1] >= 9999 else f"{level_bin[0]}~{level_bin[1]}"


def table(header: list[str], rows: list[list[str]]) -> list[str]:
    if not rows:
        return ["(해당하는 기록이 없음)", ""]
    out = ["| " + " | ".join(header) + " |", "| " + " | ".join("---" for _ in header) + " |"]
    out += ["| " + " | ".join(str(cell) for cell in row) + " |" for row in rows]
    return out + [""]


def close(a: float, b: float) -> bool:
    return abs(a - b) <= max(1.0, 0.05 * abs(b))


def load_life_table(pob_misc: Path) -> tuple[dict[int, float], str]:
    try:
        text = pob_misc.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"monsterLifeTable\s*=\s*\{([^}]*)\}", text)
        values = [float(v) for v in re.findall(r"[\d.]+", match.group(1))] if match else []
        if len(values) >= 90:
            return {i + 1: v for i, v in enumerate(values)}, f"PoB의 {pob_misc.name}"
    except OSError:
        pass
    return dict(LIFE_FALLBACK), f"문서의 표(사이는 로그 보간). PoB의 표를 읽지 못함: {pob_misc}"


def life_at(life: dict[int, float], level: float) -> float:
    level = max(1.0, min(level, float(max(life))))
    if level in life:
        return life[int(level)]
    keys = sorted(life)
    upper = bisect.bisect_left(keys, level)
    low, high = keys[upper - 1], keys[upper]
    ratio = (level - low) / (high - low)
    return math.exp(math.log(life[low]) + (math.log(life[high]) - math.log(life[low])) * ratio)


def source_kind(data: Data, hit: Hit) -> tuple[str, str]:
    """적중의 수단을 (큰 갈래, 세부)로."""
    form = data.forms.get(hit.source)
    if hit.source == 0 or form is None:
        return ("기타", "수단 없음")
    kind, weapon_type, _damage, _name = form
    if kind == "WEAP":
        if weapon_type in MELEE_WEAPON_TYPES:
            return ("근접 무기", WEAPON_TYPE_NAMES.get(weapon_type, str(weapon_type)))
        return ("원거리 무기", WEAPON_TYPE_NAMES.get(weapon_type, str(weapon_type)))
    if kind in ("SPEL", "SCRL", "SHOU", "ENCH"):
        return ("주문", kind)
    return ("기타", kind)


def analyze(data: Data, pob_misc: Path = DEFAULT_POB_MISC) -> list[str]:
    out: list[str] = []
    life, life_source = load_life_table(pob_misc)

    # 피해 기록을 쓸 수 있는 값으로 만든다(V2).
    nonzero = [d for d in data.damages if d.arg != 0 and not math.isnan(d.arg)]
    inside = before_call = either = with_previous = 0
    last_by_target: dict[int, Damage] = {}
    previous_gap: dict[int, float] = {}
    for d in data.damages:
        key = id(d.target) if d.target else None
        previous = last_by_target.get(key) if key else None
        if previous is not None and d.t - previous.t <= 5_000_000:
            previous_gap[id(d)] = previous.after - d.before
        if key:
            last_by_target[key] = d
    for d in nonzero:
        a_ok = close(d.before - d.after, abs(d.arg))
        gap = previous_gap.get(id(d))
        b_ok = gap is not None and close(gap, abs(d.arg))
        inside += a_ok
        with_previous += gap is not None
        before_call += b_ok
        either += a_ok or b_ok
    v2_rate = either / len(nonzero) if nonzero else math.nan
    v2_pass = bool(nonzero) and v2_rate >= 0.9
    use_inside = inside >= before_call
    for d in data.damages:
        if math.isnan(d.arg):
            d.amount = 0.0
        elif v2_pass:
            d.amount = abs(d.arg)
        elif use_inside:
            d.amount = max(d.before - d.after, 0.0)
        else:
            d.amount = max(previous_gap.get(id(d), 0.0), 0.0)

    # 적중 이벤트와 피해 기록을 잇는다(V4): 피해 기록마다 같은 쌍의 가장 가까운 적중 이벤트에 붙인다.
    hits_by_pair: dict[tuple, list[Hit]] = defaultdict(list)
    for h in data.hits:
        if h.target:
            hits_by_pair[(h.session, id(h.target), h.cause_ref)].append(h)
    hit_times = {pair: [h.t for h in hits] for pair, hits in hits_by_pair.items()}
    nearest_dt: list[float] = []
    for d in data.damages:
        if not d.target or d.amount <= 0:
            continue
        pair = (d.session, id(d.target), d.attacker_ref)
        candidates = hits_by_pair.get(pair)
        if not candidates:
            continue
        times = hit_times[pair]
        index = bisect.bisect_left(times, d.t)
        best = min((candidates[i] for i in (index - 1, index) if 0 <= i < len(candidates)), key=lambda h: abs(h.t - d.t))
        if d.attacker_ref == PLAYER and abs(best.t - d.t) <= 1_000_000:
            nearest_dt.append((d.t - best.t) / 1000.0)
        if abs(best.t - d.t) <= PAIR_WINDOW_US:
            d.hit = best
            best.damages.append(d)

    def is_enemy(instance: Instance) -> bool:
        if instance.is_party:
            return False
        if "H" in instance.flags:
            return True
        return any(d.attacker_ref == PLAYER and d.amount > 0 for d in instance.damage_in) or instance in attacked_player

    attacked_player = {d.attacker for d in data.damages if d.target and d.target.ref == PLAYER and d.attacker and d.amount > 0}
    enemies = [i for i in data.instances if is_enemy(i) and i.max_health > 0]
    fought = {id(i) for i in enemies if any(d.amount > 0 for d in i.damage_in) or i in attacked_player}

    # ------------------------------------------------------------ 머리말
    out += ["# 프로토타입 7 측정 보고서", ""]
    total_seconds = sum((span[1] - span[0]) / 1e6 for span in data.session_span.values())
    out += [f"- 기록 파일 {len(data.files)}개, 기록된 시간 합계 {total_seconds / 60:.1f}분(게임을 켜 둔 시간. 메뉴와 대기 포함), 세이브 불러오기 {data.loads}번",
            "- 줄 수: " + ", ".join(f"{kind} {count}" for kind, count in sorted(data.row_counts.items())) + (f", 읽지 못한 줄 {data.bad_rows}" if data.bad_rows else ""),
            f"- 기대 곡선(PoE 몬스터 생명력)의 출처: {life_source}"]
    out += [f"- {line}" for line in data.headers]
    out += [f"- 설정 {line}" for line in data.hook_configs]
    levels = sorted({c.level for items in data.contexts.values() for c in items})
    def rounded(value: float) -> float | None:  # nan은 서로 같지 않아 셈의 열쇠로 쓸 수 없다
        return None if math.isnan(value) else round(value, 3)

    multipliers = Counter((c.difficulty, rounded(c.mult_by), rounded(c.mult_to)) for items in data.contexts.values() for c in items)
    out += [f"- 플레이어 레벨: {', '.join(map(str, levels)) if levels else '기록 없음'}",
            "- 난이도와 배율(난이도, 주는 피해, 받는 피해): " + (", ".join(f"({d}, {b}, {t})" for (d, b, t), _ in multipliers.most_common()) or "기록 없음"), ""]

    def unscaled(hits: list[Hit], received: bool, per_max_health: bool = False) -> list[float]:
        """타격의 피해를 그 타격이 있던 때의 난이도 배율로 나눈 값들. 배율을 모르거나 0인 타격은 뺀다."""
        values = []
        for h in hits:
            context = data.context_at(h.session, h.t)
            mult = (context.mult_to if received else context.mult_by) if context else math.nan
            if h.amount <= 0 or math.isnan(mult) or mult <= 0:
                continue
            value = h.amount / mult
            if per_max_health:
                max_health = h.damages[0].max_health
                if max_health <= 0:
                    continue
                value = 100.0 * value / max_health
            values.append(value)
        return values

    # ------------------------------------------------------------ 도구가 맞는지
    out += ["## 1. 도구가 맞는지 (README의 V1~V4)", ""]
    signs = Counter("양수" if d.arg > 0 else "음수" if d.arg < 0 else "0" for d in data.damages if not math.isnan(d.arg))
    out += [f"피해 기록 {len(data.damages)}줄. 받은 값의 부호: " + (", ".join(f"{k} {v}" for k, v in signs.items()) or "기록 없음"), ""]

    # V1
    covered: list[float] = []
    for i in data.instances:
        if i.death_t is None or i.ref == PLAYER or i.max_health <= 0 or not i.damage_in:
            continue
        seen_full = max(i.first_health, i.damage_in[0].before) >= 0.95 * i.max_health
        if seen_full:
            covered.append(sum(d.amount for d in i.damage_in if d.t <= i.death_t + 1_000_000) / i.max_health)
    v1_ok = sum(1 for c in covered if 0.9 <= c <= 1.6)
    v1_pass = bool(covered) and v1_ok / len(covered) >= 0.9
    # V3
    ratios: list[float] = []
    for i in data.instances:
        if not i.damage_in or len(i.readings) < 2:
            continue
        times = [d.t for d in i.damage_in]
        prefix = [0.0]
        for d in i.damage_in:
            prefix.append(prefix[-1] + d.amount)
        for (t1, h1), (t2, h2) in zip(i.readings, i.readings[1:]):
            if h1 - h2 > 5 and t2 - t1 <= 3_000_000:
                ratios.append((prefix[bisect.bisect_right(times, t2)] - prefix[bisect.bisect_right(times, t1)]) / (h1 - h2))
    v3_ok = sum(1 for r in ratios if 0.7 <= r <= 1.5)
    v3_pass = bool(ratios) and v3_ok / len(ratios) >= 0.8
    # V4
    player_hits = [h for h in data.hits if h.cause_ref == PLAYER and h.target and h.target.ref != PLAYER]
    paired = sum(1 for h in player_hits if h.damages)
    # 이어지지 않은 적중 가운데, 같은 대상에 공격자만 다르게 적힌 피해 기록이 가까이 있는 것(공격자가 비어 오는 길이 있는지 본다).
    other_attacker = 0
    damage_times: dict[int, list[int]] = {}
    for h in player_hits:
        if h.damages:
            continue
        times = damage_times.get(id(h.target))
        if times is None:
            times = damage_times[id(h.target)] = [d.t for d in h.target.damage_in]
        index = bisect.bisect_left(times, h.t - PAIR_WINDOW_US)
        other_attacker += any(d.amount > 0 and d.attacker_ref != PLAYER for d in h.target.damage_in[index:bisect.bisect_right(times, h.t + PAIR_WINDOW_US)])

    def verdict(passed: bool, sample: int) -> str:
        return "표본 없음" if sample == 0 else "통과" if passed else "**실패**"

    out += table(["기준", "결과", "값"], [
        ["V1 끼어든 함수가 체력 피해를 다 보는가", verdict(v1_pass, len(covered)),
         f"온전한 체력에서부터 본 뒤 죽은 액터 {len(covered)} 가운데 피해 합이 최대 체력의 90~160%인 것 {share(v1_ok, len(covered))}, 중앙값 {fmt(pct(covered, 0.5) * 100, 0)}%"],
        ["V2 받은 값이 체력 변화와 같은가", verdict(v2_pass, len(nonzero)),
         f"0이 아닌 기록 {len(nonzero)} 가운데 맞는 것 {share(either, len(nonzero))}. 호출 안에서 깎임 {share(inside, len(nonzero))}, 호출 전에 이미 깎임 {share(before_call, with_previous)}"],
        ["V3 주기적으로 읽은 체력과 맞는가", verdict(v3_pass, len(ratios)),
         f"체력이 5 넘게 줄어든 구간 {len(ratios)} 가운데 피해 합이 70~150%인 것 {share(v3_ok, len(ratios))}, 중앙값 {fmt(pct(ratios, 0.5) * 100, 0)}%"],
        ["V4 적중 이벤트와 피해 기록이 이어지는가", "기록만",
         f"플레이어의 적중 이벤트 {len(player_hits)} 가운데 ±{PAIR_WINDOW_US // 1000}ms 안에 피해 기록이 있는 것 {share(paired, len(player_hits))}. "
         f"없는 것 가운데 같은 대상에 공격자가 다르게 적힌 피해 기록이 있는 것 {other_attacker}. "
         f"피해 기록에서 가장 가까운 적중 이벤트까지(ms, 양수는 피해가 뒤): 5% {fmt(pct(nearest_dt, 0.05))}, 중앙값 {fmt(pct(nearest_dt, 0.5))}, 95% {fmt(pct(nearest_dt, 0.95))}"],
    ])
    damage_usable = v1_pass or (not covered and v3_pass)
    if not data.damages:
        out += ["피해 기록이 없습니다(HookDamage = false이거나 싸우지 않음). 아래의 피해 표는 비어 있습니다.", ""]
    elif not v2_pass:
        out += [f"V2가 통과하지 못해, 받은 값 대신 {'호출 앞뒤의 체력 차이' if use_inside else '앞 기록과의 체력 차이'}를 피해로 썼습니다.", ""]
    if data.damages and not damage_usable:
        out += ["**V1이 통과하지 못했습니다(V1의 표본이 없으면 V3). 아래 3~5장의 피해 값은 믿지 않습니다(체력 표인 2장은 영향 없음).**", ""]
    elif data.damages and ratios and not v3_pass:
        out += ["V1은 통과했으나 V3이 통과하지 못했습니다. 체력 재생이나 읽는 시점의 차이일 수 있으니 피해 값을 쓸 때 참고하세요.", ""]

    # ------------------------------------------------------------ 적의 체력
    out += ["## 2. 적의 최대 체력 (K와 B의 자료)", "",
            "적: 동료·소환수·조종받는 액터가 아니고, 플레이어에게 적대로 적혔거나 플레이어의 피해를 받았거나 플레이어에게 피해를 준 액터. "
            "같은 액터를 세이브를 불러와 다시 본 것은 한 번만 셉니다(폼, 틀이 된 기본 폼, 레벨, 최대 체력이 같으면 같은 액터). "
            "불러오기 없이 같은 값으로 다시 나타난 것은 폼 ID가 다시 쓰인 다른 액터로 보고 따로 셉니다.", ""]
    # 같은 열쇠(폼, 틀, 레벨, 최대 체력)가 여러 구간(불러오기 사이, 기록 파일 사이)에 나오면 같은 액터를 다시 본 것으로 보고
    # 가장 많이 나온 구간의 것만 센다. 한 구간 안에서 같은 열쇠가 둘이면 폼 ID가 다시 쓰인 다른 액터이므로 둘 다 센다.
    by_key: dict[tuple, dict[tuple, list[Instance]]] = defaultdict(lambda: defaultdict(list))
    for i in enemies:
        by_key[(i.ref, i.root or i.base, i.level, round(i.max_health))][(i.session, i.segment)].append(i)
    counted: list[Instance] = []
    for per_segment in by_key.values():
        counted += max(per_segment.values(), key=lambda group: (len(group), sum(id(i) in fought for i in group)))
    by_bin: dict[tuple, list[float]] = defaultdict(list)
    fought_by_bin: Counter = Counter()
    for i in counted:
        by_bin[bin_of(i.level)].append(i.max_health)
        fought_by_bin[bin_of(i.level)] += id(i) in fought
    rows = []
    for level_bin in LEVEL_BINS:
        values = by_bin.get(level_bin)
        if values:
            rows.append([bin_name(level_bin), len(values), fought_by_bin[level_bin], fmt(pct(values, 0.1), 0), fmt(pct(values, 0.25), 0),
                         fmt(pct(values, 0.5), 0), fmt(pct(values, 0.75), 0), fmt(pct(values, 0.9), 0)])
    out += table(["적 레벨", "개체", "그중 싸운 것", "10%", "25%", "중앙값", "75%", "90%"], rows)

    low = [i.max_health for i in counted if i.level <= LOW_LEVEL]
    out += [f"**K (초반 적의 최대 체력 중앙값, 적 레벨 {LOW_LEVEL} 이하)**: "]
    if len(low) >= MIN_LOW_ENEMIES:
        k_value = pct(low, 0.5)
        out[-1] += f"{fmt(k_value, 0)} (표본 {len(low)}). B = K ÷ N: N=2면 {fmt(k_value / 2)}, **N=3이면 {fmt(k_value / 3)}**, N=4면 {fmt(k_value / 4)}."
    else:
        out[-1] += f"내지 않음. 표본 {len(low)}개로 {MIN_LOW_ENEMIES}개에 못 미칩니다(더 재야 함)."
        if rows:
            lowest = min(by_bin, key=lambda b: b[0])
            out += [f"참고로만: 가장 낮은 레벨대({bin_name(lowest)})의 중앙값은 {fmt(pct(by_bin[lowest], 0.5), 0)} (표본 {len(by_bin[lowest])})."]
    out += [""]

    reference = next((b for b in LEVEL_BINS if len(by_bin.get(b, [])) >= 10), None)
    if reference:
        ref_median = pct(by_bin[reference], 0.5)
        ref_level = statistics.median(i.level for i in counted if bin_of(i.level) == reference)
        rows = []
        for level_bin in LEVEL_BINS:
            values = by_bin.get(level_bin, [])
            if len(values) < 10:
                continue
            level = statistics.median(i.level for i in counted if bin_of(i.level) == level_bin)
            growth = life_at(life, level) / life_at(life, ref_level)
            rows.append([bin_name(level_bin), fmt(level, 0), f"x{pct(values, 0.5) / ref_median:.2f}", f"x{growth ** 0.5:.2f}", f"x{growth ** 0.25:.2f}", f"x{growth:.1f}"])
        out += ["r을 볼 자료: 레벨대별 체력 중앙값이 기준 레벨대의 몇 배인지와, 같은 레벨 차이에서 우리 피해가 자라는 배수. "
                "유배자 레벨과 적 레벨이 같이 간다는 가정이 들어간 비교입니다(표본 10개 이상인 레벨대만).", ""]
        out += table(["적 레벨", "레벨 중앙값", "이 모드 목록의 체력", "r=50%일 때 우리 피해", "r=75%", "r=0%(PoE 그대로)"], rows)

    names = Counter(f"{i.name} ({i.plugin})" for i in counted)
    out += ["가장 많이 본 적: " + (", ".join(f"{name} {count}" for name, count in names.most_common(12)) or "없음"), ""]

    # ------------------------------------------------------------ 주는 피해
    out += ["## 3. 플레이어가 주는 피해 (지금의 속도)", ""]
    enemy_ids = {id(i) for i in enemies}
    groups: dict[tuple, list[Hit]] = defaultdict(list)
    for h in player_hits:
        if id(h.target) in enemy_ids:
            major, minor = source_kind(data, h)
            groups[(major, minor, "강공격" if h.flags & 1 else "일반")].append(h)
    rows = []
    for (major, minor, power), hits in sorted(groups.items(), key=lambda item: -len(item[1])):
        amounts = [h.amount for h in hits if h.amount > 0]
        rows.append([major, minor, power, len(hits), share(len(hits) - len(amounts), len(hits)), fmt(pct(amounts, 0.25)), fmt(pct(amounts, 0.5)),
                     fmt(pct(amounts, 0.75)), fmt(pct(unscaled(hits, False), 0.5))])
    out += ["타격 한 번의 피해 = 적중 이벤트 하나에 이어진 피해 기록의 합(체력에서 실제로 깎인 값, 난이도 배율이 곱해진 뒤).", ""]
    out += table(["갈래", "수단", "종류", "적중", "피해 0인 적중", "25%", "중앙값", "75%", "배율로 나눈 값의 중앙값"], rows)
    out += ["마지막 칸은 타격마다 그때의 주는 배율로 나눈 값의 중앙값입니다(기대 곡선이 배율을 곱하기 전 기준이라는 잠정 항목에 견줄 값).", ""]
    player_damage = [d for d in data.damages if d.attacker_ref == PLAYER and d.target and id(d.target) in enemy_ids and d.amount > 0]
    loose = sum(d.amount for d in player_damage if d.hit is None)
    total = sum(d.amount for d in player_damage)
    out += [f"플레이어가 적에게 준 피해 합 {fmt(total, 0)} 가운데 적중 이벤트에 이어지지 않은 것(지속 피해 등) {fmt(100 * loose / total if total else math.nan, 0)}%.", ""]

    kills_hits: list[float] = []
    kills_seconds: list[float] = []
    shares: list[float] = []
    party_refs = {i.ref for i in data.instances if i.is_party}
    hits_on: dict[int, list[Hit]] = defaultdict(list)
    for h in player_hits:
        hits_on[id(h.target)].append(h)
    for i in enemies:
        if i.death_t is None or not i.damage_in:
            continue
        all_damage = sum(d.amount for d in i.damage_in)
        mine = sum(d.amount for d in i.damage_in if d.attacker_ref == PLAYER)
        if all_damage <= 0:
            continue
        shares.append(mine / all_damage)
        if mine / all_damage >= 0.8:
            my_hits = hits_on.get(id(i))
            if my_hits:
                kills_hits.append(len(my_hits))
                kills_seconds.append((i.death_t - my_hits[0].t) / 1e6)
    out += table(["항목", "표본", "25%", "중앙값", "75%"], [
        ["죽은 적에게 준 피해에서 플레이어의 몫(%)", len(shares), fmt(pct(shares, 0.25) * 100, 0), fmt(pct(shares, 0.5) * 100, 0), fmt(pct(shares, 0.75) * 100, 0)],
        ["플레이어 몫이 80% 이상인 처치의 타격 수", len(kills_hits), fmt(pct(kills_hits, 0.25)), fmt(pct(kills_hits, 0.5)), fmt(pct(kills_hits, 0.75))],
        ["같은 처치에서 첫 타격부터 죽을 때까지(초)", len(kills_seconds), fmt(pct(kills_seconds, 0.25)), fmt(pct(kills_seconds, 0.5)), fmt(pct(kills_seconds, 0.75))],
    ])
    killers = Counter("플레이어" if i.killer == PLAYER else "동료·소환수" if i.killer in party_refs else "그 밖" for i in enemies if i.death_t is not None)
    out += ["죽은 적을 죽인 쪽: " + (", ".join(f"{who} {count}" for who, count in killers.most_common()) or "죽은 적 없음"), ""]

    # ------------------------------------------------------------ 타격률
    out += ["## 4. 플레이어의 타격률 (기준 타격률의 자료)", ""]
    rates: list[float] = []
    for session in data.session_span:
        melee = sorted(h.t for h in player_hits if h.session == session and id(h.target) in enemy_ids and source_kind(data, h)[0] == "근접 무기")
        swings: list[int] = []
        for t in melee:
            if not swings or t - swings[-1] > SWING_WINDOW_US:
                swings.append(t)
        streak: list[int] = []
        for t in swings + [None]:
            if t is not None and (not streak or t - streak[-1] <= STREAK_GAP_US):
                streak.append(t)
                continue
            if len(streak) >= 3:
                rates.append((len(streak) - 1) / ((streak[-1] - streak[0]) / 1e6))
            streak = [t] if t is not None else []
    if len(rates) >= MIN_STREAKS:
        out += [f"**기준 타격률: 초당 {round(pct(rates, 0.5), 1):.1f}회** (구간 {len(rates)}개. 25% {fmt(pct(rates, 0.25), 2)}, 중앙값 {fmt(pct(rates, 0.5), 2)}, 75% {fmt(pct(rates, 0.75), 2)})"]
    else:
        out += [f"내지 않음. 타격이 3번 이상 이어진 구간이 {len(rates)}개로 {MIN_STREAKS}개에 못 미칩니다."
                + (f" 참고로만: 중앙값 초당 {fmt(pct(rates, 0.5), 2)}회." if rates else "")]
    out += ["근접 무기로 적을 맞힌 적중만 셉니다. 같은 순간(80ms 안)에 여러 대상에 맞은 것은 한 번, 2초 넘게 끊기면 다른 구간입니다.", ""]

    # ------------------------------------------------------------ 받는 피해
    out += ["## 5. 플레이어가 받는 피해 (받는 쪽 식의 자료, 값은 정하지 않음)", ""]
    received_hits = [h for h in data.hits if h.target and h.target.ref == PLAYER and h.cause and h.damages]
    by_level: dict[tuple, list[float]] = defaultdict(list)
    hits_by_level: dict[tuple, list[Hit]] = defaultdict(list)
    for h in received_hits:
        max_health = h.damages[0].max_health
        if max_health > 0:
            by_level[bin_of(h.cause.level)].append(100.0 * h.amount / max_health)
            hits_by_level[bin_of(h.cause.level)].append(h)
    rows = [[bin_name(b), len(v), fmt(pct(v, 0.25)), fmt(pct(v, 0.5)), fmt(pct(v, 0.9)), fmt(max(v)), fmt(pct(unscaled(hits_by_level[b], True, True), 0.5))]
            for b, v in sorted(by_level.items())]
    out += ["타격 한 번에 깎인 체력을 그때의 최대 체력에 대한 %로(방어구·막기·난이도 배율이 모두 적용된 뒤).", ""]
    out += table(["공격자 레벨", "타격", "25%", "중앙값", "90%", "가장 큰 것", "배율로 나눈 값의 중앙값"], rows)
    taken = [d for d in data.damages if d.target and d.target.ref == PLAYER and d.amount > 0]
    taken_total = sum(d.amount for d in taken)
    out += [f"플레이어가 받은 피해 기록 {len(taken)}줄, 합 {fmt(taken_total, 0)}. 그 가운데 적중 이벤트에 이어지지 않은 것(지속 피해, 낙하 등) "
            f"{fmt(100 * sum(d.amount for d in taken if d.hit is None) / taken_total if taken_total else math.nan, 0)}%, 공격자가 없는 것 "
            f"{fmt(100 * sum(d.amount for d in taken if d.attacker_ref == 0) / taken_total if taken_total else math.nan, 0)}%.", ""]

    # ------------------------------------------------------------ 표본
    out += ["## 6. 표본이 충분한가 (README의 목표)", ""]
    out += table(["항목", "지금", "목표"], [
        ["본 적(겹친 것 뺌)", len(counted), "150 이상"],
        [f"그중 레벨 {LOW_LEVEL} 이하", len(low), f"{MIN_LOW_ENEMIES} 이상"],
        ["플레이어의 적중 이벤트", len(player_hits), "300 이상"],
        ["죽은 적", sum(1 for i in enemies if i.death_t is not None), "40 이상"],
        ["플레이어가 받은 피해 기록", len(taken), "100 이상"],
        ["타격이 이어진 구간", len(rates), f"{MIN_STREAKS} 이상"],
    ])
    return out


# ---------------------------------------------------------------- 스스로 확인

def hard_rows() -> list[str]:
    """까다로운 경우를 꾸민다: 받은 값이 음수이고 호출 전에 이미 깎인 모양, 피해 기록에서 먼저 본 액터('?' 줄),
    죽은 뒤 같은 폼 ID로 다시 나타난 액터, 세이브 불러오기, 도중에 바뀐 난이도 배율."""
    rows = ["# PoEProtoMeasure selftest-hard", "S\t0\tstart\tselftest",
            "F\t100\t00000F01\tWEAP\t1\t10\tsword",
            "A\t100\t00000014\t00000007\t00000007\t20\t300.0\t300.0\t300.0\t-\t00000001\tSkyrim.esm\t(player)\tNord"]
    t = 1_000_000

    def actor(ref: int, flags: str, health: float, when: int) -> str:
        return f"A\t{when}\t{ref:08X}\t{ref + 0x100000:08X}\t00002000\t20\t120.0\t120.0\t{health:.1f}\t{flags}\t00000002\tTest.esp\tBandit\tNord"

    for segment, mult in enumerate((0.5, 1.0)):
        if segment:
            rows.append(f"S\t{t}\tload\t-")
        rows.append(f"C\t{t}\t20\t300\t{segment}\t{mult:.3f}\t3.000\t00000F01\t00000000\t0\t00000100\t00000200\tcell\tloc")
        for n in range(4):  # 적 넷, 레벨 20, 체력 120, 10씩 열두 번
            ref = 0xFF000200 + n
            remaining = 120.0
            if segment:  # 둘째 구간: 훑기가 먼저 본다
                rows.append(actor(ref, "H", remaining, t))
            for hit in range(12):
                t += 500_000
                remaining -= 10.0
                if segment == 0 and hit == 0:  # 첫 구간: 이벤트 수신부가 먼저 본다(적대 여부 모름, 체력은 이미 깎인 뒤)
                    rows.append(actor(ref, "?", remaining, t))
                rows.append(f"H\t{t}\t1\t{ref:08X}\t00000014\t00000F01\t00000000\t0")
                rows.append(f"D\t{t + 200}\t1\t{ref:08X}\t00000014\t-10.000\t{remaining:.2f}\t{remaining:.2f}\t120.0")
                if segment == 0 and hit == 0:
                    rows.append(actor(ref, "H", remaining, t + 1000))
                elif remaining > 0:
                    rows.append(f"P\t{t + 1000}\t{ref:08X}\t{remaining:.2f}\t120.0")
            rows.append(f"K\t{t + 300}\t1\t{ref:08X}\t00000014\t0")
            rows.append(f"K\t{t + 400}\t1\t{ref:08X}\t00000014\t1")
            if segment == 0 and n == 1:  # 훑기가 죽은 모습을 본 뒤 같은 폼 ID가 산 채로 다시 나타남
                rows.append(actor(ref, "HD", 0.0, t + 1000))
                rows.append(actor(ref, "H", 120.0, t + 3_000_000))
            if segment == 0 and n == 2:  # 죽음이 끝났다는 이벤트만 있고, 같은 폼 ID가 이벤트에서 산 채로 다시 나타남
                rows.append(actor(ref, "?", 120.0, t + 3_000_000))
                rows.append(actor(ref, "H", 120.0, t + 3_500_000))
            t += 5_000_000
    return rows


def selftest() -> int:
    """꾸민 기록 둘을 만들어 돌려 보고, 미리 아는 답과 견준다. 스크립트의 계산을 보는 것이고 게임의 동작과는 무관하다."""
    rows = ["# PoEProtoMeasure selftest", "S\t0\tstart\tselftest",
            "C\t100\t20\t300\t4\t0.600\t3.000\t00000F01\t00000000\t0\t00000100\t00000200\tcell\tloc",
            "F\t100\t00000F01\tWEAP\t1\t10\tsword",
            "A\t100\t00000014\t00000007\t00000007\t20\t300.0\t300.0\t300.0\t-\t00000001\tSkyrim.esm\t(player)\tNord"]
    t = 1_000_000
    expected_health = []
    for n in range(40):  # 적 40: 레벨 1~10, 체력 50 + 레벨 × 10
        ref = 0xFF000100 + n
        level = 1 + n % 10
        health = 50.0 + level * 10
        expected_health.append(health)
        rows.append(f"A\t{t}\t{ref:08X}\t{0xFF100000 + n:08X}\t00001000\t{level}\t{health:.1f}\t{health:.1f}\t{health:.1f}\tH\t00000002\tTest.esp\tBandit\tNord")
        remaining = health
        hits = 0
        while remaining > 0:  # 0.5초마다 30씩 깎는다. 호출 안에서 깎이는 모양(체력 차이 = 받은 값)
            t += 500_000
            dealt = min(30.0, remaining)
            rows.append(f"H\t{t}\t1\t{ref:08X}\t00000014\t00000F01\t00000000\t0")
            rows.append(f"D\t{t + 200}\t1\t{ref:08X}\t00000014\t{dealt:.3f}\t{remaining:.2f}\t{remaining - dealt:.2f}\t{health:.1f}")
            remaining -= dealt
            hits += 1
            if remaining > 0:
                rows.append(f"P\t{t + 1000}\t{ref:08X}\t{remaining:.2f}\t{health:.1f}")
        rows.append(f"K\t{t + 300}\t1\t{ref:08X}\t00000014\t0")
        if n % 4 == 0:  # 적이 플레이어를 한 번 친다
            rows.append(f"H\t{t + 400}\t1\t00000014\t{ref:08X}\t00000F01\t00000000\t0")
            rows.append(f"D\t{t + 450}\t1\t00000014\t{ref:08X}\t15.000\t300.00\t285.00\t300.0")
        t += 5_000_000
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "measure-selftest.tsv"
        path.write_text("\n".join(rows) + "\n", encoding="utf-8")
        report = "\n".join(analyze(load([path])))
        hard_path = Path(folder) / "measure-selftest-hard.tsv"
        hard_path.write_text("\n".join(hard_rows()) + "\n", encoding="utf-8")
        hard = "\n".join(analyze(load([hard_path])))
    checks = {
        # 까다로운 기록: 적 넷을 두 구간에서 잡고, 첫 구간에서 둘이 같은 폼 ID로 다시 나타난다 -> 센 적 6, 그중 싸운 것 4, 죽은 적 8.
        "까다로운 기록의 V2(호출 전에 깎임)": "V2 받은 값이 체력 변화와 같은가 | 통과" in hard and "호출 전에 이미 깎임 100% (88/88)" in hard,
        "음수인 값": "받은 값의 부호: 음수 96" in hard,
        "온전한 체력에서 본 것만 V1에": "죽은 액터 4 가운데" in hard,
        "재사용과 불러오기": "| 16~20 | 6 | 4 |" in hard and "| 죽은 적 | 8 |" in hard,
        "타격마다의 배율": "| 10.0 | 10.0 | 10.0 | 15.0 |" in hard,
        "V1 통과": "V1 끼어든 함수가 체력 피해를 다 보는가 | 통과" in report,
        "V2 통과": "V2 받은 값이 체력 변화와 같은가 | 통과" in report,
        "V3 통과": "V3 주기적으로 읽은 체력과 맞는가 | 통과" in report,
        "K": f"{statistics.median(expected_health):.0f} (표본 40)" in report,
        "타격률 2.0": "기준 타격률: 초당 2.0회" in report,
        "받는 피해 5%": "| 5.0 | 5.0 | 5.0 | 5.0 |" in report,
    }
    print(report)
    print("스스로 확인:", ", ".join(f"{name} {'맞음' if ok else '틀림'}" for name, ok in checks.items()))
    return 0 if all(checks.values()) else 1


def default_folder() -> Path:
    return Path(os.path.expanduser("~")) / "Documents" / "My Games" / "Skyrim Special Edition" / "SKSE" / "PoEProtoMeasure"


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="PoE Proto 7 측정 기록을 읽어 보고서를 낸다.")
    parser.add_argument("paths", nargs="*", type=Path, help="기록 파일이나 폴더. 없으면 문서의 SKSE/PoEProtoMeasure")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "out" / "report.md", help="보고서를 쓸 파일")
    parser.add_argument("--pob", type=Path, default=DEFAULT_POB_MISC, help="PoB의 Data/Misc.lua(몬스터 생명력 표). 읽지 못하면 문서의 표로 대신함")
    parser.add_argument("--selftest", action="store_true", help="꾸민 기록으로 스크립트를 확인")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    files: list[Path] = []
    for path in args.paths or [default_folder()]:
        if path.is_dir():
            files += sorted(path.glob("measure-*.tsv"))
        elif path.is_file():
            files.append(path)
        else:
            print(f"찾지 못함: {path}", file=sys.stderr)
    if not files:
        print("읽을 기록 파일(measure-*.tsv)이 없습니다.", file=sys.stderr)
        return 1
    report = "\n".join(analyze(load(files), args.pob)) + "\n"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(report, encoding="utf-8")
    print(report)
    print(f"보고서: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
