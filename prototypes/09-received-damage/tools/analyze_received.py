"""PoE Proto 9 로그에서 README의 R1~R3 판정과 R4~R8 통계, 마법 자리별 통계를 계산한다.

사용: python analyze_received.py <PoEProtoReceived.log>
      python analyze_received.py --selftest
"""

import argparse
from bisect import bisect_left
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from datetime import datetime
import math
from pathlib import Path
import statistics
import sys
import unittest


PLAYER = 0x14
YES, NO, EMPTY = "그렇다", "아니다", "판정할 줄 없음"
MODES = {"observe", "scale"}
HIT_SITES = ("explosion", "beam", "nonactor_arrow", "trap", "collision", "unknown", "melee")
STEP_HIT_SITES = {"populate": ("melee",), "arrow": ("melee", "beam", "nonactor_arrow")}


def number(text):
    value = float(text)
    if not math.isfinite(value):
        raise ValueError("유한수가 아님")
    return value


def integer(text, base=10):
    value = int(text, base)
    if value < 0:
        raise ValueError("음수 식별자")
    return value


def optional_integer(text):
    return None if text == "-" else int(text)


@dataclass
class Sample:
    kind: str
    t: int
    thread: int
    mode: str
    modified: bool
    target: int
    before: float
    after: float
    health_before: float
    health_after: float
    multiplier: float
    site: str
    spell: int = 0
    effect: int = 0
    casting: int | None = None
    duration: float = 0.0
    elapsed: float = 0.0
    magnitude: float = 0.0
    base_magnitude: float = 0.0
    resistance: float | None = None
    physical: float = 0.0
    late: float | None = None
    caster: int = 0

    @property
    def end_health(self):
        # 그 줄에서 체력이 그대로였을 때만 늦은 읽기를 사용한다.
        if self.health_before == self.health_after and self.late is not None:
            return self.late
        return self.health_after

    @property
    def drop(self):
        return self.health_before - self.end_health

    @property
    def expected(self):
        return (abs(self.after) if self.kind == "MAG" else self.after) * self.multiplier


@dataclass
class MagnitudeStep:
    t: int
    thread: int
    target: int
    caster: int | None
    spell: int
    spell_type: int
    casting: int
    effect: int
    base_magnitude: float
    mag_before: float
    eff: float
    resistance: float | None
    power: float | None
    mag_after: float
    dur_before: float
    dur_after: float
    caster_from_one: float | None
    target_from_one: float | None
    predicted: float | None


@dataclass
class ExplosionStep:
    t: int
    thread: int
    aggressor: int
    target: int
    weapon: int
    source_ref: int
    total_before: float
    total_after: float
    ratio: float | None
    physical: float
    percent_blocked: float
    sneak: float
    flags: int
    predicted: float


@dataclass
class HitStep:
    t: int
    thread: int
    site: str
    aggressor: int
    target: int
    weapon: int
    source_ref: int
    total_before: float
    total_after: float
    ratio: float | None
    sneak: float
    flags: int


@dataclass
class Log:
    magic: list = field(default_factory=list)
    hits: list = field(default_factory=list)
    amp: list = field(default_factory=list)
    exp: list = field(default_factory=list)
    step: list = field(default_factory=list)
    settings: dict = field(default_factory=dict)
    skipped: int = 0
    late_paired: int = 0
    late_unpaired: int = 0
    late_missing: int = 0

    @property
    def value(self):
        return float(self.settings.get("Value", "0.5"))


def parse_sample(kind, c):
    if kind == "MAG":
        if len(c) != 24 or c[2] not in MODES or c[3] not in {"0", "1"} or not c[4]:
            raise ValueError("MAG 칸 수 또는 모드 오류")
        t, thread = integer(c[0]), integer(c[1])
        target, caster, spell = (integer(x, 16) for x in c[5:8])
        optional_integer(c[8])  # 주문 종류
        casting = optional_integer(c[9])
        effect = integer(c[10], 16)
        optional_integer(c[11])  # 저항 변수: kNone(-1)도 유효하다.
        integer(c[12], 16)
        duration, elapsed, magnitude, base = (number(x) for x in c[13:17])
        resistance = None if c[17] == "-" else number(c[17])
        before, after, hp0, hp1, _, mult = (number(x) for x in c[18:24])
        return Sample(kind, t, thread, c[2], c[3] == "1", target, before, after,
                      hp0, hp1, mult, c[4], spell, effect, casting, duration,
                      elapsed=elapsed, magnitude=magnitude, base_magnitude=base,
                      resistance=resistance, caster=caster)
    if len(c) != 19 or c[3] not in MODES or c[4] not in {"0", "1"} or not c[2]:
        raise ValueError("HIT 칸 수 또는 모드 오류")
    t, thread = integer(c[0]), integer(c[1])
    _, target, _, _ = (integer(x, 16) for x in c[5:9])
    optional_integer(c[9])
    integer(c[10], 16)
    before, after, physical, _, hp0, hp1, _, mult = (number(x) for x in c[11:19])
    return Sample(kind, t, thread, c[3], c[4] == "1", target, before, after,
                  hp0, hp1, mult, c[2], physical=physical)


def parse_amp(c):
    if len(c) != 19:
        raise ValueError("AMP 칸 수 오류")
    t, thread = integer(c[0]), integer(c[1])
    target = integer(c[2], 16)
    caster = None if c[3] == "-" else integer(c[3], 16)
    spell, spell_type, casting, effect = integer(c[4], 16), integer(c[5]), integer(c[6]), integer(c[7], 16)
    base, before, eff = (number(x) for x in c[8:11])
    resistance, power = (None if x == "-" else number(x) for x in c[11:13])
    after, dur_before, dur_after = (number(x) for x in c[13:16])
    caster_one, target_one, predicted = (None if x == "-" else number(x) for x in c[16:19])
    return MagnitudeStep(t, thread, target, caster, spell, spell_type, casting, effect,
                         base, before, eff, resistance, power, after, dur_before, dur_after,
                         caster_one, target_one, predicted)


def parse_exp(c):
    if len(c) != 14:
        raise ValueError("EXP 칸 수 오류")
    t, thread = integer(c[0]), integer(c[1])
    aggressor, target, weapon, source_ref = (integer(x, 16) for x in c[2:6])
    before, after = (number(x) for x in c[6:8])
    ratio = None if c[8] == "-" else number(c[8])
    physical, blocked, sneak = (number(x) for x in c[9:12])
    flags = integer(c[12], 16)
    predicted = number(c[13])
    return ExplosionStep(t, thread, aggressor, target, weapon, source_ref, before, after,
                         ratio, physical, blocked, sneak, flags, predicted)


def parse_step(c):
    if len(c) != 12 or c[2] not in STEP_HIT_SITES:
        raise ValueError("STEP 칸 수 또는 자리 오류")
    t, thread = integer(c[0]), integer(c[1])
    aggressor, target, weapon, source_ref = (integer(x, 16) for x in c[3:7])
    before, after = (number(x) for x in c[7:9])
    ratio = None if c[9] == "-" else number(c[9])
    sneak, flags = number(c[10]), integer(c[11], 16)
    return HitStep(t, thread, c[2], aggressor, target, weapon, source_ref, before, after, ratio, sneak, flags)


def read_log(text):
    log = Log()
    late = []
    for line in text.splitlines():
        if not line.strip():
            continue
        cols = line.split("\t")
        try:
            if len(cols) < 3:
                raise ValueError("로그 머리 없음")
            datetime.strptime(cols[0], "%Y-%m-%d %H:%M:%S.%f")
            if cols[1] not in {"trace", "debug", "info", "warning", "warn", "error", "critical", "off"}:
                raise ValueError("로그 등급 오류")
            kind, c = cols[2], cols[3:]
            if kind in {"MAG", "HIT"}:
                sample = parse_sample(kind, c)
                (log.magic if kind == "MAG" else log.hits).append(sample)
            elif kind == "AMP":
                log.amp.append(parse_amp(c))
            elif kind == "EXP":
                log.exp.append(parse_exp(c))
            elif kind == "STEP":
                log.step.append(parse_step(c))
            elif kind == "LATE":
                if len(c) != 4:
                    raise ValueError("LATE 칸 수 오류")
                t, target, original = integer(c[0]), integer(c[1], 16), integer(c[3])
                if t < original:
                    raise ValueError("LATE 시각 역전")
                health = None if c[2] == "-" else number(c[2])
                late.append((t, target, original, health))
            elif kind == "MODE":
                if len(c) != 2 or c[1] not in MODES:
                    raise ValueError("MODE 칸 오류")
                integer(c[0])
            elif kind.startswith("설정: "):
                settings = dict(pair.split("=", 1) for pair in kind[4:].split() if "=" in pair)
                if "Value" in settings and not 0 <= number(settings["Value"]) <= 100:
                    raise ValueError("설정 Value 범위 오류")
                log.settings.update(settings)
            elif c:
                raise ValueError("알 수 없는 자료 줄")
            # 설치·알림 등 탭 칸이 없는 일반 로그는 정상이다.
        except (ValueError, IndexError, OverflowError):
            log.skipped += 1
    by_key = defaultdict(list)
    for sample in log.magic + log.hits:
        by_key[(sample.t, sample.target)].append(sample)
    # 로그가 스레드 실행 순서와 다르게 기록되어도 시각과 대상으로 연결한다.
    for _, target, original, health in sorted(late):
        if health is None:
            log.late_missing += 1
            continue
        samples = by_key.get((original, target), [])
        if len(samples) != 1 or samples[0].late is not None:
            log.late_unpaired += 1
            continue
        samples[0].late = health
        log.late_paired += 1
    return log


def close_damage(actual, expected):
    if not math.isfinite(actual) or not math.isfinite(expected):
        return False
    return abs(actual - expected) <= max(0.05, abs(expected) * 0.01)


def close_resistance(actual, expected):
    if not math.isfinite(actual) or not math.isfinite(expected):
        return False
    return actual == 0 if expected == 0 else abs(actual - expected) <= abs(expected) * 0.05


@dataclass
class Verdict:
    total: int
    matched: int
    threshold: float
    dead: int = 0
    mismatches: list = field(default_factory=list)
    scale_total: int = 0
    scale_matched: int = 0

    @property
    def answer(self):
        if self.total == 0:
            return EMPTY
        return YES if self.matched / self.total >= self.threshold else NO


def damage_verdict(samples, value, scale=False):
    chosen = [s for s in samples if s.target == PLAYER and
              (s.mode == "scale" and s.modified if scale else s.before < 0)]
    dead = sum(s.end_health <= 0 for s in chosen)
    alive = [s for s in chosen if s.end_health > 0]
    # R1의 분모는 실제 체력이 준 마법 피해 줄, R3는 바뀐 모든 줄이다.
    judged = alive if scale else [s for s in alive if s.drop > 0]
    bad = [s for s in judged if not close_damage(s.drop, s.expected)]
    result = Verdict(len(judged), len(judged) - len(bad), 0.9, dead, bad)
    if scale:
        result.scale_total = len(chosen)
        result.scale_matched = sum(math.isclose(s.after, s.before * value, rel_tol=0.00001, abs_tol=0.00001)
                                   for s in chosen)
    return result


def resistance_verdict(samples):
    groups = defaultdict(list)
    omitted = 0
    for sample in samples:
        if sample.resistance is None:
            omitted += 1
            continue
        groups[(sample.effect, sample.spell)].append(sample)
    # 같은 효과가 반복되었다고 그 효과의 표가 더 무거워지지 않게 효과마다 한 표.
    # 각 효과의 유효 줄이 모두 맞아야 그 효과가 맞은 것으로 보수적으로 센다.
    # 엔진은 깎는 효과의 magnitude를 음수로 둔다(2026-11-18 첫 시험). 크기만 비교한다.
    bad = {key: [s for s in part if not close_resistance(abs(s.magnitude), s.base_magnitude * s.resistance)]
           for key, part in groups.items()}
    bad = {key: part for key, part in bad.items() if part}
    verdict = Verdict(len(groups), len(groups) - len(bad), 0.8, mismatches=bad)
    return verdict, groups, omitted


def magnitude_resistance_verdict(amp):
    total = 0
    bad = []
    for sample in amp:
        if sample.resistance is None or sample.mag_before == 0:
            continue
        total += 1
        before = abs(sample.mag_before)
        # SetEffectiveness는 eff가 1이면 크기를 그대로 두고, 그 외에는 하한 1.0을 적용한다.
        expected = before if sample.eff == 1 else max(before * sample.eff, 1.0)
        if not math.isfinite(expected) or abs(abs(sample.mag_after) - expected) > max(0.01, expected * 0.01):
            bad.append((sample, expected))
    return Verdict(total, total - len(bad), 0.9, mismatches=bad)


def hit_statistics(hits):
    groups = defaultdict(list)
    for sample in hits:
        groups[sample.site].append(sample)
    stats = {}
    for site, part in groups.items():
        ratios = [s.before / s.physical for s in part if s.physical > 0]
        stats[site] = (len(part), len(ratios) / len(part),
                       (min(ratios), statistics.median(ratios), max(ratios)) if ratios else None)
    return stats


def magic_statistics(magic):
    return Counter((s.casting, s.duration > 0) for s in magic)


def magic_site_statistics(magic, value):
    """마법 자리별 (줄 수, 지난 시간 0인 첫 호출 수, R1 판정 줄 수, R1 맞은 줄 수). 플레이어가 받은 피해 줄만 센다."""
    groups = defaultdict(list)
    for sample in magic:
        if sample.target == PLAYER and sample.before < 0:
            groups[sample.site].append(sample)
    stats = {}
    for site, part in groups.items():
        r1 = damage_verdict(part, value)
        stats[site] = (len(part), sum(s.elapsed == 0 for s in part), r1.total, r1.matched)
    return stats


@dataclass
class MagnitudeStatistics:
    count: int
    ranges: dict
    matched: int = 0
    compared: int = 0
    unpaired: int = 0
    prediction_missing: int = 0


def magnitude_step_statistics(amp, magic):
    """AMP를 효과 설정×주문×시전자로 묶고 시각순 첫 MAG와 비교한다. 판정에는 쓰지 않는다."""
    first_calls = defaultdict(list)
    for sample in magic:
        if sample.elapsed == 0:
            first_calls[(sample.target, sample.caster, sample.spell, sample.effect)].append(sample)
    for candidates in first_calls.values():
        candidates.sort(key=lambda s: s.t)
    groups = defaultdict(list)
    for sample in amp:
        groups[(sample.effect, sample.spell, sample.caster)].append(sample)
    stats = {}
    for key, part in groups.items():
        values = {
            "initial_ratio": [abs(s.mag_before) / s.base_magnitude if s.base_magnitude else None for s in part],
            "eff": [s.eff for s in part],
            "r": [s.resistance for s in part],
            "power": [s.power for s in part],
            "effectiveness_ratio": [abs(s.mag_after) / abs(s.mag_before) if s.mag_before else None for s in part],
            "casterFromOne": [s.caster_from_one for s in part],
            "targetFromOne": [s.target_from_one for s in part],
        }
        ranges = {}
        for name, items in values.items():
            present = [v for v in items if v is not None]
            ranges[name] = (min(present), max(present)) if present else None
        result = MagnitudeStatistics(len(part), ranges)
        for sample in part:
            # 시전자 액터가 없을 때 MAG는 00000000, AMP는 '-'로 적는다.
            candidates = first_calls.get((sample.target, sample.caster or 0, sample.spell, sample.effect), [])
            index = bisect_left(candidates, sample.t, key=lambda s: s.t)
            if index == len(candidates) or candidates[index].t - sample.t > 2_000_000:
                result.unpaired += 1
            elif sample.predicted is None:
                result.prediction_missing += 1
            else:
                result.compared += 1
                # 엔진은 퍽까지 적용한 뒤에야 해로운 효과의 크기에 음수 부호를 붙인다(2026-11-18 세 번째 시험). 크기만 비교한다.
                if abs(abs(candidates[index].magnitude) - abs(sample.predicted)) <= max(0.01, abs(sample.predicted) * 0.01):
                    result.matched += 1
        stats[key] = result
    return stats


@dataclass
class ExplosionStatistics:
    count: int
    ratio_range: tuple | None
    ratio_missing: int
    physical_positive: float
    predicted_range: tuple | None
    matched: int = 0
    compared: int = 0
    unpaired: int = 0


def explosion_step_statistics(exp, hits):
    """같은 스레드의 가장 최근 EXP와 그 뒤 1초 안에 오는 같은 대상의 explosion HIT를 비교한다.

    44016 호출과 HitData 적용은 같은 스레드에서 차례로 일어난다. 같은 스레드에 다음 EXP가 먼저 오면
    앞의 EXP는 짝 없음이다.
    """
    ratios = [s.ratio for s in exp if s.ratio is not None]
    predicted = [s.predicted for s in exp]
    result = ExplosionStatistics(len(exp), (min(ratios), max(ratios)) if ratios else None,
                                 len(exp) - len(ratios), sum(s.physical > 0 for s in exp) / len(exp) if exp else 0.0,
                                 (min(predicted), max(predicted)) if predicted else None)
    events = sorted([(s.t, 0, s) for s in exp] + [(h.t, 1, h) for h in hits if h.site == "explosion"],
                    key=lambda event: (event[0], event[1]))
    pending = {}
    for _, kind, item in events:
        if kind == 0:
            pending[item.thread] = item
            continue
        step = pending.get(item.thread)
        if step is None or item.target != step.target:
            continue
        del pending[item.thread]
        if item.t - step.t > 1_000_000:
            continue
        result.compared += 1
        if abs(item.before - step.total_after) <= max(0.01, abs(step.total_after) * 0.01):
            result.matched += 1
    result.unpaired = result.count - result.compared
    return result


@dataclass
class HitStepStatistics:
    count: int
    ratio_range: tuple | None
    ratio_missing: int
    matched: int = 0
    compared: int = 0
    unpaired: int = 0


def hit_step_statistics(steps, hits):
    """자리별로 STEP과 그 뒤에 같은 스레드에서 이어지는 HIT의 physicalDamage를 비교한다. 판정에는 쓰지 않는다.

    44016 호출과 HitData 적용은 같은 스레드에서 차례로 일어나므로, HIT는 같은 스레드의 가장 최근 STEP과만
    짝짓는다(2026-11-18 여섯 번째 실행: 다른 스레드에서 피해로 이어지지 않는 STEP이 세 번씩 찍혀 시각만으로는
    엉뚱한 HIT와 짝지어졌음). 같은 스레드에 다음 STEP이 먼저 오면 앞의 STEP은 짝 없음이다.
    """
    stats = {}
    for site in STEP_HIT_SITES:
        part = [s for s in steps if s.site == site]
        ratios = [s.ratio for s in part if s.ratio is not None]
        stats[site] = HitStepStatistics(len(part), (min(ratios), max(ratios)) if ratios else None,
                                        len(part) - len(ratios))
    allowed = {hit_site for sites in STEP_HIT_SITES.values() for hit_site in sites}
    events = sorted([(s.t, 0, s) for s in steps] + [(h.t, 1, h) for h in hits if h.site in allowed],
                    key=lambda event: (event[0], event[1]))
    pending = {}
    for _, kind, item in events:
        if kind == 0:
            previous = pending.get(item.thread)
            if previous is not None:
                stats[previous.site].unpaired += 1
            pending[item.thread] = item
            continue
        step = pending.get(item.thread)
        if step is None or item.site not in STEP_HIT_SITES[step.site] or item.target != step.target:
            continue
        del pending[item.thread]
        result = stats[step.site]
        if item.t - step.t > 1_000_000:
            result.unpaired += 1
            continue
        result.compared += 1
        if abs(item.physical - step.total_after) <= max(0.01, abs(step.total_after) * 0.01):
            result.matched += 1
    for step in pending.values():
        stats[step.site].unpaired += 1
    return stats


def print_damage(label, verdict):
    ratio = f"{verdict.matched / verdict.total:.1%}" if verdict.total else "-"
    print(f"{label}: {verdict.answer} — {verdict.matched}/{verdict.total}줄 맞음({ratio}), 죽음으로 잘려 제외 {verdict.dead}줄")
    for sample in verdict.mismatches[:5]:
        print(f"  어긋남: 시각 {sample.t}, 자리 {sample.site}, 실제 감소 {sample.drop:.6f}, 기대 {sample.expected:.6f}")


def report(log):
    print(f"읽은 줄: STEP {len(log.step)}, MAG {len(log.magic)}, HIT {len(log.hits)}, EXP {len(log.exp)}, AMP {len(log.amp)}, 형식 오류로 건너뜀 {log.skipped}")
    print(f"LATE: 연결 {log.late_paired}, 짝 없음·중복 {log.late_unpaired}, 대상 소실로 체력 없음 {log.late_missing}")
    print(f"설정 Value={log.value:g}" + (" (설정 줄이 없어 기본값 사용)" if "Value" not in log.settings else ""))
    r1 = damage_verdict(log.magic, log.value)
    print_damage("R1 마법 피해", r1)
    if not r1.total:
        print("  판정 대상이 모두 죽음으로 잘림" if r1.dead else "  이 시험에서는 마법 피해를 받지 않음")
    r2, groups, omitted = resistance_verdict(log.magic)
    ratio = f"{r2.matched / r2.total:.1%}" if r2.total else "-"
    print(f"R2 저항이 크기에 포함됨: {r2.answer} — {r2.matched}/{r2.total}효과 맞음({ratio}), 유효 {sum(map(len, groups.values()))}줄, 저항 '-' 제외 {omitted}줄")
    if r2.total:
        print("  이 판정은 플레이어의 받는 주문 크기 퍽이 있으면 어긋남. 아래 AMP 기준을 볼 것")
    print("  효과 설정×주문별 유효 줄 모두가 5% 이내일 때 맞은 효과로 셈. 기대가 0이면 실제도 0이어야 함.")
    for (effect, spell), bad in sorted(r2.mismatches.items()):
        print(f"  어긋난 효과 {effect:08X} × 주문 {spell:08X}: {len(bad)}/{len(groups[(effect, spell)])}줄")
        for sample in bad[:3]:
            ratio = f"{abs(sample.magnitude) / sample.base_magnitude:.6f}" if sample.base_magnitude else "-"
            print(f"    시각 {sample.t}: |magnitude| {abs(sample.magnitude):.6f}, 기본×저항 {sample.base_magnitude * sample.resistance:.6f}"
                  f" (|magnitude|/기본 {ratio}, 저항 배율 {sample.resistance:.6f})")
    r2_amp = magnitude_resistance_verdict(log.amp)
    ratio = f"{r2_amp.matched / r2_amp.total:.1%}" if r2_amp.total else "-"
    print(f"R2 AMP 기준(엔진 저항이 크기에 곱해짐): {r2_amp.answer} — {r2_amp.matched}/{r2_amp.total}줄 맞음({ratio})")
    for sample, expected in r2_amp.mismatches[:3]:
        print(f"  어긋남: 시각 {sample.t}, 효과 설정 {sample.effect:08X}, |magBefore| {abs(sample.mag_before):.6f}, "
              f"eff {sample.eff:.6f}, 기대 {expected:.6f}, 실제 {abs(sample.mag_after):.6f}")
    for kind, samples in (("MAG", log.magic), ("HIT", log.hits)):
        verdict = damage_verdict(samples, log.value, scale=True)
        print_damage(f"R3 scale {kind}", verdict)
        print(f"  Value 적용 확인(주 판정과 별도): before×{log.value:g}=after {verdict.scale_matched}/{verdict.scale_total}줄")
    print("R4 자리별 HIT 통계(판정 없음):")
    stats = hit_statistics(log.hits)
    for site in sorted(set(HIT_SITES) | set(stats)):
        count, positive, ratios = stats.get(site, (0, 0.0, None))
        spread = "/".join(f"{v:.6f}" for v in ratios) if ratios else "-"
        positive_text = f"{positive:.1%}" if count else "-"
        print(f"  {site}: {count}줄, physicalDamage>0 {positive_text}, totalBefore/physical 최소·중앙·최대 {spread}")
    print("R5 시전 방식 × duration>0 MAG 통계(판정 없음, 집중 주문=2):")
    counts = magic_statistics(log.magic)
    if not counts:
        print("  MAG 줄 없음")
    for (casting, persistent), count in sorted(counts.items(), key=lambda item: (-1 if item[0][0] is None else item[0][0], item[0][1])):
        print(f"  시전 방식 {casting if casting is not None else '-'}, duration>0 {'예' if persistent else '아니오'}: {count}줄")
    print("마법 자리별 통계(판정 없음, 플레이어가 받은 피해 줄):")
    sites = magic_site_statistics(log.magic, log.value)
    if not sites:
        print("  MAG 줄 없음")
    for site, (count, first, judged, matched) in sorted(sites.items()):
        print(f"  {site}: {count}줄, 첫 호출(지난 시간 0) {first}줄, R1 {matched}/{judged}줄 맞음")
    print("R6 크기 분해(판정 없음):")
    steps = magnitude_step_statistics(log.amp, log.magic)
    if not steps:
        print("  AMP 줄 없음")
    for (effect, spell, caster), result in sorted(steps.items(), key=lambda item: (*item[0][:2], item[0][2] or 0)):
        caster_text = f"{caster:08X}" if caster is not None else "-"
        print(f"  효과 설정 {effect:08X} × 주문 {spell:08X} × 시전자 {caster_text}: {result.count}줄")
        for name, label in (("initial_ratio", "|magBefore|/효과 항목 크기"), ("eff", "eff"), ("r", "r"), ("power", "power"),
                            ("effectiveness_ratio", "|magAfter|/|magBefore|"), ("casterFromOne", "casterFromOne"), ("targetFromOne", "targetFromOne")):
            spread = result.ranges[name]
            text = "-" if spread is None else f"{spread[0]:.6f}"
            if spread is not None and spread[0] != spread[1]:
                text += f"~{spread[1]:.6f} (최소·최대)"
            print(f"    {label}: {text}")
        print(f"    예측 일치 {result.matched}/{result.compared}, 짝 없음 {result.unpaired}, 예측값 없음 {result.prediction_missing}")
    print("R7 폭발 시작 값(판정 없음):")
    if not log.exp:
        print("  EXP 줄 없음")
    else:
        result = explosion_step_statistics(log.exp, log.hits)
        ratio = "/".join(f"{v:.6f}" for v in result.ratio_range) if result.ratio_range else "-"
        predicted = "/".join(f"{v:.6f}" for v in result.predicted_range)
        print(f"  EXP {result.count}줄, ratio 최소·최대 {ratio}, ratio 없음 {result.ratio_missing}줄")
        print(f"  physical>0 {result.physical_positive:.1%}, predicted 최소·최대 {predicted}")
        print(f"  totalDamage 이어짐 {result.matched}/{result.compared}, 짝 없음 {result.unpaired}")
    print("R8 근접·화살의 physicalDamage(판정 없음):")
    if not log.step:
        print("  STEP 줄 없음")
    else:
        for site, result in hit_step_statistics(log.step, log.hits).items():
            ratio = "/".join(f"{v:.6f}" for v in result.ratio_range) if result.ratio_range else "-"
            print(f"  {site}: STEP {result.count}줄, ratio 최소·최대 {ratio}, ratio 없음 {result.ratio_missing}줄")
            print(f"    physicalDamage = 44016 뒤 값 {result.matched}/{result.compared}, 짝 없음 {result.unpaired}")


def selftest():
    """외부 파일 없이 합성한 실제 로그 문자열로 파싱부터 판정까지 검사한다."""
    from contextlib import redirect_stdout
    from io import StringIO

    prefix = "2026-11-17 23:00:00.001\tinfo\t"

    def row(kind, fields):
        return prefix + kind + "\t" + "\t".join(map(str, fields))

    def magic(t=1, **changes):
        names = "t thread mode modified site target caster spell spell_type casting effect resist_variable flags duration elapsed magnitude base resistance before after hp0 hp1 max_health mult".split()
        values = [t, 7, "observe", 0, "ValueModifierEffect", "00000014", "00000001", "00000002", 0, 2,
                  "00000003", 41, "0", 1, 0, 5, 10, 0.5, -10, -10, 100, 80, 100, 2]
        d = dict(zip(names, values))
        d.update(changes)
        return row("MAG", [d[name] for name in names])

    def hit(t=2, **changes):
        names = "t thread site mode modified aggressor target weapon source_ref source_type flags before after physical blocked hp0 hp1 max_health mult".split()
        values = [t, 8, "explosion", "scale", 1, "00000001", "00000014", "00000000", "00000000", "-", "0", 20, 10, 20, 0, 100, 80, 100, 2]
        d = dict(zip(names, values))
        d.update(changes)
        return row("HIT", [d[name] for name in names])

    def amp(t=1, **changes):
        names = "t thread target caster spell spell_type casting effect base mag_before eff resistance power mag_after dur_before dur_after caster_from_one target_from_one predicted".split()
        values = [t, 7, "00000014", "00000001", "00000002", 0, 2, "00000003",
                  10, -10, 0.5, 0.5, 1, -5, 1, 1, 2, 0.9, -9]
        d = dict(zip(names, values))
        d.update(changes)
        return row("AMP", [d[name] for name in names])

    def log(*lines, value=0.5):
        return read_log("\n".join([prefix + f"설정: ModeKey=0x3C StartMode=observe Value={value:.6f} LogAll=false", *lines]))

    def exp(t=1, **changes):
        names = "t thread aggressor target weapon source_ref total_before total_after ratio physical blocked sneak flags predicted".split()
        # HIT 도우미와 같은 스레드(8)라 기본값끼리 짝지어진다.
        values = [t, 8, "00000001", "00000014", "00000002", "00000003", 20, 18, 0.9, 16, 0.25, 1, "100000", 10.8]
        d = dict(zip(names, values))
        d.update(changes)
        return row("EXP", [d[name] for name in names])

    def step(t=1, **changes):
        names = "t thread site aggressor target weapon source_ref total_before total_after ratio sneak flags".split()
        # HIT 도우미와 같은 스레드(8)라 기본값끼리 짝지어진다.
        values = [t, 8, "populate", "00000001", "00000014", "00000002", "00000003", 20, 18, 0.9, 1, "100008"]
        d = dict(zip(names, values))
        d.update(changes)
        return row("STEP", [d[name] for name in names])

    class Tests(unittest.TestCase):
        def test_r1_yes_no_empty(self):
            for lines, answer in (([magic()], YES), ([magic(hp1=70)], NO), ([], EMPTY), ([magic(hp1=100)], EMPTY)):
                with self.subTest(answer=answer, lines=len(lines)):
                    data = log(*lines)
                    self.assertEqual(damage_verdict(data.magic, data.value).answer, answer)

        def test_r1_threshold_and_tolerance(self):
            data = log(*(magic(t=i, hp1=80 if i < 9 else 70) for i in range(10)))
            self.assertEqual(damage_verdict(data.magic, data.value).answer, YES)
            data = log(*(magic(t=i, hp1=80 if i < 8 else 70) for i in range(10)))
            self.assertEqual(damage_verdict(data.magic, data.value).answer, NO)
            self.assertTrue(close_damage(0.049, 0))
            self.assertFalse(close_damage(0.051, 0))
            self.assertTrue(close_damage(100.9, 100))
            self.assertFalse(close_damage(101.1, 100))

        def test_r2_yes_no_empty(self):
            for lines, answer in (([magic()], YES), ([magic(magnitude=8)], NO), ([], EMPTY), ([magic(resistance="-")], EMPTY)):
                with self.subTest(answer=answer):
                    verdict, _, _ = resistance_verdict(log(*lines).magic)
                    self.assertEqual(verdict.answer, answer)

        def test_r2_distinct_effects_and_threshold(self):
            rows = [magic(t=i, effect=f"{i + 1:08X}", magnitude=5 if i < 4 else 8) for i in range(5)]
            rows += [magic(t=100 + i, effect="00000005", magnitude=8) for i in range(10)]
            verdict, groups, _ = resistance_verdict(log(*rows).magic)
            self.assertEqual((verdict.total, verdict.matched, verdict.answer), (5, 4, YES))
            self.assertEqual(len(groups[(5, 2)]), 11)
            rows += [magic(t=500, effect="00000006", magnitude=8)]
            self.assertEqual(resistance_verdict(log(*rows).magic)[0].answer, NO)

        def test_r2_negative_magnitude(self):
            # 깎는 효과는 magnitude가 음수로 기록된다. 크기가 맞으면 맞은 것으로 센다.
            for magnitude, answer in ((-5, YES), (-8, NO)):
                with self.subTest(magnitude=magnitude):
                    self.assertEqual(resistance_verdict(log(magic(magnitude=magnitude)).magic)[0].answer, answer)

        def test_magic_site_statistics(self):
            data = log(magic(t=1, site="ValueModifierEffect", elapsed=0),
                       magic(t=2, site="ValueModifierEffect", elapsed=0.5, hp1=70),
                       magic(t=3, site="DualValueModifierEffect+0x28", elapsed=0),
                       magic(t=4, site="DualValueModifierEffect+0x5C", before=5),
                       magic(t=5, site="DualValueModifierEffect+0x28", target="00000099"))
            self.assertEqual(magic_site_statistics(data.magic, data.value),
                             {"ValueModifierEffect": (2, 1, 2, 1), "DualValueModifierEffect+0x28": (1, 1, 1, 1)})
            self.assertEqual(magic_site_statistics(log().magic, 0.5), {})

        def test_r2_zero_and_optional(self):
            self.assertTrue(close_resistance(0, 0))
            self.assertFalse(close_resistance(0.000001, 0))
            self.assertTrue(close_resistance(10.49, 10))
            self.assertFalse(close_resistance(10.51, 10))
            verdict, groups, omitted = resistance_verdict(log(magic(), magic(t=2, resistance="-")).magic)
            self.assertEqual((verdict.answer, len(groups), omitted), (YES, 1, 1))

        def test_r2_amp_effectiveness(self):
            for before, eff, after, matched in ((-10, 0.5, -5.049, 1), (-10, 0.5, -5.051, 0),
                                                 (0.5, 1, -0.509, 1), (0.5, 1, 1, 0),
                                                 (0.5, 0.5, -1.009, 1), (0.5, 0.5, 1.011, 0)):
                with self.subTest(before=before, eff=eff, after=after):
                    data = log(amp(mag_before=before, eff=eff, mag_after=after))
                    verdict = magnitude_resistance_verdict(data.amp)
                    self.assertEqual((verdict.total, verdict.matched, verdict.answer), (1, matched, YES if matched else NO))

        def test_r2_amp_excludes_missing_resistance_and_zero_magnitude(self):
            excluded = [amp(resistance="-", power="-"), amp(t=2, mag_before=0)]
            verdict = magnitude_resistance_verdict(log(*excluded).amp)
            self.assertEqual((verdict.total, verdict.matched, verdict.answer), (0, 0, EMPTY))
            data = log(*excluded, amp(t=3, resistance=0, power="-", eff=0, mag_after=-1))
            verdict = magnitude_resistance_verdict(data.amp)
            self.assertEqual((verdict.total, verdict.matched, verdict.answer), (1, 1, YES))

        def test_r2_amp_threshold(self):
            for matched, answer in ((9, YES), (8, NO)):
                with self.subTest(matched=matched):
                    data = log(*(amp(t=i, mag_after=-5 if i < matched else -8) for i in range(10)))
                    verdict = magnitude_resistance_verdict(data.amp)
                    self.assertEqual((verdict.total, verdict.matched, verdict.answer), (10, matched, answer))

        def test_r2_amp_report(self):
            guide = "  이 판정은 플레이어의 받는 주문 크기 퍽이 있으면 어긋남. 아래 AMP 기준을 볼 것"
            label = "R2 AMP 기준(엔진 저항이 크기에 곱해짐): "
            for data, has_guide, outcome in ((log(magic(), amp()), True, "그렇다 — 1/1줄 맞음(100.0%)"),
                                             (log(magic(magnitude=-4)), True, "판정할 줄 없음 — 0/0줄 맞음(-)"),
                                             (log(magic(resistance="-"), amp()), False, "그렇다 — 1/1줄 맞음(100.0%)"),
                                             (log(), False, "판정할 줄 없음 — 0/0줄 맞음(-)")):
                with self.subTest(has_guide=has_guide, outcome=outcome):
                    out = StringIO()
                    with redirect_stdout(out):
                        report(data)
                    lines = out.getvalue().splitlines()
                    self.assertIn(label + outcome, lines)
                    self.assertEqual(guide in lines, has_guide)
                    if has_guide:
                        mag_index = next(i for i, line in enumerate(lines) if line.startswith("R2 저항이 크기에 포함됨:"))
                        self.assertEqual(lines[mag_index + 1], guide)
            out = StringIO()
            with redirect_stdout(out):
                report(log(*(amp(t=i, mag_after=-8) for i in range(1, 5))))
            lines = out.getvalue().splitlines()
            self.assertIn(label + "아니다 — 0/4줄 맞음(0.0%)", lines)
            self.assertEqual([line for line in lines if line.startswith("  어긋남:")], [
                f"  어긋남: 시각 {i}, 효과 설정 00000003, |magBefore| 10.000000, eff 0.500000, 기대 5.000000, 실제 8.000000"
                for i in range(1, 4)
            ])

        def test_r3_yes_no_empty_for_magic_and_hit(self):
            for kind in ("MAG", "HIT"):
                for hp1, answer in ((80, YES), (70, NO)):
                    line = magic(mode="scale", modified=1, before=-20, hp1=hp1) if kind == "MAG" else hit(hp1=hp1)
                    data = log(line)
                    samples = data.magic if kind == "MAG" else data.hits
                    with self.subTest(kind=kind, answer=answer):
                        self.assertEqual(damage_verdict(samples, data.value, scale=True).answer, answer)
                self.assertEqual(damage_verdict([], 0.5, scale=True).answer, EMPTY)
            data = log(magic(mode="observe", modified=0), hit(mode="observe", modified=0))
            self.assertEqual(damage_verdict(data.magic, data.value, scale=True).answer, EMPTY)
            self.assertEqual(damage_verdict(data.hits, data.value, scale=True).answer, EMPTY)

        def test_r3_value_and_zero(self):
            data = log(magic(mode="scale", modified=1, before=-40), value=0.25)
            result = damage_verdict(data.magic, data.value, scale=True)
            self.assertEqual((data.value, result.scale_total, result.scale_matched), (0.25, 1, 1))
            data = log(hit(after=0, hp1=100), value=0)
            result = damage_verdict(data.hits, data.value, scale=True)
            self.assertEqual((result.answer, result.scale_matched), (YES, 1))
            data = log(hit(), value=0.25)
            result = damage_verdict(data.hits, data.value, scale=True)
            self.assertEqual((result.answer, result.scale_matched), (YES, 0))

        def test_r3_threshold(self):
            for matched, answer in ((9, YES), (8, NO)):
                for kind in ("MAG", "HIT"):
                    rows = [magic(t=i, mode="scale", modified=1, hp1=80 if i < matched else 70)
                            if kind == "MAG" else hit(t=i, hp1=80 if i < matched else 70)
                            for i in range(10)]
                    data = log(*rows)
                    samples = data.magic if kind == "MAG" else data.hits
                    with self.subTest(kind=kind, matched=matched):
                        self.assertEqual(damage_verdict(samples, data.value, scale=True).answer, answer)

        def test_late_pairing_and_out_of_order(self):
            data = log(row("LATE", [300001, "00000014", 80, 1]), magic(hp1=100),
                       hit(t=3, hp1=100), row("LATE", [300003, "00000014", 80, 3]),
                       row("LATE", [300004, "00000099", 0, 1]))
            self.assertEqual((data.late_paired, data.late_unpaired), (2, 1))
            self.assertEqual(damage_verdict(data.magic, data.value).answer, YES)
            self.assertEqual(damage_verdict(data.hits, data.value, scale=True).answer, YES)

        def test_late_does_not_replace_immediate_drop(self):
            data = log(magic(), row("LATE", [300001, "00000014", 0, 1]),
                       row("LATE", [300002, "00000014", "-", 2]))
            self.assertEqual((data.magic[0].end_health, data.late_missing), (80, 1))

        def test_death_exclusion_immediate_and_late(self):
            data = log(magic(t=1, hp1=0), magic(t=2, hp1=100),
                       row("LATE", [300002, "00000014", -1, 2]),
                       magic(t=3, mode="scale", modified=1, hp1=-1), hit(t=4, hp1=0))
            r1 = damage_verdict(data.magic, data.value)
            r3m = damage_verdict(data.magic, data.value, scale=True)
            r3h = damage_verdict(data.hits, data.value, scale=True)
            self.assertEqual((r1.total, r1.dead, r1.answer), (0, 3, EMPTY))
            self.assertEqual((r3m.total, r3m.dead, r3m.answer), (0, 1, EMPTY))
            self.assertEqual((r3h.total, r3h.dead, r3h.answer), (0, 1, EMPTY))

        def test_malformed_lines_and_null_fields(self):
            data = log("머리 없는 줄", row("MAG", [1]), magic(t="bad"), magic(magnitude="nan"),
                       hit(after="inf"), row("MODE", [1, "bad"]),
                       magic(t=8, spell="00000000", spell_type="-", casting="-", effect="00000000", resist_variable="-", resistance="-"),
                       prefix + "런타임 1.6.1170.0")
            self.assertEqual((data.skipped, len(data.magic), len(data.hits)), (6, 1, 0))
            self.assertIsNone(data.magic[0].casting)
            self.assertEqual(magic_statistics(data.magic), Counter({(None, True): 1}))

        def test_player_filter_and_mixed_effect_result(self):
            data = log(magic(target="00000099", hp1=70), hit(target="00000099", hp1=70))
            self.assertEqual(damage_verdict(data.magic, data.value).answer, EMPTY)
            self.assertEqual(damage_verdict(data.hits, data.value, scale=True).answer, EMPTY)
            verdict, _, _ = resistance_verdict(log(magic(), magic(t=2, magnitude=8)).magic)
            self.assertEqual((verdict.total, verdict.matched, verdict.answer), (1, 0, NO))

        def test_r4_statistics(self):
            data = log(hit(t=1, before=2, physical=2), hit(t=2, before=12, physical=4),
                       hit(t=3, before=20, physical=4), hit(t=4, before=100, physical=0),
                       hit(t=5, site="trap", before=10, physical=-1))
            self.assertEqual(hit_statistics(data.hits), {"explosion": (4, 0.75, (1, 3, 5)), "trap": (1, 0, None)})
            self.assertEqual(hit_statistics(log().hits), {})

        def test_r5_statistics(self):
            data = log(magic(t=1, casting=2, duration=0), magic(t=2, casting=2, duration=3),
                       magic(t=3, casting=2, duration=4), magic(t=4, casting=1, duration=0),
                       magic(t=5, casting="-", duration=0))
            self.assertEqual(magic_statistics(data.magic), Counter({(2, False): 1, (2, True): 2, (1, False): 1, (None, False): 1}))

        def test_amp_parsing(self):
            data = log(amp())
            self.assertEqual((len(data.amp), data.skipped), (1, 0))
            self.assertEqual(data.amp[0], MagnitudeStep(1, 7, PLAYER, 1, 2, 0, 2, 3,
                                                      10, -10, 0.5, 0.5, 1, -5, 1, 1, 2, 0.9, -9))

        def test_amp_optional_fields(self):
            data = log(amp(caster="-", resistance="-", power="-", caster_from_one="-", target_from_one="-", predicted="-"),
                       amp(t=2, resistance=0, power="-"))
            sample = data.amp[0]
            self.assertEqual((data.skipped, len(data.amp)), (0, 2))
            self.assertEqual((sample.caster, sample.resistance, sample.power, sample.caster_from_one,
                              sample.target_from_one, sample.predicted), (None,) * 6)
            self.assertEqual(data.amp[1].resistance, 0)
            self.assertIsNone(data.amp[1].power)

        def test_amp_malformed_fields(self):
            fields = amp().split("\t")[3:]
            data = log(row("AMP", fields[:-1]), row("AMP", fields + [0]), amp(eff="nan"),
                       amp(mag_after="-"), amp(t=-1), amp(caster="invalid"), amp(predicted="inf"), amp())
            self.assertEqual((data.skipped, len(data.amp)), (7, 1))

        def test_r6_group_ranges(self):
            data = log(amp(), amp(t=2, base=20, mag_before=-5, eff=1, resistance=1, mag_after=-10,
                                  caster_from_one=3, target_from_one=1),
                       amp(t=3, effect="00000004"), amp(t=4, spell="00000005"), amp(t=5, caster="00000006"))
            stats = magnitude_step_statistics(data.amp, data.magic)
            self.assertEqual(set(stats), {(3, 2, 1), (4, 2, 1), (3, 5, 1), (3, 2, 6)})
            self.assertEqual(stats[(3, 2, 1)].count, 2)
            self.assertEqual(stats[(3, 2, 1)].ranges,
                             {"initial_ratio": (0.25, 1), "eff": (0.5, 1), "r": (0.5, 1), "power": (1, 1),
                              "effectiveness_ratio": (0.5, 2), "casterFromOne": (2, 3), "targetFromOne": (0.9, 1)})

        def test_r6_zero_denominators_and_missing_values(self):
            data = log(amp(base=0, mag_before=0, resistance="-", power="-", caster_from_one="-", target_from_one="-"))
            result = magnitude_step_statistics(data.amp, data.magic)[(3, 2, 1)]
            self.assertEqual(result.ranges,
                             {"initial_ratio": None, "eff": (0.5, 0.5), "r": None, "power": None,
                              "effectiveness_ratio": None, "casterFromOne": None, "targetFromOne": None})
            self.assertEqual(magnitude_step_statistics([], data.magic), {})

        def test_amp_mag_pairing_time_window(self):
            for offset, paired in ((-1, False), (0, True), (1, True), (1_999_999, True), (2_000_000, True), (2_000_001, False)):
                with self.subTest(offset=offset):
                    data = log(amp(t=10), magic(t=10 + offset, magnitude=-9))
                    result = magnitude_step_statistics(data.amp, data.magic)[(3, 2, 1)]
                    self.assertEqual((result.matched, result.compared, result.unpaired),
                                     (1, 1, 0) if paired else (0, 0, 1))

        def test_amp_mag_pairing_identity_and_elapsed(self):
            for changes in ({"elapsed": 0.1}, {"target": "00000099"}, {"caster": "00000099"},
                            {"spell": "00000099"}, {"effect": "00000099"}):
                with self.subTest(changes=changes):
                    data = log(amp(), magic(t=2, magnitude=-9, **changes))
                    result = magnitude_step_statistics(data.amp, data.magic)[(3, 2, 1)]
                    self.assertEqual((result.compared, result.unpaired), (0, 1))

        def test_amp_mag_pairing_first_by_time(self):
            # 로그 줄 순서나 예측과 맞는지에 끌리지 않고, AMP 뒤 첫 호출만 고른다.
            data = log(magic(t=30, magnitude=-9), magic(t=9, magnitude=-9), amp(t=10),
                       magic(t=11, magnitude=-20, elapsed=0.1), magic(t=20, magnitude=-8))
            result = magnitude_step_statistics(data.amp, data.magic)[(3, 2, 1)]
            self.assertEqual((result.matched, result.compared, result.unpaired), (0, 1, 0))

        def test_amp_prediction_counts(self):
            data = log(amp(), magic(t=2, magnitude=-9),
                       amp(t=3_000_001), magic(t=3_000_002, magnitude=-8), amp(t=6_000_001))
            result = magnitude_step_statistics(data.amp, data.magic)[(3, 2, 1)]
            self.assertEqual((result.count, result.matched, result.compared, result.unpaired), (3, 1, 2, 1))

        def test_amp_prediction_tolerance(self):
            for predicted, actual, matched in ((-100, -100.9, 1), (-100, -101.1, 0),
                                               (0, 0.009, 1), (0, 0.011, 0), (9, -9, 1), (9, -9.2, 0)):
                with self.subTest(predicted=predicted, actual=actual):
                    data = log(amp(predicted=predicted), magic(t=2, magnitude=actual))
                    result = magnitude_step_statistics(data.amp, data.magic)[(3, 2, 1)]
                    self.assertEqual((result.matched, result.compared), (matched, 1))

        def test_amp_prediction_missing_and_no_caster(self):
            data = log(amp(caster="-", predicted="-"), magic(t=2, caster="00000000"))
            result = magnitude_step_statistics(data.amp, data.magic)[(3, 2, None)]
            self.assertEqual((result.matched, result.compared, result.unpaired, result.prediction_missing), (0, 0, 0, 1))

        def test_r6_target_perk_without_caster(self):
            data = log(amp(caster="-", caster_from_one="-", target_from_one=0.9, predicted="-"),
                       magic(t=2, caster="00000000"))
            result = magnitude_step_statistics(data.amp, data.magic)[(3, 2, None)]
            self.assertIsNone(result.ranges["casterFromOne"])
            self.assertEqual(result.ranges["targetFromOne"], (0.9, 0.9))
            self.assertEqual((result.compared, result.unpaired, result.prediction_missing), (0, 0, 1))

        def test_r6_report(self):
            out = StringIO()
            with redirect_stdout(out):
                report(log())
            self.assertIn("R6 크기 분해(판정 없음):\n  AMP 줄 없음", out.getvalue())
            out = StringIO()
            with redirect_stdout(out):
                report(log(amp(), amp(t=2, eff=0.2), magic(t=3, magnitude=-9)))
            text = out.getvalue()
            self.assertIn("AMP 2, 형식 오류로 건너뜀 0", text)
            self.assertIn("효과 설정 00000003 × 주문 00000002 × 시전자 00000001: 2줄", text)
            self.assertIn("eff: 0.200000~0.500000 (최소·최대)", text)
            self.assertIn("예측 일치 2/2, 짝 없음 0", text)

        def test_exp_parsing(self):
            data = log(exp())
            self.assertEqual((len(data.exp), data.skipped), (1, 0))
            self.assertEqual(data.exp[0], ExplosionStep(1, 8, 1, PLAYER, 2, 3, 20, 18, 0.9, 16, 0.25, 1, 0x100000, 10.8))

        def test_exp_missing_ratio_and_null_forms(self):
            data = log(exp(total_before=0, total_after=0, ratio="-", aggressor="00000000", weapon="00000000",
                           source_ref="00000000", physical=4, blocked=0.5, predicted=2))
            self.assertEqual((len(data.exp), data.skipped), (1, 0))
            sample = data.exp[0]
            self.assertIsNone(sample.ratio)
            self.assertEqual((sample.aggressor, sample.weapon, sample.source_ref, sample.predicted), (0, 0, 0, 2))

        def test_exp_malformed_fields(self):
            fields = exp().split("\t")[3:]
            data = log(row("EXP", fields[:-1]), row("EXP", fields + [0]), exp(t=-1), exp(thread=-1),
                       exp(aggressor="-"), exp(target="invalid"), exp(weapon="-1"), exp(source_ref="invalid"),
                       exp(flags="invalid"), exp(total_after="-"), exp())
            self.assertEqual((data.skipped, len(data.exp)), (10, 1))

        def test_exp_nonfinite_fields(self):
            for name in ("total_before", "total_after", "ratio", "physical", "blocked", "sneak", "predicted"):
                for value in ("nan", "inf", "-inf"):
                    with self.subTest(name=name, value=value):
                        data = log(exp(**{name: value}), exp(t=2))
                        self.assertEqual((data.skipped, len(data.exp)), (1, 1))

        def test_r7_statistics(self):
            data = log(exp(), exp(t=2, ratio=0.5, physical=0, predicted=2),
                       exp(t=3, total_before=0, total_after=0, ratio="-", physical=-1, predicted=0))
            result = explosion_step_statistics(data.exp, data.hits)
            self.assertEqual((result.count, result.ratio_range, result.ratio_missing, result.physical_positive,
                              result.predicted_range), (3, (0.5, 0.9), 1, 1 / 3, (0, 10.8)))
            self.assertEqual((result.matched, result.compared, result.unpaired), (0, 0, 3))
            empty = explosion_step_statistics([], data.hits)
            self.assertEqual((empty.count, empty.ratio_range, empty.ratio_missing, empty.physical_positive,
                              empty.predicted_range, empty.unpaired), (0, None, 0, 0, None, 0))

        def test_exp_hit_pairing_time_window(self):
            for offset, paired in ((-1, False), (0, True), (1, True), (999_999, True), (1_000_000, True), (1_000_001, False)):
                with self.subTest(offset=offset):
                    data = log(exp(t=10), hit(t=10 + offset, before=18))
                    result = explosion_step_statistics(data.exp, data.hits)
                    self.assertEqual((result.matched, result.compared, result.unpaired),
                                     (1, 1, 0) if paired else (0, 0, 1))

        def test_exp_hit_pairing_site_and_target(self):
            for changes in ({"site": "beam"}, {"target": "00000099"}):
                with self.subTest(changes=changes):
                    data = log(exp(), hit(before=18, **changes))
                    result = explosion_step_statistics(data.exp, data.hits)
                    self.assertEqual((result.compared, result.unpaired), (0, 1))
            # 같은 스레드라면 모드와 플레이어 여부는 짝짓기의 조건이 아니다.
            data = log(exp(target="00000099"), hit(before=18, target="00000099", mode="observe", modified=0))
            result = explosion_step_statistics(data.exp, data.hits)
            self.assertEqual((result.matched, result.compared, result.unpaired), (1, 1, 0))

        def test_exp_hit_pairing_different_thread(self):
            data = log(exp(), hit(before=18, thread=99))
            result = explosion_step_statistics(data.exp, data.hits)
            self.assertEqual((result.matched, result.compared, result.unpaired), (0, 0, 1))

        def test_exp_hit_pairing_same_thread_latest(self):
            data = log(exp(t=1, thread=3, total_after=99), exp(t=2, total_after=50), exp(t=3),
                       hit(t=4, before=18), hit(t=5, before=50), hit(t=6, thread=3, before=99))
            result = explosion_step_statistics(data.exp, data.hits)
            self.assertEqual((result.count, result.matched, result.compared, result.unpaired), (3, 2, 2, 1))

        def test_exp_hit_pairing_first_by_time(self):
            # 로그 순서와 값의 일치 여부에 끌리지 않고 EXP 뒤 시각순 첫 explosion만 고른다.
            data = log(hit(t=30, before=18), hit(t=9, before=18), exp(t=10),
                       hit(t=11, site="beam", before=18), hit(t=12, target="00000099", before=18), hit(t=20, before=17))
            result = explosion_step_statistics(data.exp, data.hits)
            self.assertEqual((result.matched, result.compared, result.unpaired), (0, 1, 0))

        def test_exp_continuity_counts(self):
            data = log(exp(), hit(before=18), exp(t=2_000_001), hit(t=2_000_002, before=20), exp(t=4_000_001))
            result = explosion_step_statistics(data.exp, data.hits)
            self.assertEqual((result.count, result.matched, result.compared, result.unpaired), (3, 1, 2, 1))

        def test_exp_continuity_tolerance(self):
            for after, actual, matched in ((100, 100.9, 1), (100, 101.1, 0), (0, 0.009, 1), (0, 0.011, 0),
                                           (-100, -100.9, 1), (-100, -101.1, 0), (18, -18, 0)):
                with self.subTest(after=after, actual=actual):
                    data = log(exp(total_after=after), hit(before=actual))
                    result = explosion_step_statistics(data.exp, data.hits)
                    self.assertEqual((result.matched, result.compared), (matched, 1))

        def test_r7_report(self):
            out = StringIO()
            with redirect_stdout(out):
                report(log())
            self.assertIn("R7 폭발 시작 값(판정 없음):\n  EXP 줄 없음", out.getvalue())
            out = StringIO()
            with redirect_stdout(out):
                report(log(exp(), exp(t=2_000_001, total_before=0, ratio="-", physical=0, predicted=0),
                           exp(t=4_000_001, ratio=0.5, predicted=4), hit(before=18), hit(t=4_000_002, before=20)))
            text = out.getvalue()
            self.assertIn("EXP 3, AMP 0, 형식 오류로 건너뜀 0", text)
            self.assertIn("EXP 3줄, ratio 최소·최대 0.500000/0.900000, ratio 없음 1줄", text)
            self.assertIn("physical>0 66.7%, predicted 최소·최대 0.000000/10.800000", text)
            self.assertIn("totalDamage 이어짐 1/2, 짝 없음 1", text)
            out = StringIO()
            with redirect_stdout(out):
                report(log(exp(total_before=0, ratio="-")))
            self.assertIn("ratio 최소·최대 -, ratio 없음 1줄", out.getvalue())

        def test_step_parsing(self):
            for site in STEP_HIT_SITES:
                with self.subTest(site=site):
                    data = log(step(site=site))
                    self.assertEqual((len(data.step), data.skipped), (1, 0))
                    self.assertEqual(data.step[0], HitStep(1, 8, site, 1, PLAYER, 2, 3, 20, 18, 0.9, 1, 0x100008))

        def test_step_missing_ratio_and_null_forms(self):
            data = log(step(total_before=0, total_after=0, ratio="-", aggressor="00000000",
                            weapon="00000000", source_ref="00000000"))
            self.assertEqual((len(data.step), data.skipped), (1, 0))
            sample = data.step[0]
            self.assertIsNone(sample.ratio)
            self.assertEqual((sample.aggressor, sample.weapon, sample.source_ref), (0, 0, 0))

        def test_step_malformed_fields(self):
            fields = step().split("\t")[3:]
            data = log(row("STEP", fields[:-1]), row("STEP", fields + [0]), step(t=-1), step(thread=-1),
                       step(site="melee"), step(site=""), step(site="Arrow"), step(aggressor="-"),
                       step(target="invalid"), step(weapon="-1"), step(source_ref="invalid"),
                       step(flags="invalid"), step(total_after="-"), step())
            self.assertEqual((data.skipped, len(data.step)), (13, 1))

        def test_step_nonfinite_fields(self):
            for name in ("total_before", "total_after", "ratio", "sneak"):
                for value in ("nan", "inf", "-inf"):
                    with self.subTest(name=name, value=value):
                        data = log(step(**{name: value}), step(t=2))
                        self.assertEqual((data.skipped, len(data.step)), (1, 1))

        def test_r8_statistics_by_site(self):
            data = log(step(), step(t=2, ratio=0.5), step(t=3, total_before=0, ratio="-"),
                       step(t=4, site="arrow", ratio=2), step(t=5, site="arrow", ratio=1))
            stats = hit_step_statistics(data.step, data.hits)
            self.assertEqual(stats["populate"], HitStepStatistics(3, (0.5, 0.9), 1, unpaired=3))
            self.assertEqual(stats["arrow"], HitStepStatistics(2, (1, 2), 0, unpaired=2))
            for result in hit_step_statistics([], data.hits).values():
                self.assertEqual(result, HitStepStatistics(0, None, 0))
            missing = hit_step_statistics(log(step(ratio="-")).step, [])["populate"]
            self.assertEqual(missing, HitStepStatistics(1, None, 1, unpaired=1))

        def test_step_hit_pairing_time_window(self):
            for site in STEP_HIT_SITES:
                for offset, paired in ((-1, False), (0, True), (1, True), (999_999, True), (1_000_000, True), (1_000_001, False)):
                    with self.subTest(site=site, offset=offset):
                        data = log(step(t=10, site=site), hit(t=10 + offset, site="melee", physical=18))
                        result = hit_step_statistics(data.step, data.hits)[site]
                        self.assertEqual((result.matched, result.compared, result.unpaired),
                                         (1, 1, 0) if paired else (0, 0, 1))

        def test_step_hit_pairing_site_and_target(self):
            for site, allowed in STEP_HIT_SITES.items():
                for hit_site in HIT_SITES:
                    with self.subTest(site=site, hit_site=hit_site):
                        data = log(step(site=site), hit(site=hit_site, physical=18))
                        result = hit_step_statistics(data.step, data.hits)[site]
                        self.assertEqual((result.matched, result.compared, result.unpaired),
                                         (1, 1, 0) if hit_site in allowed else (0, 0, 1))
                data = log(step(site=site), hit(site="melee", physical=18, target="00000099"))
                result = hit_step_statistics(data.step, data.hits)[site]
                self.assertEqual((result.compared, result.unpaired), (0, 1))
                # 다른 액터도 LogAll로 기록된다. 모드는 조건이 아니지만 스레드는 같아야 짝이다.
                data = log(step(site=site, target="00000099"),
                           hit(site="melee", physical=18, target="00000099", mode="observe", modified=0))
                result = hit_step_statistics(data.step, data.hits)[site]
                self.assertEqual((result.matched, result.compared, result.unpaired), (1, 1, 0))
                data = log(step(site=site), hit(site="melee", physical=18, thread=99))
                result = hit_step_statistics(data.step, data.hits)[site]
                self.assertEqual((result.compared, result.unpaired), (0, 1))

        def test_step_hit_pairing_same_thread_latest(self):
            # 다른 스레드의 STEP은 이 스레드의 HIT와 짝짓지 않고, 같은 스레드에서 뒤에 온 STEP이 앞의 것을 밀어낸다.
            data = log(step(t=1, thread=3, total_after=99), step(t=2, total_after=50), step(t=3),
                       hit(t=4, site="melee", physical=18))
            result = hit_step_statistics(data.step, data.hits)["populate"]
            self.assertEqual((result.count, result.matched, result.compared, result.unpaired), (3, 1, 1, 2))

        def test_step_hit_pairing_first_by_time(self):
            for site in STEP_HIT_SITES:
                with self.subTest(site=site):
                    data = log(hit(t=30, site="melee", physical=18), hit(t=9, site="melee", physical=18), step(t=10, site=site),
                               hit(t=11, site="explosion", physical=18), hit(t=12, site="melee", target="00000099", physical=18),
                               hit(t=20, site="melee", physical=17))
                    result = hit_step_statistics(data.step, data.hits)[site]
                    self.assertEqual((result.matched, result.compared, result.unpaired), (0, 1, 0))
            # arrow는 허용된 세 HIT 자리 전체에서 시각순 첫 줄을 고른다.
            data = log(step(site="arrow"), hit(t=4, site="melee", physical=18),
                       hit(t=3, site="nonactor_arrow", physical=18), hit(t=2, site="beam", physical=17))
            result = hit_step_statistics(data.step, data.hits)["arrow"]
            self.assertEqual((result.matched, result.compared, result.unpaired), (0, 1, 0))

        def test_step_physical_counts(self):
            for site in STEP_HIT_SITES:
                with self.subTest(site=site):
                    data = log(step(site=site), hit(site="melee", physical=18, before=999),
                               step(t=2_000_001, site=site), hit(t=2_000_002, site="melee", physical=20, before=18),
                               step(t=4_000_001, site=site))
                    result = hit_step_statistics(data.step, data.hits)[site]
                    self.assertEqual((result.count, result.matched, result.compared, result.unpaired), (3, 1, 2, 1))

        def test_step_physical_tolerance(self):
            for after, actual, matched in ((100, 100.9, 1), (100, 101.1, 0), (0, 0.009, 1), (0, 0.011, 0),
                                           (-100, -100.9, 1), (-100, -101.1, 0), (18, -18, 0)):
                with self.subTest(after=after, actual=actual):
                    data = log(step(total_after=after), hit(site="melee", physical=actual))
                    result = hit_step_statistics(data.step, data.hits)["populate"]
                    self.assertEqual((result.matched, result.compared), (matched, 1))

        def test_r8_report(self):
            out = StringIO()
            with redirect_stdout(out):
                report(log())
            self.assertIn("R8 근접·화살의 physicalDamage(판정 없음):\n  STEP 줄 없음", out.getvalue())
            out = StringIO()
            with redirect_stdout(out):
                report(log(step(), step(t=2_000_001, ratio="-"), step(t=4_000_001, ratio=0.5),
                           step(t=6_000_001, site="arrow", ratio=2), hit(site="melee", physical=18),
                           hit(t=4_000_002, site="melee", physical=20), hit(t=6_000_002, site="beam", physical=18)))
            text = out.getvalue()
            self.assertIn("읽은 줄: STEP 4, MAG 0, HIT 3", text)
            self.assertIn("populate: STEP 3줄, ratio 최소·최대 0.500000/0.900000, ratio 없음 1줄", text)
            self.assertIn("physicalDamage = 44016 뒤 값 1/2, 짝 없음 1", text)
            self.assertIn("arrow: STEP 1줄, ratio 최소·최대 2.000000/2.000000, ratio 없음 0줄", text)
            self.assertIn("physicalDamage = 44016 뒤 값 1/1, 짝 없음 0", text)
            out = StringIO()
            with redirect_stdout(out):
                report(log(step(ratio="-")))
            self.assertIn("populate: STEP 1줄, ratio 최소·최대 -, ratio 없음 1줄", out.getvalue())
            self.assertIn("arrow: STEP 0줄, ratio 최소·최대 -, ratio 없음 0줄", out.getvalue())

        def test_r4_melee_statistics_and_report(self):
            data = log(hit(site="melee", before=18, physical=18))
            self.assertEqual(hit_statistics(data.hits)["melee"], (1, 1.0, (1, 1, 1)))
            for lines, count in (([], 0), ([hit(site="melee")], 1)):
                out = StringIO()
                with redirect_stdout(out):
                    report(log(*lines))
                r4 = out.getvalue().split("R4 자리별 HIT 통계(판정 없음):\n")[1].split("R5 ")[0]
                self.assertIn(f"  melee: {count}줄, physicalDamage>0", r4)

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests))
    if result.wasSuccessful():
        print("자체 검사 모두 통과")
    return 0 if result.wasSuccessful() else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description="PoE Proto 9 받는 피해 로그 분석")
    parser.add_argument("log", nargs="?", type=Path, help="PoEProtoReceived.log 경로")
    parser.add_argument("--selftest", action="store_true", help="합성 로그로 자체 검사")
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.log is None:
        parser.error("로그 경로 또는 --selftest가 필요합니다")
    try:
        text = args.log.read_text(encoding="utf-8-sig", errors="replace")
    except OSError as error:
        print(f"로그를 읽지 못함: {error}", file=sys.stderr)
        return 1
    report(read_log(text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
