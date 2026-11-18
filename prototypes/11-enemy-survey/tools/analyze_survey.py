"""적 체력 조사 TSV를 읽어 K와 레벨별 성장 배수를 낸다."""

import argparse
import bisect
import io
import math
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import TextIO

DEFAULT_POB_MISC = Path(r"G:\poe\PathOfBuilding-2.68.1\Data\Misc.lua")  # 이 프로젝트의 기준 PoB(설계-현황 6장). --pob로 바꾼다
# PoB를 찾지 못했을 때 쓰는 값(설계-현황 5장의 표, PoB 3.29의 monsterLifeTable에서 옮긴 것). 사이는 로그 보간한다.
LIFE_FALLBACK = {1: 22, 10: 78, 20: 207, 30: 467, 40: 972, 50: 1927, 60: 3698, 68: 6127, 75: 9436, 84: 16265, 90: 23250}
MIN_ENEMIES = 30
ZONE_MINIMUM = 5
PERCENTILES = (10, 25, 50, 75, 90)
# 방어구 계산의 게임 설정값(이 모드 목록에서는 ParagonPerks.esp가 정함, houseCARL 2026-11-19). 기록의 비율과 식을 대조하는 데만 쓴다.
# 식: 1 − min(fMaxArmorRating, 방어도 × fArmorScalingFactor) ÷ 100. --armor-scale, --armor-max로 바꾼다.
ARMOR_SCALING = 0.15
ARMOR_MAX = 75.0
FORMULA_TOLERANCE = 0.005
# HitData::Flag(CommonLibSSE-NG의 HitData.h)와 플러그인이 적는 무기 종류(TESObjectWEAP::WEAPON_TYPE, -1 맨손·생물, -2 알 수 없음)
FLAG_CRITICAL = 0x8
FLAG_SNEAK = 0x800
UNKNOWN_WEAPON = -2
WEAPON_GROUPS = {"근접": {-1, 0, 1, 2, 3, 4, 5, 6}, "활·석궁": {7, 9}, "지팡이": {8}}


def read_pob_table(pob_misc: Path, name: str) -> dict[int, float] | None:
    """PoB Data/Misc.lua의 레벨별 표(1부터)를 읽는다. 읽지 못하거나 90칸보다 짧으면 None."""
    try:
        text = pob_misc.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(rf"{name}\s*=\s*\{{([^}}]*)\}}", text)
    values = [float(v) for v in re.findall(r"[\d.]+", match.group(1))] if match else []
    return {i + 1: v for i, v in enumerate(values)} if len(values) >= 90 else None


def load_life_table(pob_misc: Path) -> tuple[dict[int, float], str]:
    if (table := read_pob_table(pob_misc, "monsterLifeTable")) is not None:
        return table, f"PoB의 {pob_misc.name}"
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


@dataclass(frozen=True)
class Actor:
    ref: int
    level: int
    max_health: float
    flags: str
    damage_resist: float | None = None  # 방어구 열이 없는 예전 기록은 None
    hit_ratio: float | None = None  # 엔진 HitData의 totalDamage ÷ physicalDamage(방어구·막기)
    out_type: int | None = None  # 이 액터가 플레이어를 칠 때의 오른손 무기 종류(-1 맨손·생물, -2 알 수 없음). 열이 없으면 None
    out_physical: float | None = None  # 그 타격의 physicalDamage(플레이어의 방어구 전)
    out_ratio: float | None = None  # 그 타격의 totalDamage ÷ physicalDamage(플레이어의 방어구·막기)
    out_flags: int = 0

    @property
    def enemy(self) -> bool:
        return "H" in self.flags and not set(self.flags).intersection("TSDX")

    @property
    def out_usable(self) -> bool:
        """적 피해 표에 쓸 수 있는 타격: 장비를 알고, 피해가 0보다 크고, 치명타·은신이 섞이지 않음."""
        return (self.out_type is not None and self.out_type != UNKNOWN_WEAPON and self.out_physical is not None
                and self.out_physical > 0 and not self.out_flags & (FLAG_CRITICAL | FLAG_SNEAK))


@dataclass
class Stage:
    requested: int
    actual: int
    visits: dict[str, str] = field(default_factory=dict)
    actors: dict[str, list[Actor]] = field(default_factory=dict)

    def samples(self) -> dict[str, list[Actor]]:
        return {
            cell: actors for cell, actors in self.actors.items()
            if self.requested == self.actual and self.visits.get(cell) == "ok"
        }


@dataclass
class Run:
    label: str
    save: str = "-"
    status: str = "미완료"
    stages: list[Stage] = field(default_factory=list)
    invalid_rows: int = 0


def read_survey(source: TextIO, label: str) -> Run:
    run = Run(label)
    stage = None
    for line in source:
        if line.startswith("#") or not line.strip():
            continue
        try:
            if not line.endswith("\n"):
                raise ValueError("미완성 줄")
            kind, ms, *fields = line.rstrip("\r\n").split("\t")
            if int(ms) < 0:
                raise ValueError("시각")
            match kind:
                case "S":
                    if fields[0] == "start":
                        run.save = fields[3]
                    elif fields[0] in ("DONE", "ABORT"):
                        run.status = " ".join(fields)
                case "L":
                    requested, actual, _ = fields
                    stage = Stage(int(requested), int(actual))
                    run.stages.append(stage)
                case "A" | "V":
                    if stage is None or int(fields[0]) != stage.requested:
                        raise ValueError("단계")
                    cell = fields[1]
                    if kind == "A":
                        if len(fields) not in (14, 18, 23):
                            raise ValueError("액터 열 수")
                        _, _, ref, _, _, _, _, _, level, maximum, _, _, _, flags = fields[:14]
                        max_health = float(maximum)
                        if cell in stage.visits or not math.isfinite(max_health):
                            raise ValueError("액터")
                        resist = ratio = None
                        outgoing = {}
                        if len(fields) == 23:
                            _, out_type, out_physical, out_total, out_flags = fields[18:]
                            out_physical, out_total = float(out_physical), float(out_total)
                            if not (math.isfinite(out_physical) and math.isfinite(out_total)):
                                raise ValueError("적 피해")
                            outgoing = {
                                "out_type": int(out_type), "out_physical": out_physical, "out_flags": int(out_flags, 16),
                                "out_ratio": out_total / out_physical if out_physical > 0 and out_total > 0 else None,
                            }
                        if len(fields) >= 18:
                            resist, physical, total, _ = (float(value) for value in fields[14:18])
                            if not math.isfinite(resist):
                                raise ValueError("방어도")
                            # physical −1은 HitData를 만들지 못함, 0은 엔진이 피해를 채우지 않음. 둘 다 비율 없음.
                            if physical > 0 and math.isfinite(physical) and math.isfinite(total) and total > 0:
                                ratio = total / physical
                        actor = Actor(int(ref, 16), int(level), max_health, flags, resist, ratio, **outgoing)
                        stage.actors.setdefault(cell, []).append(actor)
                    else:
                        _, _, _, result, _, count = fields
                        actor_count = int(count)
                        if cell in stage.visits or result not in ("ok", "timeout", "skipped") or actor_count < 0:
                            raise ValueError("방문")
                        stage.visits[cell] = result
                        if result == "ok" and len(stage.actors.setdefault(cell, [])) != actor_count:
                            run.invalid_rows += 1
                            del stage.actors[cell]
                case _:
                    raise ValueError("줄 종류")
        except (ValueError, IndexError):
            run.invalid_rows += 1
    return run


def combine(runs: list[Run]) -> Run:
    """여러 실행의 단계를 요청 레벨별로 하나씩 모은다. 같은 레벨이 여럿이면 ok 방문이 많은 쪽, 같으면 나중 것을 쓴다.

    "나중"은 기록 파일 이름(survey-YYYYMMDD-HHMMSS) 순서다. 인자 순서와 관계없다.
    """
    runs = sorted(runs, key=lambda run: run.label)
    chosen: dict[int, Stage] = {}
    for run in runs:
        for stage in run.stages:
            ok = list(stage.visits.values()).count("ok")
            current = chosen.get(stage.requested)
            if current is None or ok >= list(current.visits.values()).count("ok"):
                chosen[stage.requested] = stage
    merged = Run("합친 기록", " + ".join(dict.fromkeys(run.save for run in runs if run.save != "-")) or "-",
                 " / ".join(f"{run.label}: {run.status}" for run in runs))
    merged.stages = [chosen[level] for level in sorted(chosen)]
    merged.invalid_rows = sum(run.invalid_rows for run in runs)
    return merged


@dataclass
class Summary:
    samples: dict[str, list[Actor]]
    enemies: dict[str, list[Actor]]
    levels: list[int]
    health: list[float]
    matching: int

    @property
    def median(self) -> float | None:
        return percentile(self.health, 50)


def summarize(stage: Stage) -> Summary:
    samples = stage.samples()
    enemies = {cell: [actor for actor in actors if actor.enemy] for cell, actors in samples.items()}
    levels = sorted(actor.level for actors in enemies.values() for actor in actors)
    health = sorted(actor.max_health for actors in enemies.values() for actor in actors)
    matching = levels.count(max(stage.requested, ZONE_MINIMUM))
    return Summary(samples, enemies, levels, health, matching)


def percentile(values: list[float], percent: int) -> float | None:
    if not values:
        return None
    position = (len(values) - 1) * percent / 100
    low, high = math.floor(position), math.ceil(position)
    return values[low] + (values[high] - values[low]) * (position - low)


def level_changes(previous: Summary, current: Summary) -> tuple[int, int]:
    before = {(cell, actor.ref): actor.level for cell, actors in previous.samples.items() for actor in actors}
    after = {(cell, actor.ref): actor.level for cell, actors in current.samples.items() for actor in actors}
    common = before.keys() & after.keys()
    return sum(before[key] != after[key] for key in common), len(common)


def fmt(value: float | None) -> str:
    return "-" if value is None else f"{value:.3f}".rstrip("0").rstrip(".")


def fraction(part: int, total: int) -> str:
    return f"{part}/{total} ({part / total:.1%})" if total else "-"


def table(headers: list[str], rows: list[list[str]]) -> list[str]:
    def row(fields: list[str]) -> str:
        return "| " + " | ".join(field.replace("|", r"\|") for field in fields) + " |"

    return [row(headers), row(["---"] * len(headers)), *(row(fields) for fields in rows), ""] if rows else ["기록 없음", ""]


def armor_formula(resist: float, scale: float, cap: float) -> float:
    return 1 - min(cap, resist * scale) / 100


def armor_row(label: str, summary: Summary, scale: float, cap: float) -> list[str]:
    """비율(0보다 큼)이 있는 적만 센다. 적이 없는 단계는 "-"로 채운다."""
    enemies = [actor for actors in summary.enemies.values() for actor in actors]
    measured = [actor for actor in enemies if actor.hit_ratio is not None]
    ratios = sorted(actor.hit_ratio for actor in measured)
    resists = sorted(actor.damage_resist for actor in measured)
    matches = sum(abs(actor.hit_ratio - armor_formula(actor.damage_resist, scale, cap)) <= FORMULA_TOLERANCE
                  for actor in measured)
    effective = sorted(actor.max_health / actor.hit_ratio for actor in measured)
    return [
        label, f"{len(measured)}/{len(enemies)}", *(fmt(percentile(ratios, p)) for p in PERCENTILES),
        fraction(sum(ratio < 1 - FORMULA_TOLERANCE for ratio in ratios), len(ratios)),
        fmt(percentile(resists, 50)), fraction(matches, len(measured)), fmt(percentile(effective, 50)),
    ]


def make_report(run: Run, life: dict[int, float], armor_scale: float = ARMOR_SCALING,
                armor_max: float = ARMOR_MAX, damage: dict[int, float] | None = None) -> list[str]:
    sections = [f"## {run.label}", "", f"세이브 `{run.save}` · 상태 `{run.status}`", ""]
    if run.invalid_rows:
        sections += [f"형식 또는 액터 수가 맞지 않는 줄 {run.invalid_rows}개를 확인했다.", ""]
    stages = [(stage, summarize(stage)) for stage in run.stages]
    baseline = next((summary for stage, summary in stages if stage.requested == 1 and summary.health), None)
    k = baseline.median if baseline else None
    if k is None:
        sections += ["K를 계산할 레벨 1 적 표본이 없다.", ""]
    else:
        sample_size = len(baseline.health)
        note = " · 표본 부족(참고값)" if sample_size < MIN_ENEMIES else ""
        sections += [f"K = **{fmt(k)}** (레벨 1 적 {sample_size}명){note}.", ""]
        sections += table(["N", "B = K ÷ N"], [[str(n), fmt(k / n)] for n in (2, 3, 4)])

    total, per_cell, growth, resets, armor = [], [], [], [], []
    # 방어구 열이 있는 기록(2026-11-19 이후)이면 모든 단계에 방어구 표의 행을 만든다.
    has_armor = any(actor.damage_resist is not None for stage in run.stages
                    for actors in stage.actors.values() for actor in actors)
    previous = None
    for index, (stage, summary) in enumerate(stages, 1):
        label = f"{index} (L{stage.requested})"
        if has_armor:
            armor.append(armor_row(label, summary, armor_scale, armor_max))
        outcomes = list(stage.visits.values())
        levels = summary.levels
        total.append([
            label, str(stage.actual), str(outcomes.count("ok")), str(outcomes.count("timeout")),
            str(outcomes.count("skipped")), str(len(levels)),
            f"{levels[0]} / {fmt(statistics.median(levels))} / {levels[-1]}" if levels else "-",
            *(fmt(percentile(summary.health, p)) for p in PERCENTILES),
        ])
        for cell, outcome in stage.visits.items():
            enemies = summary.enemies.get(cell)
            per_cell.append([
                label, cell, outcome, str(len(enemies)) if enemies is not None else "-",
                fmt(statistics.median(a.max_health for a in enemies)) if enemies else "-",
            ])
        ratio = life_at(life, stage.requested) / life_at(life, 1)
        median = summary.median
        growth.append([label, fmt(median / k if k and median is not None else None), *(fmt(ratio ** (1 - r)) for r in (0, 0.5, 0.75))])
        if stage.actual == stage.requested:
            changes = fraction(*level_changes(previous[1], summary)) if previous else "-"
            resets.append([label, str(max(stage.requested, ZONE_MINIMUM)), fraction(summary.matching, len(levels)),
                           previous[0] if previous else "-", changes])
            previous = label, summary

    sections += ["### 레벨별 표본", ""]
    sections += table(["단계", "실제 L", "ok", "timeout", "skipped", "적 수", "적 레벨 최소 / 중앙 / 최대",
                       *(f"체력 {p}%" for p in PERCENTILES)], total)
    sections += ["### 셀별 표본", ""]
    sections += table(["단계", "셀", "방문", "적 수", "체력 중앙값"], per_cell)
    sections += ["### 체력 성장", "", "스카이림 배수는 각 단계의 적 체력 중앙값을 K로 나눈 값이다.", ""]
    sections += table(["단계", "스카이림", "PoE r = 0", "PoE r = 0.5", "PoE r = 0.75"], growth)
    sections += ["### 초기화 확인", "",
                 "이번 단계와 직전 레벨 확인에 성공한 단계에서 공통 셀·참조의 액터 레벨을 비교한다.",
                 "적 레벨은 요청 레벨과 구역 최소 5 중 큰 값과 비교한다.", ""]
    sections += table(["단계", "비교할 적 레벨", "적 레벨 일치", "직전 단계", "공통 참조 레벨 변경"], resets)
    if armor:
        sections += ["### 방어구 비율", "",
                     "플레이어의 오른손 무기로 적을 쳤다고 할 때 엔진이 채운 `totalDamage ÷ physicalDamage`(방어구·막기)다.",
                     f"식은 `1 − min({fmt(armor_max)}, 방어도 × {fmt(armor_scale)}) ÷ 100`, 차이 {FORMULA_TOLERANCE} 이하를 맞음으로 센다.",
                     "방어구를 넣은 체력은 `최대 체력 ÷ 비율`이다(공격 스킬이 그 적을 잡는 데 드는 피해).", ""]
        sections += table(["단계", "비율이 있는 적", *(f"비율 {p}%" for p in PERCENTILES), f"비율 < {fmt(1 - FORMULA_TOLERANCE)}",
                           "방어도 중앙값", "식과 맞음", "방어구를 넣은 체력 중앙값"], armor)
    if any(actor.out_type is not None for stage in run.stages for actors in stage.actors.values() for actor in actors):
        sections += outgoing_section(stages, damage)
    return sections


def outgoing_section(stages: list[tuple[Stage, Summary]], damage: dict[int, float] | None) -> list[str]:
    """적이 오른손 장비로 플레이어를 쳤다고 할 때의 physicalDamage(플레이어 방어구 전)를 무기 무리별로 낸다."""
    groups: list[tuple[str, set[int] | None]] = [("전체", None), *WEAPON_GROUPS.items()]

    def hits(summary: Summary, types: set[int] | None) -> list[float]:
        return sorted(actor.out_physical for actors in summary.enemies.values() for actor in actors
                      if actor.out_usable and (types is None or actor.out_type in types))

    base_summary = next((summary for stage, summary in stages if stage.requested == 1), None)
    baseline = {name: percentile(hits(base_summary, types), 50) if base_summary else None for name, types in groups}
    rows, excluded = [], []
    for index, (stage, summary) in enumerate(stages, 1):
        label = f"{index} (L{stage.requested})"
        enemies = [actor for actors in summary.enemies.values() for actor in actors if actor.out_type is not None]
        excluded.append([
            label, str(len(enemies)), str(sum(actor.out_type == UNKNOWN_WEAPON for actor in enemies)),
            str(sum(bool(actor.out_flags & (FLAG_CRITICAL | FLAG_SNEAK)) for actor in enemies)),
            str(sum(actor.out_type != UNKNOWN_WEAPON and (actor.out_physical or 0) <= 0 for actor in enemies)),  # -1(생성 실패) 포함
            fmt(percentile(sorted(actor.out_ratio for actor in enemies if actor.out_usable and actor.out_ratio is not None), 50)),
        ])
        poe = damage[stage.requested] / damage[1] if damage and stage.requested in damage else None
        for name, types in groups:
            values = hits(summary, types)
            median, base = percentile(values, 50), baseline[name]
            rows.append([label, name, str(len(values)), *(fmt(percentile(values, p)) for p in PERCENTILES),
                         fmt(median / base if median is not None and base else None), fmt(poe)])
    lines = ["### 적이 주는 피해", "",
             "적이 오른손 장비 항목으로 플레이어를 쳤다고 할 때 엔진이 채운 `physicalDamage`(플레이어의 방어구·막기 전, 난이도 배율 전)다.",
             "장비를 알 수 없는 적(처리 단계가 낮음), 치명타·은신이 섞인 타격, 피해가 0인 타격은 뺀다. 근접에는 맨손·생물의 기본 공격(항목 없음)이 들어간다.",
             "PoE 배수는 PoB `monsterDamageTable(L) ÷ monsterDamageTable(1)`이다.", ""]
    lines += table(["단계", "무리", "적 수", *(f"피해 {p}%" for p in PERCENTILES), "중앙값 ÷ 레벨 1", "PoE 배수"], rows)
    lines += table(["단계", "적 피해 열이 있는 적", "장비 모름", "치명타·은신", "피해 0 이하", "플레이어 방어구 비율 중앙값"], excluded)
    return lines


def selftest() -> None:
    def actors(level: int, cell: str, rows: list[tuple[int, int, int, str]]) -> str:
        return "".join(
            f"A\t1\t{level}\t{cell}\t{ref:X}\t0\t0\t-\t산적\t0\t{actor_level}\t{hp}\t0\t0\t0\t{flags}\n"
            for ref, actor_level, hp, flags in rows
        )

    fixture = (
        "S\t0\tstart\t0.1.0\t1.6.1170\tExample\n"
        "L\t1\t1\t1\t100\n"
        + actors(1, "Mine", [(1, 5, 50, "H"), (2, 4, 100, "HEPU"), (3, 5, 150, "H"), (4, 5, 200, "H"),
                             *((ref, 5, 9999, flags) for ref, flags in enumerate(("HT", "HS", "HD", "HX", "U"), 5))])
        + "V\t2\t1\tMine\t100\tok\t1\t9\nV\t3\t1\tCave\t200\ttimeout\t90000\t0\n"
        "L\t4\t10\t10\t100\n"
        + actors(10, "Mine", [(1, 10, 100, "H"), (2, 4, 200, "H"), (3, 10, 300, "H"), (4, 10, 400, "H")])
        + "V\t5\t10\tMine\t100\tok\t1\t4\n"
        + actors(10, "Cave", [(1, 10, 9999, "H")])
        + "L\t7\t20\t19\t100\nV\t8\t20\tMine\t100\tskipped\t-1\t0\n"
        "S\t9\tABORT\tplayer-dead\nA\t10\t20"
    )
    run = read_survey(io.StringIO(fixture), "selftest")
    low, high, skipped = map(summarize, run.stages)
    assert run.save == "Example" and run.status == "ABORT player-dead" and run.invalid_rows == 1
    assert run.stages[0].visits == {"Mine": "ok", "Cave": "timeout"}
    assert len(low.samples["Mine"]) == 9 and len(low.enemies["Mine"]) == 4
    assert low.levels == [4, 5, 5, 5] and low.matching == 3
    assert low.median == 125 and high.median == 250 and high.median / low.median == 2
    assert [percentile(low.health, p) for p in PERCENTILES] == [65, 87.5, 125, 162.5, 185]
    assert percentile([], 90) is None and percentile([8], 10) == 8
    assert high.matching == 3 and not skipped.health
    assert level_changes(low, high) == (3, 4)
    report = "\n".join(make_report(run, LIFE_FALLBACK))
    assert "K = **125**" in report and "표본 부족" in report
    assert "| 2 | 62.5 |" in report and "| 4 | 31.25 |" in report
    assert "| 2 (L10) | 2 | 3.545 | 1.883 | 1.372 |" in report
    assert "| 2 (L10) | 10 | 3/4 (75.0%) | 1 (L1) | 3/4 (75.0%) |" in report
    extra = read_survey(io.StringIO("L\t1\t10\t10\t100\n" + actors(10, "Mine", [(1, 10, 500, "H")]) +
                                    "V\t2\t10\tMine\t100\tok\t1\t1\nS\t3\tDONE\n"), "survey-z.tsv")
    worse = read_survey(io.StringIO("L\t1\t10\t10\t100\nV\t2\t10\tMine\t100\ttimeout\t90000\t0\n"), "survey-zz.tsv")
    merged = combine([worse, extra, run])  # 인자 순서와 관계없이 파일 이름 순서로 합침
    assert [stage.requested for stage in merged.stages] == [1, 10, 20]
    assert summarize(merged.stages[1]).median == 500  # ok 방문 수가 같으면 나중 실행의 단계를 씀
    assert merged.stages[0] is run.stages[0] and merged.stages[2] is run.stages[2]
    assert merged.status == "selftest: ABORT player-dead / survey-z.tsv: DONE / survey-zz.tsv: 미완료"
    assert merged.save == "Example"  # 같은 세이브 이름은 한 번만
    assert "### 방어구 비율" not in report  # 예전 형식(방어구 열 없음)이면 표를 내지 않음

    def armored(rows: list[tuple[int, int, float, float, float]]) -> str:
        return "".join(
            f"A\t1\t1\tFort\t{ref:X}\t0\t0\t-\t산적\t0\t1\t{hp}\t0\t0\t0\tH\t{resist}\t{physical}\t{total}\t0\n"
            for ref, hp, resist, physical, total in rows
        )

    # 방어도 0(비율 1), 100(0.85), 600(상한 0.25), 물리 0·HitData 없음(−1)·total 0(비율 없음), 식과 다른 0.5
    armor_run = read_survey(io.StringIO(
        "L\t1\t1\t1\t100\n" + armored([(1, 100, 0, 10, 10), (2, 85, 100, 10, 8.5), (3, 50, 600, 10, 2.5),
                                       (4, 70, 50, 0, 0), (5, 60, 0, 10, 5), (6, 80, 10, -1, 0), (7, 90, 10, 10, 0)])
        + "A\t3\t1\tFort\t9\t0\t0\t-\t산적\t0\t1\t50\t0\t0\t0\tH\t1\n"  # 방문 줄보다 앞, 열 수만 틀림
        + "A\t3\t1\tFort\t8\t0\t0\t-\t산적\t0\t1\t50\t0\t0\t0\tH\tnan\t10\t5\t0\n"  # 방어도 NaN
        + "V\t4\t1\tFort\t1\tok\t1\t7\nL\t5\t30\t30\t100\nV\t6\t30\tFort\t1\ttimeout\t90000\t0\n"), "armor")
    assert armor_run.invalid_rows == 2
    fort = armor_run.stages[0].actors["Fort"]
    assert [actor.hit_ratio for actor in fort] == [1, 0.85, 0.25, None, 0.5, None, None]
    assert armor_formula(100, ARMOR_SCALING, ARMOR_MAX) == 0.85 and armor_formula(600, ARMOR_SCALING, ARMOR_MAX) == 0.25
    armor_report = "\n".join(make_report(armor_run, LIFE_FALLBACK))
    # 비율 [0.25, 0.5, 0.85, 1] · 방어도 [0, 0, 100, 600] · 식과 맞음 3 · 방어구를 넣은 체력 [100, 100, 120, 200]
    assert "| 1 (L1) | 4/7 | 0.325 | 0.438 | 0.675 | 0.887 | 0.955 | 3/4 (75.0%) | 50 | 3/4 (75.0%) | 110 |" in armor_report
    assert "| 2 (L30) | 0/0 | - | - | - | - | - | - | - | - | - |" in armor_report  # 적이 없는 단계도 행을 냄
    assert "비율 < 0.995" in armor_report
    assert "### 적이 주는 피해" not in armor_report  # 적 피해 열이 없는 기록이면 표를 내지 않음

    def outgoing(level: int, rows: list[tuple[int, int, float, float, int]]) -> str:
        return "".join(
            f"A\t1\t{level}\tFort\t{ref:X}\t0\t0\t-\t산적\t0\t1\t50\t0\t0\t0\tH\t0\t10\t10\t0\t0\t{kind}\t{phys}\t{total}\t{flags:X}\n"
            for ref, kind, phys, total, flags in rows
        )

    # 레벨 1: 한손검 10, 맨손 20, 활 30, 장비 모름 40, 치명타 100, 피해 0. 레벨 10: 한손검 20, 활 60.
    out_run = read_survey(io.StringIO(
        "L\t1\t1\t1\t100\n" + outgoing(1, [(1, 1, 10, 5, 0x40000), (2, -1, 20, 10, 0), (3, 7, 30, 15, 0),
                                           (4, -2, 40, 0, 0), (5, 1, 100, 50, 0x8), (6, 1, 0, 0, 0)])
        + "V\t2\t1\tFort\t1\tok\t1\t6\nL\t3\t10\t10\t100\n"
        + outgoing(10, [(1, 1, 20, 10, 0), (3, 7, 60, 30, 0)]) + "V\t4\t10\tFort\t1\tok\t1\t2\n"), "out")
    assert out_run.invalid_rows == 0
    out_report = "\n".join(make_report(out_run, LIFE_FALLBACK, damage={1: 5.0, 10: 10.0}))
    assert "| 1 (L1) | 전체 | 3 | 12 | 15 | 20 | 25 | 28 | 1 | 1 |" in out_report
    assert "| 1 (L1) | 근접 | 2 | 11 | 12.5 | 15 | 17.5 | 19 | 1 | 1 |" in out_report
    assert "| 2 (L10) | 전체 | 2 | 24 | 30 | 40 | 50 | 56 | 2 | 2 |" in out_report
    assert "| 2 (L10) | 활·석궁 | 1 | 60 | 60 | 60 | 60 | 60 | 2 | 2 |" in out_report
    assert "| 2 (L10) | 지팡이 | 0 | - | - | - | - | - | - | 2 |" in out_report
    assert "| 1 (L1) | 6 | 1 | 1 | 1 | 0.5 |" in out_report  # 뺀 적 수와 플레이어 방어구 비율
    assert "| 1 (L1) | 전체 | 3 | 12 | 15 | 20 | 25 | 28 | 1 | - |" in "\n".join(make_report(out_run, LIFE_FALLBACK))
    print("분석 검증 통과")


def default_record_dir() -> Path:
    documents = Path.home() / "Documents"
    if sys.platform == "win32":
        import ctypes

        buffer = ctypes.create_unicode_buffer(260)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buffer) == 0:  # CSIDL_PERSONAL
            documents = Path(buffer.value)
    return documents / "My Games" / "Skyrim Special Edition" / "SKSE" / "PoEProtoSurvey"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, help="기록 TSV 또는 폴더. 기본값은 문서의 SKSE/PoEProtoSurvey")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "out" / "report.md", help="보고서 경로")
    parser.add_argument("--pob", type=Path, default=DEFAULT_POB_MISC, help="PoB의 Data/Misc.lua 경로")
    parser.add_argument("--selftest", action="store_true", help="꾸민 기록으로 분석 검증")
    parser.add_argument("--combine", action="store_true", help="여러 기록의 단계를 요청 레벨별로 합쳐 한 표로 낸다(실행이 도중에 끊겨 나눠 잰 때)")
    parser.add_argument("--armor-scale", type=float, default=ARMOR_SCALING, help="방어도 1당 감소 %%(fArmorScalingFactor)")
    parser.add_argument("--armor-max", type=float, default=ARMOR_MAX, help="방어구 감소 상한 %%(fMaxArmorRating)")
    args = parser.parse_args()
    if args.armor_scale < 0 or not 0 <= args.armor_max <= 100:
        parser.error("--armor-scale은 0 이상, --armor-max는 0~100이어야 함")
    if args.selftest:
        selftest()
        return 0
    files = []
    for path in args.paths or [default_record_dir()]:
        if path.is_dir():
            files.extend(sorted(path.glob("survey-*.tsv")))
        elif path.is_file():
            files.append(path)
        else:
            parser.error(f"기록 경로를 찾지 못함: {path}")
    if not files:
        parser.error("기록 TSV를 찾지 못함")

    life, source = load_life_table(args.pob)
    damage = read_pob_table(args.pob, "monsterDamageTable")
    damage_source = f"PoB의 {args.pob.name}" if damage else f"읽지 못함(적 피해 표의 PoE 배수는 비움): {args.pob}"
    report = ["# 적 체력 조사", "", f"PoE 몬스터 생명력 표 · {source}", f"PoE 몬스터 피해 표 · {damage_source}", ""]
    runs = []
    for path in dict.fromkeys(path.resolve() for path in files):
        try:
            with path.open(encoding="utf-8-sig", errors="replace") as handle:
                runs.append(read_survey(handle, path.name))
        except OSError as error:
            parser.error(str(error))
    for run in [combine(runs)] if args.combine else runs:
        report.extend(make_report(run, life, args.armor_scale, args.armor_max, damage))
    text = "\n".join(report)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    if sys.stdout.encoding and sys.stdout.encoding.lower() != "utf-8":
        sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
