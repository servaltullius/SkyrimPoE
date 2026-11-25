"""Check the P4a gem bundle and outputs against the two PoB distributions."""

from collections import Counter
from copy import deepcopy
import json
from pathlib import Path
import re

import pytest

from compile_gems_poe2 import (
    BASELINE_PATH, BUNDLE_PATH, CORE_PATH, DEFAULT_POB, DEFAULT_RAW, PACK_PATH, REPORT_PATH,
    PARTIAL_STATS, ROOT, STAT_ENGINE, STATUSES, StatDescriptions, fingerprint, rebuild,
)
from extract import InputFiles, returned
from luadata import field, table_array, table_fields

POE1 = Path("G:/poe/PathOfBuilding-2.68.1")


def read_json(path):
    return json.loads(path.read_bytes())


@pytest.fixture(scope="module")
def bundle():
    return read_json(BUNDLE_PATH)


@pytest.fixture(scope="module")
def catalog():
    return read_json(PACK_PATH)["gems"]


@pytest.fixture(scope="module")
def report():
    return read_json(REPORT_PATH)


@pytest.fixture(scope="module")
def raw_gems():
    path = DEFAULT_RAW / "gems.json"
    if not path.is_file():
        pytest.skip("PoE2 0.5 raw gems.json unavailable")
    return {entry["key"]: entry["value"] for entry in read_json(path)}


@pytest.fixture(scope="module")
def raw_skills():
    path = DEFAULT_RAW / "skills.json"
    if not path.is_file():
        pytest.skip("PoE2 0.5 raw skills.json unavailable")
    return {entry["key"]: entry["value"] for entry in read_json(path)["entries"]}


def test_bundle_ids_plans_inputs_and_ready(bundle, catalog):
    assert len(bundle) == 43
    assert Counter(row["form"]["plan"] for row in bundle) == {"keep": 9, "new": 34}
    assert len({row["form"]["formId"] for row in bundle}) == 43
    assert len({row["form"]["editorId"] for row in bundle}) == 43
    assert {row["contentId"] for row in bundle if row["ready"]} == {"gem.Spark"}
    assert all(type(row["dropCandidate"]) is bool for row in bundle)
    assert {row["shape"] for row in bundle} == {f"S{index}" for index in range(1, 13)}
    ids = [row["contentId"] for row in bundle]
    assert len(set(ids)) == 43
    assert set(ids) == catalog.keys()
    assert all(re.fullmatch(r"(gem|sup)\.[A-Za-z0-9._]+", key) and len(key.encode()) <= 64 for key in ids)
    assert all(row["contentId"].startswith("sup.") == (row["shape"] == "S11") for row in bundle)


def test_poe2_bundle_keys_match_source(bundle, raw_gems, raw_skills):
    poe2 = [row for row in bundle if "borrowedFrom" not in row]
    assert len(poe2) == 42
    for row in poe2:
        raw = raw_gems[row["sourceKey"]]
        for key in ("gameId", "variantId", "grantedEffectId", "Tier", "gemFamily"):
            assert field(raw, key) == row[key], (row["contentId"], key)
        assert row["grantedEffectId"] in raw_skills
        if row["displayGrade"]:
            assert field(raw, "name").endswith(" " + row["displayGrade"])


def test_borrowed_cyclone_keys_match_poe1(bundle):
    if not (POE1 / "Data/Gems.lua").is_file():
        pytest.skip("PoE1 3.30 Gems.lua unavailable")
    cyclone, = [row for row in bundle if "borrowedFrom" in row]
    assert (cyclone["contentId"], cyclone["borrowedFrom"]) == ("gem.Cyclone", "poe1-3.30")
    raw = field(returned(InputFiles(POE1), "Data/Gems.lua"), cyclone["sourceKey"])
    for key in ("gameId", "variantId", "grantedEffectId", "naturalMaxLevel"):
        assert cyclone[key] == field(raw, key)
    assert cyclone["tags"] == [tag for tag, enabled in table_fields(field(raw, "tags")) if enabled]


def test_borrowed_cyclone_level_values_match_core(catalog):
    source = read_json(CORE_PATH)["gems"]["Cyclone"]
    cyclone = catalog["gem.Cyclone"]
    assert cyclone["borrowedFrom"] == "poe1-3.30"
    assert cyclone["naturalMaxLevel"] == cyclone["calculationLevelCount"] == 20
    effect, = cyclone["effects"]
    assert effect["source"]["file"] == "data/packs/core_3_30.json"
    stat_set, = effect["statSets"]
    assert stat_set["levelValuesIncludeConstants"]
    for level, row in source["levels"].items():
        compiled = effect["levels"][level]
        for key in ("attackSpeedMultiplier", "baseMultiplier", "damageEffectiveness", "critChance"):
            assert compiled[key] == row[key]
        assert compiled["cost"] == {"Mana": row["manaCost"]}
        assert compiled["requirements"] == {key: row[key] for key in ("reqStr", "reqDex", "reqInt")}
        assert dict(zip(stat_set["stats"], stat_set["levels"][level]["values"], strict=True)) == row["stats"]


def test_spark_keeps_separate_stat_sets_and_raw_base_values(catalog, raw_skills):
    raw = raw_skills["SparkPlayer"]
    spark = catalog["gem.Spark"]
    effect, = spark["effects"]
    assert spark["naturalMaxLevel"] == 20
    assert spark["calculationLevelCount"] == len(table_fields(field(raw, "levels"))) == 40
    raw_level = field(field(raw, "levels"), 1)
    assert effect["levels"]["1"]["cost"]["Mana"] == field(field(raw_level, "cost"), "Mana") == 5
    assert effect["levels"]["1"]["critChance"] == field(raw_level, "critChance") == 9
    # PoB passes character level requirement, rather than gem row number, to the formula.
    assert effect["levels"]["1"]["requirements"] == {"reqStr": 0, "reqDex": 0, "reqInt": 0}
    assert effect["levels"]["20"]["requirements"] == {"reqStr": 0, "reqDex": 0, "reqInt": 157}
    base, infused = effect["statSets"]
    assert (base["label"], infused["label"]) == ("Base", "Cold-Infused")
    raw_base = field(field(raw, "statSets"), 1)
    assert base["stats"][:3] == table_array(field(raw_base, "stats"))[:3] == [
        "spell_minimum_base_lightning_damage", "spell_maximum_base_lightning_damage", "base_number_of_projectiles",
    ]
    assert base["levels"]["1"]["values"] == table_array(field(field(raw_base, "levels"), 1)) == [1, 10, 4]
    # The infused set contains repeated keys at separate source positions.
    assert infused["stats"].count("base_number_of_projectiles") == 2
    assert infused["levels"]["1"]["values"] == [0, 0, 1, 10, 7, 11, 0, 12]
    reference, = spark["references"]
    assert reference["statSet"] == 2
    assert reference["condition"] == "Consuming a Cold Infusion"


def test_source_meta_support_reservation_and_level_values(catalog, raw_skills):
    coc = catalog["gem.CastOnCriticalStrike"]
    primary, hidden = coc["effects"]
    raw_coc = field(field(raw_skills["MetaCastOnCritPlayer"], "levels"), 1)
    assert primary["levels"]["1"]["spiritReservationFlat"] == field(raw_coc, "spiritReservationFlat") == 100
    assert hidden["grantedEffectId"] == coc["references"][0]["grantedEffectId"] == "SupportMetaCastOnCritPlayer"
    hidden_source = field(field(raw_skills[hidden["grantedEffectId"]], "statSets"), 1)
    constants = dict(table_array(pair) for pair in table_array(field(hidden_source, "constantStats")))
    assert dict(hidden["statSets"][0]["constantStats"])["trigger_meta_gem_damage_+%_final"] == constants[
        "trigger_meta_gem_damage_+%_final"
    ] == -20
    assert hidden["requireSkillTypes"] == ["Spell", "Triggerable", "AND"]
    assert catalog["gem.IronWard"]["effects"][1]["grantedEffectId"] == "IronWardNovaPlayer"
    chain = catalog["sup.Chain1"]
    assert chain["naturalMaxLevel"] == chain["calculationLevelCount"] == 1
    assert len(table_fields(field(raw_skills["SupportChainPlayer"], "levels"))) == 1
    deadly = catalog["sup.DeadlyHerald"]["effects"][0]
    source = field(field(raw_skills["SupportDeadlyHeraldsPlayer"], "levels"), 1)
    assert deadly["levels"]["1"]["spiritReservationFlat"] == field(source, "spiritReservationFlat") == 20
    echo = catalog["sup.SpellEcho"]["effects"][0]
    assert echo["requireSkillTypes"] == ["Cascadable"]
    assert echo["excludeSkillTypes"] == ["Triggered", "UsedByProxy", "Persistent"]
    assert echo["levels"]["1"]["manaMultiplier"] == 50
    assert echo["levels"]["1"]["costMultiplier"] == 1.5
    assert deadly["levels"]["1"]["costMultiplier"] == 1.0
    weapons = catalog["gem.HeraldOfIce"]["effects"][0]["weaponTypes"]
    assert "Staff" in weapons and "Wand" not in weapons


def test_every_source_reference_resolves_or_is_recorded(bundle, catalog, raw_gems):
    missing_refs = []
    for row in bundle:
        if row.get("borrowedFrom"):
            continue
        gem = catalog[row["contentId"]]
        raw = raw_gems[row["sourceKey"]]
        effects = {effect["grantedEffectId"]: effect for effect in gem["effects"]}
        assert gem["effects"][0]["grantedEffectId"] == field(raw, "grantedEffectId")
        expected = [(key, ref) for key, ref in table_fields(raw)
                    if re.fullmatch(r"additional(?:GrantedEffectId|StatSet)\d*", key)]
        assert [(ref["field"], ref["ref"]) for ref in gem["references"]] == expected
        misses = {item["ref"]: item for item in gem["missingInSource"]}
        for ref in gem["references"]:
            assert ref["condition"] and ref["conditionSource"]["field"] == "description"
            if ref["ref"] in misses:
                assert "grantedEffectId" not in ref
                assert ref["where"] == misses[ref["ref"]]["where"]
            else:
                effect = effects[ref["grantedEffectId"]]
                if ref["field"].startswith("additionalStatSet"):
                    assert ref["statSet"] in {item["index"] for item in effect["statSets"]}
                else:
                    assert effect["grantedEffectId"] == ref["ref"]
        assert misses.keys() <= {ref for _, ref in expected}
        missing_refs.extend(misses)
    assert missing_refs == ["IceNovaPlayerOnFrostbolt"]


def test_ice_nova_missing_reference_in_catalog_and_report(catalog, report):
    expected = [{"ref": "IceNovaPlayerOnFrostbolt", "where": "Data/Gems.lua:11 additionalStatSet1",
                 "reason": "PoB 0.23.1 Skills에 정의 없음"}]
    gem = catalog["gem.IceNova"]
    assert gem["missingInSource"] == expected
    row = next(row for row in report["gems"] if row["contentId"] == gem["contentId"])
    assert row["missingInSource"] == expected
    missing_effect, = [effect for effect in row["effects"] if effect.get("reason") == "원천 누락"]
    assert missing_effect["status"] == "unsupported"
    assert missing_effect["evidence"] == expected[0]
    assert gem["references"][1]["statSet"] == 2
    assert gem["effects"][0]["statSets"][1]["label"] == "Cold-Infused"


def test_report_has_all_columns_with_counts_and_stat_evidence(bundle, report):
    assert len(report["gems"]) == 43
    assert [row["contentId"] for row in report["gems"]] == [row["contentId"] for row in bundle]
    assert [row["contentId"] for row in report["gems"] if row["acquisition"]["readyDrop"]] == ["gem.Spark"]
    engine = (ROOT / STAT_ENGINE).read_text(encoding="utf-8")
    total = Counter()
    for row in report["gems"]:
        fields = ("contentId", "name", "acquisition", "form", "input", "cost", "requirements", "shape", "effects",
                  "effectCounts", "storage")
        assert all(row[key] for key in fields)
        assert set(row["requirements"]) == {"level1", "level20"}
        assert type(row["ready"]) is bool
        assert row["input"].get("default") or row["input"]["reason"]
        assert row["storage"] == {"inventory": "폼 아이템", "GENR": 1, "CORE": "v3 콘텐츠 ID", "GKEY": "v2 콘텐츠 ID"}
        for cost in row["cost"]:
            assert cost["values"]
            for value in cost["values"]:
                assert all(value[key] not in (None, "") for key in ("unit", "level1", "level20"))
                if row["shape"] == "S11":
                    assert value["level20"] == "해당 없음"
        counts = Counter(effect["status"] for effect in row["effects"])
        assert counts.keys() <= set(STATUSES)
        assert row["effectCounts"] == {status: counts[status] for status in STATUSES}
        for effect in row["effects"]:
            evidence = effect["evidence"]
            assert evidence
            if effect["status"] in ("implemented", "partial"):
                key = effect["key"]
                if effect["status"] == "partial":
                    key = PARTIAL_STATS[key][0]
                assert evidence["reads"] == [{"file": STAT_ENGINE, "key": key}]
                assert f'GetStat("{key}"' in engine
        total.update(counts)
    assert report["effectCounts"] == dict(total)


def test_semantic_id_baseline_includes_every_gem(catalog):
    baseline = read_json(BASELINE_PATH)
    assert baseline["contentGeneration"] == 1
    assert {key: len(entries) for key, entries in baseline["entries"].items()} == {"gem": 24, "sup": 19}
    for kind, entries in baseline["entries"].items():
        for content_id, (name, digest) in entries.items():
            assert content_id.startswith(kind + ".")
            assert name == catalog[content_id]["name"]
            assert re.fullmatch(r"[0-9a-f]{64}", digest)


def test_fingerprint_distinguishes_semantic_and_magnitude_changes(catalog):
    if not (DEFAULT_POB / "manifest.xml").is_file():
        pytest.skip("PoB 0.23.1 descriptions unavailable")
    descriptions = StatDescriptions(InputFiles(DEFAULT_POB))
    original = catalog["gem.Spark"]
    digest = fingerprint(original, descriptions)
    changed = deepcopy(original)
    changed["effects"][0]["statSets"][0]["levels"]["1"]["values"][1] += 1
    assert fingerprint(changed, descriptions) == digest
    changed["effects"][0]["statSets"][0]["stats"][:2] = reversed(changed["effects"][0]["statSets"][0]["stats"][:2])
    assert fingerprint(changed, descriptions) != digest
    changed = deepcopy(original)
    changed["references"][0]["condition"] = "Always"
    assert fingerprint(changed, descriptions) != digest


def test_regeneration_identical_to_checked_in_outputs():
    if not (DEFAULT_POB / "manifest.xml").is_file() or not (DEFAULT_RAW / "meta.json").is_file():
        pytest.skip("PoB 0.23.1 and the PoE2 0.5 raw pack are needed for regeneration")
    first = rebuild()
    assert first == rebuild()
    assert set(first) == {PACK_PATH, REPORT_PATH, BASELINE_PATH}
    assert all(contents == path.read_bytes() for path, contents in first.items())
    meta = json.loads(first[PACK_PATH])["meta"]
    assert (meta["schema"], meta["game"], meta["contentGeneration"], meta["pobVersion"]) == (
        "skyrimpoe.gems", "poe2", 1, "0.23.1",
    )
    assert "Modules/CalcTools.lua" in meta["sourceSha256"]
    assert "Modules/CalcTools.lua" in meta["pobManifestSha1"]


def test_report_counts_only_the_first_stat_set_the_dll_reads(report):
    spark = next(row for row in report["gems"] if row["contentId"] == "gem.Spark")
    assert spark["requirements"]["level20"] == {"reqStr": 0, "reqDex": 0, "reqInt": 157}
    infused = [effect for effect in spark["effects"] if effect.get("statSet", 1) != 1]
    assert infused and all(effect["status"] == "unsupported" for effect in infused)
    assert any(effect["key"] == "spell_minimum_base_cold_damage" for effect in infused)
