"""PoE Proto 8의 로그(PoEProtoHitDamage.log)를 읽어 README의 Q1~Q5를 계산한다.

  python tools\\analyze_hits.py <로그 파일> [--skill <같은 실행의 PoEProtoSkillInput.log>]

--skill을 주면 프로토타입 2가 사이클론 타격마다 남기는 `[타격] … / 채널링 중` 줄과 같은 대상·0.1초 안의 HIT 줄을 사이클론 타격으로 가른다.
"""
import argparse
import re
import statistics
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

PLAYER = 0x14
# CommonLibSSE-NG v16.4.0의 RE::HitData::Flag
FLAG_NAMES = {0x1: "막힘", 0x2: "무기로 막음", 0x4: "막기 후보", 0x8: "치명타", 0x10: "죽을 때 치명타", 0x20: "치명상", 0x40: "사지 절단", 0x80: "사지 폭발",
              0x100: "사지 불구", 0x200: "무장 해제", 0x400: "무기 못 씀", 0x800: "은신", 0x1000: "치명타 무시", 0x2000: "피해 예측", 0x4000: "방패 치기",
              0x8000: "때맞춘 방패 치기", 0x10000: "강공격", 0x20000: "왼손", 0x40000: "근접", 0x80000: "튕김", 0x100000: "폭발"}
PAIR_US = 50_000


@dataclass
class Hit:
    wall: datetime
    t: int
    thread: int
    site: str
    mode: str
    modified: bool
    aggressor: int
    target: int
    weapon: int
    source_ref: int
    source_type: int
    flags: int
    total_before: float
    total_after: float
    physical: float
    blocked: float
    resisted_physical: float
    resisted_typed: float
    field_6c: float     # HitData +0x6C. CommonLib의 이름은 sneakAttackBonus이나 은신 타격에서도 1.0이었음
    sneak_mult: float   # HitData +0x70. 은신 배율(은신 타격 1.5, 그 밖 1.0). CommonLib의 이름은 bonusHealthDamageMult
    reflected: float
    stagger: float
    health_before: float
    health_after: float
    max_health: float
    mult: float
    late: float | None = None
    cyclone: bool = False

    @property
    def drop(self) -> float:
        return self.health_before - self.health_after


def flag_text(flags: int) -> str:
    names = [name for bit, name in FLAG_NAMES.items() if flags & bit]
    rest = flags & ~sum(FLAG_NAMES)
    return "+".join(names + ([f"0x{rest:X}"] if rest else [])) or "-"


def wall_time(text: str) -> datetime:
    return datetime.strptime(text, "%Y-%m-%d %H:%M:%S.%f")


def load(path: Path):
    """로그를 읽어 (HIT, 적중 이벤트, 사망, 대신 주기, 그 밖의 줄, 설정)을 돌려준다. 설정은 머리의 `설정: 키=값 …` 줄."""
    hits: list[Hit] = []
    events: list[tuple] = []
    deaths: list[tuple] = []
    late: dict[int, float] = {}
    own: list[tuple] = []
    notes: list[str] = []
    settings: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        cols = line.split("\t")
        if len(cols) < 3:
            continue
        kind = cols[2]
        try:
            if kind == "HIT":
                c = cols[3:]
                hits.append(Hit(wall_time(cols[0]), int(c[0]), int(c[1]), c[2], c[3], c[4] == "1", int(c[5], 16), int(c[6], 16), int(c[7], 16), int(c[8], 16),
                                int(c[9]), int(c[10], 16), *map(float, c[11:26])))
            elif kind == "EVHIT":
                c = cols[3:]
                events.append((int(c[0]), int(c[2], 16), int(c[3], 16), int(c[4], 16), int(c[6])))
            elif kind == "EVDEATH":
                c = cols[3:]
                deaths.append((int(c[0]), int(c[1], 16), int(c[2], 16), c[3]))
            elif kind == "LATE":
                c = cols[3:]
                if c[2] != "-":
                    late[int(c[3])] = float(c[2])
            elif kind == "OWN":
                own.append(tuple(cols[3:]))
            else:
                notes.append(line)
                if kind.startswith("설정: "):
                    for pair in kind[len("설정: "):].split():
                        key, _, value = pair.partition("=")
                        if value:
                            settings[key] = value
        except (ValueError, IndexError) as error:
            notes.append(f"(읽지 못한 줄: {error}) {line[:120]}")
    for h in hits:
        if h.t in late and h.site == "melee":
            h.late = late[h.t]
    return hits, events, deaths, own, notes, settings


def channel_hits(path: Path) -> list[tuple[datetime, int]]:
    """프로토타입 2 로그의 `[타격] 대상 '이름'(폼) 수단 … / 채널링 중` 줄에서 (시각, 대상 폼)을 돌려준다.

    프로토타입 2는 사이클론의 DLL 타격마다 이 줄을 남긴다(채널링이 아닐 때는 `/ 채널링 아님`).
    """
    out = []
    pattern = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\].*\[타격\] 대상 '.*'\(([0-9A-Fa-f]{8})\).* / 채널링 중")
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = pattern.match(line)
        if m:
            out.append((wall_time(m.group(1)), int(m.group(2), 16)))
    return out


CHANNEL_MATCH_SECONDS = 0.1  # 프로토타입 2의 [타격] 줄과 이 플러그인의 HIT 줄이 같은 타격이라고 볼 시각 차이


def close(a: float, b: float) -> bool:
    return abs(a - b) <= max(0.05, 0.01 * abs(b))


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("log", type=Path)
    ap.add_argument("--skill", type=Path)
    a = ap.parse_args()
    hits, events, deaths, own, notes, settings = load(a.log)
    channel = channel_hits(a.skill) if a.skill else []
    by_target = defaultdict(list)
    for t, target in channel:
        by_target[target].append(t)
    for h in hits:
        h.cyclone = h.aggressor == PLAYER and h.site == "melee" and any(
            abs((t - h.wall).total_seconds()) <= CHANNEL_MATCH_SECONDS for t in by_target.get(h.target, ()))

    print("# 설치와 모드")
    for n in notes:
        print(" ", n[:200])
    print(f"\nHIT {len(hits)}, 적중 이벤트 {len(events)}, 사망 {len(deaths)}, 대신 주기 {len(own)}, 프로토타입 2의 채널링 중 타격 줄 {len(channel)}, 그와 짝지은 HIT {sum(h.cyclone for h in hits)}")
    print("자리·모드·바꿨는지·공격자:", dict(Counter((h.site, h.mode, h.modified, "플레이어" if h.aggressor == PLAYER else ("플레이어가 대상" if h.target == PLAYER else "그 밖")) for h in hits)))

    print("\n# Q1 바꾸기(set): 줄어든 체력 대 Value × 배율")
    try:
        value = float(settings["Value"])
    except (KeyError, ValueError):
        value = None
        print("  로그 머리의 설정 줄에서 Value를 읽지 못해 Q1을 판정하지 않음")
    set_hits = [h for h in hits if h.site == "melee" and h.modified and h.mode == "set"] if value is not None else []
    if value is not None:
        print(f"  Value = {value:g}(로그 머리의 설정 줄)")
    rows = []
    for h in set_hits:
        expected = value * h.mult
        where = "호출 안" if h.drop > 0.001 else ("늦은 읽기" if h.late is not None else "줄지 않음")
        measured = h.drop if h.drop > 0.001 else (h.health_before - h.late if h.late is not None else 0.0)
        rows.append((h, expected, measured, where, close(measured, expected)))
    for cyclone in (False, True):
        part = [r for r in rows if r[0].cyclone == cyclone]
        if part:
            ok = sum(r[4] for r in part)
            print(f"  {'사이클론 중' if cyclone else '평소'}: {len(part)}타, 맞음 {ok} ({100 * ok / len(part):.0f}%), 자리 {dict(Counter(r[3] for r in part))}")
            print("    줄어든 양 분포:", Counter(round(r[2], 2) for r in part).most_common(6), " 기대:", Counter(round(r[1], 2) for r in part).most_common(3))
            print("    바꾸기 전 totalDamage:", Counter(round(r[0].total_before, 1) for r in part).most_common(6))
    bad = [r for r in rows if not r[4]][:8]
    for h, expected, measured, where, _ in bad:
        print(f"    어긋남: t={h.t} 대상 {h.target:08X} 전 {h.total_before:.2f} 기대 {expected:.2f} 잰 값 {measured:.2f}({where}) 표시 {flag_text(h.flags)} 막힘 {h.blocked:.2f} 체력 {h.health_before:.1f}->{h.health_after:.1f}")

    print("\n# Q2 작업 큐")
    queue = [h for h in hits if h.site == "queue"]
    print(f"  큐가 다시 부른 기록 {len(queue)}")
    for mode in sorted({h.mode for h in queue}):
        part = [h for h in queue if h.mode == mode]
        if part:
            print(f"   {mode}: {len(part)}, totalDamage {Counter(round(h.total_before, 2) for h in part).most_common(5)}, 줄어든 체력 {Counter(round(h.drop, 2) for h in part).most_common(5)}")
    melee_unchanged = [h for h in hits if h.site == "melee" and h.total_after > 0 and h.drop <= 0.001]
    print(f"  근접 자리에서 피해가 있는데 호출 안에서 체력이 줄지 않은 것 {len(melee_unchanged)}")
    print("  근접 자리의 스레드:", Counter(h.thread for h in hits if h.site == "melee").most_common(5))

    print("\n# Q3 지우기(zero)")
    zero_hits = [h for h in hits if h.site == "melee" and h.modified and h.mode == "zero"]
    ev_by = defaultdict(list)
    for t, target, cause, source, flags in events:
        ev_by[(target, cause)].append(t)
    with_event = sum(1 for h in zero_hits if any(abs(t - h.t) <= PAIR_US for t in ev_by[(h.target, h.aggressor)]))
    no_drop = sum(1 for h in zero_hits if h.drop <= 0.001 and (h.late is None or h.health_before - h.late <= 0.001))
    print(f"  {len(zero_hits)}타: ±50ms 안의 적중 이벤트 {with_event}, 체력이 줄지 않음 {no_drop}, 사이클론 중 {sum(h.cyclone for h in zero_hits)}")
    print("  stagger 칸:", Counter(round(h.stagger, 2) for h in zero_hits).most_common(5))
    print("  대신 주기:", len(own), "줄. 사망 이벤트:", [(f"{d[1]:08X}", f"{d[2]:08X}", d[3]) for d in deaths])

    print("\n# Q4 관찰(observe), 플레이어의 타격: 칸들의 관계")
    obs = [h for h in hits if h.site == "melee" and h.mode == "observe" and h.aggressor == PLAYER]
    groups = defaultdict(list)
    for h in obs:
        groups[(flag_text(h.flags), h.cyclone)].append(h)
    for (flags, cyclone), part in sorted(groups.items(), key=lambda x: -len(x[1])):
        ratio = [h.total_before / h.physical for h in part if h.physical > 0]
        tb = [h.total_before / (d) for h, d in ((h, h.drop / h.mult) for h in part if h.drop > 0.001 and h.mult > 0) if d > 0]
        print(f"  {flags}{' (사이클론)' if cyclone else ''}: {len(part)}타, total 중앙값 {statistics.median(h.total_before for h in part):.2f}, physical 중앙값 {statistics.median(h.physical for h in part):.2f}, "
              f"total/physical {statistics.median(ratio) if ratio else float('nan'):.3f}, 막힘 {Counter(round(h.blocked, 2) for h in part).most_common(2)}, 은신 배율(+0x70) {Counter(round(h.sneak_mult, 2) for h in part).most_common(2)}, "
              f"줄어든 체력/배율 대 total {statistics.median(tb) if tb else float('nan'):.3f}")

    print("\n# Q5 플레이어가 맞은 것, 투사체")
    taken = [h for h in hits if h.target == PLAYER and h.site == "melee"]
    print(f"  플레이어가 대상인 근접 자리 기록 {len(taken)}, 공격자 {Counter(f'{h.aggressor:08X}' for h in taken).most_common(4)}")
    for h in taken[:5]:
        print(f"    total {h.total_before:.2f} physical {h.physical:.2f} 막힘 {h.blocked:.2f} 표시 {flag_text(h.flags)} 체력 {h.health_before:.1f}->{h.health_after:.1f} 배율 {h.mult:.2f}")
    arrows = [h for h in hits if h.source_ref]
    print(f"  투사체 참조가 있는 기록 {len(arrows)}, 폼 종류 {Counter(h.source_type for h in arrows)}")
    print("  공격자가 플레이어도 대상도 플레이어도 아닌 기록:", sum(1 for h in hits if h.aggressor != PLAYER and h.target != PLAYER))
    return 0


if __name__ == "__main__":
    sys.exit(main())
