r"""PoB 2.68.1 (PoE 3.30) 아이템 어픽스·베이스 커브·화폐 메타데이터 컴파일러.

사용법:
  python tools/compiler/compile_items.py [--pob G:\poe\PathOfBuilding-2.68.1] [--out data]

규칙(4단계 구현 계획 S4-M1, 결정 35, 156~160):
  1. ModExplicit.lua에서 영향력 접미사 9종만 제외하고, 비영향력 0-오버라이드 태그와
     원본 weightKey 순서를 그대로 보존한다(PoE first-match-wins 평가 정확성 보장).
  2. 방패·원거리·근접 확장 태그(str_shield, dex_shield, focus, ranged, attack_dagger,
     attack_staff, warstaff 등)를 빠짐없이 포함한다.
  3. 역순 범위(min > max)는 오름차순으로 정규화하고, 소수점 범위(isFloat=True)는
     float 정밀도를 유지한다.
  4. PoB 어픽스 ID의 Local 접두 및 도메인 기준으로 isLocal 플래그를 명시 출력한다.
  5. 4단계 전투·방어·능력치 엔진이 실제 적용하는 핵심 스탯 라인만 rollable=True로 선별해
     "붙었는데 무효과인 어픽스" 롤링을 방지한다.
  6. Data/Bases/*.lua에서 부위·방어유형/무기종류별 iLvl 기준 기본 방어/피해/요구 능력치 커브를 추출한다.
  7. 한글 설명은 PoB 한글 CSV를 복사하지 않고 자체 스탯 템플릿 규칙으로 생성한다(결정 35).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

DEFAULT_POB = Path(r"G:\poe\PathOfBuilding-2.68.1")
DEFAULT_OUT = Path("data")

INFLUENCE_SUFFIXES = (
    "_shaper",
    "_elder",
    "_crusader",
    "_adjudicator",
    "_eyrie",
    "_basilisk",
    "_warlord",
    "_hunter",
    "_redeemer",
)

# 4단계에서 장비가 가질 수 있는 모든 기본 태그 (Data/Bases/*.lua 대조)
RELEVANT_ITEM_TAGS = frozenset({
    "weapon", "one_hand_weapon", "two_hand_weapon", "onehand", "twohand",
    "sword", "axe", "mace", "dagger", "bow", "staff", "sceptre", "wand", "claw",
    "attack_dagger", "attack_staff", "warstaff", "ranged",
    "armour", "body_armour", "helmet", "gloves", "boots", "shield",
    "str_armour", "dex_armour", "int_armour",
    "str_dex_armour", "str_int_armour", "dex_int_armour", "str_dex_int_armour",
    "str_shield", "dex_shield", "focus",
    "str_dex_shield", "str_int_shield", "dex_int_shield",
    "amulet", "ring", "default",
})

# 4단계 스탯 엔진이 직접 파싱·적용하는 정규화 스탯 템플릿과 자체 한글화 패턴(결정 35)
# 정규화 규칙: 숫자 범위 `(a-b)` 또는 단일 숫자 `N`을 `{0}`, `{1}` 자리표시자로 치환
SUPPORTED_STAT_KO: dict[str, str] = {
    # 능력치
    "+{0} to Strength": "힘 +{0}",
    "+{0} to Dexterity": "민첩 +{0}",
    "+{0} to Intelligence": "지능 +{0}",
    "+{0} to all Attributes": "모든 능력치 +{0}",
    # 생명력·마나·ES
    "+{0} to maximum Life": "최대 생명력 +{0}",
    "{0}% increased maximum Life": "최대 생명력 {0}% 증가",
    "+{0} to maximum Mana": "최대 마나 +{0}",
    "{0}% increased maximum Mana": "최대 마나 {0}% 증가",
    "+{0} to maximum Energy Shield": "최대 에너지 실드 +{0}",
    "{0}% increased Energy Shield": "에너지 실드 {0}% 증가",
    "{0}% increased maximum Energy Shield": "최대 에너지 실드 {0}% 증가",
    # 방어도·회피·복합 방어
    "+{0} to Armour": "방어도 +{0}",
    "{0}% increased Armour": "방어도 {0}% 증가",
    "+{0} to Evasion Rating": "회피 +{0}",
    "{0}% increased Evasion Rating": "회피 {0}% 증가",
    "{0}% increased Armour and Evasion": "방어도 및 회피 {0}% 증가",
    "{0}% increased Armour and Energy Shield": "방어도 및 에너지 실드 {0}% 증가",
    "{0}% increased Evasion and Energy Shield": "회피 및 에너지 실드 {0}% 증가",
    "{0}% increased Armour, Evasion and Energy Shield": "방어도, 회피 및 에너지 실드 {0}% 증가",
    # 저항·물리 피해 감소
    "+{0}% to Fire Resistance": "화염 저항 +{0}%",
    "+{0}% to Cold Resistance": "냉기 저항 +{0}%",
    "+{0}% to Lightning Resistance": "번개 저항 +{0}%",
    "+{0}% to Chaos Resistance": "카오스 저항 +{0}%",
    "+{0}% to all Elemental Resistances": "모든 원소 저항 +{0}%",
    "{0}% additional Physical Damage Reduction": "추가 물리 피해 감소 {0}%",
    # 막기·주문 억제·ES 재충전
    "+{0}% chance to Suppress Spell Damage": "주문 피해 억제 확률 +{0}%",
    "{0}% increased Chance to Block": "막기 확률 {0}% 증가",
    "+{0}% Chance to Block Attack Damage": "공격 피해 막기 확률 +{0}%",
    "+{0}% Chance to Block Spell Damage": "주문 피해 막기 확률 +{0}%",
    "{0}% Chance to Block Spell Damage": "주문 피해 막기 확률 +{0}%",
    "{0}% increased Energy Shield Recharge Rate": "에너지 실드 재충전 속도 {0}% 증가",
    "{0}% faster start of Energy Shield Recharge": "에너지 실드 재충전 시작 {0}% 더 빠름",
    # 로컬/글로벌 무기 및 주문 피해
    "Adds {0} to {1} Physical Damage": "물리 피해 {0}~{1} 추가",
    "Adds {0} to {1} Fire Damage": "화염 피해 {0}~{1} 추가",
    "Adds {0} to {1} Cold Damage": "냉기 피해 {0}~{1} 추가",
    "Adds {0} to {1} Lightning Damage": "번개 피해 {0}~{1} 추가",
    "Adds {0} to {1} Chaos Damage": "카오스 피해 {0}~{1} 추가",
    "Adds {0} to {1} Physical Damage to Attacks": "공격에 물리 피해 {0}~{1} 추가",
    "Adds {0} to {1} Fire Damage to Attacks": "공격에 화염 피해 {0}~{1} 추가",
    "Adds {0} to {1} Cold Damage to Attacks": "공격에 냉기 피해 {0}~{1} 추가",
    "Adds {0} to {1} Lightning Damage to Attacks": "공격에 번개 피해 {0}~{1} 추가",
    "Adds {0} to {1} Chaos Damage to Attacks": "공격에 카오스 피해 {0}~{1} 추가",
    "Adds {0} to {1} Fire Damage to Spells": "주문에 화염 피해 {0}~{1} 추가",
    "Adds {0} to {1} Cold Damage to Spells": "주문에 냉기 피해 {0}~{1} 추가",
    "Adds {0} to {1} Lightning Damage to Spells": "주문에 번개 피해 {0}~{1} 추가",
    "Adds {0} to {1} Chaos Damage to Spells": "주문에 카오스 피해 {0}~{1} 추가",
    "{0}% increased Physical Damage": "물리 피해 {0}% 증가",
    "{0}% increased Spell Damage": "주문 피해 {0}% 증가",
    "{0}% increased Fire Damage": "화염 피해 {0}% 증가",
    "{0}% increased Cold Damage": "냉기 피해 {0}% 증가",
    "{0}% increased Lightning Damage": "번개 피해 {0}% 증가",
    "{0}% increased Elemental Damage with Attack Skills": "공격 스킬의 원소 피해 {0}% 증가",
    # 속도·치명타
    "{0}% increased Attack Speed": "공격 속도 {0}% 증가",
    "{0}% increased Cast Speed": "시전 속도 {0}% 증가",
    "{0}% increased Critical Strike Chance": "치명타 확률 {0}% 증가",
    "{0}% increased Global Critical Strike Chance": "전역 치명타 확률 {0}% 증가",
    "{0}% increased Spell Critical Strike Chance": "주문 치명타 확률 {0}% 증가",
    "+{0}% to Global Critical Strike Multiplier": "전역 치명타 피해 배율 +{0}%",
    # 보조/유틸리티(하이브리드 어픽스 보조 줄 포함 — 4단계에서도 표시/부분 반영)
    "+{0} to Accuracy Rating": "명중 +{0}",
    "{0}% increased Global Accuracy Rating": "전역 명중 {0}% 증가",
    "{0}% increased Stun and Block Recovery": "기절 및 막기 회복 {0}% 증가",
    "Regenerate {0} Life per second": "초당 생명력 {0} 재생",
    "{0}% increased Mana Regeneration Rate": "마나 재생 속도 {0}% 증가",
    "{0}% increased Movement Speed": "이동 속도 {0}% 증가",
}

# 로컬 전용이지만 2줄짜리 하이브리드 어픽스(예: % Physical Damage + Accuracy, % Armour + Stun Recovery)의
# 핵심 1번 줄이 지원되고 보조 줄도 SUPPORTED_STAT_KO에 있으면 rollable로 인정한다.
PURE_UTILITY_ONLY_TEMPLATES = frozenset({
    "+{0} to Accuracy Rating",
    "{0}% increased Global Accuracy Rating",
    "{0}% increased Stun and Block Recovery",
})

TIERS = [
    {"tier": 1, "minLevel": 1, "maxLevel": 20, "label": "T1 (Lv 1~20)"},
    {"tier": 2, "minLevel": 21, "maxLevel": 40, "label": "T2 (Lv 21~40)"},
    {"tier": 3, "minLevel": 41, "maxLevel": 60, "label": "T3 (Lv 41~60)"},
    {"tier": 4, "minLevel": 61, "maxLevel": 80, "label": "T4 (Lv 61~80)"},
    {"tier": 5, "minLevel": 81, "maxLevel": 100, "label": "T5 (Lv 81+)"},
]

# 결정 158: 핵심 화폐 오브 12종 (PoE1 공식 영문명 + 한글명 + 자체 기능 설명)
CURRENCIES = [
    {
        "id": "ScrollOfWisdom",
        "enName": "Scroll of Wisdom",
        "koName": "지혜의 두루마리",
        "koDesc": "미감정 봉인 토큰을 감정해 장비와 어픽스를 확정합니다.",
        "dropWeight": 260,
        "worldDropTier": 0,
    },
    {
        "id": "Transmutation",
        "enName": "Orb of Transmutation",
        "koName": "변화의 오브",
        "koDesc": "일반 장비를 매직 장비(어픽스 1~2개)로 업그레이드합니다.",
        "dropWeight": 190,
        "worldDropTier": 0,
    },
    {
        "id": "Augmentation",
        "enName": "Orb of Augmentation",
        "koName": "증폭의 오브",
        "koDesc": "어픽스가 1개인 매직 장비에 새 어픽스를 1개 추가합니다.",
        "dropWeight": 160,
        "worldDropTier": 0,
    },
    {
        "id": "Alteration",
        "enName": "Orb of Alteration",
        "koName": "진화의 오브",
        "koDesc": "매직 장비의 어픽스를 무작위로 다시 부여합니다.",
        "dropWeight": 170,
        "worldDropTier": 1,
    },
    {
        "id": "Regal",
        "enName": "Regal Orb",
        "koName": "제왕의 오브",
        "koDesc": "매직 장비를 희귀 장비로 승격하고 어픽스를 1개 추가합니다.",
        "dropWeight": 55,
        "worldDropTier": 2,
    },
    {
        "id": "Alchemy",
        "enName": "Orb of Alchemy",
        "koName": "연금술의 오브",
        "koDesc": "일반 장비를 희귀 장비(어픽스 4~6개)로 업그레이드합니다.",
        "dropWeight": 95,
        "worldDropTier": 2,
    },
    {
        "id": "Chaos",
        "enName": "Chaos Orb",
        "koName": "카오스 오브",
        "koDesc": "희귀 장비의 모든 어픽스를 무작위로 다시 부여합니다.",
        "dropWeight": 75,
        "worldDropTier": 2,
    },
    {
        "id": "Exalted",
        "enName": "Exalted Orb",
        "koName": "엑잘티드 오브",
        "koDesc": "빈 어픽스 자리가 있는 희귀 장비에 새 어픽스를 1개 추가합니다.",
        "dropWeight": 22,
        "worldDropTier": 2,
    },
    {
        "id": "Scouring",
        "enName": "Orb of Scouring",
        "koName": "정제의 오브",
        "koDesc": "매직·희귀 장비의 어픽스를 모두 지워 일반 장비로 되돌립니다(아이템 레벨 유지).",
        "dropWeight": 85,
        "worldDropTier": 1,
    },
    {
        "id": "Divine",
        "enName": "Divine Orb",
        "koName": "신성한 오브",
        "koDesc": "장비의 기존 어픽스 종류와 티어를 유지한 채 수치만 다시 굴립니다.",
        "dropWeight": 12,
        "worldDropTier": 2,
    },
    {
        "id": "Regret",
        "enName": "Orb of Regret",
        "koName": "후회의 오브",
        "koDesc": "패시브 트리 노드 회수에 사용됩니다(일반/마스터리 1개, 전직 5개).",
        "dropWeight": 65,
        "worldDropTier": 2,
    },
    {
        "id": "Jeweller",
        "enName": "Jeweller's Orb",
        "koName": "쥬얼러의 오브",
        "koDesc": "스킬·발동 젬의 보조 소켓 수를 1칸 늘립니다(최대 5칸).",
        "dropWeight": 90,
        "worldDropTier": 2,
    },
]

# 숫자 범위 `(min-max)` 또는 단일 숫자 토큰 파싱 정규식
# 예: "+(8-12) to Strength", "Adds 1 to (2-3) Physical Damage", "-(8-6) to Total Mana Cost"
_NUM_OR_RANGE_RE = re.compile(r"\((\d+(?:\.\d+)?)-(\d+(?:\.\d+)?)\)|(\d+(?:\.\d+)?)")


def parse_stat_line(raw_line: str) -> dict[str, Any]:
    """PoB 스탯 줄 1개를 자리표시자 템플릿과 정규화된 범위 목록으로 변환한다.

    검수 C1:
      - 역순 범위(min > max)는 min <= max가 되도록 정규화한다.
      - 소수점이 포함된 범위는 isFloat=True로 표시한다.
      - 고정 숫자도 min == max인 단일 범위로 포함시켜 자리표시자 인덱스 {0}, {1}...와 1:1 매칭한다.
    """
    ranges: list[list[Any]] = []
    idx = 0

    def repl(m: re.Match[str]) -> str:
        nonlocal idx
        if m.group(1) is not None and m.group(2) is not None:
            s_lo, s_hi = m.group(1), m.group(2)
            is_float = ("." in s_lo) or ("." in s_hi)
            lo = float(s_lo)
            hi = float(s_hi)
            if lo > hi:
                lo, hi = hi, lo
            if not is_float:
                ranges.append([int(lo), int(hi), False])
            else:
                ranges.append([lo, hi, True])
        else:
            s_val = m.group(3)
            is_float = "." in s_val
            val = float(s_val)
            if not is_float:
                ranges.append([int(val), int(val), False])
            else:
                ranges.append([val, val, True])
        token = "{" + str(idx) + "}"
        idx += 1
        return token

    template = _NUM_OR_RANGE_RE.sub(repl, raw_line)
    ko_template = SUPPORTED_STAT_KO.get(template, template)
    return {
        "template": template,
        "koTemplate": ko_template,
        "ranges": ranges,
    }


def is_local_affix(affix_id: str, templates: list[str], weight_keys: list[str]) -> bool:
    """PoB 어픽스 ID와 도메인 규칙으로 로컬 모디파이어 여부를 결정한다(검수 C1, m1)."""
    # 검수 C1: LocalCriticalMultiplier1..6처럼 ID는 Local로 시작해도 스탯이 Global이면 글로벌로 분류
    if templates and all("Global" in t for t in templates):
        return False
    if affix_id.startswith("Local") or "Local" in affix_id:
        return True
    # 무기 전용 flat 피해("Adds {0} to {1} X Damage", "to Attacks"/"to Spells" 없음)는 항상 로컬
    for t in templates:
        if t.startswith("Adds {0} to {1} ") and t.endswith(" Damage") and "to Attacks" not in t and "to Spells" not in t:
            return True
    return False


def is_rollable_in_stage4(parsed_lines: list[dict[str, Any]]) -> bool:
    """4단계 엔진이 모든 줄을 해석 가능하고, 핵심 전투/방어/능력치 줄이 1개 이상인지 판정."""
    if not parsed_lines:
        return False
    has_core_stat = False
    for line in parsed_lines:
        tpl = line["template"]
        if tpl not in SUPPORTED_STAT_KO:
            return False
        if tpl not in PURE_UTILITY_ONLY_TEMPLATES:
            has_core_stat = True
    return has_core_stat


def parse_mod_explicit(pob: Path) -> list[dict[str, Any]]:
    src = (pob / "Data" / "ModExplicit.lua").read_text(encoding="utf-8")
    raw_affixes: list[dict[str, Any]] = []

    for m in re.finditer(r'\["([^"]+)"\]\s*=\s*\{([^\n]+)\}', src):
        affix_id, body = m.group(1), m.group(2)
        m_type = re.search(r'type = "([^"]+)"', body)
        m_affix = re.search(r'affix = "([^"]*)"', body)
        m_level = re.search(r'\blevel = (\d+)', body)
        m_group = re.search(r'group = "([^"]+)"', body)
        m_wk = re.search(r'weightKey = \{([^}]*)\}', body)
        m_wv = re.search(r'weightVal = \{([^}]*)\}', body)
        m_lines = re.search(r'affix = "[^"]*",\s*(.*?),\s*statOrder =', body)
        if not (m_type and m_affix and m_level and m_group and m_wk and m_wv and m_lines):
            continue

        affix_type = m_type.group(1)
        if affix_type not in ("Prefix", "Suffix"):
            continue

        keys = re.findall(r'"([^"]+)"', m_wk.group(1))
        vals = [int(x.strip()) for x in m_wv.group(1).split(",") if x.strip()]
        if len(keys) != len(vals):
            continue

        # 검수 C1: 영향력 접미사 9종만 제거하고, 비영향력 0-오버라이드 태그와 순서는 원본 그대로 보존
        weights: list[list[Any]] = []
        has_positive_relevant = False
        for k, v in zip(keys, vals):
            if any(k.endswith(suf) for suf in INFLUENCE_SUFFIXES):
                continue
            weights.append([k, v])
            if v > 0 and k in RELEVANT_ITEM_TAGS:
                has_positive_relevant = True

        if not has_positive_relevant:
            continue

        raw_lines = re.findall(r'"([^"]+)"', m_lines.group(1))
        parsed_lines = [parse_stat_line(rl) for rl in raw_lines]
        templates = [pl["template"] for pl in parsed_lines]

        m_tags = re.search(r'modTags = \{([^}]*)\}', body)
        mod_tags = re.findall(r'"([^"]+)"', m_tags.group(1)) if m_tags else []

        raw_affixes.append({
            "id": affix_id,
            "type": affix_type,
            "affixName": m_affix.group(1),
            "group": m_group.group(1),
            "level": int(m_level.group(1)),
            "tier": 1,  # 아래에서 group별로 역순(최고 iLvl = T1) 매김
            "isLocal": is_local_affix(affix_id, templates, [w[0] for w in weights]),
            "rollable": is_rollable_in_stage4(parsed_lines),
            "lines": parsed_lines,
            "weights": weights,
            "tags": mod_tags,
        })

    # 검수 M2: group + type + ID 패밀리 단위로 최고 iLvl이 Tier 1이 되도록 티어 번호 부여
    by_group: dict[tuple[str, str, str], list[dict[str, Any]]] = {}
    for a in raw_affixes:
        family = re.sub(r"\d+_*$", "", a["id"])
        key = (a["group"], a["type"], family)
        by_group.setdefault(key, []).append(a)

    for items in by_group.values():
        distinct_levels = sorted({it["level"] for it in items}, reverse=True)
        level_to_tier = {lv: idx + 1 for idx, lv in enumerate(distinct_levels)}
        for it in items:
            it["tier"] = level_to_tier[it["level"]]

    raw_affixes.sort(key=lambda a: (a["group"], a["type"], a["level"], a["id"]))
    return raw_affixes


def parse_armor_curves(pob: Path) -> dict[str, list[dict[str, Any]]]:
    """Data/Bases/{helmet,body,gloves,boots,shield}.lua에서 (slotGroup, defenseSubtype)별 iLvl 커브 추출."""
    slot_files = {
        "Helmet": "helmet.lua",
        "Body": "body.lua",
        "Gloves": "gloves.lua",
        "Boots": "boots.lua",
        "Shield": "shield.lua",
    }
    subtype_map = {
        "Armour": ("Armour", "Armour"),
        "Evasion": ("Evasion", "Evasion"),
        "Energy Shield": ("EnergyShield", "EnergyShield"),
    }
    curves: dict[str, list[dict[str, Any]]] = {}

    for slot_name, fname in slot_files.items():
        src = (pob / "Data" / "Bases" / fname).read_text(encoding="utf-8")
        grouped: dict[str, list[dict[str, Any]]] = {}
        for m in re.finditer(r'itemBases\["([^"]+)"\] = \{(.*?)\n\}', src, re.S):
            body = m.group(2)
            sub_m = re.search(r'subType = "([^"]+)"', body)
            tags_m = re.search(r'tags = \{([^}]*)\}', body)
            if not sub_m or sub_m.group(1) not in subtype_map:
                continue
            if tags_m and any(bad in tags_m.group(1) for bad in ("not_for_sale", "demigods", "atlas_base_type")):
                continue
            sub_key, field_prefix = subtype_map[sub_m.group(1)]
            arm_m = re.search(field_prefix + r"BaseMin = (\d+), " + field_prefix + r"BaseMax = (\d+)", body)
            if not arm_m:
                continue
            avg_def = (int(arm_m.group(1)) + int(arm_m.group(2))) / 2.0
            blk_m = re.search(r"BlockChance = (\d+)", body)
            req_m = re.search(r"req = \{([^}]*)\}", body)
            req_lvl, req_str, req_dex, req_int = 1, 0, 0, 0
            if req_m:
                rb = req_m.group(1)
                ml = re.search(r"\blevel = (\d+)", rb)
                ms = re.search(r"\bstr = (\d+)", rb)
                md = re.search(r"\bdex = (\d+)", rb)
                mi = re.search(r"\bint = (\d+)", rb)
                if ml:
                    req_lvl = int(ml.group(1))
                if ms:
                    req_str = int(ms.group(1))
                if md:
                    req_dex = int(md.group(1))
                if mi:
                    req_int = int(mi.group(1))
            entry = {
                "reqLevel": req_lvl,
                "reqStr": req_str,
                "reqDex": req_dex,
                "reqInt": req_int,
                "baseArmour": avg_def if sub_key == "Armour" else 0.0,
                "baseEvasion": avg_def if sub_key == "Evasion" else 0.0,
                "baseEnergyShield": avg_def if sub_key == "EnergyShield" else 0.0,
                "baseBlockChance": float(blk_m.group(1)) if blk_m else 0.0,
            }
            grouped.setdefault(f"{slot_name}:{sub_key}", []).append(entry)

        for key, rows in grouped.items():
            # 같은 reqLevel 중복은 방어력이 가장 높은 기본 베이스 1개만 유지, reqLevel 오름차순 정렬
            by_lvl: dict[int, dict[str, Any]] = {}
            for r in rows:
                lv = r["reqLevel"]
                score = r["baseArmour"] + r["baseEvasion"] + r["baseEnergyShield"]
                prev = by_lvl.get(lv)
                if prev is None or score > (prev["baseArmour"] + prev["baseEvasion"] + prev["baseEnergyShield"]):
                    by_lvl[lv] = r
            curves[key] = [by_lvl[lv] for lv in sorted(by_lvl)]

    return curves


def parse_weapon_curves(pob: Path) -> dict[str, list[dict[str, Any]]]:
    """Data/Bases/{sword,axe,mace,dagger,bow,staff}.lua에서 무기 종류별 iLvl 커브 추출."""
    sources = [
        ("OneHandSword", "sword.lua", "One Handed Sword"),
        ("TwoHandSword", "sword.lua", "Two Handed Sword"),
        ("OneHandAxe", "axe.lua", "One Handed Axe"),
        ("TwoHandAxe", "axe.lua", "Two Handed Axe"),
        ("OneHandMace", "mace.lua", "One Handed Mace"),
        ("TwoHandMace", "mace.lua", "Two Handed Mace"),
        ("Dagger", "dagger.lua", "Dagger"),
        ("Bow", "bow.lua", "Bow"),
        ("Staff", "staff.lua", "Staff"),
    ]
    file_cache: dict[str, str] = {}
    curves: dict[str, list[dict[str, Any]]] = {}

    for key, fname, poe_type in sources:
        if fname not in file_cache:
            file_cache[fname] = (pob / "Data" / "Bases" / fname).read_text(encoding="utf-8")
        src = file_cache[fname]
        by_lvl: dict[int, dict[str, Any]] = {}
        for m in re.finditer(r'itemBases\["([^"]+)"\] = \{(.*?)\n\}', src, re.S):
            body = m.group(2)
            t_m = re.search(r'type = "([^"]+)"', body)
            if not t_m or t_m.group(1) != poe_type:
                continue
            tags_m = re.search(r'tags = \{([^}]*)\}', body)
            if tags_m and any(bad in tags_m.group(1) for bad in ("not_for_sale", "demigods", "maraketh")):
                continue
            w_m = re.search(
                r"PhysicalMin = (\d+), PhysicalMax = (\d+), CritChanceBase = ([0-9.]+), AttackRateBase = ([0-9.]+)",
                body,
            )
            if not w_m:
                continue
            pmin = float(w_m.group(1))
            pmax = float(w_m.group(2))
            crit = float(w_m.group(3))
            aps = float(w_m.group(4))
            req_m = re.search(r"req = \{([^}]*)\}", body)
            req_lvl, req_str, req_dex, req_int = 1, 0, 0, 0
            if req_m:
                rb = req_m.group(1)
                ml = re.search(r"\blevel = (\d+)", rb)
                ms = re.search(r"\bstr = (\d+)", rb)
                md = re.search(r"\bdex = (\d+)", rb)
                mi = re.search(r"\bint = (\d+)", rb)
                if ml:
                    req_lvl = int(ml.group(1))
                if ms:
                    req_str = int(ms.group(1))
                if md:
                    req_dex = int(md.group(1))
                if mi:
                    req_int = int(mi.group(1))
            entry = {
                "reqLevel": req_lvl,
                "reqStr": req_str,
                "reqDex": req_dex,
                "reqInt": req_int,
                "physMin": pmin,
                "physMax": pmax,
                "critChance": crit,
                "attackRate": aps,
            }
            dps = (pmin + pmax) * 0.5 * aps
            prev = by_lvl.get(req_lvl)
            if prev is None or dps > (prev["physMin"] + prev["physMax"]) * 0.5 * prev["attackRate"]:
                by_lvl[req_lvl] = entry
        curves[key] = [by_lvl[lv] for lv in sorted(by_lvl)]

    return curves


def compile_items(pob: Path, out_dir: Path) -> dict[str, Any]:
    affixes = parse_mod_explicit(pob)
    armor_curves = parse_armor_curves(pob)
    weapon_curves = parse_weapon_curves(pob)

    rollable_count = sum(1 for a in affixes if a["rollable"])
    local_count = sum(1 for a in affixes if a["isLocal"])

    pack = {
        "meta": {
            "schemaVersion": 1,
            "pobVersion": "2.68.1",
            "poeVersion": "3.30",
            "totalAffixes": len(affixes),
            "rollableAffixes": rollable_count,
            "localAffixes": local_count,
        },
        "tiers": TIERS,
        "currencies": CURRENCIES,
        "baseArmorCurves": armor_curves,
        "baseWeaponCurves": weapon_curves,
        "affixes": affixes,
    }

    packs_dir = out_dir / "packs"
    packs_dir.mkdir(parents=True, exist_ok=True)
    out_path = packs_dir / "items_3_30.json"
    out_path.write_text(json.dumps(pack, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    return pack


def main() -> int:
    for s in (sys.stdout, sys.stderr):
        if hasattr(s, "reconfigure"):
            s.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description="PoB 2.68.1 -> SkyrimPoE 4단계 아이템 데이터 팩 변환기")
    ap.add_argument("--pob", type=Path, default=DEFAULT_POB)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()
    pack = compile_items(args.pob, args.out)
    m = pack["meta"]
    print(
        f"생성 완료: {args.out / 'packs' / 'items_3_30.json'} "
        f"(total={m['totalAffixes']}, rollable={m['rollableAffixes']}, local={m['localAffixes']}, "
        f"armorCurves={len(pack['baseArmorCurves'])}, weaponCurves={len(pack['baseWeaponCurves'])})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
