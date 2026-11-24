"""Stamp, wrap, and verify a SkyrimPoE package and its deployed copy."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "compiler"))
sys.path.insert(0, str(ROOT / "tools" / "esp"))

from compile_tree import write_tree_js_wrapper
from formid_ledger import GENERATION_EDITOR_ID, read_esp


PLUGIN_DIR = "SKSE/Plugins/SkyrimPoE"
UI_DIR = "PrismaUI/views/SkyrimPoE"
PACK_DIR = f"{PLUGIN_DIR}/packs"
TREE_PACK = f"{PACK_DIR}/tree_3_30.json"
RULES_PACK = f"{PACK_DIR}/rules.json"
PACK_PATHS = (f"{PACK_DIR}/core_3_30.json", TREE_PACK, f"{PACK_DIR}/items_3_30.json", RULES_PACK)
PACK_IDENTITY_KEYS = ("game", "gameVersion", "contentGeneration")
MANIFEST = f"{PLUGIN_DIR}/manifest.json"


def fail_on_walk_error(error: OSError) -> None:
    raise error


def list_files(root: Path) -> dict[str, Path]:
    if not root.is_dir():
        raise ValueError(f"폴더가 없습니다: {root}")
    files = {}
    for folder, dirs, names in os.walk(root, onerror=fail_on_walk_error):
        directory = Path(folder)
        # A deployed directory can be a Windows junction or a symbolic link.
        dirs[:] = [
            name for name in dirs
            if not (directory / name).is_symlink()
            and not getattr((directory / name).lstat(), "st_file_attributes", 0)
            & stat.FILE_ATTRIBUTE_REPARSE_POINT
        ]
        for name in names:
            path = directory / name
            relative = path.relative_to(root).as_posix()
            if os.path.normcase(relative) != os.path.normcase(MANIFEST):
                files[relative] = path
    return dict(sorted(files.items()))


def file_entry(relative: str, path: Path) -> dict:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
            size += len(block)
    return {"path": relative, "sha256": digest.hexdigest(), "size": size}


def pack_bytes(relative: str, data: dict) -> bytes:
    if relative in (f"{PACK_DIR}/core_3_30.json", RULES_PACK):
        text = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    else:
        text = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
        if relative != TREE_PACK:
            text += "\n"
    return text.encode("utf-8")


def identity(meta: dict, stamped: bool) -> dict:
    game = meta.get("game")
    version = meta.get("gameVersion")
    generation = meta.get("contentGeneration")
    if not isinstance(game, str) or not game.strip():
        raise ValueError("game은 비어 있지 않은 문자열이어야 합니다")
    if not isinstance(version, str) or not version.strip():
        raise ValueError("gameVersion은 비어 있지 않은 문자열이어야 합니다")
    if type(generation) is not int or generation < 0:
        raise ValueError("contentGeneration은 0 이상의 정수여야 합니다")
    result = {"game": game, "gameVersion": version, "contentGeneration": generation}
    if stamped:
        build_id = meta.get("buildId")
        if not isinstance(build_id, str) or not re.fullmatch(r"[0-9a-f]{16}", build_id):
            raise ValueError("buildId는 16자리 소문자 16진수여야 합니다")
        result = {"buildId": build_id, **result}
    return result


def read_packs(
    files: dict[str, Path], problems: list[str], *, stamped: bool, expected: dict | None = None,
) -> tuple[dict, dict]:
    packs = {}
    for relative in sorted(files.keys() - set(PACK_PATHS)):
        path = PurePosixPath(relative)
        if path.parent.as_posix().casefold() == PACK_DIR.casefold() and path.suffix.lower() == ".json":
            problems.append(f"{relative}: 허용되지 않은 추가 팩입니다")
    for relative in PACK_PATHS:
        try:
            if relative not in files:
                raise ValueError("필수 팩 파일이 없습니다")
            raw = files[relative].read_bytes()
            data = json.loads(raw)
            if not isinstance(data, dict) or not isinstance(data.get("meta"), dict):
                raise ValueError("meta 객체가 없습니다")
            packs[relative] = (raw, data)
            meta = data["meta"]
            if relative == RULES_PACK and not stamped:
                continue
            actual = identity(meta, stamped)
        except (OSError, ValueError) as error:
            problems.append(f"{relative}: {error}")
            continue
        if expected is None:
            expected = actual
        for key, value in expected.items():
            if actual[key] != value:
                problems.append(f"{relative}: {key} 불일치 (실제 {actual[key]!r}, 기대 {value!r})")
    return packs, expected or {}


def esp_generation(path: Path) -> int:
    markers = [record for record in read_esp(path).records if record.editor_id == GENERATION_EDITOR_ID]
    if not markers:
        return 0
    if len(markers) != 1 or markers[0].signature != "GLOB":
        raise ValueError(f"{GENERATION_EDITOR_ID}는 GLOB 하나여야 합니다")
    marker = markers[0]
    value = marker.global_value
    if marker.global_type != "s" or value is None or not value.is_integer() or value < 0:
        raise ValueError(f"{GENERATION_EDITOR_ID}의 FNAM은 short이고 FLTV는 0 이상의 정수여야 합니다")
    return int(value)


def stamp(package: Path, esp_name: str) -> list[str]:
    files = list_files(package)
    problems: list[str] = []
    packs, common = read_packs(files, problems, stamped=False)
    if "/" in esp_name or "\\" in esp_name or esp_name not in files:
        problems.append(f"패키지 루트에 ESP가 없습니다: {esp_name}")
    else:
        try:
            generation = esp_generation(files[esp_name])
            if common and generation != common["contentGeneration"]:
                problems.append(
                    f"{esp_name}: contentGeneration 불일치 "
                    f"(ESP {generation}, 팩 {common['contentGeneration']})"
                )
        except (OSError, ValueError) as error:
            problems.append(f"{esp_name}: {error}")
    if problems:
        return problems

    build_digest = hashlib.sha256()
    for relative, path in files.items():
        if relative in packs:
            raw, data = packs[relative]
            if relative == RULES_PACK:
                # The rules source is independent of the content generation.
                for key in (*PACK_IDENTITY_KEYS, "buildId"):
                    data["meta"].pop(key, None)
                raw = pack_bytes(relative, data)
            elif "buildId" in data["meta"]:
                del data["meta"]["buildId"]
                raw = pack_bytes(relative, data)
            digest = hashlib.sha256(raw).hexdigest()
        else:
            digest = file_entry(relative, path)["sha256"]
        build_digest.update(f"{relative}\0{digest}\n".encode("utf-8"))

    build_id = build_digest.hexdigest()[:16]
    for relative, (_, data) in packs.items():
        if relative == RULES_PACK:
            data["meta"].update({key: common[key] for key in PACK_IDENTITY_KEYS})
        data["meta"]["buildId"] = build_id
        if files[relative].is_symlink():
            raise ValueError(f"팩은 일반 파일이어야 합니다: {relative}")
    for relative, (_, data) in packs.items():
        files[relative].write_bytes(pack_bytes(relative, data))
    print(f"빌드 ID {build_id} 기록 완료 ({len(packs)}개 팩)")
    return []


def wrap_tree(package: Path) -> list[str]:
    files = list_files(package)
    problems: list[str] = []
    packs, _ = read_packs(files, problems, stamped=True)
    if TREE_PACK not in packs:
        problems.append(f"트리 팩이 없습니다: {TREE_PACK}")
    if not problems:
        write_tree_js_wrapper(files[TREE_PACK], package / UI_DIR / "tree-data.js")
        print(f"트리 래퍼 생성 완료 ({UI_DIR}/tree-data.js)")
    return problems


def write_manifest(package: Path) -> list[str]:
    files = list_files(package)
    problems: list[str] = []
    _, common = read_packs(files, problems, stamped=True)
    if problems:
        return problems

    entries = [file_entry(relative, path) for relative, path in files.items()]
    manifest = {"manifestVersion": 1, **common, "files": entries}
    (package / MANIFEST).write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8", newline="\n",
    )
    print(f"manifest 기록 완료 ({len(entries)}개 파일)")
    return []


def read_manifest(path: Path) -> tuple[dict, dict[str, dict], bytes]:
    raw = path.read_bytes()
    manifest = json.loads(raw)
    if not isinstance(manifest, dict) or type(manifest.get("manifestVersion")) is not int:
        raise ValueError(f"manifestVersion이 없습니다: {path}")
    if manifest["manifestVersion"] != 1 or not isinstance(manifest.get("files"), list):
        raise ValueError(f"지원하지 않는 manifest 구조: {path}")
    common = identity(manifest, stamped=True)
    entries = {}
    for entry in manifest["files"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            raise ValueError(f"manifest 항목의 path가 잘못되었습니다: {entry!r}")
        relative = entry["path"]
        parts = relative.split("/")
        if any(part in ("", ".", "..") for part in parts) or "\\" in relative or ":" in relative:
            raise ValueError(f"manifest 상대 경로가 잘못되었습니다: {relative!r}")
        if relative == MANIFEST or relative in entries:
            raise ValueError(f"manifest에 허용되지 않는 경로 또는 중복 경로: {relative}")
        digest, size = entry.get("sha256"), entry.get("size")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError(f"{relative}: manifest sha256이 잘못되었습니다")
        if type(size) is not int or size < 0:
            raise ValueError(f"{relative}: manifest size가 잘못되었습니다")
        entries[relative] = entry
    for relative in PACK_PATHS:
        if relative not in entries:
            raise ValueError(f"{relative}: manifest에 필수 팩이 없습니다")
    return common, entries, raw


def compare_files(entries: dict[str, dict], files: dict[str, Path], *, deployed: bool = False) -> list[str]:
    problems = []
    if deployed:
        files = {os.path.normcase(relative): path for relative, path in files.items()}
    for relative, expected in entries.items():
        key = os.path.normcase(relative) if deployed else relative
        if key not in files:
            problems.append(f"누락 파일: {relative}")
            continue
        try:
            actual = file_entry(relative, files[key])
        except OSError as error:
            problems.append(f"{relative}: {error}")
            continue
        for field in ("sha256", "size"):
            if actual[field] != expected[field]:
                problems.append(
                    f"{relative}: {field} 불일치 (실제 {actual[field]}, 기대 {expected[field]})"
                )
    return problems


def verify(package: Path) -> list[str]:
    common, entries, _ = read_manifest(package / MANIFEST)
    files = list_files(package)
    problems = compare_files(entries, files)
    problems.extend(f"목록 밖 파일: {relative}" for relative in sorted(files.keys() - entries.keys()))
    read_packs(files, problems, stamped=True, expected=common)
    if not problems:
        print(f"패키지 검사 통과 ({len(entries)}개 파일, 빌드 ID {common['buildId']})")
    return problems


def deploy_clean(package: Path, target: Path) -> list[str]:
    if not target.is_dir():
        return [f"배치 대상 폴더가 없습니다: {target}"]
    _, entries, manifest_bytes = read_manifest(package / MANIFEST)
    target_manifest = target / MANIFEST
    if not target_manifest.is_file():
        return [f"배치 대상에 manifest가 없습니다: {target_manifest}"]

    files = list_files(target)
    expected = {os.path.normcase(relative) for relative in entries}
    managed = tuple(os.path.normcase(f"{directory}/") for directory in (PLUGIN_DIR, UI_DIR))
    problems: list[str] = []
    for relative, path in files.items():
        key = os.path.normcase(relative)
        if key in expected:
            continue
        if key.startswith(managed):
            try:
                path.unlink()
                print(f"삭제: {relative}")
            except OSError as error:
                problems.append(f"삭제 실패: {relative}: {error}")
        else:
            print(f"남은 파일: {relative}")
    problems.extend(compare_files(entries, list_files(target), deployed=True))
    if target_manifest.read_bytes() != manifest_bytes:
        problems.append("배치 대상의 manifest가 패키지의 manifest와 다릅니다")
    if not problems:
        print(f"배치 검사 통과 ({len(entries)}개 파일)")
    return problems


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("stamp", "wrap-tree", "manifest", "verify", "deploy-clean"):
        command = commands.add_parser(name)
        command.add_argument("--package", type=Path, required=True)
        if name == "stamp":
            command.add_argument("--esp-name", required=True)
        elif name == "deploy-clean":
            command.add_argument("--target", type=Path, required=True)
    args = parser.parse_args()

    try:
        if args.command == "stamp":
            problems = stamp(args.package, args.esp_name)
        elif args.command == "wrap-tree":
            problems = wrap_tree(args.package)
        elif args.command == "manifest":
            problems = write_manifest(args.package)
        elif args.command == "verify":
            problems = verify(args.package)
        else:
            problems = deploy_clean(args.package, args.target)
    except (OSError, ValueError) as error:
        problems = [str(error)]
    for problem in problems:
        print(f"오류: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
