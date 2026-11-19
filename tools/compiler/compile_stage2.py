r"""PoB 2.68.1 (PoE 3.30) 데이터를 2단계용 데이터 팩(core_3_30.json)과 언어 팩으로 변환한다.

사용법:
  python tools/compiler/compile_stage2.py [--pob G:\poe\PathOfBuilding-2.68.1] [--out data]
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any

DEFAULT_POB = Path(r"G:\poe\PathOfBuilding-2.68.1")
DEFAULT_OUT = Path("data")

SLOTS = ("body", "helmet", "gloves", "boots")
DEFENSE_TYPES = {
    "Armour": "Armour",
    "Evasion": "Evasion",
    "Energy Shield": "EnergyShield",
}

STAGE2_GEMS: list[dict[str, Any]] = [
    {"id": "Cyclone", "file": "act_dex.lua", "kind": "attack_channel"},
    {
        "id": "SupportCastOnCriticalStrike",
        "secondaryId": "SupportCastOnCritTriggered",
        "file": "sup_dex.lua",
        "kind": "trigger",
    },
    {"id": "IceNova", "file": "act_int.lua", "kind": "spell"},
    {"id": "ShockNova", "file": "act_int.lua", "kind": "spell"},
    {"id": "SupportIncreasedCriticalStrikes", "file": "sup_int.lua", "kind": "support"},
    {"id": "SupportMeleePhysicalDamage", "file": "sup_str.lua", "kind": "support"},
    {"id": "SupportBrutality", "file": "sup_str.lua", "kind": "support"},
    {"id": "SupportIncreasedAreaOfEffect", "file": "sup_int.lua", "kind": "support"},
    {"id": "SupportConcentratedEffect", "file": "sup_int.lua", "kind": "support"},
]


def poe_round(x: float) -> int:
    """PoB의 round(x) = floor(x + 0.5)."""
    return math.floor(x + 0.5)


def gem_stat_requirement(level_req: int, is_support: bool, multi: int) -> int:
    """PoB Modules/CalcTools.lua:getGemStatRequirement 공식."""
    if multi <= 0:
        return 0
    stat_type = 0.5 if is_support else 0.7
    req = poe_round((20 + (level_req - 3) * 3) * ((multi / 100.0) ** 0.9) * stat_type)
    return 0 if req < 14 else req


def read_gem_req_multipliers(pob: Path) -> dict[str, tuple[int, int, int]]:
    """Data/Gems.lua에서 grantedEffectId별 (reqStr, reqDex, reqInt) 배율(0..100)을 추출한다."""
    gems_src = (pob / "Data" / "Gems.lua").read_text(encoding="utf-8")
    out: dict[str, tuple[int, int, int]] = {}
    for m in re.finditer(r'\["[^"]+"\]\s*=\s*\{', gems_src):
        block = extract_balanced_block(gems_src, m.end() - 1)
        gid_m = re.search(r'grantedEffectId\s*=\s*"([^"]+)"', block)
        if not gid_m:
            continue
        gid = gid_m.group(1)
        # 기본 젬(AltX/AltY 등 변형 젬보다 먼저 또는 variantId == grantedEffectId 우선)
        var_m = re.search(r'variantId\s*=\s*"([^"]+)"', block)
        if gid in out and (not var_m or var_m.group(1) != gid):
            continue
        rs = re.search(r'\breqStr\s*=\s*(\d+)', block)
        rd = re.search(r'\breqDex\s*=\s*(\d+)', block)
        ri = re.search(r'\breqInt\s*=\s*(\d+)', block)
        out[gid] = (
            int(rs.group(1)) if rs else 0,
            int(rd.group(1)) if rd else 0,
            int(ri.group(1)) if ri else 0,
        )
    return out


def extract_balanced_block(src: str, open_brace_idx: int) -> str:
    """open_brace_idx 위치의 '{'에 대응하는 '}'까지의 블록(중괄호 포함)을 돌려준다."""
    assert src[open_brace_idx] == "{"
    depth = 0
    in_str: str | None = None
    i = open_brace_idx
    n = len(src)
    while i < n:
        ch = src[i]
        if in_str is not None:
            if ch == "\\" and i + 1 < n:
                i += 2
                continue
            if ch == in_str:
                in_str = None
        else:
            if ch in ('"', "'"):
                in_str = ch
            elif ch == "-" and i + 1 < n and src[i + 1] == "-":
                nl = src.find("\n", i)
                i = n if nl == -1 else nl + 1
                continue
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return src[open_brace_idx : i + 1]
        i += 1
    raise ValueError(f"Unclosed Lua brace at offset {open_brace_idx}")


def read_lua_number_table(src: str, name: str) -> list[float]:
    m = re.search(r"^data\." + re.escape(name) + r"\s*=\s*\{([^}]*)\}", src, re.M)
    if not m:
        raise ValueError(f"Data/Misc.lua에서 {name}을 찾지 못했습니다")
    return [float(x) for x in m.group(1).split(",") if x.strip()]


def read_lua_const(src: str, key: str) -> float:
    m = re.search(r'\["' + re.escape(key) + r'"\]\s*=\s*([0-9eE.+\-]+)', src)
    if not m:
        raise ValueError(f"상수 {key}를 찾지 못했습니다")
    return float(m.group(1))


def read_module_misc_const(src: str, key: str) -> float:
    m = re.search(r"\b" + re.escape(key) + r"\s*=\s*([0-9eE.+\-]+)", src)
    if not m:
        raise ValueError(f"Modules/Data.lua에서 {key}를 찾지 못했습니다")
    return float(m.group(1))


def read_calc_setup_base(src: str, stat: str) -> float:
    m = re.search(
        r'NewMod\("' + re.escape(stat) + r'",\s*"BASE"[^\n]*base\s*=\s*([0-9.]+)',
        src,
    )
    if not m:
        raise ValueError(f"Modules/CalcSetup.lua에서 {stat} base를 찾지 못했습니다")
    return float(m.group(1))


def read_defense_bases(pob: Path) -> dict[tuple[str, str], list[tuple[int, float]]]:
    bases: dict[tuple[str, str], list[tuple[int, float]]] = {}
    for slot in SLOTS:
        src = (pob / "Data" / "Bases" / f"{slot}.lua").read_text(encoding="utf-8")
        for m in re.finditer(r'itemBases\["([^"]+)"\] = \{(.*?)\n\}', src, re.S):
            body = m.group(2)
            sub = re.search(r'subType = "([^"]+)"', body)
            tags = re.search(r"tags = \{([^}]*)\}", body)
            if not sub or sub.group(1) not in DEFENSE_TYPES:
                continue
            if tags and ("not_for_sale" in tags.group(1) or "demigods" in tags.group(1)):
                continue
            key = DEFENSE_TYPES[sub.group(1)]
            val = re.search(key + r"BaseMin = (\d+), " + key + r"BaseMax = (\d+)", body)
            if not val:
                continue
            req = re.search(r"level = (\d+)", body)
            req_lvl = int(req.group(1)) if req else 1
            avg_val = (int(val.group(1)) + int(val.group(2))) / 2.0
            bases.setdefault((slot, sub.group(1)), []).append((req_lvl, avg_val))
    return bases


def read_resist_tiers(pob: Path) -> list[tuple[int, float]]:
    src = (pob / "Data" / "ModExplicit.lua").read_text(encoding="utf-8")
    tiers: list[tuple[int, float]] = []
    for m in re.finditer(
        r'\["FireResist\d+"\] = \{ type = "Suffix", affix = "[^"]*", '
        r'"\+\((\d+)-(\d+)\)% to Fire Resistance", statOrder = \{ \d+ \}, level = (\d+),',
        src,
    ):
        tiers.append((int(m.group(3)), (int(m.group(1)) + int(m.group(2))) / 2.0))
    if not tiers:
        raise ValueError("Data/ModExplicit.lua에서 FireResist 등급을 찾지 못했습니다")
    return sorted(tiers)


def build_expected_defense_table(
    pob: Path, max_level: int
) -> list[dict[str, float]]:
    bases = read_defense_bases(pob)
    tiers = read_resist_tiers(pob)

    def best_sum(kind: str, level: int) -> float:
        total = 0.0
        for slot in SLOTS:
            opts = [val for req, val in bases.get((slot, kind), []) if req <= level]
            total += max(opts) if opts else 0.0
        return total

    rows: list[dict[str, float]] = []
    for level in range(1, max_level + 1):
        best_res = max(val for req, val in tiers if req <= level)
        rows.append(
            {
                "level": level,
                "bestArmour": best_sum("Armour", level),
                "bestEvasion": best_sum("Evasion", level),
                "bestEnergyShield": best_sum("Energy Shield", level),
                "bestResistTier": best_res,
            }
        )
    return rows


def read_classes(pob: Path) -> list[dict[str, Any]]:
    tree_path = pob / "TreeData" / "3_30" / "tree.lua"
    src = tree_path.read_text(encoding="utf-8")
    m = re.search(r'\["classes"\]\s*=\s*\{', src)
    if not m:
        raise ValueError("TreeData/3_30/tree.lua에서 classes 블록을 찾지 못했습니다")
    classes_block = extract_balanced_block(src, m.end() - 1)

    classes: list[dict[str, Any]] = []
    # 최상위 클래스 항목만 추출 (base_str/base_dex/base_int/name이 모두 있는 1단 하위 테이블)
    i = 1
    n = len(classes_block)
    while i < n - 1:
        if classes_block[i] == "{":
            item = extract_balanced_block(classes_block, i)
            i += len(item)
            name_m = re.search(r'\n\t\t\t\["name"\]\s*=\s*"([^"]+)"', item)
            str_m = re.search(r'\["base_str"\]\s*=\s*(\d+)', item)
            dex_m = re.search(r'\["base_dex"\]\s*=\s*(\d+)', item)
            int_m = re.search(r'\["base_int"\]\s*=\s*(\d+)', item)
            if name_m and str_m and dex_m and int_m:
                classes.append(
                    {
                        "name": name_m.group(1),
                        "baseStr": int(str_m.group(1)),
                        "baseDex": int(dex_m.group(1)),
                        "baseInt": int(int_m.group(1)),
                    }
                )
        else:
            i += 1
    if len(classes) != 7:
        raise ValueError(f"클래스 7종을 기대했으나 {len(classes)}종 추출됨: {classes}")
    return classes


def extract_field_block(skill_block: str, field_name: str) -> str | None:
    m = re.search(r"\n\t" + re.escape(field_name) + r"\s*=\s*\{", skill_block)
    if not m:
        return None
    return extract_balanced_block(skill_block, m.end() - 1)


def parse_skill_type_set(block: str | None) -> list[str]:
    if not block:
        return []
    return re.findall(r"\[SkillType\.([A-Za-z0-9_]+)\]\s*=\s*true", block)


def parse_skill_type_list(block: str | None) -> list[str]:
    if not block:
        return []
    return re.findall(r"SkillType\.([A-Za-z0-9_]+)", block)


def parse_string_list(block: str | None) -> list[str]:
    if not block:
        return []
    return re.findall(r'"([^"]+)"', block)


def parse_constant_stats(block: str | None) -> dict[str, float]:
    out: dict[str, float] = {}
    if not block:
        return out
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*([0-9eE.+\-]+)\s*\}', block):
        out[m.group(1)] = float(m.group(2))
    return out


def parse_weapon_types(block: str | None) -> list[str]:
    if not block:
        return []
    return re.findall(r'\["([^"]+)"\]\s*=\s*true', block)


def parse_skill_block(
    skill_src: str,
    skill_id: str,
    base_eff_const: float,
    inc_eff_const: float,
    req_mult: tuple[int, int, int] = (0, 0, 0),
) -> dict[str, Any]:
    m = re.search(r'skills\["' + re.escape(skill_id) + r'"\]\s*=\s*\{', skill_src)
    if not m:
        raise ValueError(f"스킬 ID '{skill_id}'를 찾지 못했습니다")
    block = extract_balanced_block(skill_src, m.end() - 1)

    def scalar_num(key: str, default: float = 0.0) -> float:
        sm = re.search(r"\n\t" + re.escape(key) + r"\s*=\s*([0-9eE.+\-]+)", block)
        return float(sm.group(1)) if sm else default

    def scalar_str(key: str, default: str = "") -> str:
        sm = re.search(r'\n\t' + re.escape(key) + r'\s*=\s*"([^"]*)"', block)
        return sm.group(1) if sm else default

    base_effectiveness = scalar_num("baseEffectiveness", 1.0)
    inc_effectiveness = scalar_num("incrementalEffectiveness", 0.0)
    cast_time = scalar_num("castTime", 0.0)
    color = int(scalar_num("color", 0))
    is_support = bool(re.search(r"\n\tsupport\s*=\s*true", block))

    skill_types = parse_skill_type_set(extract_field_block(block, "skillTypes"))
    req_types = parse_skill_type_list(extract_field_block(block, "requireSkillTypes"))
    exc_types = parse_skill_type_list(extract_field_block(block, "excludeSkillTypes"))
    add_types = parse_skill_type_list(extract_field_block(block, "addSkillTypes"))
    weapon_types = parse_weapon_types(extract_field_block(block, "weaponTypes"))
    const_stats = parse_constant_stats(extract_field_block(block, "constantStats"))
    stat_names = parse_string_list(extract_field_block(block, "stats"))

    levels_block = extract_field_block(block, "levels")
    if not levels_block:
        raise ValueError(f"스킬 '{skill_id}'에 levels 블록이 없습니다")

    levels: dict[str, dict[str, Any]] = {}
    for lv_m in re.finditer(r"\[(\d+)\]\s*=\s*\{", levels_block):
        lv_num = int(lv_m.group(1))
        if lv_num < 1 or lv_num > 20:
            continue
        lv_body = extract_balanced_block(levels_block, lv_m.end() - 1)[1:-1]

        # 위치 기반 숫자 값들(키=값 쌍이 나오기 앞의 숫자들) 파싱
        first_kv = re.search(r"[A-Za-z_][A-Za-z0-9_]*\s*=", lv_body)
        pos_part = lv_body[: first_kv.start()] if first_kv else lv_body
        pos_vals = [float(x) for x in re.findall(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?", pos_part)]

        def lv_scalar(key: str, default: float = 0.0) -> float:
            km = re.search(r"\b" + re.escape(key) + r"\s*=\s*([0-9eE.+\-]+)", lv_body)
            return float(km.group(1)) if km else default

        cost_m = re.search(r"cost\s*=\s*\{\s*Mana\s*=\s*([0-9eE.+\-]+)", lv_body)
        mana_cost = float(cost_m.group(1)) if cost_m else 0.0

        interp_m = re.search(r"statInterpolation\s*=\s*\{([^}]*)\}", lv_body)
        interps = (
            [int(x) for x in re.findall(r"\d+", interp_m.group(1))]
            if interp_m
            else []
        )

        level_req = int(lv_scalar("levelRequirement", 1))
        actor_level = level_req if level_req >= 1 else 1
        avail_eff = (
            (base_eff_const + inc_eff_const * (actor_level - 1))
            * base_effectiveness
            * ((1.0 + inc_effectiveness) ** (actor_level - 1))
        )

        resolved_stats: dict[str, float] = dict(const_stats)
        for idx, sname in enumerate(stat_names):
            raw_val = pos_vals[idx] if idx < len(pos_vals) else 1.0
            mode = interps[idx] if idx < len(interps) else 1
            if mode == 3:
                val = float(poe_round(avail_eff * raw_val))
            else:
                val = raw_val
            resolved_stats[sname] = resolved_stats.get(sname, 0.0) + val

        levels[str(lv_num)] = {
            "level": lv_num,
            "levelRequirement": level_req,
            "reqStr": gem_stat_requirement(level_req, is_support, req_mult[0]),
            "reqDex": gem_stat_requirement(level_req, is_support, req_mult[1]),
            "reqInt": gem_stat_requirement(level_req, is_support, req_mult[2]),
            "manaCost": mana_cost,
            "manaMultiplier": lv_scalar("manaMultiplier", 0.0),
            "attackSpeedMultiplier": lv_scalar("attackSpeedMultiplier", 0.0),
            "baseMultiplier": lv_scalar("baseMultiplier", 1.0),
            "damageEffectiveness": lv_scalar("damageEffectiveness", 1.0),
            "critChance": lv_scalar("critChance", 0.0),
            "cooldown": lv_scalar("cooldown", 0.0),
            "stats": resolved_stats,
        }

    if len(levels) < 20:
        raise ValueError(f"스킬 '{skill_id}'의 레벨 1~20이 부족합니다: {len(levels)}")

    return {
        "id": skill_id,
        "name": scalar_str("name", skill_id),
        "color": color,
        "support": is_support,
        "castTime": cast_time,
        "skillTypes": skill_types,
        "requireSkillTypes": req_types,
        "excludeSkillTypes": exc_types,
        "addSkillTypes": add_types,
        "weaponTypes": weapon_types,
        "constantStats": const_stats,
        "levels": levels,
    }


def build_lang_packs() -> tuple[dict[str, Any], dict[str, Any]]:
    """PoB 원문 설명을 복사하지 않고 공식 명칭과 자체 스탯 기반 한글/영어 팩을 만든다(결정 31, 35)."""
    ko = {
        "locale": "ko-KR",
        "gems": {
            "Cyclone": {"name": "사이클론", "short": "회전하며 이동하는 근접 범위 채널링 공격"},
            "SupportCastOnCriticalStrike": {
                "name": "치명타 시 시전",
                "short": "연결한 공격의 치명타 적중 시 담긴 주문을 순환 발동",
            },
            "IceNova": {"name": "얼음 회오리", "short": "대상 중심 원형 범위 냉기 피해 주문"},
            "ShockNova": {"name": "충격 회오리", "short": "대상 중심 원형 범위 번개 피해 주문"},
            "SupportIncreasedCriticalStrikes": {
                "name": "치명타 확률 증가",
                "short": "기본 치명타 확률과 치명타 확률 증가 부여",
            },
            "SupportMeleePhysicalDamage": {
                "name": "근접 물리 피해",
                "short": "근접 물리 피해 증폭, 공격 속도 소폭 감폭",
            },
            "SupportBrutality": {
                "name": "포악함",
                "short": "물리 피해 증폭, 원소·카오스 피해 불가",
            },
            "SupportIncreasedAreaOfEffect": {
                "name": "효과 범위 증가",
                "short": "범위 스킬의 효과 범위 증가",
            },
            "SupportConcentratedEffect": {
                "name": "집중 효과",
                "short": "범위 피해 증폭, 효과 범위 감폭",
            },
        },
        "classes": {
            "Scion": "사이온",
            "Marauder": "머라우더",
            "Ranger": "레인저",
            "Witch": "위치",
            "Duelist": "듀얼리스트",
            "Templar": "템플러",
            "Shadow": "섀도우",
        },
        "ui": {
            "exileLevel": "유배자 레벨",
            "energyShield": "에너지 실드",
            "skillSlots": "스킬 칸",
            "linkedAttack": "연결 공격 칸",
            "equip": "장착",
            "unequip": "해제",
            "evade": "회피",
            "block": "막음",
            "levelUp": "유배자 레벨 상승!",
        },
    }
    en = {
        "locale": "en-US",
        "gems": {
            "Cyclone": {"name": "Cyclone", "short": "Channelling melee area attack while moving"},
            "SupportCastOnCriticalStrike": {
                "name": "Cast On Critical Strike",
                "short": "Triggers socketed spells in order on linked attack critical hit",
            },
            "IceNova": {"name": "Ice Nova", "short": "Area cold damage spell centered on target"},
            "ShockNova": {"name": "Shock Nova", "short": "Area lightning damage spell centered on target"},
            "SupportIncreasedCriticalStrikes": {
                "name": "Increased Critical Strikes",
                "short": "Adds base critical strike chance and increased critical strike chance",
            },
            "SupportMeleePhysicalDamage": {
                "name": "Melee Physical Damage",
                "short": "More melee physical damage, slightly less attack speed",
            },
            "SupportBrutality": {
                "name": "Brutality",
                "short": "More physical damage, deals no elemental or chaos damage",
            },
            "SupportIncreasedAreaOfEffect": {
                "name": "Increased Area of Effect",
                "short": "Increased area of effect for area skills",
            },
            "SupportConcentratedEffect": {
                "name": "Concentrated Effect",
                "short": "More area damage, less area of effect",
            },
        },
        "classes": {
            "Scion": "Scion",
            "Marauder": "Marauder",
            "Ranger": "Ranger",
            "Witch": "Witch",
            "Duelist": "Duelist",
            "Templar": "Templar",
            "Shadow": "Shadow",
        },
        "ui": {
            "exileLevel": "Exile Level",
            "energyShield": "Energy Shield",
            "skillSlots": "Skill Slots",
            "linkedAttack": "Linked Attack Slot",
            "equip": "Equip",
            "unequip": "Unequip",
            "evade": "Evaded",
            "block": "Blocked",
            "levelUp": "Exile Level Up!",
        },
    }
    return ko, en


def compile_stage2(pob: Path, out_dir: Path) -> dict[str, Any]:
    misc_src = (pob / "Data" / "Misc.lua").read_text(encoding="utf-8")
    mod_data_src = (pob / "Modules" / "Data.lua").read_text(encoding="utf-8")
    calc_setup_src = (pob / "Modules" / "CalcSetup.lua").read_text(encoding="utf-8")

    monster_life = read_lua_number_table(misc_src, "monsterLifeTable")[:100]
    monster_damage = read_lua_number_table(misc_src, "monsterDamageTable")[:100]
    monster_accuracy = read_lua_number_table(misc_src, "monsterAccuracyTable")[:100]
    if not (len(monster_life) == 100 and len(monster_damage) == 100 and len(monster_accuracy) == 100):
        raise ValueError("몬스터 표는 1~100레벨 100개 항목이어야 합니다")

    base_eff_const = read_lua_const(misc_src, "SkillDamageBaseEffectiveness")
    inc_eff_const = read_lua_const(misc_src, "SkillDamageIncrementalEffectiveness")

    constants = {
        "lifeBase": read_calc_setup_base(calc_setup_src, "Life"),
        "lifePerLevel": read_lua_const(misc_src, "life_per_level"),
        "manaBase": read_calc_setup_base(calc_setup_src, "Mana"),
        "manaPerLevel": read_lua_const(misc_src, "mana_per_level"),
        "resistCap": read_lua_const(misc_src, "base_maximum_all_resistances_%"),
        "esRechargeDelay": read_module_misc_const(mod_data_src, "EnergyShieldRechargeDelay"),
        "esRechargeRatePerMinPct": read_lua_const(misc_src, "energy_shield_recharge_rate_per_minute_%"),
        "esRechargeRate": 0.33,  # 결정 85 기준 초당 33%
        "skillDamageBaseEffectiveness": base_eff_const,
        "skillDamageIncrementalEffectiveness": inc_eff_const,
        "monsterDamageMult": 1.5,
        "armourMitigationCap": 0.90,
        "minHitChance": 0.05,
    }

    expected_defense = build_expected_defense_table(pob, 100)
    classes = read_classes(pob)
    gem_req_mults = read_gem_req_multipliers(pob)

    skill_files_cache: dict[str, str] = {}
    gems_out: dict[str, Any] = {}
    for entry in STAGE2_GEMS:
        fname = entry["file"]
        if fname not in skill_files_cache:
            skill_files_cache[fname] = (pob / "Data" / "Skills" / fname).read_text(encoding="utf-8")
        req_mult = gem_req_mults.get(entry["id"], (0, 0, 0))
        parsed = parse_skill_block(
            skill_files_cache[fname], entry["id"], base_eff_const, inc_eff_const, req_mult
        )
        parsed["kind"] = entry["kind"]
        if "secondaryId" in entry:
            sec = parse_skill_block(
                skill_files_cache[fname], entry["secondaryId"], base_eff_const, inc_eff_const, req_mult
            )
            parsed["triggeredEffect"] = sec
        gems_out[entry["id"]] = parsed

    pack = {
        "meta": {
            "schemaVersion": 1,
            "pobVersion": "2.68.1",
            "poeVersion": "3.30",
        },
        "constants": constants,
        "tables": {
            "monsterLife": monster_life,
            "monsterDamage": monster_damage,
            "monsterAccuracy": monster_accuracy,
            "expectedDefense": expected_defense,
        },
        "classes": classes,
        "gems": gems_out,
    }

    packs_dir = out_dir / "packs"
    lang_dir = out_dir / "lang"
    packs_dir.mkdir(parents=True, exist_ok=True)
    lang_dir.mkdir(parents=True, exist_ok=True)

    (packs_dir / "core_3_30.json").write_text(
        json.dumps(pack, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    ko, en = build_lang_packs()
    (lang_dir / "ko-KR.json").write_text(
        json.dumps(ko, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (lang_dir / "en-US.json").write_text(
        json.dumps(en, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return pack


def main() -> int:
    for s in (sys.stdout, sys.stderr):
        if hasattr(s, "reconfigure"):
            s.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description="PoB 2.68.1 -> SkyrimPoE 2단계 데이터 팩 변환기")
    ap.add_argument("--pob", type=Path, default=DEFAULT_POB)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()
    pack = compile_stage2(args.pob, args.out)
    print(
        f"생성 완료: {args.out / 'packs' / 'core_3_30.json'} "
        f"(gems={len(pack['gems'])}, classes={len(pack['classes'])}, levels=100)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
