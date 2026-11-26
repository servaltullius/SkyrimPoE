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
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "compiler"))
sys.path.insert(0, str(ROOT / "tools" / "esp"))
sys.path.insert(0, str(ROOT / "tools" / "poe2"))

from assemble_core import assemble_core
from compile_tree_poe2 import validate_id_baseline
from compile_tree import write_tree_js_wrapper
import formid_ledger
from formid_ledger import GENERATION_EDITOR_ID, read_esp
from id_baseline import BASELINE_PATH


PLUGIN_DIR = "SKSE/Plugins/SkyrimPoE"
UI_DIR = "PrismaUI/views/SkyrimPoE"
PACK_DIR = f"{PLUGIN_DIR}/packs"
LEGACY_GEN1_GEMS_PREP = f"{PACK_DIR}/gen1/gems.json"
TREE_PACK = f"{PACK_DIR}/tree.json"
RULES_PACK = f"{PACK_DIR}/rules.json"
PACK_PATHS = (f"{PACK_DIR}/core.json", TREE_PACK, f"{PACK_DIR}/items.json", RULES_PACK)
PACK_IDENTITY_KEYS = ("game", "gameVersion", "contentGeneration")
MANIFEST = f"{PLUGIN_DIR}/manifest.json"
PACK_KINDS = ("core", "tree", "items", "rules")

# DLL의 kSupportedContents(plugin/src/core/ContentCheck.cpp)와 같아야 한다(test_stamp_sources.py가 대조).
SUPPORTED_CONTENTS = {
    ("poe1", 0): {"gameVersion": "3.30", "schemas": {
        "core": ("skyrimpoe.core", 1), "tree": ("skyrimpoe.tree", 1),
        "items": ("skyrimpoe.items", 2), "rules": ("skyrimpoe.rules", 1),
    }},
    ("poe2", 1): {"gameVersion": "0.5", "schemas": {
        "core": ("skyrimpoe.core", 2), "tree": ("skyrimpoe.tree", 2),
        "items": ("skyrimpoe.items", 3), "rules": ("skyrimpoe.rules", 1),
    }},
}
# 결정 246: 세대마다 패키지에 넣는 원천(팩 폴더와 ESP).
GENERATION_SOURCES = {
    0: (ROOT / "data/packs/gen0", ROOT / "esp/gen0/SkyrimPoE.esp"),
    1: (ROOT / "data/packs/gen1", ROOT / "esp/SkyrimPoE.esp"),
}
RULES_SOURCE = ROOT / "data/packs/rules.json"
# 결정 239·244: 세대 1 items는 메타만 있는 빈 팩이다.
EMPTY_ITEMS_META_KEYS = {"schema", "schemaVersion", "game", "gameVersion", "contentGeneration", "buildId"}


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
    if relative in (f"{PACK_DIR}/core.json", RULES_PACK):
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
        if relative.casefold().startswith(f"{PACK_DIR.casefold()}/") and path.suffix.lower() == ".json":
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


def esp_generation(path: Path) -> int | None:
    """세대 GLOB 값. GLOB이 없으면 None(세대 0 형식)이고, 값 0인 GLOB과 구별한다."""
    markers = [record for record in read_esp(path).records if record.editor_id == GENERATION_EDITOR_ID]
    if not markers:
        return None
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
            marker = esp_generation(files[esp_name])
            generation = 0 if marker is None else marker
            if common and generation != common["contentGeneration"]:
                problems.append(
                    f"{esp_name}: contentGeneration 불일치 "
                    f"(ESP {generation}, 팩 {common['contentGeneration']})"
                )
            elif common and common["contentGeneration"] == 0 and marker is not None:
                problems.append(f"{esp_name}: 세대 0 ESP에는 {GENERATION_EDITOR_ID} GLOB이 없어야 합니다")
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
    if "staged" in manifest:
        raise ValueError(f"manifest에 허용되지 않는 staged 항목이 있습니다: {path}")
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


def verify(package: Path, *, release: bool = False) -> list[str]:
    del release
    common, entries, _ = read_manifest(package / MANIFEST)
    files = list_files(package)
    problems = compare_files(entries, files)
    if os.path.lexists(package / LEGACY_GEN1_GEMS_PREP) or any(
        relative.casefold() == LEGACY_GEN1_GEMS_PREP.casefold() for relative in files
    ):
        problems.append(f"{LEGACY_GEN1_GEMS_PREP}: 패키지에는 옛 준비 파일을 넣을 수 없습니다")
    problems.extend(f"목록 밖 파일: {relative}" for relative in sorted(files.keys() - entries.keys()))
    read_packs(files, problems, stamped=True, expected=common)
    if not problems:
        print(f"패키지 검사 통과 ({len(entries)}개 파일, 빌드 ID {common['buildId']})")
    return problems


def without_stamp(relative: str, data: dict) -> dict:
    """패키지 팩에서 stamp가 넣은 값을 빼 원천과 비교할 수 있게 한다."""
    data = json.loads(json.dumps(data))
    data["meta"].pop("buildId", None)
    if relative == RULES_PACK:
        for key in PACK_IDENTITY_KEYS:
            data["meta"].pop(key, None)
    return data


def same_json(left: Any, right: Any) -> bool:
    """JSON 값을 타입까지 비교한다. 파이썬 ==와 달리 true·1·1.0을 서로 다르게 본다(DLL은 정수 자리에 정수만 받음)."""
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(same_json(left[key], right[key]) for key in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(same_json(a, b) for a, b in zip(left, right))
    return left == right


def check_generation_content(generation: int, sources: dict[str, tuple[bytes, Any]], problems: list[str]) -> None:
    """원천 팩 내용이 그 세대의 것인지 본다(G1 계획 C2). 젬 카탈로그·기대 방어표를 PoB에서 다시 뽑는 대조는
    PoB가 있어야 해서 CTest(tools/compiler·tools/poe2의 --check 시험)가 맡는다."""
    if generation != 1:
        return
    try:
        assembled = assemble_core(ROOT)
    except (OSError, ValueError) as error:
        problems.append(f"세대 1 core 조립 실패: {error}")
    else:
        if sources["core"][0] != assembled:
            problems.append("core.json: 세대 1 core가 조립 도구(tools/poe2/assemble_core.py) 출력과 바이트가 다릅니다")
    items = sources["items"][1]
    meta = items.get("meta")
    if set(items) != {"meta"} or not isinstance(meta, dict) or not set(meta) <= EMPTY_ITEMS_META_KEYS:
        problems.append(f"items.json: 세대 1 items는 메타만 있는 빈 팩이어야 합니다 (최상위 {sorted(items)})")
    tree = sources["tree"][1]
    try:
        baseline = json.loads(BASELINE_PATH.read_bytes())
        entries = baseline["entries"]
        if not isinstance(entries, dict) or not all(isinstance(entries.get(kind), dict) for kind in ("cls", "asc", "node")):
            raise ValueError("cls·asc·node 항목이 객체가 아닙니다")
    except (OSError, ValueError, KeyError, TypeError) as error:
        problems.append(f"의미 지문 기준선을 읽지 못했습니다: {error}")
        return
    try:
        # 트리 생성기와 같은 의미 지문 검증: 기준선의 ID는 같은 뜻으로 남아 있어야 한다.
        validate_id_baseline(tree, baseline)
    except (ValueError, KeyError, TypeError, AttributeError) as error:
        problems.append(f"tree.json: 세대 1 의미 지문 기준선과 맞지 않습니다: {error}")
        return
    classes = tree.get("classes", [])
    present = {
        "cls": {cls.get("contentId") for cls in classes if isinstance(cls, dict)},
        "asc": {asc.get("id") for cls in classes if isinstance(cls, dict)
                for base in cls.get("ascendancies", []) for asc in (base, *base.get("replacements", []))},
        "node": {str(node.get("id")) for node in tree.get("nodes", []) if isinstance(node, dict)},
    }
    for kind, identifiers in present.items():
        extra = identifiers - set(entries[kind])
        if extra:
            problems.append(f"tree.json: 세대 1 의미 지문 기준선에 없는 {kind} ID가 있습니다 ({sorted(extra)[:5]})")


def check_sources(package: Path) -> list[str]:
    """패키지의 팩·ESP가 고른 세대의 원천·장부와 같은지 본다(G1 계획 C2·C4, 4.1 W5·W6)."""
    files = list_files(package)
    problems: list[str] = []
    packs, common = read_packs(files, problems, stamped=True)
    if problems:
        return problems
    key = (common["game"], common["contentGeneration"])
    supported = SUPPORTED_CONTENTS.get(key)
    if supported is None:
        return [f"지원하지 않는 게임·세대 조합: {key}"]
    generation = common["contentGeneration"]
    if common["gameVersion"] != supported["gameVersion"]:
        problems.append(f"gameVersion {common['gameVersion']!r}이 {key}의 {supported['gameVersion']!r}와 다릅니다")
    pack_dir, esp_source = GENERATION_SOURCES[generation]
    sources = {}
    for kind, relative in zip(PACK_KINDS, PACK_PATHS):
        data = packs[relative][1]
        meta = data["meta"]
        expected_schema = supported["schemas"][kind]
        if (meta.get("schema"), meta.get("schemaVersion")) != expected_schema or type(meta.get("schemaVersion")) is not int:
            problems.append(f"{relative}: 스키마 {meta.get('schema')!r} {meta.get('schemaVersion')!r}, "
                            f"기대 {expected_schema[0]!r} {expected_schema[1]!r}")
        source_path = RULES_SOURCE if kind == "rules" else pack_dir / f"{kind}.json"
        try:
            raw = source_path.read_bytes()
            source = json.loads(raw)
            if not isinstance(source, dict) or not isinstance(source.get("meta"), dict):
                raise ValueError("최상위 객체와 meta 객체가 있어야 합니다")
        except (OSError, ValueError) as error:
            problems.append(f"{source_path.relative_to(ROOT).as_posix()}: 원천을 읽지 못했습니다: {error}")
            continue
        if kind == "rules":
            stamped_keys = [key for key in (*PACK_IDENTITY_KEYS, "buildId") if key in source["meta"]]
            if stamped_keys:
                problems.append(f"{source_path.relative_to(ROOT).as_posix()}: 공용 rules 원천에 세대 표식이 있습니다 "
                                f"({stamped_keys})")
        sources[kind] = (raw, source)
        if not same_json(without_stamp(relative, data), source):
            problems.append(f"{relative}: 패키지 내용이 원천 {source_path.relative_to(ROOT).as_posix()}와 다릅니다")
    if problems:
        return problems
    check_generation_content(generation, sources, problems)

    esp = package / "SkyrimPoE.esp"
    try:
        if esp.read_bytes() != esp_source.read_bytes():
            problems.append(f"SkyrimPoE.esp: 세대 {generation} ESP 원천 "
                            f"{esp_source.relative_to(ROOT).as_posix()}과 다릅니다")
        marker = esp_generation(esp)
    except (OSError, ValueError) as error:
        return [*problems, f"SkyrimPoE.esp: {error}"]
    if (generation == 0 and marker is not None) or (generation > 0 and marker != generation):
        problems.append(f"SkyrimPoE.esp: 세대 GLOB {marker!r}이 세대 {generation}와 맞지 않습니다")
    ledger_problems = formid_ledger.check(
        formid_ledger.DEFAULT_LEDGER, ROOT / "esp/SkyrimPoE.esp" if generation == 0 else esp,
        formid_ledger.DEFAULT_RECORDS,
    )
    problems.extend(f"FormID 장부: {problem}" for problem in ledger_problems)
    if not problems:
        print(f"원천 대조 통과 (세대 {generation}, 팩 {len(PACK_PATHS)}개와 ESP, 장부)")
    return problems


def deploy_clean(package: Path, target: Path) -> list[str]:
    if not target.is_dir():
        return [f"배치 대상 폴더가 없습니다: {target}"]
    _, entries, manifest_bytes = read_manifest(package / MANIFEST)
    target_manifest = target / MANIFEST
    if not target_manifest.is_file():
        return [f"배치 대상에 manifest가 없습니다: {target_manifest}"]

    files = list_files(target)
    expected = {os.path.normcase(relative) for relative in entries.keys()}
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
    remaining = list_files(target)
    problems.extend(compare_files(entries, remaining, deployed=True))
    if os.path.lexists(target / LEGACY_GEN1_GEMS_PREP):
        problems.append(f"{LEGACY_GEN1_GEMS_PREP}: 배치 대상에 옛 준비 파일이 남았습니다")
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
    for name in ("stamp", "wrap-tree", "manifest", "verify", "sources", "deploy-clean"):
        command = commands.add_parser(name)
        command.add_argument("--package", type=Path, required=True)
        if name == "stamp":
            command.add_argument("--esp-name", required=True)
        elif name == "verify":
            command.add_argument("--release", action="store_true")
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
            problems = verify(args.package, release=args.release)
        elif args.command == "sources":
            problems = check_sources(args.package)
        else:
            problems = deploy_clean(args.package, args.target)
    except (OSError, ValueError) as error:
        problems = [str(error)]
    for problem in problems:
        print(f"오류: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
