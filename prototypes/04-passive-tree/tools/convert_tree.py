"""PoB의 패시브 트리 데이터를 화면용 데이터로 바꾼다.

입력: Path of Building Community의 TreeData/<버전>/tree.lua, sprites.lua 와 같은 폴더의 그림 파일.
출력: out/view-data/tree-data.js (window.POE_TREE = {...}) 와 out/view-data/assets/ 의 그림 파일.

노드의 최종 좌표, 연결선의 호 정보, 아이콘 좌표를 여기서 미리 계산한다(통합 설계서 4.2).
출력에는 GGG의 데이터와 그림이 들어 있으므로 저장소에 넣지 않는다(out/ 은 .gitignore 대상).

사용법:
    python -I convert_tree.py <PoB 폴더> <출력 폴더> [--version 3_29]
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import sys
from pathlib import Path


# ---------------------------------------------------------------- Lua 표 읽기
class LuaTableParser:
    """`return { ... }` 모양의 Lua 데이터 표만 읽는다. 함수나 식은 다루지 않는다."""

    def __init__(self, text: str) -> None:
        self.text = text
        self.pos = 0
        self.length = len(text)

    def parse(self):
        self._skip()
        if self.text.startswith("return", self.pos):
            self.pos += len("return")
        value = self._value()
        self._skip()
        if self.pos != self.length:
            raise ValueError(f"표 뒤에 읽지 못한 내용이 있음(위치 {self.pos})")
        return value

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

    def _value(self):
        self._skip()
        if self.pos >= self.length:
            raise ValueError("값이 와야 할 자리에서 파일이 끝남")
        c = self.text[self.pos]
        if c == "{":
            return self._table()
        if c == '"' or c == "'":
            return self._string(c)
        if self.text.startswith("true", self.pos):
            self.pos += 4
            return True
        if self.text.startswith("false", self.pos):
            self.pos += 5
            return False
        if self.text.startswith("nil", self.pos):
            self.pos += 3
            return None
        return self._number()

    def _string(self, quote: str) -> str:
        text = self.text
        self.pos += 1
        parts = []
        while True:
            if self.pos >= self.length:
                raise ValueError("문자열이 닫히지 않음")
            c = text[self.pos]
            if c == quote:
                self.pos += 1
                return "".join(parts)
            if c == "\\":
                self.pos += 1
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
                # 따옴표나 역슬래시가 나올 때까지 한 번에 가져온다
                end = self.pos
                while end < self.length and text[end] != quote and text[end] != "\\":
                    end += 1
                parts.append(text[self.pos:end])
                self.pos = end

    def _number(self):
        text = self.text
        start = self.pos
        while self.pos < self.length and (text[self.pos].isalnum() or text[self.pos] in "+-."):
            self.pos += 1
        token = text[start:self.pos]
        if not token:
            raise ValueError(f"읽을 수 없는 값(위치 {start}): {text[start:start + 20]!r}")
        try:
            return int(token)
        except ValueError:
            return float(token)

    def _table(self):
        text = self.text
        self.pos += 1  # {
        keyed = {}
        positional = []
        while True:
            self._skip()
            if self.pos >= self.length:
                raise ValueError("표가 닫히지 않음")
            c = text[self.pos]
            if c == "}":
                self.pos += 1
                break
            if c == "[":
                self.pos += 1
                key = self._value()
                self._skip()
                if text[self.pos] != "]":
                    raise ValueError(f"']' 가 와야 함(위치 {self.pos})")
                self.pos += 1
                self._skip()
                if text[self.pos] != "=":
                    raise ValueError(f"'=' 가 와야 함(위치 {self.pos})")
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
            for index, value in enumerate(positional, start=1):
                keyed.setdefault(index, value)
            return keyed
        return keyed if keyed else positional


def load_lua_table(path: Path):
    return LuaTableParser(path.read_text(encoding="utf-8")).parse()


# ---------------------------------------------------------------- 변환
# 노드 종류. 화면 쪽(view/tree.js)의 KIND와 같은 번호를 쓴다.
KIND_NORMAL, KIND_NOTABLE, KIND_KEYSTONE, KIND_MASTERY, KIND_SOCKET, KIND_CLASS_START, KIND_ASCEND_START = range(7)

# 궤도의 노드 수가 16, 40일 때는 고르게 나누지 않는다(GGG의 skilltree-export 설명과 PoB의 CalcOrbitAngles).
ANGLES_16 = [0, 30, 45, 60, 90, 120, 135, 150, 180, 210, 225, 240, 270, 300, 315, 330]
ANGLES_40 = [0, 10, 20, 30, 40, 45, 50, 60, 70, 80, 90, 100, 110, 120, 130, 135, 140, 150, 160, 170, 180, 190, 200,
             210, 220, 225, 230, 240, 250, 260, 270, 280, 290, 300, 310, 315, 320, 330, 340, 350]


def orbit_angles(count: int) -> list[float]:
    if count == 16:
        degrees = ANGLES_16
    elif count == 40:
        degrees = ANGLES_40
    else:
        degrees = [360.0 * i / count for i in range(count + 1)]
    return [math.radians(d) for d in degrees]


def node_kind(node: dict) -> int:
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


def as_list(value) -> list:
    """Lua 표가 빈 표면 [] 로, 숫자 키만 있으면 dict로 읽힌다. 어느 쪽이든 값의 목록으로 돌려준다."""
    if value is None:
        return []
    if isinstance(value, dict):
        return [value[key] for key in sorted(value, key=lambda k: (not isinstance(k, int), k))]
    return list(value)


def convert(pob_root: Path, out_dir: Path, version: str) -> dict:
    tree_dir = pob_root / "TreeData" / version
    tree = load_lua_table(tree_dir / "tree.lua")
    sprites_file = load_lua_table(tree_dir / "sprites.lua")
    sprites = sprites_file["sprites"] if "sprites" in sprites_file else sprites_file

    constants = tree["constants"]
    radii = as_list(constants["orbitRadii"])
    angles = [orbit_angles(count) for count in as_list(constants["skillsPerOrbit"])]

    # ---- 그림 묶음: 파일 이름과 그 안의 좌표
    atlas_index: dict[str, int] = {}
    atlases: list[dict] = []

    def atlas_for(sprite_set: dict) -> int:
        name = sprite_set["filename"].split("/")[-1].split("?")[0]
        if name not in atlas_index:
            atlas_index[name] = len(atlases)
            atlases.append({"file": name, "w": sprite_set["w"], "h": sprite_set["h"]})
        return atlas_index[name]

    def rect(set_name: str, key: str | None):
        """[묶음 번호, x, y, w, h] 또는 None."""
        sprite_set = sprites.get(set_name)
        if not sprite_set or key is None:
            return None
        c = sprite_set["coords"].get(key)
        if not c:
            return None
        return [atlas_for(sprite_set), c["x"], c["y"], c["w"], c["h"]]

    icon_sets = {
        KIND_NORMAL: ("normalInactive", "normalActive"),
        KIND_NOTABLE: ("notableInactive", "notableActive"),
        KIND_KEYSTONE: ("keystoneInactive", "keystoneActive"),
    }
    frames = {
        "normal": [rect("frame", "PSSkillFrame"), rect("frame", "PSSkillFrameActive")],
        "notable": [rect("frame", "NotableFrameUnallocated"), rect("frame", "NotableFrameAllocated")],
        "keystone": [rect("frame", "KeystoneFrameUnallocated"), rect("frame", "KeystoneFrameAllocated")],
        "socket": [rect("frame", "JewelFrameUnallocated"), rect("frame", "JewelFrameAllocated")],
    }

    # ---- 그룹
    groups_raw = tree["groups"]
    groups: dict[int, dict] = {}
    for key, group in (groups_raw.items() if isinstance(groups_raw, dict) else enumerate(groups_raw, start=1)):
        groups[int(key)] = group

    # ---- 노드
    counts = {"total": 0, "no_group": 0, "proxy": 0}
    nodes_out: list[dict] = []
    index_of: dict[int, int] = {}
    placed: dict[int, dict] = {}
    for key, node in tree["nodes"].items():
        if key == "root":
            continue
        counts["total"] += 1
        group_id = node.get("group")
        group = groups.get(group_id) if group_id is not None else None
        if group is None:
            counts["no_group"] += 1
            continue
        if node.get("isProxy") or group.get("isProxy"):
            counts["proxy"] += 1
            continue
        orbit = node.get("orbit", 0)
        orbit_index = node.get("orbitIndex", 0)
        angle = angles[orbit][orbit_index]
        radius = radii[orbit]
        x = group["x"] + math.sin(angle) * radius
        y = group["y"] - math.cos(angle) * radius
        kind = node_kind(node)

        icon_off = icon_on = None
        if kind in icon_sets:
            icon_off = rect(icon_sets[kind][0], node.get("icon"))
            icon_on = rect(icon_sets[kind][1], node.get("icon"))
        elif kind == KIND_MASTERY:
            icon_off = rect("masteryInactive", node.get("inactiveIcon"))
            icon_on = rect("masteryConnected", node.get("inactiveIcon")) or icon_off

        stats = [s for s in as_list(node.get("stats")) if isinstance(s, str)]
        entry = {
            "id": int(node.get("skill", key)),
            "x": round(x, 1),
            "y": round(y, 1),
            "k": kind,
            "n": node.get("name", ""),
            "s": stats,
        }
        if icon_off:
            entry["i"] = icon_off
        if icon_on:
            entry["a"] = icon_on
        if node.get("ascendancyName"):
            entry["asc"] = node["ascendancyName"]
        index_of[entry["id"]] = len(nodes_out)
        nodes_out.append(entry)
        placed[entry["id"]] = {"node": node, "group_id": group_id, "orbit": orbit, "angle": angle, "kind": kind,
                               "x": x, "y": y}

    # ---- 연결선: PoB와 같은 조건으로 고르고, 같은 그룹의 같은 궤도면 호로 잇는다
    edges: list[list] = []
    seen: set[tuple[int, int]] = set()
    skip_kinds = (KIND_CLASS_START, KIND_MASTERY)
    for node_id, info in placed.items():
        for other_key in as_list(info["node"].get("out")):
            other_id = int(other_key)
            other = placed.get(other_id)
            if other is None:
                continue
            if info["kind"] in skip_kinds or other["kind"] in skip_kinds:
                continue
            if info["node"].get("ascendancyName") != other["node"].get("ascendancyName"):
                continue
            pair = (min(node_id, other_id), max(node_id, other_id))
            if pair in seen:
                continue
            seen.add(pair)
            a, b = index_of[node_id], index_of[other_id]
            if info["group_id"] == other["group_id"] and info["orbit"] == other["orbit"] and info["orbit"] != 0:
                first, second = (info, other) if info["angle"] <= other["angle"] else (other, info)
                span = second["angle"] - first["angle"]
                start = first["angle"]
                if span >= math.pi:
                    start = second["angle"]
                    span = 2 * math.pi - span
                group = groups[info["group_id"]]
                # 캔버스의 각도는 오른쪽이 0이고 시계 방향이 양수다. 트리의 각도는 위가 0이고 시계 방향이다.
                canvas_start = start - math.pi / 2
                edges.append([a, b, round(group["x"], 1), round(group["y"], 1), radii[info["orbit"]],
                              round(canvas_start, 5), round(canvas_start + span, 5)])
            else:
                edges.append([a, b])

    # ---- 그룹 배경
    group_backgrounds: list[list] = []
    for group in groups.values():
        background = group.get("background")
        if not background or group.get("isProxy"):
            continue
        r = rect("groupBackground", background.get("image"))
        if r:
            group_backgrounds.append([round(group["x"], 1), round(group["y"], 1), 1 if background.get("isHalfImage") else 0, *r])

    xs = [n["x"] for n in nodes_out]
    ys = [n["y"] for n in nodes_out]
    zoom_levels = as_list(tree.get("imageZoomLevels")) or [0.3835]
    data = {
        "version": version,
        # 그림 파일의 1픽셀이 트리 좌표로 몇 단위인지의 역수. 그림은 가장 큰 확대 단계의 것 하나만 쓴다.
        "imageScale": zoom_levels[-1],
        "bounds": {"minX": min(xs), "minY": min(ys), "maxX": max(xs), "maxY": max(ys)},
        "mainBounds": {"minX": tree["min_x"], "minY": tree["min_y"], "maxX": tree["max_x"], "maxY": tree["max_y"]},
        "atlases": atlases,
        "frames": frames,
        "nodes": nodes_out,
        "edges": edges,
        "groups": group_backgrounds,
    }

    # ---- 쓰기
    assets_dir = out_dir / "assets"
    assets_dir.mkdir(parents=True, exist_ok=True)
    for atlas in atlases:
        source = tree_dir / atlas["file"]
        if not source.is_file():
            raise FileNotFoundError(f"그림 파일이 없음: {source}")
        shutil.copyfile(source, assets_dir / atlas["file"])
    payload = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
    (out_dir / "tree-data.js").write_text("window.POE_TREE=" + payload + ";\n", encoding="utf-8")

    kinds = [0] * 7
    for n in nodes_out:
        kinds[n["k"]] += 1
    return {
        "source_nodes": counts["total"],
        "skipped_no_group": counts["no_group"],
        "skipped_proxy": counts["proxy"],
        "nodes": len(nodes_out),
        "by_kind": dict(zip(["normal", "notable", "keystone", "mastery", "socket", "classStart", "ascendStart"], kinds)),
        "ascendancy_nodes": sum(1 for n in nodes_out if "asc" in n),
        "without_icon": sum(1 for n in nodes_out if "i" not in n and n["k"] in (0, 1, 2, 3)),
        "edges": len(edges),
        "arcs": sum(1 for e in edges if len(e) > 2),
        "groups_with_background": len(group_backgrounds),
        "atlases": [a["file"] for a in atlases],
        "data_bytes": len(payload.encode("utf-8")),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="PoB의 패시브 트리 데이터를 화면용 데이터로 바꾼다.")
    parser.add_argument("pob_root", type=Path, help="Path of Building Community 폴더")
    parser.add_argument("out_dir", type=Path, help="출력 폴더(tree-data.js 와 assets/ 가 생긴다)")
    parser.add_argument("--version", default="3_29", help="TreeData 아래의 버전 폴더 이름")
    args = parser.parse_args()
    if not (args.pob_root / "TreeData" / args.version / "tree.lua").is_file():
        print(f"트리 데이터를 찾지 못함: {args.pob_root / 'TreeData' / args.version / 'tree.lua'}", file=sys.stderr)
        return 2
    summary = convert(args.pob_root, args.out_dir, args.version)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
