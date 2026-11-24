"""Build the SkyrimPoE rules pack from the pinned PoB and game table exports."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract import InputFiles, json_bytes, write_atomic
from luadata import numeric_key, parse, table_array, table_fields


ROOT = Path(__file__).resolve().parents[2]
MISC = "Data/Misc.lua"
DATA = "Modules/Data.lua"
DEFENCE = "Modules/CalcDefence.lua"
OFFENCE = "Modules/CalcOffence.lua"
PERFORM = "Modules/CalcPerform.lua"
SETUP = "Modules/CalcSetup.lua"
CONFIG = "Modules/ConfigOptions.lua"
BUILD = "Modules/Build.lua"
ROUND = "Modules/Common.lua"
KEYWORDS = "data/balance/keywordpopups.datc64.json"
MODS = "data/balance/mods.datc64.json"
POB_VERSION = "0.23.1"
GAME_VERSION = "0.5.5h"
NUM = r"(-?\d+(?:\.\d+)?)"


class RulesSource:
    def __init__(self, pob: Path, game_tables: Path):
        self.files = InputFiles(pob)
        self.manifest = ET.fromstring(self.files.read(Path("manifest.xml")))
        if self.manifest.find("Version").attrib["number"] != POB_VERSION:
            raise ValueError(f"expected PoB {POB_VERSION}")
        self.lines = {}
        raw = (game_tables / KEYWORDS).read_bytes()
        self.keyword_sha256 = hashlib.sha256(raw).hexdigest()
        self.keyword_text = raw.decode("utf-8-sig").splitlines()
        self.keywords = {row["Id"]: row["Definition"] for row in json.loads(raw)}
        self.game_tables = game_tables
        self.game_sha256 = {KEYWORDS: self.keyword_sha256}
        self.provenance = {}
        self.excerpts = {}

    def lua(self, path):
        if path not in self.lines:
            self.lines[path] = self.files.lua(Path(path)).splitlines()
        return self.lines[path]

    def lua_value(self, path, line, expression):
        return numeric_key(parse("return " + expression, f"{path}:{line}")[0].value)

    def lua_number(self, path, line, literal):
        value = self.lua_value(path, line, literal)
        if type(value) not in (int, float) or not math.isfinite(value):
            raise ValueError(f"{path}:{line}: expected a finite Lua number")
        return value

    def match(self, path, pattern, *, start=1, end=None):
        """Match a complete source row and return its line number and captures."""
        found = [(line, result.groups()) for line, text in enumerate(self.lua(path), 1)
                 if start <= line and (end is None or line <= end)
                 and (result := re.fullmatch(pattern, text.strip()))]
        if len(found) != 1:
            raise ValueError(f"{path}:{start}-{end or 'EOF'}: expected one row matching {pattern}")
        return found[0]

    def ref(self, path, line, end=None):
        return {"file": path, "line": line, **({"endLine": end} if end is not None else {})}

    def pob(self, key, value, *refs):
        self.provenance[key] = {"grade": "pob", "references": list(refs)}
        return value

    def assignments(self, path, table, keys, *, start=1, end=None):
        values = {}
        for key, source in keys.items():
            lhs = f'["{source}"]' if path == MISC else source
            pattern = re.escape(lhs) + r"\s*=\s*" + NUM + r",\s*(?:--.*)?"
            line, (literal,) = self.match(path, pattern, start=start, end=end)
            values[key] = self.pob(f"{table}.{key}", self.lua_number(path, line, literal),
                                   self.ref(path, line))
        return values

    def formula(self, section, path, template, keys, *, start=1, end=None, refs=()):
        pattern = re.escape(template).replace(re.escape("{}"), NUM)
        line, literals = self.match(path, pattern, start=start, end=end)
        if len(literals) != len(keys):
            raise ValueError(f"{path}:{line}: incorrect rule extraction")
        return {
            name: self.pob(f"{section}.{name}", self.lua_number(path, line, literal),
                           self.ref(path, line), *refs)
            for name, literal in zip(keys, literals, strict=True)
        }

    def compare_sentence(self, key, sentence):
        definition = self.keywords[key]
        sentences = re.split(r"(?<=[.!?])[^\S\r\n]+|[\r\n]+", definition)
        if [text.strip() for text in sentences].count(sentence) != 1:
            raise ValueError(f"{KEYWORDS}: {key}: source wording changed: {sentence!r}")
        id_line = next(line for line, text in enumerate(self.keyword_text, 1)
                       if text.strip() == f'"Id": "{key}",')
        definition_line = next(line for line in range(id_line + 1, len(self.keyword_text) + 1)
                               if self.keyword_text[line - 1].lstrip().startswith('"Definition": '))
        reference = {"file": KEYWORDS, "line": definition_line, "id": key}
        self.excerpts[key, sentence] = {**reference, "excerpt": sentence}
        return reference

    def game(self, key, value, *refs, pob_refs=()):
        origin = {"grade": "gameData", "references": list(refs)}
        if pob_refs:
            origin.update(pobDiffers=True, pobReferences=list(pob_refs))
        self.provenance[key] = origin
        return value

    def meta(self):
        sha1 = {entry.attrib["name"].replace("\\", "/"): entry.attrib["sha1"]
                for entry in self.manifest.findall("File") if "sha1" in entry.attrib}
        return {
            "schema": "skyrimpoe.rules", "schemaVersion": 1,
            "pobVersion": POB_VERSION, "extractorVersion": "build_rules/2",
            "sourceSha256": {**self.files.sha256, **self.game_sha256},
            "pobManifestSha1": {name: sha1[name] for name in self.files.sha256 if name in sha1},
        }


def defence_rules(source):
    formula = source.formula
    refs = source.ref
    ratio = source.assignments(DATA, "armour", {"damageCoefficient": "ArmourRatio"})
    cap_key = "maximum_physical_damage_reduction_%"
    cap = source.assignments(MISC, "armour", {"physicalReductionCapPercent": cap_key}, start=143, end=244)
    cap_line, _ = source.match(DATA, re.escape(f'DamageReductionCap = data.characterConstants["{cap_key}"],'))
    source.provenance["armour.physicalReductionCapPercent"]["references"].append(refs(DATA, cap_line))
    rounding = formula("armour", ROUND, "return m_floor(val + {})", ("roundingOffset",), start=648, end=654)
    zero = formula("armour", DEFENCE, "return {}", ("zeroOverZeroPercent",), start=57, end=58)
    broken = formula("armour", PERFORM,
                     'local effect = {} * (1 + modDB:Sum("INC", nil, "FullyBrokenArmourEffect") / 100)',
                     ("brokenEnemyHitDamageTakenPercent",), refs=(refs(PERFORM, 720, 725),))
    config = "Modules/ConfigOptions.lua"
    line, (tooltip,) = source.match(config, r'\{ var = "conditionEnemyArmourBroken", .*?tooltip = ("[^"]+"), apply = function\(val, modList, enemyModList\)')
    expected_tooltip = (
        "Some Skills, Items, Support Gems and other effects can Break Armour, which lowers a target's Armour by a specified amount.\n"
        "If this brings the target's Armour value to 0, their Armour is Fully Broken for 12 seconds."
    )
    if source.lua_value(config, line, tooltip) != expected_tooltip:
        raise ValueError(f"{config}:{line}: armour break tooltip changed")
    enemy_duration = int(re.search(r"(\d+) seconds", expected_tooltip)[1])
    break_text = "If this brings the target's [Armour|Armour] value to 0, their [Armour|Armour] is Fully Broken for 12 seconds, or 4 seconds for players."
    break_sentence = source.compare_sentence("ArmourBreak", break_text)
    broken_bonus_sentence = source.compare_sentence(
        "ArmourBreak", "On top of not benefitting from [Armour], non-player targets with Fully Broken [Armour] take 20% increased [Physical] damage from [Hit|Hits].")
    source.provenance["armour.brokenEnemyHitDamageTakenPercent"]["corroboratedBy"] = [broken_bonus_sentence]
    player_duration = int(re.search(r"(\d+) seconds for players", break_text)[1])
    armour = {
        **ratio, **cap, **rounding, **zero, **broken,
        "brokenEnemyDurationSeconds": source.pob("armour.brokenEnemyDurationSeconds",
                                                enemy_duration, refs(config, line)),
        "negativeArmourSign": source.pob("armour.negativeArmourSign", -1, refs(DEFENCE, 59, 63)),
        "brokenPlayerDurationSeconds": source.game("armour.brokenPlayerDurationSeconds",
                                                   player_duration, break_sentence),
    }
    source.match(DEFENCE, re.escape("armour = -armour -- revert value to positive for calculation"))
    source.match(DEFENCE, re.escape("return -((armour / (armour + raw * data.misc.ArmourRatio) * 100))"))

    energy_shield = {
        **source.assignments(DATA, "energyShield", {"rechargeDelaySeconds": "EnergyShieldRechargeDelay"}),
        **source.assignments(MISC, "energyShield", {"rechargePercentPerMinute": "energy_shield_recharge_rate_per_minute_%"}, start=143, end=244),
        **formula("energyShield", DEFENCE,
                  'local esDamageTypeMultiplier = damageType == "Chaos" and not modDB:Flag(nil, "ChaosNotDoubleESDamage") and {} or {}',
                  ("chaosConsumptionMultiplier", "normalConsumptionMultiplier")),
    }
    for name, effect, sentence in (
        ("bleedBypasses", "Bleeding", "Damage from Bleeding bypasses [EnergyShield|Energy Shield]"),
        ("poisonBypasses", "Poison", "Damage from Poison bypasses [EnergyShield|Energy Shield]."),
    ):
        energy_shield[name] = source.game(
            f"energyShield.{name}", True, source.compare_sentence(effect, sentence),
            pob_refs=(refs(DEFENCE, 2733, 2747), refs(DEFENCE, 4075, 4094), refs(DEFENCE, 4179, 4193)))

    hit = {
        **formula("hit", DEFENCE, "local rawChance = ( accuracy * {} ) / ( accuracy + evasion * {} ) * 100",
                  ("playerAccuracyMultiplier", "playerEvasionMultiplier")),
        **formula("hit", DEFENCE, "local rawChance = ( 1 - ( {} * evasion ) / ( evasion + {} * accuracy ) ) * 100",
                  ("monsterEvasionMultiplier", "monsterAccuracyMultiplier")),
        **formula("hit", DEFENCE, "return m_max(m_min(round(rawChance), {}), {})",
                  ("maxPercent", "minPercent"), start=40, end=46),
        **formula("hit", DEFENCE, "return {}", ("negativeAccuracyPercent",), start=40, end=43),
        **source.assignments(MISC, "hit", {"maxEvadePercent": "DefaultMaxEvadeChancePercent"}),
        **source.assignments(MISC, "hit", {"baseEvasion": "base_evasion_rating", "accuracyPerLevel": "accuracy_rating_per_level"}),
        **source.assignments(DATA, "hit", {"accuracyPerDexterity": "AccuracyPerDexBase"}),
    }
    return {
        "armour": armour, "energyShield": energy_shield, "hit": hit,
        "block": {
            **source.assignments(MISC, "block", {"baseMaxPercent": "object_inherent_base_maximum_block_%_from_ot"}, start=143, end=244),
            **source.assignments(DATA, "block", {"capPercent": "BlockChanceCap"}),
        },
        "deflect": {
            **source.assignments(DATA, "deflect", {"capPercent": "DeflectionChanceCap"}),
            **source.assignments(MISC, "deflect", {"baseReductionPercent": "BasePercentDamageDeflected"}),
            **formula("deflect", DEFENCE, "if deflection < {} then", ("minimumRating",)),
            **formula("deflect", DEFENCE, "local chanceToNotDeflect = accuracy / ( accuracy + deflection * {} ) * {} - {}",
                      ("ratingCoefficient", "chanceScale", "chanceOffsetPercent")),
            **formula("deflect", DEFENCE, "return m_max(m_min({} - round(chanceToNotDeflect), data.misc.DeflectionChanceCap), {})",
                      ("percentScale", "minPercent")),
        },
        "resistance": {
            **source.assignments(MISC, "resistance", {"baseMaxPercent": "base_maximum_all_resistances_%"}, start=143, end=244),
            **source.assignments(MISC, "resistance", {"monsterBaseMaxPercent": "base_maximum_all_resistances_%"}, start=246, end=280),
            **source.assignments(DATA, "resistance", {"capPercent": "MaxResistCap"}),
        },
    }


def rarity_poise_modifiers(source):
    raw = (source.game_tables / MODS).read_bytes()
    source.game_sha256[MODS] = hashlib.sha256(raw).hexdigest()
    rows = json.loads(raw)
    lines = raw.decode("utf-8-sig").splitlines()
    modifiers = {}
    stat_id = "poise_threshold_+%_final_from_rarity"
    for rarity in ("Magic", "Rare"):
        mod_id = f"Monster{rarity}2"
        row, = (row for row in rows if row["Id"] == mod_id)
        stat, = (key for key in ("Stat1", "Stat2", "Stat3", "Stat4", "Stat5", "Stat6")
                 if (row.get(key) or {}).get("Id") == stat_id)
        low, high = row[f"{stat}Value"]
        if low != high or type(low) is not int or low <= 0:
            raise ValueError(f"{MODS}: {mod_id}: expected a fixed positive poise modifier")
        start = next(i for i, line in enumerate(lines) if line.strip() == f'"Id": "{mod_id}",')
        stat_line = next(i for i in range(start, len(lines)) if lines[i].strip() == f'"Id": "{stat_id}"')
        value_line = next(i for i in range(stat_line, len(lines)) if lines[i].strip() == f'"{stat}Value": [')
        key = f"{rarity.lower()}ThresholdMorePercent"
        modifiers[key] = source.game(f"poise.{key}", low,
                                     {**source.ref(MODS, stat_line + 1, value_line + 3), "id": mod_id})
    return modifiers


def ailment_rules(source):
    groups = {
        "ignite": {"damagePercentPerMinute": "IgniteHitDamagePercentPerMinute",
                   "durationSeconds": "BaseIgniteDuration", "chanceMultiplier": "IgniteChanceMultiplier",
                   "flammabilityEnemyDurationSeconds": "BaseFlammabilityDuration",
                   "flammabilityPlayerDurationSeconds": "BaseFlammabilityDurationPlayer",
                   "nonHitFlammabilityMultiplier": "IgniteNonHitFlammabilityModifier",
                   "nonHitIgniteThresholdPermyriad": "NonHitFlammabilityPermyriadThresholdToIgnite"},
        "bleed": {"damagePercentPerMinute": "BleedingHitDamagePercentPerMinute",
                  "durationSeconds": "BaseBleedingDuration"},
        "poison": {"damagePercentPerMinute": "PoisonHitDamagePercentPerMinute",
                   "durationSeconds": "BasePoisonDuration"},
        "shock": {"effectPercent": "BaseShockMagnitude", "chanceMultiplier": "ShockChanceMultiplier",
                  "enemyDurationSeconds": "BaseShockDuration", "playerDurationSeconds": "BaseShockDurationPlayer"},
        "chill": {"effectMultiplier": "ChillEffectMultiplier", "maxPercent": "ChillMaxEffect",
                  "enemyDurationSeconds": "BaseChillDuration", "playerDurationSeconds": "BaseChillDurationPlayer"},
        "freeze": {"damageScale": "FreezeDamageScale", "playerDamageScale": "FreezeDamageScalePlayer",
                   "enemyDurationSeconds": "FreezeDuration", "playerDurationSeconds": "FreezeDurationPlayer"},
        "electrocute": {"damageScale": "ElectrocuteDamageScale",
                        "enemyDurationMilliseconds": "ElectrocuteDuration",
                        "playerDurationMilliseconds": "ElectrocuteDurationPlayer",
                        "repeatThresholdModifierPercent": "ElectrocuteThresholdModifier",
                        "repeatModifierDurationSeconds": "ElectrocuteModifierDuration"},
        "heavyStun": {"damageScale": "HeavyStunDamageScale", "playerDamageScale": "HeavyStunDamageScalePlayer",
                      "repeatThresholdModifierPercent": "HeavyStunThresholdModifier",
                      "repeatModifierDurationSeconds": "HeavyStunModifierDuration",
                      "playerRepeatThresholdModifierPercent": "HeavyStunThresholdModifierPlayer",
                      "playerRepeatModifierDurationSeconds": "HeavyStunModifierDurationPlayer"},
        "pin": {"damageScale": "PinDamageScale", "enemyDurationSeconds": "PinDuration",
                "playerDurationSeconds": "PinDurationPlayer",
                "repeatThresholdModifierPercent": "PinThresholdModifier",
                "repeatModifierDurationSeconds": "PinModifierDuration"},
        "poise": {"playerDecayDelayMilliseconds": "poise_decay_delay_ms",
                  "playerDecayPercentPerSecond": "poise_decay_%_per_second"},
        "ailmentThreshold": {"playerLifeFactor": "PlayerAilmentThresholdLifeFactor"},
    }
    result = {group: source.assignments(MISC, group, keys, start=19, end=244)
              for group, keys in groups.items()}
    bleed_line, (bleed_more,) = source.match(
        SETUP, r'modDB:NewMod\("AilmentMagnitude", "MORE", ' + NUM +
        re.escape(', "Base", 0, KeywordFlag.Bleed, { type = "ActorCondition", actor = "enemy", '
                  'varList = { "Moving", "BleedAggravated" } }, { type = "Condition", '
                  'var = "NoExtraBleedDamageToMovingEnemy", neg = true })'))
    bleed_ref = source.compare_sentence(
        "Bleeding", f"Bleeding deals an extra {bleed_more}% damage while the target is moving, "
        "or if the Bleeding is [Aggravate|Aggravated].")
    result["bleed"]["movingOrAggravatedMultiplier"] = source.pob(
        "bleed.movingOrAggravatedMultiplier",
        1 + source.lua_number(SETUP, bleed_line, bleed_more) / 100, source.ref(SETUP, bleed_line), bleed_ref)

    result["poison"].update(source.formula(
        "poison", SETUP, 'modDB:NewMod("PoisonStacksMax", "BASE", {}, "Base")', ("baseMaxStacks",)))
    for key, label, comment in (
        ("uniqueThresholdMorePercent", "Unique", " -- MonsterUnique2"),
        ("mapBossThresholdMorePercent", "Map Boss", ""),
    ):
        result["poise"].update(source.formula(
            "poise", CONFIG,
            f'enemyModList:NewMod("PoiseThreshold", "MORE", {{}}, "{label}", {{ type = "Condition", var = "Effective" }}){comment}',
            (key,), start=1999, end=2010, refs=(source.ref(OFFENCE, 5403, 5417),)))
    result["poise"].update(rarity_poise_modifiers(source))
    result["poise"].update(source.assignments(
        MISC, "poise", {"enemyDecayDelayMilliseconds": "poise_decay_delay_ms",
                        "enemyDecayPercentPerSecond": "poise_decay_%_per_second"}, start=246, end=280))
    full_buildup_text = "Targets become Frozen when they reach 100% [Freeze] buildup."
    full_buildup = source.compare_sentence("Frozen", full_buildup_text)
    result["poise"]["fullBuildupPercent"] = source.game(
        "poise.fullBuildupPercent", int(re.search(r"(\d+)%", full_buildup_text)[1]), full_buildup)
    for target, start, end in (("player", 143, 244), ("enemy", 246, 280)):
        result["heavyStun"].update(source.assignments(
            MISC, "heavyStun", {f"{target}DurationMilliseconds": "base_heavy_stun_duration_ms"},
            start=start, end=end))
    result["chill"].update(source.formula(
        "chill", DATA, '["Chill"] = { associatedType = "Cold", alt = false, default = {}, min = {}, max = data.gameConstants["ChillMaxEffect"], precision = {}, duration = data.gameConstants["BaseChillDuration"] },',
        ("defaultPercent", "minPercent", "precision")))
    chill_gate = source.compare_sentence(
        "Chill", "However, Chills smaller than 30% will be ignored, so small [Hit|Hits] can fail to Chill.")
    chill_range = source.compare_sentence(
        "Chill", "Chill [BuffMagnitude|Magnitude] depends on the Chilling damage dealt relative to the target's [AilmentThreshold|Elemental Ailment Threshold], with a minimum of 30% and a default maximum of 50%.")
    result["chill"]["discardBelowMinimum"] = source.game(
        "chill.discardBelowMinimum", True, chill_gate, chill_range,
        pob_refs=(source.ref(OFFENCE, 5385, 5396), source.ref(OFFENCE, 5507, 5510),
                  source.ref(OFFENCE, 5571, 5575)))
    for rarity in ("Magic", "Rare", "Unique"):
        line, = (line for line in source.keywords["Slow"].splitlines()
                 if re.fullmatch(r"\d+% less Slow effect on " + rarity + " monsters", line))
        reduction = int(line.split("%", 1)[0])
        ref = source.compare_sentence("Slow", line)
        key = f"{rarity.lower()}SlowReductionPercent"
        result["chill"][key] = source.game(f"chill.{key}", reduction, ref)
        if rarity == "Unique":
            pob_line, (pob_slow,) = source.match(
                CONFIG, r'enemyModList:NewMod\("SlowEffectOnSelf", "MORE", ' + NUM +
                re.escape(', "Unique", { type = "Condition", var = "Effective" }) -- MonsterUnique11'),
                start=1999, end=2010)
            if reduction != -source.lua_number(CONFIG, pob_line, pob_slow):
                raise ValueError("unique slow reduction disagrees between PoB and the game")
            source.provenance[f"chill.{key}"]["references"].append(source.ref(CONFIG, pob_line))
    chance = source.compare_sentence(
        "Ignite", "The chance for a [Hit] to Ignite a target is determined by the total [BuffMagnitude|Magnitude] of [Flammability] on the target, including the amount added by that [Hit].")
    expiry = source.compare_sentence(
        "Flammability", "Each instance of Flammability has its own independent duration, lasting 8 seconds by default.")
    cap_text = "Multiple instances of Flammability stack, raising the chance for [Hit|Hits] to [Ignite] the target up to 100%."
    cap = source.compare_sentence("Flammability", cap_text)
    fire_ratio = source.compare_sentence(
        "Flammability", "The base [BuffMagnitude|Magnitude] of Flammability is 1% chance to Ignite the target for every 5% of the target's [AilmentThreshold|Ailment Threshold] dealt as [Fire] damage.")
    pob_refs = (source.ref(OFFENCE, 5449, 5464),)
    ignite = result["ignite"]
    ignite.update(
        flammabilityIncludesCurrentHit=source.game("ignite.flammabilityIncludesCurrentHit", True, chance, pob_refs=pob_refs),
        flammabilityIndependentExpiry=source.game("ignite.flammabilityIndependentExpiry", True, expiry, pob_refs=pob_refs),
        flammabilityCapPercent=source.game("ignite.flammabilityCapPercent",
                                          int(re.search(r"(\d+)%", cap_text)[1]), cap),
    )
    source.provenance["ignite.chanceMultiplier"]["corroboratedBy"] = [fire_ratio]
    source.provenance["ignite.flammabilityEnemyDurationSeconds"]["corroboratedBy"] = [expiry]
    return result


def build_rules(pob: Path, game_tables: Path):
    source = RulesSource(pob, game_tables)
    result = defence_rules(source)
    result.update(ailment_rules(source))
    for section, target, per_level, attribute, kind in (
        ("life", "Life", "life_per_level", "Str", "Strength"),
        ("mana", "Mana", "mana_per_level", "Int", "Intelligence"),
    ):
        result[section] = {
            **source.assignments(MISC, section, {"perLevel": per_level}),
            **source.formula(section, SETUP,
                             f'modDB:NewMod("{target}", "BASE", data.characterConstants["{per_level}"], "Base", {{ type = "Multiplier", var = "Level", base = {{}} }})',
                             ("base",), start=671, end=672),
            **source.formula(section, PERFORM,
                             f'modDB:NewMod("{target}", "BASE", output.{attribute} * {{}} * inherentAttributeMultiplier, "{kind}")',
                             ("perAttribute",), start=504, end=515),
        }
    crit_key = "base_critical_hit_damage_bonus"
    result["critical"] = {
        **source.assignments(MISC, "critical", {"playerBonusPercent": crit_key}, start=143, end=244),
        **source.assignments(MISC, "critical", {"monsterBonusPercent": crit_key}, start=246, end=280),
        **source.assignments(MISC, "critical", {"playerMinionAdditionalBonusPercent": crit_key}, start=282, end=287),
    }
    for name, line in (("playerBonusPercent", source.ref(SETUP, 679)),
                       ("monsterBonusPercent", source.ref("Modules/ConfigOptions.lua", 1968)),
                       ("playerMinionAdditionalBonusPercent", source.ref(PERFORM, 1064))):
        source.provenance[f"critical.{name}"]["references"].append(line)

    result["levelTables"] = {}
    for name in ("Life", "Damage", "Accuracy", "Evasion", "AilmentThreshold", "PoiseThreshold"):
        line, (expression,) = source.match(MISC, rf"data\.monster{name}Table = (\{{.*\}})")
        values = [numeric_key(value) for value in table_array(source.lua_value(MISC, line, expression))]
        if len(values) != 100 or any(type(value) not in (int, float) or
                                     not math.isfinite(value) or value <= 0 for value in values):
            raise ValueError(f"{MISC}:{line}: expected positive values for levels 1 through 100")
        key = name[0].lower() + name[1:]
        result["levelTables"][key] = source.pob(f"levelTables.{key}", values, source.ref(MISC, line))
    line, (expression,) = source.match(MISC, r"data\.mapLevelLifeMult = (\{.*\})")
    map_life = {key: numeric_key(value) for key, value in table_fields(source.lua_value(MISC, line, expression))}
    if set(map_life) != set(range(66, 91)) or any(
            type(value) not in (int, float) or not math.isfinite(value) or value <= 0
            for value in map_life.values()):
        raise ValueError(f"{MISC}:{line}: expected positive map life multipliers for levels 66 through 90")
    fallback_line, (fallback,) = source.match(
        OFFENCE, r"output\.BaseCorpseLife = .* \* \(env\.data\.mapLevelLifeMult\[env\.enemyLevel\] or " + NUM + r"\)")
    default = source.lua_number(OFFENCE, fallback_line, fallback)
    result["levelTables"]["mapLevelLifeMult"] = source.pob(
        "levelTables.mapLevelLifeMult", [map_life.get(level, default) for level in range(1, 101)],
        source.ref(MISC, line), source.ref(OFFENCE, fallback_line))

    formula = source.formula
    result["experienceReference"] = {
        **source.assignments(DATA, "experienceReference", {
            "maxPenaltyFreeAreaLevel": "maxExperiencePenaltyFreeAreaLevel",
            "effectiveAreaLevelPenaltyMultiplier": "experiencePenaltyMultiplier"}),
        **formula("experienceReference", BUILD, "local safeZone = {} + m_floor(playerLevel / {})",
                  ("safeZoneBase", "safeZoneLevelDivisor")),
        **formula("experienceReference", BUILD, "mult = ((playerLevel + {}) / (playerLevel + {} + diff ^ {})) ^ {}",
                  ("numeratorLevelOffset", "denominatorLevelOffset", "differenceExponent", "penaltyExponent")),
        **formula("experienceReference", BUILD, "if playerLevel >= {} then",
                  ("highLevelPenaltyStartsAt",), start=218, end=230),
        **formula("experienceReference", BUILD, "if diff <= {} then",
                  ("safeDifferenceMax",), start=218, end=230),
        **formula("experienceReference", BUILD, "mult = {}",
                  ("safeMultiplier",), start=218, end=230),
        **formula("experienceReference", BUILD, "if mult > {} then",
                  ("displayMinimumMultiplierExclusive",), start=218, end=234),
        **formula("experienceReference", BUILD, "mult = mult * (1 / (1 + {} * (playerLevel - {}))) * xpPenalty",
                  ("highLevelPenaltyPerLevel", "highLevelPenaltyOffset")),
    }
    line, (expression,) = source.match(BUILD, r"local xpPenalty = \((\{.*\})\)\[playerLevel - 94\] or 0")
    result["experienceReference"]["highLevelMultipliers"] = source.pob(
        "experienceReference.highLevelMultipliers", [numeric_key(value)
            for value in table_array(source.lua_value(BUILD, line, expression))],
        source.ref(BUILD, line))

    result["project"] = {
        "spirit": {"base": 100, "achievementRewards": [30, 30, 40, 30], "decision": 203},
        "experience": {"higherEnemyBonusPerLevel": 0.10, "higherEnemyMaxMultiplier": 2.0, "decision": 198},
    }
    for name, values in result["project"].items():
        for key in values.keys() - {"decision"}:
            source.provenance[f"project.{name}.{key}"] = {
                "grade": "project", "decisions": [values["decision"]],
            }
    source.lua(OFFENCE)
    source.files.read(Path("Data/SkillStatMap.lua"))
    rules = {
        "meta": source.meta(),
        "rulesSource": {"pob": "PathOfBuilding-PoE2", "pobVersion": POB_VERSION, "gameData": "0.5"},
        "supplementalGameData": {
            "version": GAME_VERSION,
            "igniteEnergyShield": [
                source.compare_sentence("Ignite", "Ignite is an [Ailments|Ailment] that deals [Fire] damage over time, and lasts for 4 seconds by default."),
                source.compare_sentence("EnergyShield", "Damage from [Bleeding] and [Poison] bypasses Energy Shield to remove Life directly."),
                source.compare_sentence("EnergyShield", "Rapidly [ESRecharge|Recharges] if you don't lose Energy Shield for a short time."),
            ],
        },
        **result,
        "provenance": dict(sorted(source.provenance.items())),
    }
    provenance = {
        "gameDataVersion": GAME_VERSION,
        "sourceSha256": source.game_sha256,
        "excerpts": [source.excerpts[key] for key in sorted(source.excerpts)],
    }
    return rules, provenance


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pob", type=Path, required=True)
    parser.add_argument("--game-tables", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / "data/packs/rules.json")
    parser.add_argument("--provenance-out", type=Path, default=ROOT / "data/poe2/rules-provenance.json")
    args = parser.parse_args(argv)
    try:
        rules, provenance = build_rules(args.pob, args.game_tables)
        write_atomic(args.out, json_bytes(rules, indent=2))
        write_atomic(args.provenance_out, json_bytes(provenance, indent=2))
    except (OSError, ValueError, KeyError, TypeError, AttributeError, ET.ParseError) as error:
        parser.exit(1, f"build_rules: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
