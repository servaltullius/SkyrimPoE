"""Verify the generation-one tree, geometry and coverage without game runtime."""

from collections import Counter
from copy import deepcopy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess

import pytest

from compile_tree_poe2 import (
    BASELINE_PATH, DEFAULT_GAME, DEFAULT_POB, KIND_ASCEND_START, KIND_ATTRIBUTE, KIND_CLASS_START,
    KIND_KEYSTONE, KIND_NOTABLE, KIND_NORMAL, KIND_SOCKET, PACK_PATH, REPORT_PATH,
    ROOT, TREE_FILE, PARSE_PATH, KoreanTables, compile_tree, json_bytes, stat_categories, stat_shape,
    validate_id_baseline, validate_parse_display,
)


@pytest.fixture(scope="module")
def pack():
    return json.loads(PACK_PATH.read_bytes())


@pytest.fixture(scope="module")
def nodes(pack):
    return {node["id"]: node for node in pack["nodes"]}


@pytest.fixture(scope="module")
def report():
    return json.loads(REPORT_PATH.read_bytes())


def test_main_graph_denominators(pack, nodes, report):
    starts = {klass["startNodeId"] for klass in pack["classes"]}
    main = {node_id: node for node_id, node in nodes.items()
            if not node.get("asc") and node_id not in starts}
    seeds = {neighbor for klass in pack["classes"] for neighbor in klass["startNeighbors"]}

    def traverse(allow_unlock):
        stack = list(seeds)
        visited = set()
        while stack:
            node_id = stack.pop()
            if node_id in visited or (main[node_id].get("unlock") and not allow_unlock):
                continue
            visited.add(node_id)
            stack.extend(main[node_id]["adj"])
        return visited

    ordinary = traverse(False)
    conditional = traverse(True)
    assert len(ordinary) == report["graph"]["ordinaryReachable"] == 3909
    assert Counter(main[node_id]["k"] for node_id in ordinary) == {
        KIND_NORMAL: 2647, KIND_NOTABLE: 924, KIND_ATTRIBUTE: 293,
        KIND_KEYSTONE: 33, KIND_SOCKET: 12,
    }
    assert len(conditional) == report["graph"]["reachableWithUnlocks"] == 4087
    assert all(main[node_id]["unlock"] for node_id in conditional - ordinary)
    disconnected = [node for node in main.values() if not node["adj"]]
    assert {node["id"] for node in disconnected} == main.keys() - conditional
    assert len(disconnected) == 22
    assert sum(bool(node.get("recipeOnly")) for node in disconnected) == 16
    assert sum(bool(node.get("alias")) for node in disconnected) == 6
    assert all(node["k"] == KIND_SOCKET and node["jewelSocket"]
               for node in disconnected if node.get("alias"))


def test_classes_ascendancies_and_start_links(pack, nodes):
    classes = pack["classes"]
    assert [klass["id"] for klass in classes] == list(range(8))
    assert [klass["source"]["integerId"] for klass in classes] == [2, 8, 6, 9, 11, 1, 7, 10]
    by_name = {klass["name"]: klass for klass in classes}
    for first, second in (("Ranger", "Huntress"), ("Witch", "Sorceress")):
        assert by_name[first]["startNodeId"] == by_name[second]["startNodeId"]
        assert by_name[first]["startNeighbors"] == by_name[second]["startNeighbors"]
    starts = {klass["startNodeId"] for klass in classes}
    assert len(starts) == 6
    for klass in classes:
        assert klass["contentId"] == f"cls.{klass['name']}"
        start = nodes[klass["startNodeId"]]
        assert start["k"] == KIND_CLASS_START and not start["adj"]
        assert len(set(klass["startNeighbors"])) == 2
        assert all(not nodes[node_id].get("asc") for node_id in klass["startNeighbors"])

    ascendancies = {asc["id"]: asc for klass in classes for asc in klass["ascendancies"]}
    assert len(ascendancies) == len({asc["startNodeId"] for asc in ascendancies.values()}) == 22
    for asc_id, asc in ascendancies.items():
        assert re.fullmatch(r"asc\.[A-Za-z0-9._]+", asc_id)
        assert nodes[asc["startNodeId"]]["k"] == KIND_ASCEND_START
        assert nodes[asc["startNodeId"]]["asc"] == asc_id
        assert nodes[asc["startNodeId"]]["adj"]
    lich = ascendancies["asc.Lich"]
    replacement, = lich["replacements"]
    assert replacement["id"] == "asc.AbyssalLich"
    assert replacement["startNodeId"] == lich["startNodeId"]
    for node in nodes.values():
        assert node["id"] not in node["adj"]
        assert node["adj"] == sorted(set(node["adj"]))
        for neighbor in node["adj"]:
            assert neighbor not in starts
            assert node["id"] in nodes[neighbor]["adj"]
            assert node.get("asc") == nodes[neighbor].get("asc")
    for node_id, klass_name in ((12795, "Sorceress"), (57253, "Warrior")):
        assert nodes[node_id]["grantStart"] == by_name[klass_name]["id"]
        assert nodes[node_id]["grantPts"] == 4
        assert set(nodes[node_id]["adj"]).isdisjoint(by_name[klass_name]["startNeighbors"])


def test_node_options_and_source_flags(pack, nodes):
    assert len(nodes) == pack["meta"]["nodeCount"] == 4544
    assert pack["meta"]["onlyImageExcluded"] == 368
    assert pack["masteryEffects"] == pack["masteryGroups"] == {}
    assert sum(bool(node.get("variants")) for node in nodes.values()) == 78
    assert nodes[1143]["variants"]["cls.Druid"]["stats"] == ["Regenerate 0.2% of maximum Life per second"]
    assert nodes[23710]["variants"]["asc.AbyssalLich"]["name"] == "Abyssal Lich"
    assert sum(bool(node.get("freeAlloc")) for node in nodes.values()) == 3
    assert sum(bool(node.get("mc")) for node in nodes.values()) == 5
    assert sum(bool(node.get("mco")) for node in nodes.values()) == 15
    assert nodes[17788]["k"] == KIND_NOTABLE and nodes[17788]["jewelSocket"]
    assert nodes[479]["unlock"] == [5571]
    assert not nodes[5571].get("deferred")
    assert any(len(node.get("unlock", [])) == 3 for node in nodes.values())
    assert all(prerequisite in nodes for node in nodes.values() for prerequisite in node.get("unlock", []))
    for node in nodes.values():
        if node.get("mco"):
            assert node["mcParent"] in node["adj"] and nodes[node["mcParent"]]["mc"]
        if node["k"] == KIND_ATTRIBUTE:
            assert {option["id"] for option in node["attrOptions"]} == {26297, 14927, 57022}
            assert {option["stats"][0] for option in node["attrOptions"]} == {
                "+5 to Strength", "+5 to Dexterity", "+5 to Intelligence",
            }


def test_connector_geometry_against_pob_samples(pack):
    edges = {}
    for a, b, ascendancy, *arc in pack["visualEdges"]:
        assert a < b and len(arc) in (0, 5)
        first, second = pack["nodes"][a], pack["nodes"][b]
        pair = first["id"], second["id"]
        assert pair not in edges
        edges[pair] = arc
        assert second["id"] in first["adj"] and first["id"] in second["adj"]
        assert ascendancy == bool(first.get("asc"))
        if arc:
            cx, cy, radius, start, end = arc
            assert 0 <= end - start <= math.pi + 1e-8
            endpoints = [(cx + radius * math.cos(angle), cy + radius * math.sin(angle))
                         for angle in (start, end)]
            for node in (first, second):
                assert min(math.hypot(node["x"] - x, node["y"] - y) for x, y in endpoints) < 1e-5
    assert len(edges) == 5147 and sum(bool(arc) for arc in edges.values()) == 2771
    # PoB 0.23.1 BuildConnector with BuildArc intercepted before texture clipping.
    assert edges[16, 41619] == pytest.approx(
        [15496.3570076085, 4735.1113866886, 251, -0.2527662703, 0.7764529725], abs=1e-6, rel=0)
    assert edges[74, 24475] == pytest.approx(
        [16169.6057753184, -6726.1220896859, 1322, 1.7416481213, 2.1070576502], abs=1e-6, rel=0)
    # The two PoB texture arcs cover the same 120 degree span as one canvas arc.
    assert edges[60, 58426] == pytest.approx(
        [8971.45, 474.23, 162, 1.8325957146, 3.9269908170], abs=1e-6, rel=0)
    assert edges[483, 12601] == []  # explicit straight sentinel
    assert edges[8810, 33751] == []  # the chord exceeds the specified diameter


def test_stale_parser_lines_are_rejected():
    valid = {"node": [{"line": "+5 to Strength", "status": "full", "stats": {"flatStr": 5}}]}
    assert validate_parse_display(valid, "node", "", {"stats": ["+5 to Strength"]}) == valid["node"]
    with pytest.raises(ValueError, match="stale stat text"):
        validate_parse_display(valid, "node", "", {"stats": ["+6 to Strength"]})


def test_coverage_counts_and_keystone_review(pack, nodes, report):
    assert stat_categories("12% increased Critical Hit Chance") == ("critical",)
    assert stat_categories("+12 to maximum Life") == ("life",)
    assert stat_shape("+0.2% to Critical Hit Chance per 10 Intelligence") == (
        "+#% to Critical Hit Chance per # Intelligence")
    coverage = report["coverage"]
    assert set(coverage["all"]) == {"full", "partial", "unsupported", "mappingOnly"}
    assert coverage["all"]["mappingOnly"] == coverage["ordinaryPath"]["mappingOnly"] == 3
    assert all(coverage["all"][key] >= coverage["decision213"][key] for key in coverage["all"])
    stats = sum(coverage["all"].values())
    inherited = sum(len(variant["stats"]) for node in nodes.values()
                    for variant in node.get("variants", {}).values() if variant["stats"] == node["stats"])
    assert stats + coverage["attributeDisplayLinesExcluded"] + inherited == pack["meta"]["statLinesTotal"]
    partial = coverage["partialShapes"]
    assert sum(shape["lineCount"] for shape in partial) == coverage["all"]["partial"]
    assert all(0 <= shape["ordinaryNodeCount"] <= shape["nodeCount"] <= shape["lineCount"]
               for shape in partial)
    assert [shape["nodeCount"] for shape in partial] == sorted(
        (shape["nodeCount"] for shape in partial), reverse=True)
    keystones = {entry["id"]: entry for entry in coverage["keystones"]}
    assert keystones.keys() == {node["id"] for node in nodes.values() if node["k"] == KIND_KEYSTONE}
    assert Counter(entry["review"] for entry in keystones.values()) == {
        "implemented": 12, "mappingOnly": 1, "deferred": 20,
    }
    assert coverage["decision213OrdinaryPath"]["partial"] == 0
    unsupported = coverage["unsupportedShapes"]
    assert sum(shape["lineCount"] for shape in unsupported) == coverage["all"]["unsupported"]
    assert all(shape["reason"] for shape in unsupported)
    assert sum(reason["lines"] for reason in coverage["unsupportedByReason"].values()) == coverage["all"]["unsupported"]
    assert sum(reason["ordinaryLines"] for reason in coverage["unsupportedByReason"].values()) == (
        coverage["ordinaryPath"]["unsupported"]
    )
    assert "해당 효과를 적용하는 런타임 경로 없음" not in coverage["unsupportedByReason"]
    assert all(entry["reason"] and entry["nameKo"] != entry["name"] for entry in keystones.values())
    assert coverage["deferredNodeCount"] == sum(bool(node.get("deferred")) for node in nodes.values())
    parsed_nodes = json.loads(PARSE_PATH.read_bytes())["nodes"]
    assert {line["status"] for line in parsed_nodes["25520"]["node"]} == {"mappingOnly"}
    assert all(line["status"] != "full" for node in nodes.values()
               if node.get("deferred") for line in node["statSupport"])
    node_coverage = coverage["nodes"]
    assert set(node_coverage["ordinaryPath"]) == {"full", "partial", "unsupported", "deferred"}
    assert sum(node_coverage["ordinaryPath"].values()) == 3909
    assert set(node_coverage["byStatKindOrdinaryPath"]) == set(coverage["byStatKindOrdinaryPath"])


def test_variant_and_mixed_node_support(nodes, report):
    witch = nodes[3823]["variants"]["cls.Witch"]
    assert not nodes[3823].get("deferred") and witch["deferred"]
    assert len(witch["statSupport"]) == 2
    assert all(line["status"] == "unsupported" and line["reason"] for line in witch["statSupport"])
    assert not nodes[51142].get("deferred")
    assert nodes[51142]["variants"]["asc.AbyssalLich"]["deferred"]
    assert nodes[52191]["deferred"]
    assert all(line["status"] == "unsupported" for line in nodes[52191]["statSupport"])
    assert "미구현 대가" in nodes[52191]["statSupport"][0]["reason"]
    assert not nodes[52068].get("deferred")
    assert {line["status"] for line in nodes[52068]["statSupport"]} == {"full"}
    assert not nodes[17788].get("deferred")
    assert {line["status"] for line in nodes[17788]["statSupport"]} == {"full"}
    treatments = {entry["id"]: entry["treatment"] for entry in report["coverage"]["mixedNodes"]}
    assert treatments[52068] == "implemented" and treatments[52191] == "deferred"


def test_korean_game_tables_and_fallback(nodes, report, tmp_path):
    assert nodes[722]["nameKo"] == "능력치"
    assert nodes[722]["attrOptions"][0]["statsKo"] == ["힘 +5"]
    assert not nodes[722].get("deferred")
    assert all(option["statSupport"][0]["status"] == "full" for option in nodes[722]["attrOptions"])
    assert nodes[5571]["statsKo"] == nodes[5571]["stats"]
    assert nodes[14226]["stats"] == [
        "25% more Skill Speed while Off Hand is empty and you have "
        "a One-Handed Martial Weapon equipped in your Main Hand",
    ]
    assert nodes[14226]["statsKo"] == [
        "보조 장비가 없고 주 무기로 한손 무도 무기를 장착했을 때 스킬 속도가 25% 증폭됩니다.",
    ]
    assert nodes[13228]["statsKo"] != nodes[13228]["stats"]
    balance = tmp_path / "balance"
    balance.mkdir()
    for file in ("passiveskills", "characters", "ascendancy"):
        for parent in (balance, balance / "korean"):
            parent.mkdir(exist_ok=True)
            (parent / f"{file}.datc64.json").write_bytes(b"[]")
    tables = KoreanTables(balance, None, lambda _key, path: path.read_bytes())
    assert tables.node_name(42, "English") == "English"
    assert tables.stats(42, ["+5 to Strength"]) == ["+5 to Strength"]
    translations = report["korean"]
    assert sum(entry["count"] for entry in translations["missingStats"]) == translations["stats"]["missing"]
    translated, missing = (translations["stats"][key] for key in ("translated", "missing"))
    assert translated + missing == translations["stats"]["total"]
    fallback = translations["fallback"]
    assert fallback["translated"] > 0
    assert fallback["missingWithoutFallback"] - fallback["translated"] == missing


def test_checked_in_outputs_are_reproducible():
    if not (DEFAULT_POB / TREE_FILE).is_file():
        pytest.skip("PoB 0.23.1 source tree unavailable")
    balance = DEFAULT_GAME / "tables/data/balance"
    game_inputs = [DEFAULT_GAME / "stat_descriptions.csd"] + [
        directory / f"{table}.datc64.json"
        for directory in (balance, balance / "korean")
        for table in ("passiveskills", "characters", "ascendancy", "keywordpopups")
    ]
    if not all(path.is_file() for path in game_inputs):
        pytest.skip("Korean game inputs for the checked-in pack unavailable")
    compiled, coverage = compile_tree(DEFAULT_POB, DEFAULT_GAME)
    assert json_bytes(compiled) == PACK_PATH.read_bytes()
    assert json_bytes(coverage, indent=2) == REPORT_PATH.read_bytes()
    assert compiled["meta"]["sourceSha256"][TREE_FILE] == hashlib.sha256(
        (DEFAULT_POB / TREE_FILE).read_bytes()).hexdigest()
    assert compiled["meta"]["game"] == "poe2"
    assert compiled["meta"]["gameVersion"] == "0.5"
    assert compiled["meta"]["contentGeneration"] == 1


def test_generation_one_id_baseline(pack):
    baseline = json.loads(BASELINE_PATH.read_bytes())
    validate_id_baseline(pack, baseline)
    assert {kind: len(entries) for kind, entries in baseline["entries"].items()} == {
        "cls": 8, "asc": 23, "node": 4544, "gem": 24, "sup": 19,
    }

    changed = deepcopy(pack)
    first = changed["nodes"][0]
    new_node = {**first, "id": 999999, "adj": [first["id"]]}
    first["adj"].append(new_node["id"])
    changed["nodes"].append(new_node)
    validate_id_baseline(changed, baseline)

    changed["nodes"].pop(0)
    with pytest.raises(ValueError, match="node ID .* removed or changed meaning"):
        validate_id_baseline(changed, baseline)

    changed = deepcopy(pack)
    changed["classes"][0]["name"] += " renamed"
    with pytest.raises(ValueError, match="cls ID .* removed or changed meaning"):
        validate_id_baseline(changed, baseline)

    changed = deepcopy(pack)
    notable = next(node for node in changed["nodes"] if node["id"] == 3823)
    notable["stats"][0] = "+3% to Fire Resistance"
    with pytest.raises(ValueError, match="node ID 3823 .* changed meaning"):
        validate_id_baseline(changed, baseline)


def default_build_roots():
    """Build roots in priority order: the env override, the in-repo build folder,
    then the default BUILD_ROOT that plugin/build.bat uses."""
    roots = []
    if os.environ.get("POE_PLUGIN_BUILD_ROOT"):
        roots.append(Path(os.environ["POE_PLUGIN_BUILD_ROOT"]))
    roots.append(ROOT / "build")
    build_bat = ROOT / "plugin" / "build.bat"
    if build_bat.is_file():
        match = re.search(r'^set "BUILD_ROOT=([^"]+)"', build_bat.read_text(encoding="utf-8"), re.MULTILINE)
        if match:
            roots.append(Path(match.group(1)))
    return roots


def find_cpp_test_executable():
    exe = os.environ.get("POE_PLUGIN_TEST_EXE")
    if exe:
        return Path(exe)
    filenames = ("skyrimpoe_tests.exe", "skyrimpoe_tests")
    for root in default_build_roots():
        if not root.is_dir():
            continue
        candidates = [path for path in root.rglob("skyrimpoe_tests*") if path.is_file() and path.name in filenames]
        if len(candidates) > 1:
            raise AssertionError(f"Several C++ test executables under {root}; set POE_PLUGIN_TEST_EXE")
        if candidates:
            return candidates[0]
    return None


def test_cpp_js_parity(tmp_path):
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js unavailable for C++/JS passive tree parity")
    tests = find_cpp_test_executable()
    if tests is None:
        pytest.skip("C++ test executable not built; set POE_PLUGIN_TEST_EXE or POE_PLUGIN_BUILD_ROOT")
    fixture = tmp_path / "tree-parity.json"
    subprocess.run([tests.resolve(), "--write-passive-ui-fixture", fixture], check=True)
    completed = subprocess.run([node, ROOT / "tools/ui-preview/test_tree_parity.js", fixture],
                               check=True, capture_output=True, text=True)
    assert re.fullmatch(r"Passive tree C\+\+/JS parity: \d+ cases passed\.\s*", completed.stdout)
    print(completed.stdout, end="")
