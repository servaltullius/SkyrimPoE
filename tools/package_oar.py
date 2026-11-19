"""SkyrimPoE 패키징용 사이클론 OAR 폴더 생성 스크립트.

추적 파일 assets/oar/{mod-config.json, submod-config.json}과
prototypes/03-cyclone-clip/out/cyclone_pose.hkx(없으면 gen_cyclone.py --mode static으로 생성)를
묶어 <package_dir>/meshes/actors/character/animations/OpenAnimationReplacer/SkyrimPoE/ 를 구성한다.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

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


def ensure_pose_clip(repo_root: Path) -> Path:
    pose_clip = repo_root / "prototypes" / "03-cyclone-clip" / "out" / "cyclone_pose.hkx"
    if pose_clip.is_file():
        return pose_clip

    gen_script = repo_root / "prototypes" / "03-cyclone-clip" / "gen_cyclone.py"
    if not gen_script.is_file():
        raise FileNotFoundError(f"cyclone_pose.hkx 및 생성 스크립트를 찾지 못했습니다: {gen_script}")

    subprocess.run(
        [sys.executable, str(gen_script), "--mode", "static"],
        cwd=str(gen_script.parent),
        check=True,
    )
    if not pose_clip.is_file():
        raise FileNotFoundError(f"정지 자세 클립 생성에 실패했습니다: {pose_clip}")
    return pose_clip


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-dir", type=Path, required=True)
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    oar_src = repo_root / "assets" / "oar"
    mod_cfg = oar_src / "mod-config.json"
    sub_cfg = oar_src / "submod-config.json"

    # JSON 문법 및 OAR 필수 필드 검증
    json.loads(mod_cfg.read_text(encoding="utf-8"))
    sub_data = json.loads(sub_cfg.read_text(encoding="utf-8"))
    if not isinstance(sub_data.get("conditions"), list) or not isinstance(sub_data.get("priority"), int):
        raise ValueError("assets/oar/submod-config.json의 conditions/priority 형식이 올바르지 않습니다.")

    pose_clip = ensure_pose_clip(repo_root)

    oar_root = (
        args.package_dir
        / "meshes"
        / "actors"
        / "character"
        / "animations"
        / "OpenAnimationReplacer"
        / "SkyrimPoE"
    )
    sub_dir = oar_root / "Cyclone Channeling"
    # 잔존 파일(.hkx/user.json 등) 누적을 막기 위해 SkyrimPoE OAR 폴더를 안전 확인 후 초기화한다.
    if oar_root.exists():
        if oar_root.name != "SkyrimPoE" or oar_root.parent.name != "OpenAnimationReplacer":
            raise ValueError(f"예상과 다른 OAR 출력 경로입니다: {oar_root}")
        shutil.rmtree(oar_root)
    sub_dir.mkdir(parents=True, exist_ok=True)

    shutil.copyfile(mod_cfg, oar_root / "config.json")
    shutil.copyfile(sub_cfg, sub_dir / "config.json")
    for clip_name in LOCOMOTION_CLIPS:
        shutil.copyfile(pose_clip, sub_dir / clip_name)

    print(f"OAR packaged: {len(LOCOMOTION_CLIPS)} clips -> {sub_dir}")


if __name__ == "__main__":
    main()
