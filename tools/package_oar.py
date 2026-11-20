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


def ensure_pose_clip(repo_root: Path, require_clip: bool) -> Path | None:
    pose_clip = repo_root / "prototypes" / "03-cyclone-clip" / "out" / "cyclone_pose.hkx"
    if pose_clip.is_file():
        return pose_clip

    # C-32: 새로 클론한 저장소에는 gitignore된 로컬 에셋(hkxc/스켈레톤 XML)이 없어
    # gen_cyclone.py --mode static 단독 호출이 필수 인자 누락으로 실패한다.
    # 로컬 에셋이 모두 있으면 정식 인자로 XML을 생성하고, 없으면 경고를 남기고 config만 패키징한다.
    gen_script = repo_root / "prototypes" / "03-cyclone-clip" / "gen_cyclone.py"
    local_assets = repo_root / "prototypes" / "_local-assets"
    skel_xml = local_assets / "skeleton.xml"
    base_xml = local_assets / "2hm_idle.xml"
    if gen_script.is_file() and skel_xml.is_file() and base_xml.is_file():
        out_xml = pose_clip.with_suffix(".xml")
        subprocess.run(
            [
                sys.executable,
                str(gen_script),
                "--skeleton",
                str(skel_xml),
                "--base",
                str(base_xml),
                "--out",
                str(out_xml),
                "--mode",
                "static",
            ],
            cwd=str(gen_script.parent),
            check=True,
        )

    if pose_clip.is_file():
        return pose_clip

    msg = (
        f"[WARN] cyclone_pose.hkx가 없어 OAR 애니메이션 클립 복사를 건너뜁니다: {pose_clip}\n"
        "       (로컬 에셋 생성 절차: prototypes/03-cyclone-clip/README.md 참조)"
    )
    if require_clip:
        raise FileNotFoundError(msg)
    print(msg, file=sys.stderr)
    return None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument(
        "--require-clip",
        action="store_true",
        help="cyclone_pose.hkx가 없을 때 경고 후 건너뛰지 않고 오류로 종료",
    )
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

    pose_clip = ensure_pose_clip(repo_root, require_clip=args.require_clip)

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
    if pose_clip is not None:
        for clip_name in LOCOMOTION_CLIPS:
            shutil.copyfile(pose_clip, sub_dir / clip_name)
        print(f"OAR packaged: {len(LOCOMOTION_CLIPS)} clips -> {sub_dir}")
    else:
        print(f"OAR packaged (config only, no local cyclone_pose.hkx) -> {sub_dir}")


if __name__ == "__main__":
    main()
