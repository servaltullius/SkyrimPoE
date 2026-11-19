"""Summarize the PoEProtoMainUpdate TSV and evaluate the README criteria."""

import argparse
import csv
import io
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


CANDIDATES = ("A", "B", "C", "D")
PHASES = ("Baseline", "Journal", "AfterJournal", "Inventory", "EngineWrite", "Reload", "AfterReload")
PLAY_PHASES = ("Baseline", "AfterJournal", "AfterReload")
MENUS = {"Journal": "Journal Menu", "Inventory": "InventoryMenu", "Reload": "Loading Menu"}
COLUMNS = (
    "interval", "start_ms", "end_ms", "phase", "candidate", "installed", "calls", "threads", "thread_overflow",
    "main_calls", "unknown_main_calls", "min_gap_ms", "max_gap_ms", "first_call_ms", "last_call_ms",
    "frame_delta", "update_delta", "paused", "freeze", "menus", "load_active", "phase_result",
    "engine_write", "run_result",
)


@dataclass(frozen=True)
class Row:
    interval: int
    start: float
    end: float
    phase: str
    candidate: str
    installed: bool
    calls: int
    threads: frozenset[int]
    overflow: bool
    main: int
    unknown_main: int
    max_gap: float
    first: float
    last: float
    frames: int
    updates: int
    paused: int
    freeze: int
    menus: frozenset[str]
    phase_result: str
    write_result: str
    run_result: str


def read_rows(stream):
    reader = csv.DictReader(stream, delimiter="\t")
    if not reader.fieldnames or set(COLUMNS) - set(reader.fieldnames):
        raise ValueError("PoEProtoMainUpdate TSV 머리 줄이 없거나 열이 빠졌습니다.")
    rows = []
    seen = set()
    for raw in reader:
        try:
            if None in raw or any(raw[key] is None for key in COLUMNS):
                raise ValueError
            row = Row(
                int(raw["interval"]), float(raw["start_ms"]), float(raw["end_ms"]),
                raw["phase"], raw["candidate"], bool(int(raw["installed"])), int(raw["calls"]),
                frozenset(map(int, raw["threads"].split(","))) if raw["threads"] != "-" else frozenset(),
                bool(int(raw["thread_overflow"])), int(raw["main_calls"]), int(raw["unknown_main_calls"]),
                float(raw["max_gap_ms"]), float(raw["first_call_ms"]), float(raw["last_call_ms"]),
                int(raw["frame_delta"]), int(raw["update_delta"]), int(raw["paused"]), int(raw["freeze"]),
                frozenset(raw["menus"].split(",")) if raw["menus"] != "-" else frozenset(),
                raw["phase_result"], raw["engine_write"], raw["run_result"],
            )
            key = (row.interval, row.candidate)
            if (key in seen or row.interval < 1 or row.phase not in PHASES or row.candidate not in CANDIDATES
                    or any(raw[key] not in {"0", "1"} for key in ("installed", "thread_overflow", "load_active"))
                    or not 0 <= row.main + row.unknown_main <= row.calls or min(row.main, row.unknown_main) < 0
                    or row.paused not in {-1, 0, 1} or row.freeze not in {-1, 0, 1}
                    or min(row.frames, row.updates) < -1 or not 0 <= row.start <= row.end
                    or not all(map(math.isfinite, (row.start, row.end, row.max_gap, row.first, row.last)))
                    or len(row.threads) > min(8, row.calls) or any(thread <= 0 for thread in row.threads)
                    or row.phase_result not in {"-", "complete", "menu_not_opened", "skipped", "aborted"}
                    or row.write_result not in {"-", "passed", "failed", "not_run"}
                    or row.run_result not in {"-", "done", "aborted"}):
                raise ValueError
            seen.add(key)
            rows.append(row)
        except ValueError as error:
            raise ValueError(f"TSV {reader.line_num}행의 열 값이 잘못되었습니다.") from error
    return sorted(rows, key=lambda row: row.interval)


def counter_delta(rows, field):
    return sum(getattr(row, field) for row in rows) if all(getattr(row, field) >= 0 for row in rows) else -1


def thread_stats(rows):
    threads = set().union(*(row.threads for row in rows))
    overflow = any(row.overflow for row in rows)
    return max(len(threads), 9 if overflow else 0), overflow


def max_gap(rows):
    longest = -1.0
    last_call = -1.0
    last_interval = None
    for row in rows:
        if last_interval is not None and row.interval != last_interval + 1:
            last_call = -1.0
        longest = max(longest, row.max_gap)
        if row.first >= 0 and last_call >= 0:
            longest = max(longest, row.first - last_call)
        if row.last >= 0:
            last_call = row.last
        last_interval = row.interval
    return longest


def verdict(rows):
    if not rows or not any(row.installed for row in rows):
        return "판정 불가", ["설치된 후보의 관측 행이 없음"]

    failures = []
    missing = []
    phases = defaultdict(list)
    for row in rows:
        phases[row.phase].append(row)
    if not any(row.run_result == "done" for row in rows):
        missing.append("시험 완료 기록이 없음")

    for phase in PLAY_PHASES:
        active = [row for row in phases[phase] if row.paused == row.freeze == 0 and not row.menus]
        if not active or not any(row.phase_result == "complete" for row in phases[phase]):
            missing.append(f"{phase}의 정상 플레이 구간 기록이 부족함")
            continue
        calls = sum(row.calls for row in active)
        updates = counter_delta(active, "updates")
        frames = counter_delta(active, "frames")
        if updates <= 0:
            missing.append(f"{phase}의 유효한 갱신 증가량이 없음")
        elif abs(calls - updates) > 1:
            failures.append(f"{phase} 호출 {calls}, 갱신 {updates}로 ±1 범위를 벗어남")
        if frames <= 0:
            missing.append(f"{phase}의 유효한 프레임 증가량이 없음")
        elif abs(calls - frames) > max(1, frames / 100):
            failures.append(f"{phase} 호출 {calls}, 프레임 {frames}로 허용 오차를 벗어남")

    threads, _ = thread_stats(rows)
    if threads > 1:
        failures.append(f"서로 다른 스레드에서 호출됨(최소 {threads}개)")
    non_main = sum(row.calls - row.main - row.unknown_main for row in rows)
    if non_main:
        failures.append(f"Main::threadID와 다른 호출 {non_main}회")
    if threads == 0 or any(row.unknown_main for row in rows):
        missing.append("호출 스레드 또는 Main::threadID 확인 자료가 부족함")

    for phase, menu in MENUS.items():
        observed = [row for row in phases[phase] if menu in row.menus and row.end > row.start
                    and (phase == "Reload" or row.paused == 1)]
        if not observed or not any(row.phase_result == "complete" for row in phases[phase]):
            missing.append(f"{phase}의 {'로드 화면' if phase == 'Reload' else '열리고 일시 정지된 메뉴'} 관측이 부족함")
        elif not any(row.calls for row in observed):
            failures.append(f"{phase}에서 {('로드 화면' if phase == 'Reload' else '메뉴로 멈춘 동안')} 호출 0회")

    writes = {row.write_result for row in phases["EngineWrite"]}
    if "failed" in writes:
        failures.append("EngineWrite에서 매지카·체력 변경 또는 복원 확인 실패")
    elif "passed" not in writes:
        missing.append("이 후보에서 완료된 EngineWrite 결과가 없음")

    if failures:
        return "실패", failures + missing
    if missing:
        return "판정 불가", missing
    return "통과", ["갱신·프레임 빈도, Main 스레드, 메뉴·로드 중 호출, 엔진 쓰기 기준 충족"]


def markdown(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[row.phase, row.candidate].append(row)
    lines = [
        "## 단계별 호출 기록", "",
        "| 단계 | 후보 | 호출 수 | 호출/갱신 | 호출/프레임 | 스레드 수 | Main 스레드 비율 | 최대 간격(ms) |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for phase in PHASES:
        for candidate in CANDIDATES:
            samples = groups[phase, candidate]
            if not samples or not any(row.installed for row in samples):
                status = "미설치" if samples else "미관측"
                lines.append(f"| {phase} | {candidate} | {status} | — | — | — | — | — |")
                continue
            calls = sum(row.calls for row in samples)
            updates = counter_delta(samples, "updates")
            frames = counter_delta(samples, "frames")
            threads, overflow = thread_stats(samples)
            main = sum(row.main for row in samples)
            unknown_main = sum(row.unknown_main for row in samples)
            gap = max_gap(samples)
            update_ratio = f"{calls / updates:.3f}" if updates > 0 else "—"
            frame_ratio = f"{calls / frames:.3f}" if frames > 0 else "—"
            main_ratio = f"{main / calls:.1%}" if calls and not unknown_main else "—"
            gap_value = f"{gap:.3f}" if gap >= 0 else "—"
            lines.append(f"| {phase} | {candidate} | {calls} | {update_ratio} | {frame_ratio} | "
                         f"{threads}{'+' if overflow else ''} | {main_ratio} | {gap_value} |")

    lines.extend(["", "## 판정", ""])
    results = []
    for candidate in CANDIDATES[:-1]:
        result, reasons = verdict([row for row in rows if row.candidate == candidate])
        results.append(result)
        lines.append(f"- **{candidate} · {result}** — " + " / ".join(reasons))
    lines.append("- **D · 비교용**")
    if "통과" in results:
        overall = "통과"
    elif any(row.run_result == "done" for row in rows) or all(result == "실패" for result in results):
        overall = "실패"
    else:
        overall = "판정 불가"
    lines.extend(["", f"전체 시험 · **{overall}**"])
    return "\n".join(lines)


def selftest():
    def simulated(**changes):
        stream = io.StringIO()
        writer = csv.DictWriter(stream, fieldnames=COLUMNS, delimiter="\t")
        writer.writeheader()
        for i, phase in enumerate(PHASES, 1):
            for candidate in CANDIDATES:
                row = dict(interval=i, start_ms=(i - 1) * 1000, end_ms=i * 1000, phase=phase, candidate=candidate,
                           installed=1, calls=60, threads="41", thread_overflow=0, main_calls=60, unknown_main_calls=0,
                           min_gap_ms=16, max_gap_ms=17, first_call_ms=(i - 1) * 1000 + 16, last_call_ms=i * 1000,
                           frame_delta=60, update_delta=60, paused=int(phase in MENUS), freeze=0,
                           menus=MENUS.get(phase, "-"), load_active=int(phase == "Reload"), phase_result="complete",
                           engine_write="passed" if candidate == "A" and phase == "EngineWrite" else "not_run",
                           run_result="done" if phase == "AfterReload" else "-")
                if candidate == "B" and phase in PLAY_PHASES:
                    row.update(calls=58, main_calls=58)
                if candidate == "A" and phase == changes.get("phase"):
                    row.update(changes["values"])
                writer.writerow(row)
        stream.seek(0)
        return read_rows(stream)

    rows = simulated()
    text = markdown(rows)
    assert "A · 통과" in text and "B · 실패" in text and "C · 판정 불가" in text and "D · 비교용" in text
    assert "| Baseline | A | 60 | 1.000 | 1.000 | 1 | 100.0% | 17.000 |" in text
    assert "전체 시험 · **실패**" in markdown(simulated(phase="EngineWrite", values={"engine_write": "failed"}))
    cases = (
        ("Baseline", {"calls": 59, "main_calls": 59}, "통과"),
        ("AfterReload", {"frame_delta": 62}, "실패"),
        ("AfterJournal", {"update_delta": -1}, "판정 불가"),
        ("Journal", {"menus": "-", "phase_result": "menu_not_opened"}, "판정 불가"),
        ("Inventory", {"paused": 0}, "판정 불가"),
        ("Reload", {"calls": 0, "main_calls": 0, "threads": "-"}, "실패"),
        ("Reload", {"menus": "-", "load_active": 1}, "판정 불가"),
        ("Inventory", {"threads": "41,42", "main_calls": 59}, "실패"),
        ("Inventory", {"threads": "42"}, "실패"),
        ("Inventory", {"main_calls": 59, "unknown_main_calls": 1}, "판정 불가"),
        ("AfterReload", {"run_result": "aborted"}, "판정 불가"),
        ("EngineWrite", {"engine_write": "failed"}, "실패"),
    )
    for phase, values, expected in cases:
        samples = [row for row in simulated(phase=phase, values=values) if row.candidate == "A"]
        assert verdict(samples)[0] == expected, (phase, values, verdict(samples))
    print(f"selftest passed ({len(cases) + 1} scenarios)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tsv", nargs="?", type=Path)
    parser.add_argument("--selftest", action="store_true", help="가짜 TSV로 판정 검사")
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return
    if args.tsv is None:
        parser.error("TSV 파일 경로 또는 --selftest를 지정하세요.")
    try:
        with args.tsv.open(encoding="utf-8-sig", newline="") as stream:
            print(markdown(read_rows(stream)))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
