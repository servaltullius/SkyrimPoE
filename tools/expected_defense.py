"""기대 방어 프로필(설계-현황 결정 69)의 통과 비율 m_e(L)을 PoB 데이터로 계산한다.

  python tools/expected_defense.py [--pob <PathOfBuilding 폴더>] [--increase 1.0 ...] [--levels 1 10 ...]

계산(Claude 안, 설계-현황 5장 40번의 (3)):
- 기대 타격 h_e(L) = 1.5 × monsterDamageTable(L). 1.5는 PoB가 적의 기본 피해로 쓰는 배수(ConfigOptions.lua).
- 원형 셋(방어도, 회피, 에너지 실드)마다, 레벨 L에 착용할 수 있는(요구 레벨 ≤ L) 그 유형의 순수 기본 장비 가운데
  기본값이 가장 큰 것을 몸통·투구·장갑·장화에서 하나씩 골라 더하고, × (1 + I)로 증가를 건다. 방패는 넣지 않는다.
- 방어도 원형의 통과 비율 = 1 − min(90%, A ÷ (A + 5 × h_e))  (PoB calcs.armourReductionF, 상한은 Misc.lua의 90)
- 회피 원형의 통과 비율 = 명중률 = clamp(round(명중 ÷ (명중 + (회피 ÷ 5)^0.9) × 125)%, 5%, 100%), 명중은 monsterAccuracyTable(L)
  (PoB calcs.hitChance와 같이 정수 %로 반올림. 회피 상한 95%는 명중률 하한 5%와 같음)
- 에너지 실드 원형의 통과 비율 = 1(물리 타격을 줄이지 않음. 에너지 실드는 풀로 따로 다룸)
- m_e(L) = 세 원형 통과 비율의 평균
원소·카오스(결정 74, 받는 마법용):
- 원소마다 기대 저항 = min(75%, n × 그 레벨에 붙을 수 있는 단일 원소 저항 어픽스 최고 등급의 평균값). n은 조절값(기본 3).
  등급은 PoB `ModExplicit.lua`의 FireResist1~8(요구 레벨과 값 범위). 막 감소(5막 -30%, 10막 -60%)는 플레이어가 메운다고 봄.
- 기대 카오스 저항 = 막 감소 그대로(투자하지 않음): 5막 레벨 전 0%, 10막 레벨 전 -30%, 그 뒤 -60%. 막의 레벨은 조절값(대략값).
- 통과 비율 = 1 − 저항(카오스는 음수 저항이면 1보다 큼).
이 스크립트는 PoB의 데이터 파일만 읽는다(PoB를 실행하지 않음).
"""
import argparse
import math
import re
import sys
from pathlib import Path

DEFAULT_POB = Path(r"G:\poe\PathOfBuilding-2.68.1")
RESIST_CAP = 75.0           # PoB Misc.lua의 base_maximum_all_resistances_%
ACT5_PENALTY = -30.0        # PoB ConfigOptions.lua의 resistancePenalty: Act 5 (-30%), Act 10 (-60%). 원소와 카오스에 모두(CalcSetup.lua)
ACT10_PENALTY = -60.0
SLOTS = ("body", "helmet", "gloves", "boots")
TYPES = {"Armour": "Armour", "Evasion": "Evasion", "Energy Shield": "EnergyShield"}
DAMAGE_MULT = 1.5
ARMOUR_CAP = 0.90
MIN_HIT = 0.05


def read_table(src: str, name: str) -> list[float]:
    m = re.search(r"^data\." + name + r" = \{([^}]*)\}", src, re.M)
    if not m:
        raise SystemExit(f"PoB의 Misc.lua에서 {name}을 찾지 못함")
    return [float(x) for x in m.group(1).split(",") if x.strip()]


def read_bases(pob: Path) -> dict[tuple[str, str], list[tuple[int, float, str]]]:
    """(부위, 유형) -> [(요구 레벨, 기본값 평균, 이름)]. 순수 유형(하위 유형이 Armour, Evasion, Energy Shield 하나)만."""
    bases: dict[tuple[str, str], list[tuple[int, float, str]]] = {}
    for slot in SLOTS:
        path = pob / "Data" / "Bases" / f"{slot}.lua"
        src = path.read_text(encoding="utf-8")
        for m in re.finditer(r'itemBases\["([^"]+)"\] = \{(.*?)\n\}', src, re.S):
            name, body = m.group(1), m.group(2)
            sub = re.search(r'subType = "([^"]+)"', body)
            tags = re.search(r"tags = \{([^}]*)\}", body)
            if not sub or sub.group(1) not in TYPES or (tags and ("not_for_sale" in tags.group(1) or "demigods" in tags.group(1))):
                continue
            key = TYPES[sub.group(1)]
            value = re.search(key + r"BaseMin = (\d+), " + key + r"BaseMax = (\d+)", body)
            if not value:
                continue
            level = re.search(r"level = (\d+)", body)
            bases.setdefault((slot, sub.group(1)), []).append((int(level.group(1)) if level else 1, (int(value.group(1)) + int(value.group(2))) / 2, name))
    return bases


def read_resist_tiers(pob: Path) -> list[tuple[int, float]]:
    """단일 원소 저항 어픽스의 (요구 레벨, 값 범위의 평균). 화염 저항의 등급(FireResist1~)을 쓴다(냉기·번개도 같은 값)."""
    src = (pob / "Data" / "ModExplicit.lua").read_text(encoding="utf-8")
    tiers = []
    for m in re.finditer(r'\["FireResist\d+"\] = \{ type = "Suffix", affix = "[^"]*", "\+\((\d+)-(\d+)\)% to Fire Resistance", statOrder = \{ \d+ \}, level = (\d+),', src):
        tiers.append((int(m.group(3)), (int(m.group(1)) + int(m.group(2))) / 2))
    if not tiers:
        raise SystemExit("PoB의 ModExplicit.lua에서 FireResist 등급을 찾지 못함")
    return sorted(tiers)


def expected_elemental(tiers: list[tuple[int, float]], level: int, affixes: float) -> float:
    best = max(value for req, value in tiers if req <= level)
    return min(RESIST_CAP, affixes * best)


def expected_chaos(level: int, act5_level: int, act10_level: int) -> float:
    if level >= act10_level:
        return ACT10_PENALTY
    if level >= act5_level:
        return ACT5_PENALTY
    return 0.0


def best_sum(bases, kind: str, level: int) -> float:
    total = 0.0
    for slot in SLOTS:
        options = [value for req, value, _ in bases.get((slot, kind), []) if req <= level]
        total += max(options) if options else 0.0
    return total


def hit_chance(evasion: float, accuracy: float) -> float:
    """PoB calcs.hitChance와 같게: 명중률(%)을 정수로 반올림한 뒤 5~100%로 자른다. 비율(0~1)로 돌려준다."""
    if accuracy < 0:
        return MIN_HIT
    raw = accuracy / (accuracy + (max(evasion, 0.0) / 5) ** 0.9) * 125
    percent = math.floor(raw + 0.5)  # PoB의 round(x) = floor(x + 0.5)
    return min(max(percent, MIN_HIT * 100), 100) / 100


def armour_pass(armour: float, hit: float) -> float:
    if armour <= 0:
        return 1.0
    return 1.0 - min(ARMOUR_CAP, armour / (armour + 5 * hit))


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description="기대 방어 프로필의 통과 비율 m_e(L)")
    ap.add_argument("--pob", type=Path, default=DEFAULT_POB)
    ap.add_argument("--increase", type=float, nargs="+", default=[0.5, 1.0, 2.0], help="장비 기본값에 거는 증가 I(1.0 = +100%%)")
    ap.add_argument("--levels", type=int, nargs="+", default=[1, 5, 10, 14, 20, 30, 45, 60, 68, 75, 84, 90])
    ap.add_argument("--resist-affixes", type=float, default=3.0, help="원소마다 기대하는 단일 저항 어픽스 수 n(결정 74, 기본 3)")
    ap.add_argument("--act5-level", type=int, default=45, help="5막을 마치는 대략의 레벨(카오스 저항 감소가 붙는 때)")
    ap.add_argument("--act10-level", type=int, default=67, help="10막을 마치는 대략의 레벨")
    a = ap.parse_args()
    misc = (a.pob / "Data" / "Misc.lua").read_text(encoding="utf-8")
    damage = read_table(misc, "monsterDamageTable")
    accuracy = read_table(misc, "monsterAccuracyTable")
    top = min(len(damage), len(accuracy))
    outside = [level for level in a.levels if not 1 <= level <= top]
    if outside:
        print(f"레벨은 1~{top}이어야 합니다(PoB의 몬스터 표 길이): {outside}", file=sys.stderr)
        return 1
    if a.resist_affixes < 0 or not 1 <= a.act5_level <= a.act10_level:
        print("--resist-affixes는 0 이상, --act5-level은 1 이상이고 --act10-level 이하여야 합니다", file=sys.stderr)
        return 1
    bases = read_bases(a.pob)
    tiers = read_resist_tiers(a.pob)
    print(f"PoB: {a.pob}")
    print("레벨 | 기대 타격 h_e | 명중 | 기본 방어도 | 기본 회피 | " + " | ".join(f"I={i:g}: 방어도 원형 / 회피 원형 / m_e" for i in a.increase))
    for level in a.levels:
        h = DAMAGE_MULT * damage[level - 1]
        acc = accuracy[level - 1]
        armour = best_sum(bases, "Armour", level)
        evasion = best_sum(bases, "Evasion", level)
        cells = []
        for inc in a.increase:
            pa = armour_pass(armour * (1 + inc), h)
            pe = hit_chance(evasion * (1 + inc), acc)
            cells.append(f"{pa:.2f} / {pe:.2f} / {(pa + pe + 1.0) / 3:.3f}")
        print(f"{level} | {h:.1f} | {acc:.0f} | {armour:.0f} | {evasion:.0f} | " + " | ".join(cells))
    print()
    print(f"원소·카오스(결정 74): 단일 저항 어픽스 {a.resist_affixes:g}개, 5막 레벨 {a.act5_level}, 10막 레벨 {a.act10_level}")
    print("레벨 | 최고 등급 평균 | 기대 원소 저항 | 원소 통과 비율 | 기대 카오스 저항 | 카오스 통과 비율")
    for level in a.levels:
        best = max(value for req, value in tiers if req <= level)
        res = expected_elemental(tiers, level, a.resist_affixes)
        chaos = expected_chaos(level, a.act5_level, a.act10_level)
        print(f"{level} | {best:.1f} | {res:.1f}% | {1 - res / 100:.3f} | {chaos:.0f}% | {1 - chaos / 100:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
