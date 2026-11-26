"""stamp.py의 세대 허용표와 원천 대조(sources) 시험(7단계 G1 계획 C1·C2·C4, 4.1 W6)."""
import json
from pathlib import Path
import re
import shutil
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "package"))
import stamp  # noqa: E402
from test_stamp import add_generation_marker  # noqa: E402

PACKS = Path("SKSE/Plugins/SkyrimPoE/packs")
UI = Path("PrismaUI/views/SkyrimPoE")


def build_package(directory: Path, generation: int) -> Path:
    """실제 원천으로 패키지의 팩·ESP 부분을 만들고 stamp·wrap-tree·manifest를 돈다."""
    package = directory / f"gen{generation}"
    pack_dir, esp_source = stamp.GENERATION_SOURCES[generation]
    (package / PACKS).mkdir(parents=True)
    (package / UI).mkdir(parents=True)
    for kind in ("core", "tree", "items"):
        shutil.copyfile(pack_dir / f"{kind}.json", package / PACKS / f"{kind}.json")
    shutil.copyfile(stamp.RULES_SOURCE, package / PACKS / "rules.json")
    shutil.copyfile(esp_source, package / "SkyrimPoE.esp")
    for command in (lambda: stamp.stamp(package, "SkyrimPoE.esp"), lambda: stamp.wrap_tree(package),
                    lambda: stamp.write_manifest(package)):
        assert command() == []
    return package


@pytest.fixture(scope="module")
def packages(tmp_path_factory):
    directory = tmp_path_factory.mktemp("sources")
    return {generation: build_package(directory, generation) for generation in (0, 1)}


def copy_package(package: Path, tmp_path: Path) -> Path:
    target = tmp_path / package.name
    shutil.copytree(package, target)
    return target


def restamp(package: Path) -> list[str]:
    """팩을 고친 뒤 buildId·manifest를 다시 맞춘다(sources가 내용만 보게)."""
    return [*stamp.stamp(package, "SkyrimPoE.esp"), *stamp.write_manifest(package)]


def edit_pack(package: Path, name: str, edit) -> None:
    path = package / PACKS / name
    data = json.loads(path.read_bytes())
    data["meta"].pop("buildId", None)
    edit(data)
    path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    assert restamp(package) == []


def test_python_allowlist_matches_the_dll_allowlist():
    source = (ROOT / "plugin/src/core/ContentCheck.cpp").read_text(encoding="utf-8")
    body = source[source.index("kSupportedContents[] = {"):source.index("};", source.index("kSupportedContents[] = {"))]
    rows = re.findall(r'\{ "(\w+)", (\d+), "([\d.]+)", \{\{(.*?)\}\} \}', body, re.S)
    dll = {}
    for game, generation, game_version, schemas in rows:
        pairs = re.findall(r'\{ "skyrimpoe\.(\w+)", (\d+) \}', schemas)
        dll[(game, int(generation))] = {"gameVersion": game_version, "schemas": {
            kind: (f"skyrimpoe.{kind}", int(version)) for kind, version in pairs}}
    assert dll == stamp.SUPPORTED_CONTENTS


@pytest.mark.parametrize("generation", [0, 1])
def test_packages_built_from_each_generation_source_pass(packages, generation):
    assert stamp.check_sources(packages[generation]) == []


def test_generation_one_items_source_is_an_empty_pack():
    items = json.loads((stamp.GENERATION_SOURCES[1][0] / "items.json").read_bytes())
    assert set(items) == {"meta"}
    assert (items["meta"]["schema"], items["meta"]["schemaVersion"], items["meta"]["game"],
            items["meta"]["contentGeneration"]) == ("skyrimpoe.items", 3, "poe2", 1)


def test_rejects_a_pack_whose_content_differs_from_its_source(packages, tmp_path):
    package = copy_package(packages[1], tmp_path)
    edit_pack(package, "core.json", lambda data: data["tables"]["expectedDefense"][37].update(bestArmour=903.0))
    problems = stamp.check_sources(package)
    assert any("core.json: 패키지 내용이 원천 data/packs/gen1/core.json와 다릅니다" in p for p in problems), problems


def test_rejects_a_generation_zero_pack_mixed_into_generation_one(packages, tmp_path):
    package = copy_package(packages[1], tmp_path)
    tree = json.loads((stamp.GENERATION_SOURCES[0][0] / "tree.json").read_bytes())
    tree["meta"].update(game="poe2", gameVersion="0.5", contentGeneration=1, schemaVersion=2)
    (package / PACKS / "tree.json").write_text(json.dumps(tree, ensure_ascii=False), encoding="utf-8")
    assert restamp(package) == []
    problems = stamp.check_sources(package)
    assert any("tree.json: 패키지 내용이 원천" in p for p in problems), problems


@pytest.mark.parametrize("kind,schema_version", [("core", 1), ("items", 2), ("tree", 1)])
def test_rejects_generation_zero_schema_versions_in_generation_one(packages, tmp_path, kind, schema_version):
    package = copy_package(packages[1], tmp_path)
    edit_pack(package, f"{kind}.json", lambda data: data["meta"].update(schemaVersion=schema_version))
    problems = stamp.check_sources(package)
    assert any(f"{kind}.json: 스키마" in p and f"기대 'skyrimpoe.{kind}'" in p for p in problems), problems


def test_rejects_the_wrong_esp_for_the_generation(packages, tmp_path):
    package = copy_package(packages[0], tmp_path)
    shutil.copyfile(stamp.GENERATION_SOURCES[1][1], package / "SkyrimPoE.esp")
    problems = stamp.check_sources(package)
    assert any("SkyrimPoE.esp: 세대 0 ESP 원천" in p for p in problems), problems
    assert any("SkyrimPoE.esp: 세대 GLOB 1이 세대 0와 맞지 않습니다" in p for p in problems), problems


@pytest.fixture
def source_copy(tmp_path, monkeypatch):
    """원천을 임시 폴더에 복사하고 stamp가 그 원천을 보게 한다."""
    root = tmp_path / "sources"
    pack_dir = root / "gen1"
    shutil.copytree(stamp.GENERATION_SOURCES[1][0], pack_dir)
    shutil.copyfile(stamp.RULES_SOURCE, root / "rules.json")
    monkeypatch.setattr(stamp, "GENERATION_SOURCES", {**stamp.GENERATION_SOURCES, 1: (pack_dir, stamp.GENERATION_SOURCES[1][1])})
    monkeypatch.setattr(stamp, "RULES_SOURCE", root / "rules.json")
    monkeypatch.setattr(stamp, "ROOT", tmp_path)
    return pack_dir, root / "rules.json"


def sources_after_source_edit(packages, tmp_path, path: Path, edit) -> list[str]:
    data = json.loads(path.read_bytes())
    edit(data)
    path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    package = copy_package(packages[1], tmp_path)
    name = "rules.json" if path.name == "rules.json" else path.name
    shutil.copyfile(path, package / PACKS / name)
    assert restamp(package) == []
    return stamp.check_sources(package)


def test_rejects_generation_one_items_with_content(packages, tmp_path, source_copy, monkeypatch):
    monkeypatch.setattr(stamp, "assemble_core", lambda root: (ROOT / "data/packs/gen1/core.json").read_bytes())
    pack_dir, _ = source_copy
    problems = sources_after_source_edit(packages, tmp_path, pack_dir / "items.json",
                                         lambda data: data.update(affixes=[]))
    assert any("items.json: 세대 1 items는 메타만 있는 빈 팩이어야 합니다" in p for p in problems), problems


def test_rejects_generation_one_core_that_is_not_the_assembled_core(packages, tmp_path, source_copy, monkeypatch):
    monkeypatch.setattr(stamp, "assemble_core", lambda root: (ROOT / "data/packs/gen1/core.json").read_bytes())
    pack_dir, _ = source_copy
    problems = sources_after_source_edit(packages, tmp_path, pack_dir / "core.json",
                                         lambda data: data["tables"].update(monsterDamage=[]))
    assert any("세대 1 core가 조립 도구" in p for p in problems), problems


def test_rejects_generation_one_tree_classes_outside_the_baseline(packages, tmp_path, source_copy, monkeypatch):
    monkeypatch.setattr(stamp, "assemble_core", lambda root: (ROOT / "data/packs/gen1/core.json").read_bytes())
    pack_dir, _ = source_copy
    problems = sources_after_source_edit(packages, tmp_path, pack_dir / "tree.json",
                                         lambda data: data["classes"][0].update(contentId="cls.Other"))
    # 트리 생성기와 같은 의미 지문 검증이 바뀐 클래스 ID를 잡는다.
    assert any("tree.json: 세대 1 의미 지문 기준선과 맞지 않습니다" in p and "cls ID cls.Ranger" in p
               for p in problems), problems


def test_rejects_a_rules_source_with_generation_identity(packages, tmp_path, source_copy, monkeypatch):
    monkeypatch.setattr(stamp, "assemble_core", lambda root: (ROOT / "data/packs/gen1/core.json").read_bytes())
    _, rules = source_copy
    problems = sources_after_source_edit(packages, tmp_path, rules,
                                         lambda data: data["meta"].update(contentGeneration=1))
    assert any("공용 rules 원천에 세대 표식이 있습니다" in p for p in problems), problems


def test_stamp_rejects_a_zero_valued_glob_in_a_generation_zero_package(packages, tmp_path):
    package = copy_package(packages[0], tmp_path)
    add_generation_marker(package / "SkyrimPoE.esp", 0)
    problems = stamp.stamp(package, "SkyrimPoE.esp")
    assert any("세대 0 ESP에는 SPOE_ContentGeneration GLOB이 없어야 합니다" in p for p in problems), problems


def test_rejects_values_that_differ_only_in_json_type(packages, tmp_path):
    # 파이썬에서는 true == 1 == 1.0이지만 DLL은 정수·불 자리를 엄격히 읽는다(W6 검토 지적 1).
    for edit in (lambda data: data["gems"]["gem.Spark"].update(ready=1),
                 lambda data: data["tables"]["expectedDefense"][0].update(level=1.0)):
        package = copy_package(packages[1], tmp_path / str(id(edit)))
        edit_pack(package, "core.json", edit)
        problems = stamp.check_sources(package)
        assert any("core.json: 패키지 내용이 원천" in p for p in problems), problems
    package = copy_package(packages[1], tmp_path / "schema")
    edit_pack(package, "items.json", lambda data: data["meta"].update(schemaVersion=3.0))
    problems = stamp.check_sources(package)
    assert any("items.json: 스키마" in p for p in problems), problems


def test_reports_a_malformed_rules_source_instead_of_raising(packages, tmp_path, source_copy):
    _, rules = source_copy
    rules.write_text("[]", encoding="utf-8")
    problems = stamp.check_sources(copy_package(packages[1], tmp_path))
    assert any("원천을 읽지 못했습니다" in p for p in problems), problems


def test_rejects_generation_one_tree_ids_outside_the_baseline(packages, tmp_path, source_copy, monkeypatch):
    monkeypatch.setattr(stamp, "assemble_core", lambda root: (ROOT / "data/packs/gen1/core.json").read_bytes())
    pack_dir, _ = source_copy

    def add_node(data):
        first = data["nodes"][0]
        data["nodes"].append({**first, "id": 999999, "adj": [first["id"]]})
        first["adj"].append(999999)
    problems = sources_after_source_edit(packages, tmp_path, pack_dir / "tree.json", add_node)
    assert any("기준선에 없는 node ID" in p for p in problems), problems
