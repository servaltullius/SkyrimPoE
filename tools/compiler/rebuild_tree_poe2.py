#!/usr/bin/env python3
"""Regenerate the generation-one pack and reports from PoB and the Korean client."""

import argparse
import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from compile_tree_poe2 import (
    DEFAULT_GAME, DEFAULT_POB, MIXED_PATH, PACK_PATH, PARSE_PATH, REPORT_PATH, ROOT,
    compile_source, finalize_tree, json_bytes, write_atomic,
)


def rebuild(pob: Path, game: Path, tests: Path, directory: Path) -> dict[Path, bytes]:
    directory.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="tree-poe2-", dir=directory) as temporary:
        directory = Path(temporary)
        source_path = directory / "source.json"
        parsed_path = directory / "parsed.json"
        mixed_path = directory / "mixed.json"
        source_path.write_bytes(json_bytes(compile_source(pob, game)))
        subprocess.run([tests.resolve(), "--write-passive-parse-report", source_path, parsed_path, mixed_path], check=True)
        parsed = parsed_path.read_bytes()
        mixed = mixed_path.read_bytes()
        pack, report = finalize_tree(json.loads(source_path.read_bytes()), json.loads(parsed), json.loads(mixed))
    return {
        PACK_PATH: json_bytes(pack),
        REPORT_PATH: json_bytes(report, indent=2),
        PARSE_PATH: parsed,
        MIXED_PATH: mixed,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pob", type=Path, default=DEFAULT_POB)
    parser.add_argument("--game", type=Path, default=DEFAULT_GAME)
    parser.add_argument("--tests", type=Path, required=True)
    parser.add_argument("--check", action="store_true", help="Compare with the checked-in files")
    args = parser.parse_args()
    try:
        for path, contents in rebuild(args.pob, args.game, args.tests, ROOT / "build").items():
            if args.check:
                if path.read_bytes() != contents:
                    raise ValueError(f"Regenerated file differs: {path.relative_to(ROOT)}")
            else:
                write_atomic(path, contents)
            print(f"{'Verified' if args.check else 'Wrote'} {path.relative_to(ROOT)} ({len(contents)} bytes)")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
