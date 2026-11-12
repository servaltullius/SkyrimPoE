"""프로토타입 1(사이클론 모션)의 시험 모드 두 개를 만든다.

  1A  Animation Injector 방식: 공격 버튼을 길게 누르면 회전 반복 동작
  1B  OAR 방식: 액터 값 Variable09가 1인 동안 이동 클립을 회전 모션으로 교체

회전 모션은 파일 하나(--clip)에서 가져와 필요한 이름으로 복사한다.
클립을 바꾸려면 --clip 만 바꿔 다시 실행하면 된다.

사용:
  python build_proto1.py                       out 폴더에만 만든다
  python build_proto1.py --deploy "G:\\TAKEALOOK\\mods"   MO2 mods 폴더에도 넣는다
"""
from __future__ import annotations

import argparse
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


def build_1b(root: Path, clip: Path) -> int:
    mod = root / MOD_1B
    reset_dir(mod)
    oar = mod / ANIM_ROOT / "OpenAnimationReplacer" / "PoE Proto"
    sub = oar / "Cyclone locomotion"
    sub.mkdir(parents=True)
    # JSON을 한 번 읽어 문법 오류를 여기서 잡은 뒤 원문 그대로 복사한다.
    submod = load_json(SRC / "oar-submod-config.json")
    load_json(SRC / "oar-mod-config.json")
    if "conditions" not in submod or not isinstance(submod["conditions"], list):
        fail("oar-submod-config.json 의 최상위에 소문자 conditions 배열이 없습니다.")
    shutil.copyfile(SRC / "oar-mod-config.json", oar / "config.json")
    shutil.copyfile(SRC / "oar-submod-config.json", sub / "config.json")
    for name in LOCOMOTION_CLIPS:
        shutil.copyfile(clip, sub / name)
    return 2 + len(LOCOMOTION_CLIPS)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clip", type=Path, default=DEFAULT_CLIP, help="회전 모션으로 쓸 HKX 파일")
    parser.add_argument("--end-clip", type=Path, help="1A에서 버튼을 뗐을 때 재생할 마무리 HKX. 주지 않으면 --clip 과 같은 파일")
    parser.add_argument("--deploy", type=Path, help="MO2의 mods 폴더. 주면 그 안에도 모드 폴더를 만든다")
    args = parser.parse_args()

    clip: Path = args.clip
    end_clip: Path = args.end_clip if args.end_clip is not None else clip
    for label, path in (("회전 모션", clip), ("마무리 모션", end_clip)):
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
        count_a = build_1a(root, clip, end_clip)
        count_b = build_1b(root, clip)
        print(f"{root}\n  {MOD_1A}: 파일 {count_a}개\n  {MOD_1B}: 파일 {count_b}개")
    print(f"회전 모션 원본: {clip} ({clip.stat().st_size} bytes)")
    print(f"마무리 모션 원본: {end_clip} ({end_clip.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
