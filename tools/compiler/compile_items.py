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
import hashlib
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

# 4~5단계에서 장비·벨트·주얼이 가질 수 있는 모든 기본 태그 (Data/Bases/*.lua 대조)
RELEVANT_ITEM_TAGS = frozenset({
    "weapon", "one_hand_weapon", "two_hand_weapon", "onehand", "twohand",
    "sword", "axe", "mace", "dagger", "bow", "staff", "sceptre", "wand", "claw",
    "attack_dagger", "attack_staff", "warstaff", "ranged",
    "armour", "body_armour", "helmet", "gloves", "boots", "shield",
    "str_armour", "dex_armour", "int_armour",
    "str_dex_armour", "str_int_armour", "dex_int_armour", "str_dex_int_armour",
    "str_shield", "dex_shield", "focus",
    "str_dex_shield", "str_int_shield", "dex_int_shield",
    "amulet", "ring", "belt",
    "jewel", "not_str", "not_dex", "not_int",
    "default",
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
    # 5단계 추가: 주얼·벨트·고유 아이템 템플릿(결정 161~164)
    "+{0}% to Fire and Cold Resistances": "화염 및 냉기 저항 +{0}%",
    "+{0}% to Fire and Lightning Resistances": "화염 및 번개 저항 +{0}%",
    "+{0}% to Cold and Lightning Resistances": "냉기 및 번개 저항 +{0}%",
    "Adds {0} to {1} Fire Damage to Spells and Attacks": "주문 및 공격에 화염 피해 {0}~{1} 추가",
    "Adds {0} to {1} Cold Damage to Spells and Attacks": "주문 및 공격에 냉기 피해 {0}~{1} 추가",
    "Adds {0} to {1} Lightning Damage to Spells and Attacks": "주문 및 공격에 번개 피해 {0}~{1} 추가",
    "-{0} to Total Mana Cost of Skills": "스킬 총 마나 비용 -{0}",
    "+{0} to maximum Spirit": "최대 정신력 +{0}",
    "{0}% increased Global Physical Damage": "전역 물리 피해 {0}% 증가",
    "{0}% increased Melee Physical Damage": "근접 물리 피해 {0}% 증가",
    "{0}% increased Melee Damage": "근접 피해 {0}% 증가",
    "{0}% increased Damage": "피해 {0}% 증가",
    "{0}% increased Elemental Damage": "원소 피해 {0}% 증가",
    "{0}% increased Chaos Damage": "카오스 피해 {0}% 증가",
    "{0}% increased Projectile Damage": "투사체 피해 {0}% 증가",
    "{0}% increased Area Damage": "범위 피해 {0}% 증가",
    "{0}% increased Area of Effect": "효과 범위 {0}% 증가",
    "{0}% increased Area of Effect for Attacks": "공격 효과 범위 {0}% 증가",
    "{0}% increased Critical Strike Chance for Spells": "주문 치명타 확률 {0}% 증가",
    "+{0}% to Critical Strike Multiplier for Spell Damage": "주문 치명타 피해 배율 +{0}%",
    "+{0}% to Critical Strike Multiplier with Elemental Skills": "원소 스킬 치명타 피해 배율 +{0}%",
    "+{0}% to Melee Critical Strike Multiplier": "근접 치명타 피해 배율 +{0}%",
    "{0}% increased Damage with Hits against Rare monsters": "희귀 몬스터 대상 적중 피해 {0}% 증가",
    "Cannot be Frozen": "빙결 면역",
    "Chaos Damage taken does not bypass Energy Shield": "받는 카오스 피해가 에너지 실드를 우회하지 않음",
    "Your Physical Damage can Shock": "물리 피해로 감전 유발 가능",
    "Deal no Elemental Damage": "원소 피해를 줄 수 없음",
    "When you Kill a Rare monster, you gain its Modifiers for {0} seconds": "희귀 몬스터 처치 시 {0}초간 해당 몬스터의 속성을 획득",
    "When you Kill a Shocked Enemy, inflict an equivalent Shock on each Enemy within {0} metres": "감전된 적 처치 시 반경 {0}m 내 적에게 감전 전파",
    "When you Kill an Ignited Enemy, inflict an equivalent Ignite on each Enemy within {0} metres": "점화된 적 처치 시 반경 {0}m 내 적에게 점화 전파",
    "Regenerate {0} Mana per second": "초당 마나 {0} 재생",
    "+{0}% to Item Rarity": "아이템 희귀도 +{0}%",
    "{0}% of Damage taken Recouped as Energy Shield": "받는 피해의 {0}%를 에너지 실드로 회수",
    "{0}% reduced Damage when on Low Life": "낮은 생명력 상태일 때 피해 {0}% 감소",
    "{0}% increased Global Evasion Rating when on Low Life": "낮은 생명력 상태일 때 전역 회피 {0}% 증가",
    "-{0} Physical Damage taken from Attack Hits": "공격 명중으로 받는 물리 피해 -{0}",
    "Has 1 Abyssal Socket": "심연의 홈 1개 (스카이림PoE: 벨트 참 칸 +1)",
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
        "koName": "감정 주문서",
        "koDesc": "미감정 봉인 토큰을 감정해 장비와 어픽스를 확정합니다.",
        "dropWeight": 260,
        "worldDropTier": 0,
    },
    {
        "id": "Transmutation",
        "enName": "Orb of Transmutation",
        "koName": "진화의 오브",
        "koDesc": "일반 장비를 매직 장비(어픽스 1~2개)로 업그레이드합니다.",
        "dropWeight": 190,
        "worldDropTier": 0,
    },
    {
        "id": "Augmentation",
        "enName": "Orb of Augmentation",
        "koName": "확장의 오브",
        "koDesc": "어픽스가 1개인 매직 장비에 새 어픽스를 1개 추가합니다.",
        "dropWeight": 160,
        "worldDropTier": 0,
    },
    {
        "id": "Alteration",
        "enName": "Orb of Alteration",
        "koName": "변화의 오브",
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
        "koName": "쥬얼러 오브",
        "koDesc": "스킬·발동 젬의 보조 소켓 수를 1칸 늘립니다(최대 5칸).",
        "dropWeight": 90,
        "worldDropTier": 2,
    },
]

# 숫자 범위 `(min-max)`(음수 포함: `(-25-50)`, `(-40-40)`) 또는 단일 숫자 토큰 파싱 정규식
# 예: "+(8-12) to Strength", "+(-25-50)% to Fire Resistance", "-(8-4) to Total Mana Cost"
_NUM_OR_RANGE_RE = re.compile(r"\((-?\d+(?:\.\d+)?)-(-?\d+(?:\.\d+)?)\)|(\d+(?:\.\d+)?)")


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


def read_pob_text(pob: Path, relative_path: str, source_sha256: dict[str, str] | None) -> str:
    data = (pob / relative_path).read_bytes()
    if source_sha256 is not None:
        source_sha256[relative_path] = hashlib.sha256(data).hexdigest()
    return data.decode("utf-8").replace("\r\n", "\n").replace("\r", "\n")


def parse_mod_explicit(pob: Path, source_sha256: dict[str, str] | None = None) -> list[dict[str, Any]]:
    src = read_pob_text(pob, "Data/ModExplicit.lua", source_sha256)
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


def parse_armor_curves(
    pob: Path, source_sha256: dict[str, str] | None = None,
) -> dict[str, list[dict[str, Any]]]:
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
        src = read_pob_text(pob, f"Data/Bases/{fname}", source_sha256)
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


def parse_weapon_curves(
    pob: Path, source_sha256: dict[str, str] | None = None,
) -> dict[str, list[dict[str, Any]]]:
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
            file_cache[fname] = read_pob_text(pob, f"Data/Bases/{fname}", source_sha256)
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


def parse_mod_jewel(
    pob: Path, existing_ids: set[str], source_sha256: dict[str, str] | None = None,
) -> list[dict[str, Any]]:
    """Data/ModJewel.lua에서 일반 주얼(Crimson/Viridian/Cobalt) Prefix/Suffix 어픽스를 추출한다.

    검수 C3 반영:
      - type in ("Prefix", "Suffix")만 허용해 Corrupted 모드를 제외한다.
      - abyss_jewel 전용 모드는 제외하고 jewel/not_str/not_dex/not_int 부정 태그 순서를 보존한다.
    """
    src = read_pob_text(pob, "Data/ModJewel.lua", source_sha256)
    jewel_affixes: list[dict[str, Any]] = []
    jewel_tags = frozenset({"jewel", "not_str", "not_dex", "not_int", "default"})

    for m in re.finditer(r'\["([^"]+)"\]\s*=\s*\{([^\n]+)\}', src):
        raw_id, body = m.group(1), m.group(2)
        m_type = re.search(r'type = "([^"]+)"', body)
        m_affix = re.search(r'affix = "([^"]*)"', body)
        m_level = re.search(r'\blevel = (\d+)', body)
        m_group = re.search(r'group = "([^"]+)"', body)
        m_wk = re.search(r'weightKey = \{([^}]*)\}', body)
        m_wv = re.search(r'weightVal = \{([^}]*)\}', body)
        m_lines = re.search(r'affix = "[^"]*",\s*(.*?),\s*statOrder =', body)
        if not (m_type and m_affix and m_level and m_group and m_wk and m_wv and m_lines):
            continue

        if raw_id.startswith("Delve"):
            continue
        affix_type = m_type.group(1)
        if affix_type not in ("Prefix", "Suffix"):
            continue

        keys = re.findall(r'"([^"]+)"', m_wk.group(1))
        vals = [int(x.strip()) for x in m_wv.group(1).split(",") if x.strip()]
        if len(keys) != len(vals):
            continue

        weights: list[list[Any]] = []
        has_positive_jewel = False
        seen_keys: set[str] = set()
        for k, v in zip(keys, vals):
            if k == "abyss_jewel":
                continue
            if k in jewel_tags:
                mapped_k = "jewel" if k == "default" else k
                if mapped_k in seen_keys:
                    continue
                seen_keys.add(mapped_k)
                weights.append([mapped_k, v])
                if v > 0 and k in ("not_str", "not_dex", "not_int", "default"):
                    has_positive_jewel = True
        if not has_positive_jewel:
            continue

        raw_lines = re.findall(r'"([^"]+)"', m_lines.group(1))
        parsed_lines = [parse_stat_line(rl) for rl in raw_lines]
        if not is_rollable_in_stage4(parsed_lines):
            continue

        affix_id = f"Jewel_{raw_id}" if raw_id in existing_ids else raw_id
        existing_ids.add(affix_id)
        m_tags = re.search(r'modTags = \{([^}]*)\}', body)
        mod_tags = re.findall(r'"([^"]+)"', m_tags.group(1)) if m_tags else []

        jewel_affixes.append({
            "id": affix_id,
            "type": affix_type,
            "affixName": m_affix.group(1),
            "group": m_group.group(1),
            "level": int(m_level.group(1)),
            "tier": 1,
            "isLocal": False,
            "rollable": True,
            "lines": parsed_lines,
            "weights": weights,
            "tags": mod_tags,
        })

    jewel_affixes.sort(key=lambda a: (a["group"], a["type"], a["id"]))
    return jewel_affixes


# 5단계 벨트 베이스 5종(결정 162, 바디슬롯 비점유 가상 장착 + 고정 임플리싯)
BELT_BASES: list[dict[str, Any]] = [
    {
        "id": "LeatherBelt",
        "enName": "Leather Belt",
        "koName": "가죽 허리띠",
        "localFormId": 0x000890,
        "reqLevel": 8,
        "bonusCharmSlot": False,
        "implicitLine": "+(25-40) to maximum Life",
    },
    {
        "id": "HeavyBelt",
        "enName": "Heavy Belt",
        "koName": "무거운 허리띠",
        "localFormId": 0x000891,
        "reqLevel": 8,
        "bonusCharmSlot": False,
        "implicitLine": "+(25-35) to Strength",
    },
    {
        "id": "ChainBelt",
        "enName": "Chain Belt",
        "koName": "사슬 허리띠",
        "localFormId": 0x000892,
        "reqLevel": 1,
        "bonusCharmSlot": False,
        "implicitLine": "+(9-20) to maximum Energy Shield",
    },
    {
        "id": "RusticSash",
        "enName": "Rustic Sash",
        "koName": "소박한 띠",
        "localFormId": 0x000893,
        "reqLevel": 1,
        "bonusCharmSlot": False,
        "implicitLine": "(12-24)% increased Global Physical Damage",
    },
    {
        "id": "StygianVise",
        "enName": "Stygian Vise",
        "koName": "명계의 조임쇠",
        "localFormId": 0x000894,
        # C-16: PoB Data/Bases/belt.lua:114-122 대조 — req={} (레벨 1), implicit="Has 1 Abyssal Socket"
        # 스카이림PoE에서는 심연 소켓 대신 참 칸 +1(bonusCharmSlot=True)로 적응하고 생명력 임플리싯을 지어 넣지 않는다.
        "reqLevel": 1,
        "bonusCharmSlot": True,
        "implicitLine": "Has 1 Abyssal Socket",
    },
]

# 5단계 핵심 참(Charms) 6종(결정 44, 162)
CHARMS: list[dict[str, Any]] = [
    {
        "id": "RubyCharm",
        "kind": 0,
        "localFormId": 0x0008A2,
        "enName": "Ruby Charm",
        "koName": "루비 참",
        "koDesc": "화염 피해 피격 시 자동 발동: 4초간 화염 저항 +25%, 최대 화염 저항 +2%",
        "maxCharges": 40,
        "chargesPerUse": 20,
        "durationSec": 4.0,
    },
    {
        "id": "SapphireCharm",
        "kind": 1,
        "localFormId": 0x0008A3,
        "enName": "Sapphire Charm",
        "koName": "사파이어 참",
        "koDesc": "냉기 피해 피격 시 자동 발동: 4초간 냉기 저항 +25%, 최대 냉기 저항 +2%",
        "maxCharges": 40,
        "chargesPerUse": 20,
        "durationSec": 4.0,
    },
    {
        "id": "TopazCharm",
        "kind": 2,
        "localFormId": 0x0008A4,
        "enName": "Topaz Charm",
        "koName": "토파즈 참",
        "koDesc": "번개 피해 피격 시 자동 발동: 4초간 번개 저항 +25%, 최대 번개 저항 +2%",
        "maxCharges": 40,
        "chargesPerUse": 20,
        "durationSec": 4.0,
    },
    {
        "id": "AmethystCharm",
        "kind": 3,
        "localFormId": 0x0008A5,
        "enName": "Amethyst Charm",
        "koName": "자수정 참",
        "koDesc": "카오스 피해 피격 시 자동 발동: 4초간 카오스 저항 +25%",
        "maxCharges": 40,
        "chargesPerUse": 20,
        "durationSec": 4.0,
    },
    {
        "id": "ThawingStaunchingCharm",
        "kind": 4,
        "localFormId": 0x0008A6,
        "enName": "Purifying Charm",
        "koName": "정화의 참",
        "koDesc": "빙결·냉각·출혈 피격 시 자동 발동: 즉시 해제 및 4초간 빙결·출혈 면역",
        "maxCharges": 40,
        "chargesPerUse": 20,
        "durationSec": 4.0,
    },
    {
        "id": "GuardCrisisCharm",
        "kind": 5,
        "localFormId": 0x0008A7,
        "enName": "Bastion Charm",
        "koName": "수호의 참",
        "koDesc": "생명력이 40% 이하로 떨어지는 피격 시 자동 발동: 4초간 물리 피해 감소 +15%, 방어도 +600",
        "maxCharges": 40,
        "chargesPerUse": 20,
        "durationSec": 4.0,
    },
]

# 5단계 매직·레어 몬스터 대표 속성 12종(결정 13, 164)
MONSTER_MODS: list[dict[str, Any]] = [
    {"id": "Hasted", "koName": "신속한", "koDesc": "공격·이동 가속, 주는 피해 +15%"},
    {"id": "Armoured", "koName": "중장갑", "koDesc": "물리 방어 강화 (받는 물리 피해 -25%)"},
    {"id": "FireResistant", "koName": "화염 저항", "koDesc": "화염 저항 +40%"},
    {"id": "ColdResistant", "koName": "냉기 저항", "koDesc": "냉기 저항 +40%"},
    {"id": "LightningResistant", "koName": "번개 저항", "koDesc": "번개 저항 +40%"},
    {"id": "ChaosResistant", "koName": "카오스 저항", "koDesc": "카오스 저항 +40%"},
    {"id": "EnergyShielded", "koName": "에너지 실드", "koDesc": "최대 체력의 35%에 해당하는 적 에너지 실드 부여"},
    {"id": "ExtraPhysical", "koName": "강력한 물리", "koDesc": "주는 물리 피해 +25%"},
    {"id": "ExtraFire", "koName": "화염 주입", "koDesc": "주는 피해의 25%를 추가 화염 피해로 부여"},
    {"id": "ExtraCold", "koName": "냉기 주입", "koDesc": "주는 피해의 25%를 추가 냉기 피해로 부여"},
    {"id": "ExtraLightning", "koName": "번개 주입", "koDesc": "주는 피해의 25%를 추가 번개 피해로 부여"},
    {"id": "Deadly", "koName": "치명적인", "koDesc": "치명타 배율 증가, 주는 피해 +20%"},
]

# 5단계 PoE1 대표 고유 아이템 16종 메타(PoB 2.68.1 Data/Uniques/*.lua Variant: Current 대조 + SkyrimPoE 적응 명시)
STAGE5_UNIQUE_SPECS: list[dict[str, Any]] = [
    {
        "id": "Goldrim", "koName": "골드림", "file": "helmet.lua", "pobName": "Goldrim",
        "slotGroup": 3, "defenseSubtype": 2, "baseFormId": 0x00013913, "reqLevel": 1,
        "keepLines": ["+(30-50) to Evasion Rating", "+(30-40)% to all Elemental Resistances", "+10% to Item Rarity"],
        # U5: 근접 피격 반사(Thorns) 시스템 미도입
        "omittedLines": ["Reflects 4 Physical Damage to Melee Attackers"],
    },
    {
        "id": "StarkonjasHead", "koName": "스타콘자의 머리", "file": "helmet.lua", "pobName": "Starkonja's Head",
        "slotGroup": 3, "defenseSubtype": 2, "baseFormId": 0x0001391D, "reqLevel": 60,
        # U5 확정(2026-11-20): Low Life 단점(50% reduced Damage) 및 저체력 회피 150% 증가 원문 복원·구현
        "keepLines": [
            "+(50-70) to Dexterity",
            "10% increased Attack Speed",
            "25% increased Global Critical Strike Chance",
            "(100-130)% increased Evasion Rating",
            "+(80-100) to maximum Life",
            "50% reduced Damage when on Low Life",
            "150% increased Global Evasion Rating when on Low Life",
        ],
    },
    {
        "id": "Wanderlust", "koName": "방랑벽", "file": "boots.lua", "pobName": "Wanderlust",
        "slotGroup": 6, "defenseSubtype": 3, "baseFormId": 0x0010E2CE, "reqLevel": 1,
        "keepLines": ["+5 to Dexterity", "+(10-20) to maximum Energy Shield", "(20-40)% increased Mana Regeneration Rate", "20% increased Movement Speed", "Cannot be Frozen"],
        "specialFlags": ["cannotBeFrozen"],
    },
    {
        "id": "AtzirisStep", "koName": "앗지리의 걸음", "file": "boots.lua", "pobName": "Atziri's Step",
        "slotGroup": 6, "defenseSubtype": 2, "baseFormId": 0x00013938, "reqLevel": 69,
        "keepLines": ["+(20-26)% chance to Suppress Spell Damage", "180% increased Evasion Rating", "+(55-75) to maximum Life", "30% increased Movement Speed"],
    },
    {
        "id": "BellyOfTheBeast", "koName": "야수의 소굴", "file": "body.lua", "pobName": "Belly of the Beast",
        "slotGroup": 4, "defenseSubtype": 1, "baseFormId": 0x00013952, "reqLevel": 46,
        "keepLines": ["(150-200)% increased Armour", "(30-40)% increased maximum Life", "+(10-15)% to all Elemental Resistances"],
        # U5: 플라스크 회복 속도(참 체계 대체)·시각 효과 미도입
        "omittedLines": ["50% increased Flask Life Recovery rate", "Extra gore"],
    },
    {
        "id": "KaomsHeart", "koName": "카옴의 심장", "file": "body.lua", "pobName": "Kaom's Heart",
        "slotGroup": 4, "defenseSubtype": 1, "baseFormId": 0x00013961, "reqLevel": 68,
        "keepLines": ["+1000 to maximum Life"],
        # U5: 장비 소켓 체계 없음(PoE2식 젬 자체 소켓 사용)
        "omittedLines": ["Has no Sockets"],
    },
    {
        "id": "ShavronnesWrappings", "koName": "샤브론의 겉옷", "file": "body.lua", "pobName": "Shavronne's Wrappings",
        "slotGroup": 4, "defenseSubtype": 3, "baseFormId": 0x0010D6A5, "reqLevel": 62,
        "implicitLines": ["(3-10)% increased Spell Damage"],
        "keepLines": ["(150-200)% increased Energy Shield", "+(30-40)% to Lightning Resistance", "Chaos Damage taken does not bypass Energy Shield", "(20-30)% of Damage taken Recouped as Energy Shield"],
        "omittedLines": ["Reflects 1 to 250 Lightning Damage to Melee Attackers"],
        "specialFlags": ["shavronneChaosEs"],
    },
    {
        "id": "Facebreaker", "koName": "안면 강타자", "file": "gloves.lua", "pobName": "Facebreaker",
        "slotGroup": 5, "defenseSubtype": 1, "baseFormId": 0x00012E46, "reqLevel": 16,
        # 검수 C3: 스카이림PoE에는 맨손 공격 스킬이 없어 근접 물리 증폭 + 치명타 배율 + 생명력으로 적응
        "overrideLines": ["(30-50)% increased Melee Physical Damage", "+(60-90)% to Global Critical Strike Multiplier", "+50 to maximum Life"],
    },
    {
        "id": "PrismGuardian", "koName": "프리즘 수호자", "file": "shield.lua", "pobName": "Prism Guardian",
        "slotGroup": 2, "defenseSubtype": 3, "baseFormId": 0x0001393C, "reqLevel": 68,
        "implicitLines": ["+12% to all Elemental Resistances"],
        # 검수 C3: PoE1 생명력 점유 효율을 스카이림PoE 정신력(Spirit) +40으로 적응
        "overrideLines": ["+40 to maximum Spirit", "+(20-30) to Dexterity", "+25% to all Elemental Resistances"],
    },
    {
        "id": "Lifesprig", "koName": "생명의 가지", "file": "wand.lua", "pobName": "Lifesprig",
        # C-16: Skyrim 베이스 0x00012EB7(IronSword)은 한손검이므로 양손 Staff 곡선 대신 OneHandSword 곡선 사용(PoE 원본 Wand 적응)
        "slotGroup": 0, "defenseSubtype": 0, "weaponTag": "OneHandSword", "baseFormId": 0x00012EB7, "reqLevel": 1,
        "implicitLines": ["(8-12)% increased Spell Damage"],
        "keepLines": ["(20-28)% increased Spell Damage", "(5-8)% increased Cast Speed", "+(15-20) to maximum Life", "+(15-20) to maximum Mana"],
        # U5: 장비 소켓 젬 레벨 +1·시전당 생명력 재생 트리거 미도입
        "omittedLines": ["+1 to Level of Socketed Spell Gems", "Regenerate (6-8) Life over 1 second for each Spell you Cast"],
    },
    {
        "id": "Starforge", "koName": "별의 탄생", "file": "sword.lua", "pobName": "Starforge",
        "slotGroup": 1, "defenseSubtype": 0, "weaponTag": "TwoHandSword", "baseFormId": 0x000139B7, "reqLevel": 67,
        "implicitLines": ["30% increased Global Physical Damage"],
        "keepLines": ["(400-450)% increased Physical Damage", "(5-8)% increased Attack Speed", "+(90-100) to maximum Life", "Your Physical Damage can Shock", "20% increased Area of Effect for Attacks", "Deal no Elemental Damage"],
        "localLineIndices": [0, 1],
        "specialFlags": ["starforgeShock", "dealNoElemental"],
    },
    {
        "id": "Astramentis", "koName": "아스트라멘티스", "file": "amulet.lua", "pobName": "Astramentis",
        "slotGroup": 7, "defenseSubtype": 0, "isAmulet": True, "baseFormId": 0x000877F1, "reqLevel": 20,
        "implicitLines": ["+(10-16) to all Attributes"],
        # U5 확정(2026-11-20): -4 Physical Damage taken from Attack Hits 원문 복원·구현
        "keepLines": ["+(80-100) to all Attributes", "-4 Physical Damage taken from Attack Hits"],
    },
    {
        "id": "Praxis", "koName": "프락시스", "file": "ring.lua", "pobName": "Praxis",
        "slotGroup": 7, "defenseSubtype": 0, "isAmulet": False, "baseFormId": 0x0001CF2B, "reqLevel": 22,
        "implicitLines": ["+(20-30) to maximum Mana"],
        "keepLines": ["+(30-60) to maximum Mana", "Regenerate (3-6) Mana per second", "-(8-4) to Total Mana Cost of Skills"],
        # U5: 피해 마나 회수(Mana Recoup)·장비 마나 비용 효율 미도입
        "omittedLines": ["(20-30)% increased Mana Cost Efficiency", "8% of Damage taken Recouped as Mana"],
    },
    {
        "id": "VentorsGamble", "koName": "벤토의 도박", "file": "ring.lua", "pobName": "Ventor's Gamble",
        "slotGroup": 7, "defenseSubtype": 0, "isAmulet": False, "baseFormId": 0x000877C9, "reqLevel": 65,
        "implicitLines": ["+(6-15)% to Item Rarity"],
        "keepLines": ["+(0-60) to maximum Life", "+(-40-40)% to Item Rarity", "+(-25-50)% to Fire Resistance", "+(-25-50)% to Cold Resistance", "+(-25-50)% to Lightning Resistance"],
        # U5: 마나 점유 효율 대신 정신력(Spirit) 체계 사용
        "omittedLines": ["(-15-15)% reduced Mana Reservation Efficiency of Skills"],
    },
    {
        "id": "BereksRespite", "koName": "베렉의 휴식", "file": "ring.lua", "pobName": "Berek's Respite",
        "slotGroup": 7, "defenseSubtype": 0, "isAmulet": False, "baseFormId": 0x000877CA, "reqLevel": 20,
        "implicitLines": ["+(12-16)% to Fire and Lightning Resistances"],
        "keepLines": ["Adds (20-25) to (30-50) Fire Damage to Spells and Attacks", "(25-30)% increased Lightning Damage", "+(30-40) to maximum Mana", "When you Kill a Shocked Enemy, inflict an equivalent Shock on each Enemy within 2 metres", "When you Kill an Ignited Enemy, inflict an equivalent Ignite on each Enemy within 2 metres"],
        "specialFlags": ["berekProlif"],
    },
    {
        "id": "Headhunter", "koName": "헤드헌터", "file": "belt.lua", "pobName": "Headhunter",
        "slotGroup": 8, "defenseSubtype": 0, "baseFormId": 0x000890, "reqLevel": 40,
        "implicitLines": ["+(25-40) to maximum Life"],
        "keepLines": ["+(40-55) to Strength", "+(40-55) to Dexterity", "+(50-60) to maximum Life", "(20-30)% increased Damage with Hits against Rare monsters", "When you Kill a Rare monster, you gain its Modifiers for 60 seconds"],
        "specialFlags": ["headhunter"],
    },
]


def extract_pob_unique_current_lines(
    pob: Path, fname: str, pob_name: str, source_sha256: dict[str, str] | None = None,
) -> set[str]:
    """PoB 2.68.1 Data/Uniques/<fname>에서 해당 고유 아이템의 Variant: Current 줄 집합을 검증한다."""
    txt = read_pob_text(pob, f"Data/Uniques/{fname}", source_sha256)
    for blk in re.findall(r"\[\[(.*?)\]\]", txt, re.S):
        lines = [l.strip() for l in blk.strip().splitlines() if l.strip()]
        if not lines or lines[0] != pob_name:
            continue
        variants = [l for l in lines if l.startswith("Variant:")]
        # C-16: "Current" 라벨이 명시된 Variant를 우선 선택하고, 없을 때만 마지막 Variant(최신 판)로 폴백
        cur_var = 0
        for v_idx, v_line in enumerate(variants, start=1):
            if "current" in v_line.lower():
                cur_var = v_idx
        if cur_var == 0:
            cur_var = len(variants)
        out: set[str] = set()
        # lines[0]은 고유명, lines[1]은 베이스 아이템명(예: Silken Hood)이므로 모디파이어는 lines[2:]부터 검사
        start_idx = 2 if len(lines) > 1 and not lines[1].startswith(("Variant:", "Implicits:", "League:", "Source:", "Requires ")) else 1
        for raw in lines[start_idx:]:
            if raw.startswith(("Variant:", "Implicits:", "League:", "Source:", "Requires ", "Shaper ", "Elder ")):
                continue
            # {variant:1,3} 필터링
            vm = re.search(r"\{variant:([0-9,]+)\}", raw)
            if vm and cur_var > 0:
                allowed = {int(x) for x in vm.group(1).split(",") if x.isdigit()}
                if cur_var not in allowed:
                    continue
            cleaned = re.sub(r"\{[^}]+\}", "", raw).strip()
            if cleaned:
                out.add(cleaned)
        return out
    raise ValueError(f"PoB Uniques/{fname}에서 '{pob_name}'을 찾지 못했습니다")


def build_stage5_implicits_and_uniques(
    pob: Path,
    existing_ids: set[str],
    source_sha256: dict[str, str] | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    """벨트 임플리싯 및 고유 아이템 16종의 비롤러블 어픽스(Divine 재롤 지원)와 정의 테이블을 생성한다."""
    extra_affixes: list[dict[str, Any]] = []
    belt_defs: list[dict[str, Any]] = []

    for b in BELT_BASES:
        imp_id = f"Implicit_Belt_{b['id']}"
        existing_ids.add(imp_id)
        parsed = parse_stat_line(b["implicitLine"])
        extra_affixes.append({
            "id": imp_id,
            "type": "Prefix",
            "affixName": b["enName"],
            "group": f"Implicit_{b['id']}",
            "level": b["reqLevel"],
            "tier": 1,
            "isLocal": False,
            "rollable": False,
            "lines": [parsed],
            "weights": [],
            "tags": ["implicit", "belt"],
        })
        belt_defs.append({
            "id": b["id"],
            "enName": b["enName"],
            "koName": b["koName"],
            "localFormId": b["localFormId"],
            "reqLevel": b["reqLevel"],
            "bonusCharmSlot": b["bonusCharmSlot"],
            "implicitAffixId": imp_id,
        })

    uniques_out: list[dict[str, Any]] = []
    for spec in STAGE5_UNIQUE_SPECS:
        pob_lines = extract_pob_unique_current_lines(pob, spec["file"], spec["pobName"], source_sha256)
        imp_lines = spec.get("implicitLines", [])
        mod_lines = spec.get("overrideLines")
        if mod_lines is None:
            mod_lines = spec.get("keepLines", [])
            omitted_lines = spec.get("omittedLines", [])
            for ln in mod_lines:
                if ln not in pob_lines:
                    raise ValueError(f"고유 '{spec['pobName']}'의 줄 '{ln}'이 PoB Current에 없습니다: {sorted(pob_lines)}")
            for ln in omitted_lines:
                if ln not in pob_lines:
                    raise ValueError(f"고유 '{spec['pobName']}'의 omittedLines '{ln}'이 PoB Current에 없습니다: {sorted(pob_lines)}")
            # U5/C-16: 표시 없이 빠진 PoB Current 줄이 있으면 컴파일 오류로 즉시 차단
            accounted = set(imp_lines) | set(mod_lines) | set(omitted_lines)
            unaccounted = sorted(pob_lines - accounted)
            if unaccounted:
                raise ValueError(f"고유 '{spec['pobName']}'에 명시되지 않고 누락된 PoB Current 줄이 있습니다: {unaccounted}")
        for ln in imp_lines:
            if ln not in pob_lines:
                raise ValueError(f"고유 '{spec['pobName']}'의 임플리싯 '{ln}'이 PoB Current에 없습니다")

        local_indices = set(spec.get("localLineIndices", []))
        imp_ids: list[str] = []
        for idx, ln in enumerate(imp_lines):
            aid = f"Unique_{spec['id']}_Imp{idx}"
            existing_ids.add(aid)
            extra_affixes.append({
                "id": aid,
                "type": "Prefix",
                "affixName": spec["pobName"],
                "group": aid,
                "level": spec["reqLevel"],
                "tier": 1,
                "isLocal": False,
                "rollable": False,
                "lines": [parse_stat_line(ln)],
                "weights": [],
                "tags": ["unique", "implicit"],
            })
            imp_ids.append(aid)

        mod_ids: list[str] = []
        for idx, ln in enumerate(mod_lines):
            aid = f"Unique_{spec['id']}_Mod{idx}"
            existing_ids.add(aid)
            is_loc = (idx in local_indices) or (
                spec["slotGroup"] in (2, 3, 4, 5, 6)
                and any(
                    ln.endswith(suf)
                    for suf in ("to Armour", "increased Armour", "to Evasion Rating", "increased Evasion Rating", "to maximum Energy Shield", "increased Energy Shield")
                )
            )
            extra_affixes.append({
                "id": aid,
                "type": "Prefix" if idx % 2 == 0 else "Suffix",
                "affixName": spec["pobName"],
                "group": aid,
                "level": spec["reqLevel"],
                "tier": 1,
                "isLocal": is_loc,
                "rollable": False,
                "lines": [parse_stat_line(ln)],
                "weights": [],
                "tags": ["unique"],
            })
            mod_ids.append(aid)

        uniques_out.append({
            "id": spec["id"],
            "enName": spec["pobName"],
            "koName": spec["koName"],
            "slotGroup": spec["slotGroup"],
            "defenseSubtype": spec["defenseSubtype"],
            "isAmulet": spec.get("isAmulet", False),
            "weaponTag": spec.get("weaponTag", ""),
            "baseFormId": spec["baseFormId"],
            "reqLevel": spec["reqLevel"],
            "implicitAffixIds": imp_ids,
            "explicitAffixIds": mod_ids,
            "specialFlags": spec.get("specialFlags", []),
        })

    return extra_affixes, belt_defs, uniques_out


def build_stage6_map_affixes(existing_ids: set[str]) -> list[dict[str, Any]]:
    """6단계(결정 166): 지도 아이템(TokenSlotGroup::Map) 전용 접두 8종·접미 8종 어픽스."""
    specs: list[tuple[str, str, str, str, str]] = [
        # (id, type, affixName, group, stat_line)
        ("MapMod_MonsterDamage",    "Prefix", "야만적인", "MapMonsterDamage",    "Monsters deal (15-30)% increased Damage"),
        ("MapMod_ExtraFire",        "Prefix", "작열하는", "MapExtraFire",        "Monsters deal (30-60)% extra Physical Damage as Fire"),
        ("MapMod_ExtraCold",        "Prefix", "동결의",   "MapExtraCold",        "Monsters deal (30-60)% extra Physical Damage as Cold"),
        ("MapMod_ExtraLightning",   "Prefix", "충격의",   "MapExtraLightning",   "Monsters deal (30-60)% extra Physical Damage as Lightning"),
        ("MapMod_MonsterHaste",     "Prefix", "기민한",   "MapMonsterHaste",     "Monsters have (15-25)% increased Movement and Attack Speed"),
        ("MapMod_MonsterCrit",      "Prefix", "치명적인", "MapMonsterCrit",      "Monsters have (25-50)% increased Critical Strike Chance"),
        ("MapMod_MonsterLife",      "Prefix", "거대한",   "MapMonsterLife",      "Monsters take (15-30)% reduced Damage"),
        ("MapMod_MoreMagicRare",    "Prefix", "군단의",   "MapMoreMagicRare",    "(30-60)% increased Magic and Rare Monsters"),
        ("MapMod_MinusMaxRes",      "Suffix", "저항 약화의", "MapMinusMaxRes",   "Players have -(5-12)% to all maximum Resistances"),
        ("MapMod_LessArmour",       "Suffix", "파쇄의",   "MapLessArmour",       "Players have (25-40)% less Armour"),
        ("MapMod_LessEvasion",      "Suffix", "속박의",   "MapLessEvasion",      "Players have (25-40)% less Evasion Rating"),
        ("MapMod_LessRecovery",     "Suffix", "고갈의",   "MapLessRecovery",     "Players have (30-50)% less Energy Shield Recharge Rate"),
        ("MapMod_MonsterElemRes",   "Suffix", "원소 방벽의", "MapMonsterElemRes", "+(25-40)% Monster Elemental Resistances"),
        ("MapMod_MonsterChaosRes",  "Suffix", "혼돈 방벽의", "MapMonsterChaosRes", "+(20-35)% Monster Chaos Resistance"),
        ("MapMod_Vulnerability",    "Suffix", "취약의",   "MapVulnerability",    "Players take (15-25)% increased Physical Damage"),
        ("MapMod_PackSize",         "Suffix", "밀집의",   "MapPackSize",         "+(15-30)% increased Monster Pack Size"),
    ]
    out: list[dict[str, Any]] = []
    for aid, mtype, name, group, line in specs:
        if aid in existing_ids:
            continue
        existing_ids.add(aid)
        out.append({
            "id": aid,
            "type": mtype,
            "affixName": name,
            "group": group,
            "level": 68,
            "tier": 1,
            "isLocal": False,
            "rollable": True,
            "lines": [parse_stat_line(line)],
            "weights": [["map", 1000]],
            "tags": ["map"],
        })
    return out


def compile_items(pob: Path, out_dir: Path) -> dict[str, Any]:
    source_sha256: dict[str, str] = {}
    affixes = parse_mod_explicit(pob, source_sha256)
    existing_ids = {a["id"] for a in affixes}
    jewel_affixes = parse_mod_jewel(pob, existing_ids, source_sha256)
    extra_affixes, belt_defs, uniques = build_stage5_implicits_and_uniques(pob, existing_ids, source_sha256)
    map_affixes = build_stage6_map_affixes(existing_ids)
    all_affixes = affixes + jewel_affixes + extra_affixes + map_affixes

    armor_curves = parse_armor_curves(pob, source_sha256)
    weapon_curves = parse_weapon_curves(pob, source_sha256)

    rollable_count = sum(1 for a in all_affixes if a["rollable"])
    local_count = sum(1 for a in all_affixes if a["isLocal"])

    pack = {
        "meta": {
            "schema": "skyrimpoe.items",
            "schemaVersion": 2,
            "game": "poe1",
            "gameVersion": "3.30",
            "pobVersion": "2.68.1",
            "contentGeneration": 0,
            "extractorVersion": "compile_items/1",
            "sourceSha256": dict(sorted(source_sha256.items())),
            "poeVersion": "3.30",
            "totalAffixes": len(all_affixes),
            "rollableAffixes": rollable_count,
            "jewelAffixes": len(jewel_affixes),
            "uniqueCount": len(uniques),
            "localAffixes": local_count,
        },
        "tiers": TIERS,
        "currencies": CURRENCIES,
        "beltBases": belt_defs,
        "charms": CHARMS,
        "monsterMods": MONSTER_MODS,
        "uniques": uniques,
        "baseArmorCurves": armor_curves,
        "baseWeaponCurves": weapon_curves,
        "affixes": all_affixes,
    }

    packs_dir = out_dir / "packs"
    packs_dir.mkdir(parents=True, exist_ok=True)
    out_path = packs_dir / "items_3_30.json"
    out_path.write_text(json.dumps(pack, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8", newline="\n")
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
