"""만든 클립을 스킨 메시로 그려 한 장의 그림으로 저장한다(눈으로 확인하는 용도).

그리는 코드는 '스카이림 모션 작업' 프로젝트의 qa_render.py와 skin.py를 그대로 불러 쓴다.
그 프로젝트의 motion_gen 폴더를 --motion-gen 으로 넘긴다(읽기만 한다).

사용:
  python preview.py out/cyclone_loop.xml out/preview.png --motion-gen "G:\\skyrim project\\스카이림 모션 작업\\motion_gen"
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("anim", type=Path, help="그릴 클립(hkxc XML 또는 HKX)")
    parser.add_argument("out", type=Path, help="저장할 PNG")
    parser.add_argument("--motion-gen", type=Path, required=True, help="qa_render.py가 있는 motion_gen 폴더")
    parser.add_argument("--frames", default=None, help="그릴 프레임 번호(쉼표로 구분). 없으면 고르게 6장")
    parser.add_argument("--views", default="front,right,3/4,top", help="시점: front, right, back, 3/4, top")
    parser.add_argument("--wrists", action="store_true", help="두 손목 확대 그림을 함께 그린다")
    args = parser.parse_args()

    if not (args.motion_gen / "qa_render.py").is_file():
        print(f"오류: qa_render.py를 찾지 못했습니다: {args.motion_gen}", file=sys.stderr)
        raise SystemExit(1)
    if not args.anim.is_file():
        print(f"오류: 클립 파일이 없습니다: {args.anim}", file=sys.stderr)
        raise SystemExit(1)

    # 그 프로젝트의 모듈(hkxanim 포함)을 한 벌로 맞춰 쓰기 위해 경로 맨 앞에 둔다.
    sys.path.insert(0, str(args.motion_gen))
    import numpy as np
    import qa_render
    from hkxanim import Skeleton
    from skin import SkinMesh

    sk = Skeleton(str(args.motion_gen / "data" / "skeleton.xml"))
    mesh = SkinMesh(sk)
    anim = qa_render.load_anim(str(args.anim), sk)
    if args.frames:
        frames = [int(x) for x in args.frames.split(",")]
        bad = [f for f in frames if not 0 <= f < anim.nframes]
        if bad:
            print(f"오류: 프레임 번호가 범위를 벗어났습니다: {bad} (0~{anim.nframes - 1})", file=sys.stderr)
            raise SystemExit(1)
    else:
        frames = [int(x) for x in np.linspace(0, anim.nframes - 1, 6)]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    # 왼손 무기(AnimObjectL)는 이 모션에 없으므로 그리지 않는다.
    qa_render.contact_sheet(anim, sk, mesh, frames, str(args.out), args.anim.name, None,
                            tuple(args.views.split(",")), args.wrists, None)
    print(f"저장: {args.out} (프레임 {frames})")


if __name__ == "__main__":
    main()
