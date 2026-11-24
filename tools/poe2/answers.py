"""Generate independent reference cases for P2b in PoE units."""
from __future__ import annotations

import argparse
from configparser import ConfigParser
import json
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract import InputFiles, json_bytes, write_atomic
from luadata import numeric_key, parse, table_array, table_fields


INI_PATH = "plugin/dist/SKSE/Plugins/SkyrimPoE.ini"
KILL_TRACKER = "plugin/src/core/KillTracker.cpp"
ROOT = Path(__file__).resolve().parents[2]
MISC, DATA = "Data/Misc.lua", "Modules/Data.lua"
DEFENCE, OFFENCE = "Modules/CalcDefence.lua", "Modules/CalcOffence.lua"
PERFORM, SETUP = "Modules/CalcPerform.lua", "Modules/CalcSetup.lua"
BUILD, ROUND = "Modules/Build.lua", "Modules/Common.lua"
CONFIG = "Modules/ConfigOptions.lua"
KEYWORDS = "data/balance/keywordpopups.datc64.json"
MODS = "data/balance/mods.datc64.json"
NUMBER = r"(-?\d+(?:\.\d+)?)"


def read_reference_values(pob, game_tables):
    """Read the PoB tables and formula rows independently of the pack builder."""
    files = InputFiles(pob)
    manifest = ET.fromstring(files.read(Path("manifest.xml")))
    if manifest.find("Version").attrib["number"] != "0.23.1":
        raise ValueError("expected PoB 0.23.1")
    misc = {entry.target[-1]: entry.value for entry in parse(files.lua(Path(MISC)), MISC)
            if entry.kind == "assign" and entry.target[0] == "data"}

    def constants(name):
        return {key: numeric_key(value) for key, value in table_fields(misc[name])}

    player = constants("characterConstants")
    monster = constants("monsterConstants")
    game = constants("gameConstants")
    lua_lines = {}

    def coefficients(file, line, pattern):
        if file not in lua_lines:
            lua_lines[file] = files.lua(Path(file)).splitlines()
        match = re.search(pattern.replace("<n>", NUMBER), lua_lines[file][line - 1])
        if not match:
            raise ValueError(f"{file}:{line}: reference formula changed")
        return tuple(numeric_key(parse(f"return {literal}", file)[0].value) for literal in match.groups())

    def coefficient(file, line, pattern):
        value, = coefficients(file, line, pattern)
        return value

    keyword_raw = (game_tables / KEYWORDS).read_text(encoding="utf-8-sig")
    definitions = {row["Id"]: row["Definition"] for row in json.loads(keyword_raw)}
    keyword_lines = keyword_raw.splitlines()

    def keyword_ref(key):
        start = keyword_lines.index(f'    "Id": "{key}",')
        line = next(i for i in range(start + 1, len(keyword_lines))
                    if keyword_lines[i].lstrip().startswith('"Definition": '))
        return {"file": KEYWORDS, "line": line + 1, "id": key}

    def game_number(key, pattern):
        value, = re.findall(pattern, definitions[key])
        return int(value)

    mods = {row["Id"]: row for row in json.loads((game_tables / MODS).read_bytes())}

    def rarity_threshold(rarity):
        mod = mods[f"Monster{rarity}2"]
        field, = [f"Stat{i}" for i in range(1, 7)
                  if (mod.get(f"Stat{i}") or {}).get("Id") == "poise_threshold_+%_final_from_rarity"]
        lo, hi = mod[f"{field}Value"]
        if lo != hi:
            raise ValueError(f"{mod['Id']}: expected fixed poise threshold")
        return lo

    def origin(grade, *refs):
        return {"grade": grade, "references": list(refs)}

    def keyword_origins(*keys):
        return origin("gameData", *(keyword_ref(key) for key in keys))

    chaos_es, normal_es = coefficients(DEFENCE, 594, r"and <n> or <n>$")
    evasion_scale, accuracy_scale = coefficients(DEFENCE, 44, r"<n> \* evasion.*<n> \* accuracy")
    hit_max, hit_min = coefficients(DEFENCE, 45, r"round\(rawChance\), <n>\), <n>")
    rating_scale, deflect_scale, deflect_offset = coefficients(DEFENCE, 52, r"deflection \* <n> \) \* <n> - <n>$")
    deflect_percent, deflect_min = coefficients(DEFENCE, 53, r"<n> - round.*ChanceCap\), <n>")
    safe_base, safe_divisor = coefficients(BUILD, 218, r"= <n> \+ m_floor\(playerLevel / <n>\)")
    xp_num, xp_den, xp_diff, xp_power = coefficients(BUILD, 225, r"\+ <n>\) / \(playerLevel \+ <n> \+ diff \^ <n>\)\) \^ <n>")
    armour_break = game_number("ArmourBreak", r"take (\d+)% increased \[Physical\]")
    sources = {
        "energyShield.bleedBypasses": keyword_origins("Bleeding"),
        "energyShield.poisonBypasses": keyword_origins("Poison"),
        "armour.brokenPlayerDurationSeconds": keyword_origins("ArmourBreak"),
        "armour.brokenEnemyHitDamageTakenPercent": keyword_origins("ArmourBreak"),
        "bleed.movingOrAggravatedMultiplier": origin("pob", *references((SETUP, 710, 710)), keyword_ref("Bleeding")),
        "chill.discardBelowMinimum": keyword_origins("Chill"),
        "poise.fullBuildupPercent": keyword_origins("Frozen"),
        "poise.enemyDecayDelayMilliseconds": origin("pob", *references((MISC, 269, 269))),
        "poise.enemyDecayPercentPerSecond": origin("pob", *references((MISC, 270, 270))),
        "freeze.damageScale": origin("pob", *references((MISC, 54, 54))),
        "freeze.enemyDurationSeconds": origin("pob", *references((MISC, 56, 56))),
        "ignite.flammabilityIncludesCurrentHit": keyword_origins("Ignite"),
        "ignite.flammabilityIndependentExpiry": keyword_origins("Flammability"),
        "ignite.flammabilityCapPercent": keyword_origins("Flammability"),
        "ignite.chanceMultiplier": origin("pob", *references((MISC, 72, 72))),
        "ignite.flammabilityEnemyDurationSeconds": origin("pob", *references((MISC, 113, 113))),
        "ignite.flammabilityPlayerDurationSeconds": origin("pob", *references((MISC, 114, 114))),
        "poise.magicThresholdMorePercent": origin("gameData", *references((MODS, 705740, 705745))),
        "poise.rareThresholdMorePercent": origin("gameData", *references((MODS, 707426, 707431))),
        "poise.uniqueThresholdMorePercent": origin("pob", *references((CONFIG, 2006, 2006))),
        "poise.mapBossThresholdMorePercent": origin("pob", *references((CONFIG, 2007, 2007))),
    }
    poise = {"enemyDecayDelayMilliseconds": monster["poise_decay_delay_ms"],
             "enemyDecayPercentPerSecond": monster["poise_decay_%_per_second"],
             "fullBuildupPercent": game_number("Frozen", r"reach (\d+)% \[Freeze\]")}
    for name, line in (("unique", 2006), ("mapBoss", 2007)):
        poise[f"{name}ThresholdMorePercent"] = coefficient(CONFIG, line, r'"PoiseThreshold", "MORE", <n>')
    chill = {"effectMultiplier": game["ChillEffectMultiplier"], "maxPercent": game["ChillMaxEffect"],
             "minPercent": coefficient(DATA, 410, r"min = <n>"), "enemyDurationSeconds": game["BaseChillDuration"]}
    for rarity in ("Magic", "Rare", "Unique"):
        key = f"{rarity.lower()}SlowReductionPercent"
        chill[key] = game_number("Slow", rf"(\d+)% less Slow effect on {rarity} monsters")
        sources[f"chill.{key}"] = keyword_origins("Slow")
        if rarity != "Unique":
            poise[f"{rarity.lower()}ThresholdMorePercent"] = rarity_threshold(rarity)

    return {
        "levelTables": {name: [numeric_key(value) for value in table_array(misc[f"monster{name[0].upper()}{name[1:]}Table"])]
                        for name in ("life", "damage", "accuracy", "evasion", "ailmentThreshold", "poiseThreshold")},
        "armour": {"damageCoefficient": coefficient(DATA, 255, r"ArmourRatio = <n>"),
                   "brokenEnemyHitDamageTakenPercent": armour_break,
                   "brokenEnemyDurationSeconds": game_number("ArmourBreak", r"Broken for (\d+) seconds"),
                   "brokenPlayerDurationSeconds": game_number("ArmourBreak", r"(\d+) seconds for players")},
        "energyShield": {"normalConsumptionMultiplier": normal_es, "chaosConsumptionMultiplier": chaos_es,
                         "bleedBypasses": "Damage from Bleeding bypasses" in definitions["Bleeding"],
                         "poisonBypasses": "Damage from Poison bypasses" in definitions["Poison"]},
        "critical": {"playerBonusPercent": player["base_critical_hit_damage_bonus"]},
        "block": {"baseMaxPercent": player["object_inherent_base_maximum_block_%_from_ot"],
                  "capPercent": coefficient(DATA, 247, r"BlockChanceCap = <n>")},
        "deflect": {"baseReductionPercent": game["BasePercentDamageDeflected"],
                    "minimumRating": coefficient(DEFENCE, 49, r"deflection < <n>"),
                    "ratingCoefficient": rating_scale, "chanceScale": deflect_scale, "chanceOffsetPercent": deflect_offset,
                    "percentScale": deflect_percent, "minPercent": deflect_min,
                    "capPercent": coefficient(DATA, 245, r"DeflectionChanceCap = <n>")},
        "hit": {"monsterEvasionMultiplier": evasion_scale, "monsterAccuracyMultiplier": accuracy_scale,
                "minPercent": hit_min, "maxPercent": hit_max,
                "negativeAccuracyPercent": coefficient(DEFENCE, 42, r"return <n>")},
        "ignite": {"chanceMultiplier": game["IgniteChanceMultiplier"], "durationSeconds": game["BaseIgniteDuration"],
                   "damagePercentPerMinute": game["IgniteHitDamagePercentPerMinute"],
                   "flammabilityEnemyDurationSeconds": game["BaseFlammabilityDuration"],
                   "flammabilityPlayerDurationSeconds": game["BaseFlammabilityDurationPlayer"],
                   "flammabilityCapPercent": game_number("Flammability", r"up to (\d+)%")},
        "bleed": {"durationSeconds": game["BaseBleedingDuration"], "damagePercentPerMinute": game["BleedingHitDamagePercentPerMinute"],
                  "movingOrAggravatedMultiplier": 1 + game_number("Bleeding", r"an extra (\d+)% damage") / 100},
        "poison": {"durationSeconds": game["BasePoisonDuration"], "damagePercentPerMinute": game["PoisonHitDamagePercentPerMinute"],
                   "baseMaxStacks": coefficient(SETUP, 55, r'"PoisonStacksMax", "BASE", <n>')},
        "shock": {"chanceMultiplier": game["ShockChanceMultiplier"], "effectPercent": game["BaseShockMagnitude"],
                  "enemyDurationSeconds": game["BaseShockDuration"], "playerDurationSeconds": game["BaseShockDurationPlayer"]},
        "chill": chill, "poise": poise,
        "freeze": {"damageScale": game["FreezeDamageScale"], "enemyDurationSeconds": game["FreezeDuration"]},
        "life": {"perLevel": player["life_per_level"], "base": coefficient(SETUP, 671, r"base = <n>"),
                 "perAttribute": coefficient(PERFORM, 504, r"output.Str \* <n>")},
        "mana": {"perLevel": player["mana_per_level"], "base": coefficient(SETUP, 672, r"base = <n>"),
                 "perAttribute": coefficient(PERFORM, 515, r"output.Int \* <n>")},
        "xpCurve": (safe_base, safe_divisor, xp_num, xp_den, xp_diff, xp_power),
        "provenance": sources,
        "supplementalGameData": {"igniteEnergyShield": [keyword_ref("Ignite"), keyword_ref("EnergyShield")]},
    }


def references(*locations):
    return [{"file": file, "line": first, "endLine": last} for file, first, last in locations]


def case(name, inputs, expected, provenance, tolerance=0):
    return {
        "id": name, "input": inputs, "expected": expected,
        "tolerance": tolerance, "provenance": provenance,
    }


def pob_case(name, inputs, expected, *locations, tolerance=0):
    return case(name, inputs, expected, {"grade": "pob", "references": references(*locations)}, tolerance)


def armour_reduction(armour, damage, coefficient):
    # Modules/CalcDefence.lua:56-68 and Modules/Common.lua:648-654.
    if armour == damage == 0:
        return 0
    ratio = abs(armour) / (abs(armour) + damage * coefficient)
    return math.floor(math.copysign(100 * ratio, armour) + 0.5)


def split_damage(shield, damage, multiplier, bypass=False):
    # Modules/CalcDefence.lua:594-612. No mana redirection or life-loss prevention.
    absorbed = 0 if bypass else min(damage, shield / multiplier)
    return {"esRemaining": shield - absorbed * multiplier, "lifeLost": damage - absorbed}


def monster_hit_chance(evasion, accuracy, rule):
    # Modules/CalcDefence.lua:40-45 and Modules/Common.lua:648-654.
    if accuracy < 0:
        return rule["negativeAccuracyPercent"]
    chance = 100 * (1 - rule["monsterEvasionMultiplier"] * evasion /
                    (evasion + rule["monsterAccuracyMultiplier"] * accuracy))
    return min(rule["maxPercent"], max(rule["minPercent"], math.floor(chance + 0.5)))


def deflect_chance(rating, accuracy, rule):
    # Modules/CalcDefence.lua:48-53.
    if rating < rule["minimumRating"]:
        return rule["minPercent"]
    miss = (accuracy / (accuracy + rating * rule["ratingCoefficient"]) *
            rule["chanceScale"] - rule["chanceOffsetPercent"])
    return max(rule["minPercent"], min(rule["capPercent"], rule["percentScale"] - math.floor(miss + 0.5)))


def pob_cases(rules):
    tables = rules["levelTables"]
    cases = []
    for name, armour, damage in (("equal", 100, 100), ("half", 1000, 100),
                                  ("strong", 4000, 100), ("zero-over-zero", 0, 0),
                                  ("zero-armour", 0, 100), ("zero-damage", 100, 0),
                                  ("negative", -100, 100), ("negative-tie", -10, 39)):
        cases.append(pob_case(f"armour-{name}", {"armour": armour, "damage": damage},
                              {"reductionPercent": armour_reduction(armour, damage, rules["armour"]["damageCoefficient"])},
                              (DEFENCE, 56, 68), (DATA, 255, 255), (ROUND, 648, 654)))

    # Modules/CalcDefence.lua:594-612. Treat hit and ailment damage separately.
    for name, kind, shield, damage in (
        ("chaos-overflow", "chaos", 60, 50), ("chaos-exact", "chaos", 60, 30),
        ("chaos-absorbed", "chaos", 60, 25), ("normal-overflow", "physical", 60, 80),
        ("normal-absorbed", "fire", 60, 50), ("no-shield", "physical", 0, 20),
    ):
        rule = rules["energyShield"]
        multiplier = rule["chaosConsumptionMultiplier"] if kind == "chaos" else rule["normalConsumptionMultiplier"]
        cases.append(pob_case(f"es-{name}", {"damageType": kind, "energyShield": shield, "damage": damage},
                              split_damage(shield, damage, multiplier), (DEFENCE, 594, 612)))

    # Modules/CalcSetup.lua:679 and Modules/CalcOffence.lua:3813-3858 use the bonus above base damage.
    cases.append(pob_case("critical-player-base", {"damage": 100},
                          {"damage": 100 + rules["critical"]["playerBonusPercent"]},
                          (MISC, 159, 159), (SETUP, 679, 679), (OFFENCE, 3813, 3858)))

    block = rules["block"]
    for name, chance, added_max in (("under", 32, 0), ("base-cap", 75, 0), ("hard-cap", 100, 60)):
        # Modules/CalcDefence.lua:979-1005.
        maximum = min(block["baseMaxPercent"] + added_max, block["capPercent"])
        cases.append(pob_case(f"block-{name}", {"chancePercent": chance, "addedMaxPercent": added_max},
                              {"chancePercent": min(chance, maximum), "maxPercent": maximum},
                              (DEFENCE, 979, 1005), (DATA, 247, 247), (MISC, 150, 150)))
    for name, rating, accuracy in (("none", 0, 100), ("mid", 1000, 100), ("cap", 10000, 100)):
        cases.append(pob_case(f"deflect-{name}", {"rating": rating, "accuracy": accuracy},
                              {"chancePercent": deflect_chance(rating, accuracy, rules["deflect"]),
                               "reductionOnDeflectPercent": rules["deflect"]["baseReductionPercent"]},
                              (DEFENCE, 48, 53), (DATA, 245, 250), (MISC, 111, 111)))
    for level, evasion in ((1, 0), (1, 100), (20, 500), (65, 2500), (80, 5000), (100, 20000)):
        accuracy = tables["accuracy"][level - 1]
        cases.append(pob_case(f"monster-hit-lv{level}-ev{evasion}", {"monsterLevel": level, "evasion": evasion},
                              {"monsterAccuracy": accuracy,
                               "hitChancePercent": monster_hit_chance(evasion, accuracy, rules["hit"])},
                              (DEFENCE, 40, 45), (MISC, 6, 6), (ROUND, 648, 654)))

    # Modules/CalcOffence.lua:5448-5464. Hit min and max coincide in these examples.
    for kind in ("ignite", "shock"):
        rule = rules[kind]
        for name, damage, base, increased, more in (("base", 400, 0, 0, 1),
                                                     ("modifiers", 400, 3, 50, 2),
                                                     ("cap", 6000, 0, 0, 1)):
            chance = (damage / 1000 * rule["chanceMultiplier"] + base) * (1 + increased / 100) * more
            expected = {"chancePercent": min(100, chance)}
            if kind == "shock":
                expected.update(effectPercent=rule["effectPercent"], enemyDurationSeconds=rule["enemyDurationSeconds"],
                                playerDurationSeconds=rule["playerDurationSeconds"])
            cases.append(pob_case(f"{kind}-chance-{name}", {"damage": damage, "ailmentThreshold": 1000,
                                                           "baseChancePercent": base, "increasedChancePercent": increased,
                                                           "moreChanceMultiplier": more}, expected,
                                  (OFFENCE, 5448, 5464), (MISC, 71, 75), (MISC, 93, 94),
                                  tolerance=1e-9))

    # Modules/CalcOffence.lua:5385-5396, 5507-5510 and 5567-5598 report a raw
    # Chill effect and a separate minimum/maximum for the breakdown.
    chill = rules["chill"]
    for damage in (10, 290, 300, 500, 600):
        cases.append(pob_case(f"chill-breakdown-{damage}", {"coldDamage": damage, "ailmentThreshold": 1000},
                              {"rawEffectPercent": chill["effectMultiplier"] * (damage / 1000),
                               "minPercent": chill["minPercent"], "maxPercent": chill["maxPercent"],
                               "chanceOnHitPercent": 100 if damage > 1000 / chill["effectMultiplier"] else 0},
                              (OFFENCE, 5385, 5396), (OFFENCE, 5507, 5510), (OFFENCE, 5567, 5598),
                              (DATA, 410, 410), (MISC, 76, 77)))

    # Modules/CalcOffence.lua:5403-5417. Use base monster poise and unmodified cold damage.
    for level, damage in ((1, 10), (20, 100), (65, 4000), (80, 50000), (100, 100000)):
        poise = tables["poiseThreshold"][level - 1]
        buildup = damage * (rules["freeze"]["damageScale"] / poise) * 100
        cases.append(pob_case(f"freeze-lv{level}", {"monsterLevel": level, "coldDamage": damage},
                              {"poiseThreshold": poise, "buildupPercent": buildup},
                              (OFFENCE, 5403, 5417), (MISC, 13, 13), (MISC, 54, 54),
                              tolerance=1e-9))
    for level in (1, 20, 65, 80, 100):
        cases.append(pob_case(f"thresholds-lv{level}", {"monsterLevel": level},
                              {name: tables[name][level - 1] for name in
                               ("life", "damage", "accuracy", "evasion", "ailmentThreshold", "poiseThreshold")},
                              (MISC, 5, 13), (OFFENCE, 4810, 4810), (OFFENCE, 5404, 5405)))
    for name, map_boss in (("unique", False), ("map", True)):
        level, cold = 80, 50000
        poise = rules["poise"]
        threshold = tables["poiseThreshold"][level - 1] * (1 + poise["uniqueThresholdMorePercent"] / 100)
        if map_boss:
            threshold *= 1 + poise["mapBossThresholdMorePercent"] / 100
        cases.append(pob_case(f"freeze-boss-{name}", {"monsterLevel": level, "coldDamage": cold, "mapBoss": map_boss},
                              {"buildupPercent": cold * rules["freeze"]["damageScale"] / math.floor(threshold) * 100},
                              ("Modules/ConfigOptions.lua", 2006, 2007), (OFFENCE, 5403, 5417),
                              (MISC, 13, 13), (MISC, 54, 54), tolerance=1e-9))

    for kind, damage, moving, aggravated, source_line in (
        ("ignite", 100, False, False, 87),
        ("bleed", 100, False, False, 86),
        ("bleed", 100, True, False, 86),
        ("bleed", 100, False, True, 86),
        ("poison", 150, False, False, 88),
    ):
        suffix = "-moving" if moving else "-aggravated" if aggravated else ""
        rule = rules[kind]
        per_second = damage * rule["damagePercentPerMinute"] / 6000
        if moving or aggravated:
            per_second *= rule["movingOrAggravatedMultiplier"]
        refs = references((MISC, source_line, source_line), (MISC, 90, 95))
        if moving or aggravated:
            refs.extend(rules["provenance"]["bleed.movingOrAggravatedMultiplier"]["references"])
        cases.append(case(f"dot-{kind}{suffix}", {"ailment": kind, "hitDamage": damage, "moving": moving,
                                                 "aggravated": aggravated},
                          {"damagePerSecond": per_second, "durationSeconds": rule["durationSeconds"]},
                          {"grade": "pob", "references": refs}, 1e-9))

    poison = rules["poison"]
    cases.append(pob_case("poison-base-stacks", {"hitDamage": [100, 50, 200]},
                          {"activeStacks": poison["baseMaxStacks"],
                           "damagePerSecond": 200 * poison["damagePercentPerMinute"] / 6000,
                           "durationSeconds": poison["durationSeconds"]},
                          (SETUP, 55, 55), (MISC, 88, 95), tolerance=1e-9))

    # Modules/CalcSetup.lua:671-672 and Modules/CalcPerform.lua:501-515.
    for level, strength, intelligence in ((1, 0, 0), (20, 10, 30), (65, 450, 300), (100, 200, 600)):
        life, mana = rules["life"], rules["mana"]
        cases.append(pob_case(f"life-mana-lv{level}", {"level": level, "strength": strength, "intelligence": intelligence},
                              {"life": life["base"] + life["perLevel"] * level + life["perAttribute"] * strength,
                               "mana": mana["base"] + mana["perLevel"] * level + mana["perAttribute"] * intelligence},
                              (MISC, 155, 156), (SETUP, 671, 672), (PERFORM, 501, 515)))
    cases.append(pob_case("life-mana-rounding", {"level": 1, "strength": 20, "intelligence": 20,
                                                "flatLife": 27, "flatMana": 21, "increasedPercent": 5, "moreMultiplier": 1.1},
                          {pool: math.floor((rules[pool]["base"] + rules[pool]["perLevel"] +
                                             20 * rules[pool]["perAttribute"] + flat) * 1.05 * 1.1 + 0.5)
                           for pool, flat in (("life", 27), ("mana", 21))}, (DEFENCE, 86, 95)))
    return cases


def project_xp(exile, enemy, per_level, cap, floor, curve):
    # Decision 198 uses the PoB safe zone and base penalty. INI controls the project's cap and floor.
    exile = min(100, max(1, exile))
    enemy = min(100, max(1, enemy))
    if enemy >= exile:
        return min(cap, 1 + (enemy - exile) * per_level)
    safe_base, safe_divisor, numerator_offset, denominator_offset, difference_power, penalty_power = curve
    outside_safe_zone = exile - enemy - (safe_base + exile // safe_divisor)
    if outside_safe_zone <= 0:
        return 1
    denominator = exile + denominator_offset + outside_safe_zone ** difference_power
    return max(floor, ((exile + numerator_offset) / denominator) ** penalty_power)


def project_cases(rules, ini):
    parser = ConfigParser(interpolation=None)
    parser.read_string(ini.read_text(encoding="utf-8-sig"))
    xp = parser["Experience"]
    per_level, cap, floor = (xp.getfloat(key) for key in (
        "HigherEnemyBonusPerLevel", "HigherEnemyMaxMultiplier", "LowerEnemyMinMultiplier"))
    cases = []
    # Decision 203 defines the defaults independently of both the pack and INI overrides.
    base_spirit, spirit_rewards = 100, (30, 30, 40, 30)
    for mask in range(1 << len(spirit_rewards)):
        awards = (amount for bit, amount in enumerate(spirit_rewards) if mask & (1 << bit))
        cases.append(case(f"spirit-mask-{mask}", {"achievementMask": mask},
                          {"spirit": base_spirit + sum(awards)},
                          {"grade": "project", "decisions": [197, 203]}))
    for exile, enemy in ((20, 20), (20, 21), (20, 29), (20, 30), (20, 40),
                         (20, 16), (20, 15), (60, 20), (100, 1), (105, 105)):
        cases.append(case(f"xp-lv{exile}-enemy{enemy}", {"exileLevel": exile, "effectiveMonsterLevel": enemy},
                          {"multiplier": project_xp(exile, enemy, per_level, cap, floor, rules["xpCurve"])},
                          {"grade": "project", "decisions": [198],
                           "references": references((BUILD, 218, 225), (KILL_TRACKER, 272, 299), (INI_PATH, 51, 54))}, 1e-9))
    # One hit has 50% chances for each effect, with rolls on opposite sides of the threshold.
    cases.append(case("ailment-independent-rolls", {"threshold": 100, "damagePerType": 100, "chance": 50,
                                                   "rolls": [0.49, 0.51, 0.51, 0.49]},
                      {"applied": [True, False, False, True]}, {
                          "grade": "project", "decisions": [205, 209],
                          "interpretation": "Roll each eligible ailment separately."}, 1e-9))
    return cases


def flammability_sequence(hits, duration, chance_multiplier, cap, threshold):
    # KeywordPopups: Ignite/Flammability. Add this hit before rolling and expire each entry independently.
    stacks = []
    chances = []
    for hit in hits:
        time = hit["timeSeconds"]
        stacks = [(expiry, magnitude) for expiry, magnitude in stacks if time < expiry]
        stacks.append((time + duration, hit["fireDamage"] / threshold * chance_multiplier))
        chances.append(min(cap, sum(magnitude for _, magnitude in stacks)))
    return chances


def conversion_cases(rules, ini):
    parser = ConfigParser(interpolation=None)
    parser.read_string(ini.read_text(encoding="utf-8-sig"))
    config = parser["Conversion"]
    cases = []
    life_table = rules["levelTables"]["life"]
    k = config.getfloat("OffenseK")
    rollback = config.getfloat("OffenseRollback")
    classes = {entry["name"]: entry for entry in json.loads((ROOT / "data/packs/core_3_30.json").read_bytes())["classes"]}
    scion = classes["Scion"]
    for level in (1, 20, 50, 80, 100):
        cases.append(case(f"offense-life-lv{level}", {"level": level, "offenseK": k, "offenseRollback": rollback},
                          {"referenceLife": k * (life_table[level - 1] / life_table[0]) ** (1 - rollback)},
                          {"grade": "conversion", "decisions": [62, 205]}, 1e-9))
        # With full rollback, the weighted profile's pass is divided by its own level's pass.
        cases.append(case(f"physical-profile-lv{level}", {
            "level": level, "baseHit": config.getfloat("DefenseDref"), "defenseRollback": 1,
        }, {"normalizedPass": 1}, {"grade": "conversion", "decisions": [69, 74, 142]}, 1e-9))
        life = rules["life"]
        base_life = life["base"] + life["perLevel"] * level + scion["baseStr"] * life["perAttribute"]
        expected_life = math.floor(base_life * (1 + config.getfloat("ExpectedLifeIncrease")) + 0.5)
        cases.append(case(f"expected-life-lv{level}", {
            "level": level, "class": "Scion", "baseSkyrimHealth": 123, "poeLife": expected_life,
        }, {"skyrimHealth": 123}, {"grade": "conversion", "decisions": [70, 208]}, 1e-9))

    for name, stats in classes.items():
        cases.append(case(f"starting-pools-{name}", {"class": name, "startingStrength": stats["baseStr"],
                                                     "startingIntelligence": stats["baseInt"]},
                          {"healthMultiplier": 1, "magickaMultiplier": 1},
                          {"grade": "conversion", "decisions": [208]}, 1e-9))

    for name, shield, damage, difficulty in (("full", 100, 20, 3), ("partial", 45, 20, 3), ("easy", 10, 20, 0.5)):
        effective = damage * difficulty
        result = split_damage(shield, effective, rules["energyShield"]["normalConsumptionMultiplier"])
        cases.append(case(f"es-difficulty-{name}", {
            "energyShield": shield, "rawDamage": damage, "difficulty": difficulty,
        }, {"esRemaining": result["esRemaining"], "absorbedEffectiveDamage": effective - result["lifeLost"],
            "remainingRawDamage": result["lifeLost"] / difficulty},
            {"grade": "conversion", "decisions": [67, 70]}, 1e-9))
    return cases


def game_cases(rules):
    provenance = rules["provenance"]
    cases = []
    for ailment, kind in (("bleed", "physical"), ("poison", "chaos")):
        key = f"{ailment}Bypasses"
        cases.append(case(f"es-{ailment}-bypass", {"ailment": ailment, "damageType": kind,
                                                 "damageOverTime": True, "energyShield": 60, "damage": 50},
                          split_damage(60, 50, 1, rules["energyShield"][key]), provenance[f"energyShield.{key}"]))

    for name, damage in (("absorbed", 50), ("overflow", 80)):
        result = split_damage(60, damage, rules["energyShield"]["normalConsumptionMultiplier"])
        cases.append(case(f"es-ignite-{name}", {"ailment": "ignite", "damageType": "fire",
                                                "damageOverTime": True, "energyShield": 60, "damage": damage},
                          {**result, "secondsSinceEsLoss": 0},
                          {"grade": "gameData", "references": rules["supplementalGameData"]["igniteEnergyShield"],
                           "interpretation": "Ignite has no ES bypass clause. Energy Shield specifies Bleeding and Poison, and recharge waits after ES loss."}))

    for target, interval in (("enemy", 8), ("player", 4)):
        ignite = rules["ignite"]
        duration = ignite[f"flammability{target.title()}DurationSeconds"]
        hits = [{"timeSeconds": time, "fireDamage": damage} for time, damage in
                ((0, 1000), (1, 3000), (2, 4000), (interval, 1000), (interval + 1, 1000),
                 (interval + 2, 1000), (interval * 2 + 1, 1000))]
        origin = provenance["ignite.flammabilityIncludesCurrentHit"]
        cases.append(case(f"ignite-flammability-{target}", {"target": target, "ailmentThreshold": 1000, "hits": hits},
                          {"chancePercentByHit": flammability_sequence(
                              hits, duration, ignite["chanceMultiplier"], ignite["flammabilityCapPercent"], 1000)},
                          {**origin, "references": [*origin["references"],
                              *provenance["ignite.flammabilityIndependentExpiry"]["references"],
                              *provenance["ignite.flammabilityCapPercent"]["references"]],
                           "pobConstants": [*provenance["ignite.chanceMultiplier"]["references"],
                               *provenance[f"ignite.flammability{target.title()}DurationSeconds"]["references"]]},
                          1e-9))

    # KeywordPopups: Chill, together with the linear PoB magnitude formula (CalcOffence.lua:5509).
    chill = rules["chill"]
    for damage in (10, 290, 300, 450, 500, 600):
        raw = chill["effectMultiplier"] * (damage / 1000)
        effect = 0 if raw < chill["minPercent"] else min(raw, chill["maxPercent"])
        cases.append(case(f"chill-game-{damage}", {"coldDamage": damage, "ailmentThreshold": 1000},
                          {"effectPercent": effect, "applies": effect > 0},
                          provenance["chill.discardBelowMinimum"], 1e-9))

    poise = rules["poise"]
    freeze = rules["freeze"]
    threshold = 1000
    steps = [{"timeSeconds": time, "coldDamage": damage} for time, damage in
             ((0, 300), (7, 0), (9, 0), (9, 210), (11, 1000), (13, 1000), (23, 400))]
    expected_steps = []
    time = buildup = decay_after = frozen_until = 0.0
    for step in steps:
        now, damage = step["timeSeconds"], step["coldDamage"]
        buildup = max(0, buildup - max(0, now - max(time, decay_after)) * poise["enemyDecayPercentPerSecond"] / 100)
        if damage and now >= frozen_until:
            buildup += damage * freeze["damageScale"] / threshold
            decay_after = now + poise["enemyDecayDelayMilliseconds"] / 1000
            if buildup * 100 >= poise["fullBuildupPercent"]:
                buildup = 0
                frozen_until = now + freeze["enemyDurationSeconds"]
        expected_steps.append({"buildupPercent": buildup * 100, "frozen": now < frozen_until})
        time = now
    freeze_origin = provenance["poise.fullBuildupPercent"]
    cases.append(case("freeze-game-buildup", {"poiseThreshold": threshold, "steps": steps},
                      {"steps": expected_steps}, {**freeze_origin, "pobConstants": [
                          ref for key in ("poise.enemyDecayDelayMilliseconds", "poise.enemyDecayPercentPerSecond",
                                          "freeze.damageScale", "freeze.enemyDurationSeconds")
                          for ref in provenance[key]["references"]
                      ]}, 1e-9))

    debuffs = {"grade": "gameData", "references": references((KEYWORDS, 1499, 1499),
                                                             (KEYWORDS, 2126, 2135))}
    for kind in ("ignite", "bleed", "poison"):
        duration = rules[kind]["durationSeconds"]
        weak_dps = 100 * rules[kind]["damagePercentPerMinute"] / 6000
        cases.append(case(f"ailment-handoff-{kind}", {"strongHit": 200, "weakHit": 100, "weakAt": duration / 2,
                                                     "crossingTick": duration * 3 / 4},
                          {"damageThroughCrossing": weak_dps * duration * 1.25,
                           "remainingDamage": weak_dps * duration / 4}, debuffs, 1e-9))
    for kind, strong, weak, duration in (
        ("chill", chill["maxPercent"], (chill["minPercent"] + chill["maxPercent"]) / 2, chill["enemyDurationSeconds"]),
        ("shock", rules["shock"]["effectPercent"], rules["shock"]["effectPercent"] / 2,
         rules["shock"]["enemyDurationSeconds"]),
    ):
        cases.append(case(f"ailment-handoff-{kind}", {"strongPercent": strong, "weakPercent": weak,
                                                     "weakAt": duration / 2, "strongExpires": duration},
                          {"effects": [strong, weak, 0]}, debuffs, 1e-9))

    cold_damage = 250
    for rarity, more, less, unique, map_boss in (
        ("magic", poise["magicThresholdMorePercent"], chill["magicSlowReductionPercent"], False, False),
        ("rare", poise["rareThresholdMorePercent"], chill["rareSlowReductionPercent"], False, False),
        ("unique", poise["uniqueThresholdMorePercent"], chill["uniqueSlowReductionPercent"], True, False),
        ("map-boss", poise["uniqueThresholdMorePercent"], chill["uniqueSlowReductionPercent"], True, True),
    ):
        effective_threshold = threshold * (1 + more / 100)
        poise_key = "unique" if unique else rarity
        refs = [*provenance[f"poise.{poise_key}ThresholdMorePercent"]["references"],
                *provenance[f"chill.{poise_key}SlowReductionPercent"]["references"]]
        if map_boss:
            effective_threshold *= 1 + poise["mapBossThresholdMorePercent"] / 100
            refs.extend(provenance["poise.mapBossThresholdMorePercent"]["references"])
        cases.append(case(f"ailment-rarity-{rarity}", {"rarity": rarity, "unique": unique, "mapBoss": map_boss,
                                                       "coldDamage": cold_damage, "poiseThreshold": threshold},
                          {"chillPercent": chill["maxPercent"] * (1 - less / 100),
                           "buildupPercent": cold_damage * freeze["damageScale"] * 100 /
                           math.floor(effective_threshold)}, {"grade": "gameData", "references": refs}, 1e-9))

    cold_damage, cold_resistance = 400, 40
    cases.append(case("ailment-freeze-resistance", {"coldDamage": cold_damage,
                                                   "coldResistancePercent": cold_resistance,
                                                   "poiseThreshold": threshold},
                      {"buildupPercent": cold_damage * (1 - cold_resistance / 100) *
                       freeze["damageScale"] * 100 / threshold},
                      {"grade": "gameData", "references": references((KEYWORDS, 32, 32),
                                                                      (OFFENCE, 4914, 4920),
                                                                      (OFFENCE, 5405, 5413))}, 1e-9))
    fire_damage = 100
    base_bonus = 40
    base_magnitude = fire_damage / threshold * rules["ignite"]["chanceMultiplier"]
    cases.append(case("ailment-flammability-chance-bonus", {"fireDamage": fire_damage, "threshold": threshold,
                                                            "firstBonus": base_bonus, "secondRoll": 0.43},
                      {"magnitudeByHit": [base_magnitude + base_bonus, 2 * base_magnitude + base_bonus],
                       "secondIgnites": True},
                      {"grade": "gameData", "references": references((KEYWORDS, 239, 239),
                                                                      (OFFENCE, 5449, 5464))}, 1e-9))

    armour = rules["armour"]
    origin = provenance["armour.brokenPlayerDurationSeconds"]
    cases.append(case("armour-break-player", {"target": "player", "fullyBroken": True},
                      {"durationSeconds": armour["brokenPlayerDurationSeconds"], "physicalHitBonusPercent": 0},
                      origin))
    duration = armour["brokenEnemyDurationSeconds"]
    cases.append(case("armour-break-enemy", {"target": "enemy", "damage": 100, "elapsedSeconds": duration},
                      {"physicalHit": 100 * (1 + armour["brokenEnemyHitDamageTakenPercent"] / 100),
                       "physicalDot": 100, "elementalHit": 100, "physicalHitAfterExpiry": 100},
                      provenance["armour.brokenEnemyHitDamageTakenPercent"], 1e-9))
    return cases


def generate_answers(pob, game_tables, ini=ROOT / INI_PATH):
    rules = read_reference_values(pob, game_tables)
    return {"pob": pob_cases(rules), "project": project_cases(rules, ini),
            "conversion": conversion_cases(rules, ini), "gameRule": game_cases(rules)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pob", type=Path, required=True)
    parser.add_argument("--game-tables", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / "data/poe2/answers/p2b.json")
    args = parser.parse_args(argv)
    try:
        write_atomic(args.out, json_bytes(generate_answers(args.pob, args.game_tables), indent=2))
    except (OSError, ValueError, KeyError, TypeError, AttributeError, ET.ParseError) as error:
        parser.exit(1, f"answers: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
