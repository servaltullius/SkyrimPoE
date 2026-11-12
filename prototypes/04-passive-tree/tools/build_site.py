"""화면 파일과 변환한 트리 데이터를 한 폴더로 모은다.

out/site/ 에 index.html, tree.js, tree.css 와 data/(tree-data.js, assets/)가 생긴다.
이 폴더를 그대로 브라우저로 열거나, Prisma UI의 뷰 폴더로 복사해 쓴다.

사용법:
    python -I build_site.py [--pob <PoB 폴더>] [--version 3_29] [--out <출력 폴더>]
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import convert_tree  # noqa: E402  (같은 폴더의 변환 스크립트)

DEFAULT_POB = Path("G:/poe/PathOfBuilding-2.67.2")
VIEW_FILES = ("index.html", "tree.js", "tree.css")


def main() -> int:
    parser = argparse.ArgumentParser(description="패시브 트리 화면을 out/site 에 조립한다.")
    parser.add_argument("--pob", type=Path, default=DEFAULT_POB, help="Path of Building Community 폴더")
    parser.add_argument("--version", default="3_29", help="TreeData 아래의 버전 폴더 이름")
    parser.add_argument("--out", type=Path, default=HERE.parent / "out" / "site", help="출력 폴더")
    args = parser.parse_args()

    tree_file = args.pob / "TreeData" / args.version / "tree.lua"
    if not tree_file.is_file():
        print(f"트리 데이터를 찾지 못함: {tree_file}", file=sys.stderr)
        return 2

    view_dir = HERE.parent / "view"
    missing = [name for name in VIEW_FILES if not (view_dir / name).is_file()]
    if missing:
        print(f"화면 파일이 없음: {', '.join(missing)}", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    for name in VIEW_FILES:
        shutil.copyfile(view_dir / name, args.out / name)
    summary = convert_tree.convert(args.pob, args.out / "data", args.version)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    if summary["jpeg_not_converted"]:
        print("주의: Pillow가 없어 JPEG 그림을 그대로 복사했습니다: " + ", ".join(summary["jpeg_not_converted"]),
              file=sys.stderr)
    print(f"조립함: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
