"""세대 1 콘텐츠 ID의 의미 지문 기준선(data/poe2/id-baseline/gen1.json)을 읽고 쓴다.

한 파일에 종류별 주인이 있다(G1 계획 4.1 W1·W2·W5).
- 트리 생성기(compile_tree_poe2.py)는 cls·asc·node를 맡는다.
- 젬 생성기(compile_gems_poe2.py)는 gem·sup을 맡는다.
생성기는 자기 종류만 바꾸고 다른 종류는 그대로 둔다. 이미 있는 ID를 지우거나 뜻을 바꾸면 거부한다.
"""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE_PATH = ROOT / "data/poe2/id-baseline/gen1.json"
TREE_KINDS = ("cls", "asc", "node")
GEM_KINDS = ("gem", "sup")
KIND_ORDER = (*TREE_KINDS, *GEM_KINDS)


def baseline_bytes(baseline: dict) -> bytes:
    """한 ID를 한 줄에 쓰는 고정 형식. 종류는 KIND_ORDER 순서, 종류 안의 ID는 들어온 순서를 지킨다."""
    entries = baseline["entries"]
    unknown = set(entries) - set(KIND_ORDER)
    if unknown:
        raise ValueError(f"의미 지문 기준선에 모르는 종류가 있습니다: {sorted(unknown)}")
    kinds = [kind for kind in KIND_ORDER if kind in entries]
    lines = ["{", f'  "contentGeneration": {json.dumps(baseline["contentGeneration"])},', '  "entries": {']
    for kind_index, kind in enumerate(kinds):
        lines.append(f"    {json.dumps(kind)}: {{")
        items = list(entries[kind].items())
        for index, (identifier, signature) in enumerate(items):
            comma = "," if index + 1 < len(items) else ""
            lines.append(f"      {json.dumps(identifier, ensure_ascii=False)}: "
                         f"{json.dumps(signature, ensure_ascii=False)}{comma}")
        lines.append("    }" + ("," if kind_index + 1 < len(kinds) else ""))
    lines.extend(["  }", "}", ""])
    return "\n".join(lines).encode("utf-8")


def merge_owned(baseline: dict, kinds: tuple[str, ...], rebuilt: dict[str, dict[str, list[str]]]) -> dict:
    """baseline의 kinds 종류를 rebuilt로 바꾼 새 기준선. 있던 ID의 삭제·의미 변경은 거부한다."""
    if set(rebuilt) != set(kinds):
        raise ValueError(f"다시 만든 종류 {sorted(rebuilt)}가 맡은 종류 {sorted(kinds)}와 다릅니다.")
    entries = baseline["entries"]
    for kind in kinds:
        for identifier, signature in entries.get(kind, {}).items():
            current = rebuilt[kind].get(identifier)
            if current is None:
                raise ValueError(f"세대 {baseline['contentGeneration']} {kind} ID {identifier}가 사라졌습니다. "
                                 "ID를 지키거나 다음 세대로 가는 대응표를 내야 합니다.")
            if current != signature:
                raise ValueError(f"세대 {baseline['contentGeneration']} {kind} ID {identifier}의 뜻이 바뀌었습니다. "
                                 "ID를 지키거나 다음 세대로 가는 대응표를 내야 합니다.")
    merged = {kind: entries[kind] for kind in KIND_ORDER if kind in entries and kind not in kinds}
    merged.update(rebuilt)
    return {"contentGeneration": baseline["contentGeneration"],
            "entries": {kind: merged[kind] for kind in KIND_ORDER if kind in merged}}
