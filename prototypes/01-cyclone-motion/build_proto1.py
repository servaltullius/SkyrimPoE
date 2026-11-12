"""프로토타입 1(사이클론 모션)의 시험 모드 두 개를 만든다.

  1A  Animation Injector 방식: 공격 버튼을 길게 누르면 회전 반복 동작
  1B  OAR 방식: 액터 값 Variable09가 1인 동안 이동 클립을 회전 모션으로 교체

회전 모션은 파일 하나(--clip)에서 가져와 필요한 이름으로 복사한다.
클립을 바꾸려면 --clip 만 바꿔 다시 실행하면 된다.

1B에는 비교용 클립을 더 넣을 수 있다. --variant-clip 을 줄 때마다 서브모드가 하나씩 늘고,
Variable09가 2, 3, ... 일 때 그 클립이 재생된다(--clip 이 1번).

사용:
  python build_proto1.py                       out 폴더에만 만든다
  python build_proto1.py --deploy "G:\\TAKEALOOK\\mods"   MO2 mods 폴더에도 넣는다
"""
from __future__ import annotations

import argparse
import copy
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "src"
DEFAULT_CLIP = HERE.parent / "_local-assets" / "placeholder_spin.hkx"

MOD_1A = "[PoE Proto] 1A Cyclone - Animation Injector"
MOD_1B = "[PoE Proto] 1B Cyclone - OAR"

ANIM_ROOT = Path("meshes") / "actors" / "character" / "animations"

# 무기를 든 상태의 3인칭 이동 클립. 이름은 바닐라 Skyrim - Animations.bsa에서 확인했다.
LOCOMOTION_SUFFIXES = [
    "idle",
    "runforward", "runforwardleft", "runforwardright",
    "runleft", "runright",
    "runbackward", "runbackwardleft", "runbackwardright",
    "runstrafeleft", "runstraferight",
    "walkforward", "walkforwardleft", "walkforwardright",
    "walkleft", "walkright",
    "walkbackward", "walkbackwardleft", "walkbackwardright",
    "turnleft60", "turnleft180", "turnright60", "turnright180",
]
LOCOMOTION_CLIPS = [f"{prefix}_{suffix}.hkx" for prefix in ("1hm", "2hm") for suffix in LOCOMOTION_SUFFIXES]
LOCOMOTION_CLIPS.append("2hw_idle.hkx")


def fail(message: str) -> None:
    print(f"오류: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"{path} 을(를) 읽지 못했습니다: {exc}")
    raise AssertionError("unreachable")


def reset_dir(path: Path) -> None:
    """이 스크립트가 만든 모드 폴더만 지우고 다시 만든다."""
    if path.exists():
        if not path.name.startswith("[PoE Proto] "):
            fail(f"지우려는 폴더 이름이 예상과 다릅니다: {path}")
        shutil.rmtree(path)
    path.mkdir(parents=True)


def build_1a(root: Path, clip: Path, end_clip: Path) -> int:
    mod = root / MOD_1A
    reset_dir(mod)
    xml_dir = mod / "SKSE" / "Plugins" / "AnimationInjector" / "PoEProtoCyclone"
    xml_dir.mkdir(parents=True)
    shutil.copyfile(SRC / "cyclone.xml", xml_dir / "cyclone.xml")
    clip_dir = mod / ANIM_ROOT / "PoEProto"
    clip_dir.mkdir(parents=True)
    shutil.copyfile(clip, clip_dir / "cyclone_loop.hkx")
    shutil.copyfile(end_clip, clip_dir / "cyclone_end.hkx")
    return 3


def variant_config(template: dict, number: int) -> dict:
    """서브모드 설정의 틀에서 number번 클립용 설정을 만든다.
    조건의 비교 값을 number로, 우선순위를 틀의 값 + (number - 1)로 둔다(서브모드끼리 우선순위가 겹치지 않게)."""
    config = copy.deepcopy(template)
    compare = [c for c in config["conditions"] if isinstance(c, dict) and c.get("condition") == "CompareValues"]
    if len(compare) != 1 or "Value B" not in compare[0] or "value" not in compare[0]["Value B"]:
        fail("oar-submod-config.json 에 값을 비교하는 CompareValues 조건이 정확히 하나 있어야 합니다.")
    if not isinstance(config.get("priority"), int):
        fail("oar-submod-config.json 의 priority 가 정수가 아닙니다.")
    compare[0]["Value B"]["value"] = float(number)
    config["priority"] = config["priority"] + number - 1
    if config["priority"] > 2147483647:
        fail("서브모드 우선순위가 OAR이 받는 범위(32비트 정수)를 넘습니다.")
    config["name"] = f"{config.get('name', 'Cyclone locomotion')} {number}"
    return config


def build_1b(root: Path, clips: list[Path]) -> int:
    mod = root / MOD_1B
    reset_dir(mod)
    oar = mod / ANIM_ROOT / "OpenAnimationReplacer" / "PoE Proto"
    oar.mkdir(parents=True)
    template = load_json(SRC / "oar-submod-config.json")
    load_json(SRC / "oar-mod-config.json")  # 문법 오류를 여기서 잡는다
    if "conditions" not in template or not isinstance(template["conditions"], list):
        fail("oar-submod-config.json 의 최상위에 소문자 conditions 배열이 없습니다.")
    shutil.copyfile(SRC / "oar-mod-config.json", oar / "config.json")
    count = 1
    for number, clip in enumerate(clips, start=1):
        sub = oar / f"Cyclone locomotion {number}"
        sub.mkdir()
        text = json.dumps(variant_config(template, number), ensure_ascii=False, indent=2) + "\n"
        (sub / "config.json").write_text(text, encoding="utf-8", newline="\n")
        for name in LOCOMOTION_CLIPS:
            shutil.copyfile(clip, sub / name)
        count += 1 + len(LOCOMOTION_CLIPS)
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clip", type=Path, default=DEFAULT_CLIP, help="회전 모션으로 쓸 HKX 파일")
    parser.add_argument("--variant-clip", type=Path, action="append", default=[],
                        help="1B에 비교용으로 더 넣을 HKX. 줄 때마다 2번, 3번, ... 서브모드가 된다")
    parser.add_argument("--end-clip", type=Path, help="1A에서 버튼을 뗐을 때 재생할 마무리 HKX. 주지 않으면 --clip 과 같은 파일")
    parser.add_argument("--deploy", type=Path, help="MO2의 mods 폴더. 주면 그 안에도 모드 폴더를 만든다")
    parser.add_argument("--only", choices=("1a", "1b"), help="한쪽 모드만 만든다. 주지 않으면 둘 다 만든다")
    args = parser.parse_args()

    clip: Path = args.clip
    end_clip: Path = args.end_clip if args.end_clip is not None else clip
    clips_1b: list[Path] = [clip, *args.variant_clip]
    checks = [("회전 모션", clip), ("마무리 모션", end_clip)]
    checks += [(f"비교용 클립 {n}번", path) for n, path in enumerate(args.variant_clip, start=2)]
    for label, path in checks:
        if not path.is_file():
            fail(f"{label} 파일이 없습니다: {path}")
        if path.suffix.lower() != ".hkx":
            fail(f"{label} 파일이 HKX가 아닙니다: {path}")
    for required in ("cyclone.xml", "oar-mod-config.json", "oar-submod-config.json"):
        if not (SRC / required).is_file():
            fail(f"src 폴더에 {required} 이(가) 없습니다.")

    targets = [HERE / "out"]
    if args.deploy is not None:
        if not args.deploy.is_dir():
            fail(f"mods 폴더가 없습니다: {args.deploy}")
        targets.append(args.deploy)

    for root in targets:
        root.mkdir(parents=True, exist_ok=True)
        print(root)
        if args.only != "1b":
            print(f"  {MOD_1A}: 파일 {build_1a(root, clip, end_clip)}개")
        if args.only != "1a":
            print(f"  {MOD_1B}: 파일 {build_1b(root, clips_1b)}개, 클립 {len(clips_1b)}가지")
    for number, path in enumerate(clips_1b, start=1):
        print(f"클립 {number}번(Variable09 = {number}): {path} ({path.stat().st_size} bytes)")
    if args.only != "1b":
        print(f"마무리 모션 원본(1A만 씀): {end_clip} ({end_clip.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
