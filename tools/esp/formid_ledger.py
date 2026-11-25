r"""SkyrimPoE.esp의 FormID 장부(esp/formid-ledger.json)를 만들고 ESP·records.json과 대조한다.

결정 192와 docs/7단계-P0-계약.md 5장의 규칙을 따른다.
- 장부가 ESP 폼의 기준이다. 폐기 폼도 지우지 않고 status="retired"로 남긴다.
- 대조는 장부의 모든 항목이 ESP에 같은 FormID·EditorID·레코드 타입으로 있고, ESP에 장부 밖 폼이 없으며,
  records.json의 EditorID·타입과도 같은지 본다. 배포 기준선의 번호·저장 키·폐기 상태를 보존한다.

사용:
  python tools/esp/formid_ledger.py check                # 장부와 ESP·records.json 대조
  python tools/esp/formid_ledger.py create --out <경로>  # ESP의 기준선 폼에서 세대 0 첫 장부 재생성
  python tools/esp/formid_ledger.py freeze --generation N  # 현재 장부의 배포 기준선을 고정
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ESP = ROOT / "esp" / "SkyrimPoE.esp"
DEFAULT_RECORDS = ROOT / "esp" / "records.json"
DEFAULT_LEDGER = ROOT / "esp" / "formid-ledger.json"
DEFAULT_PUBLISHED = ROOT / "esp" / "formid-ledger.published"
DEFAULT_GEM_BUNDLE = ROOT / "data" / "poe2" / "gem-bundle.json"
GEN0_CORE = ROOT / "data" / "packs" / "core_3_30.json"

LEDGER_VERSION = 2
PLUGIN_NAME = "SkyrimPoE.esp"
ESL_FLAG = 0x200
DELETED_FLAG = 0x20
COMPRESSED_FLAG = 0x00040000
GENERATION_EDITOR_ID = "SPOE_ContentGeneration"

# 계약 5.1 규칙 4와 결정 231. 새 범위의 번호는 분류와 무관하게 배정한다.
LEGACY_CATEGORIES = (
    "proxy", "gem", "currency", "token", "belt", "jewel", "charm", "map", "tribute", "effect",
)
CATEGORIES = (*LEGACY_CATEGORIES, "equipmentBase", "waystone", "marker")
FORM_RANGES = {
    "legacy": (0x800, 0x8BF, LEGACY_CATEGORIES),
    "stage7": (0x8C0, 0xFFF, CATEGORIES),
}
FIRST_PLANS = ("keep", "keep-candidate", "retire-candidate", "undecided")
FORM_ID_PATTERN = re.compile(r"0x[0-9A-F]{6}")
GEM_ID_PATTERN = re.compile(r"(?:gem|sup)\.[A-Za-z0-9._]+")

# records.json의 record_type 이름 → ESP 레코드 시그니처
RECORD_TYPE_SIGNATURES = {
    "MagicEffect": "MGEF",
    "Spell": "SPEL",
    "MiscItem": "MISC",
    "Explosion": "EXPL",
    "Static": "STAT",
    "Projectile": "PROJ",
    "Global": "GLOB",
}

# EditorID 접두 → 분류(계약 5.2). 먼저 맞는 것을 쓴다.
CATEGORY_PREFIXES = [
    ("SPOE_ProxySpellEffect", "proxy"),
    ("SPOE_ProxyPowerEffect", "proxy"),
    ("SPOE_ProxySpell_", "proxy"),
    ("SPOE_ProxyPower_", "proxy"),
    ("SPOE_Gem_", "gem"),
    ("SPOE_Orb_", "currency"),
    ("SPOE_Tok_", "token"),
    ("SPOE_Belt_", "belt"),
    ("SPOE_Jewel_", "jewel"),
    ("SPOE_Charm_", "charm"),
    ("SPOE_Map_", "map"),
    ("SPOE_Tribute_", "tribute"),
    ("SPOE_Vfx_", "effect"),
    ("SPOE_Proj_", "effect"),
    ("SPOE_LootBeam_", "effect"),
]

# 계약 5.2의 첫 분류와 "정하는 곳"
FIRST_PLAN: dict[str, tuple[str, str]] = {
    # 젬: 대응 있음
    "SPOE_Gem_Cyclone": ("keep-candidate", "U-4"),
    "SPOE_Gem_IceNova": ("keep-candidate", "U-4"),
    "SPOE_Gem_Fireball": ("keep-candidate", "U-4"),
    "SPOE_Gem_Arc": ("keep-candidate", "U-4"),
    "SPOE_Gem_Spark": ("keep-candidate", "U-4"),
    "SPOE_Gem_HeraldOfIce": ("keep-candidate", "U-4"),
    "SPOE_Gem_HeraldOfAsh": ("keep-candidate", "U-4"),
    "SPOE_Gem_Discipline": ("undecided", "U-4"),
    # 젬: 규칙이 다름 / 보조 등급
    "SPOE_Gem_CastOnCrit": ("undecided", "U-6"),
    "SPOE_Gem_SupBrutality": ("undecided", "U-5"),
    "SPOE_Gem_SupChain": ("undecided", "U-5"),
    "SPOE_Gem_SupPierce": ("undecided", "U-5"),
    "SPOE_Gem_SupIncAoE": ("undecided", "U-5"),
    "SPOE_Gem_SupConcEffect": ("keep-candidate", "U-4, U-5"),
    "SPOE_Gem_SupGMP": ("undecided", "U-5"),
    "SPOE_Gem_SupFork": ("keep-candidate", "U-4, U-5"),
    "SPOE_Gem_SupSpellEcho": ("keep-candidate", "U-4, U-5"),
    # 젬: 대응 없음, 하나가 여럿으로
    "SPOE_Gem_ShockNova": ("retire-candidate", "U-4"),
    "SPOE_Gem_SupIncCrit": ("retire-candidate", "U-4"),
    "SPOE_Gem_SupMeleePhys": ("retire-candidate", "U-4"),
    "SPOE_Gem_FreezingPulse": ("retire-candidate", "U-4"),
    "SPOE_Gem_Determination": ("retire-candidate", "U-4"),
    "SPOE_Gem_Grace": ("retire-candidate", "U-4"),
    "SPOE_Gem_Hatred": ("retire-candidate", "U-4"),
    "SPOE_Gem_Wrath": ("retire-candidate", "U-4"),
    "SPOE_Gem_Anger": ("retire-candidate", "U-4"),
    "SPOE_Gem_MoltenShell": ("retire-candidate", "U-4"),
    "SPOE_Gem_PurityOfElements": ("retire-candidate", "U-4"),
    # 화폐
    "SPOE_Orb_Transmutation": ("keep-candidate", "U-7"),
    "SPOE_Orb_Augmentation": ("keep-candidate", "U-7"),
    "SPOE_Orb_Regal": ("keep-candidate", "U-7"),
    "SPOE_Orb_Alchemy": ("keep-candidate", "U-7"),
    "SPOE_Orb_Chaos": ("undecided", "U-7"),
    "SPOE_Orb_Exalted": ("keep-candidate", "U-7"),
    "SPOE_Orb_Divine": ("keep-candidate", "U-7"),
    "SPOE_Orb_Wisdom": ("undecided", "U-7, U-9"),
    "SPOE_Orb_Alteration": ("undecided", "U-7, U-9"),
    "SPOE_Orb_Scouring": ("undecided", "U-7, U-9"),
    "SPOE_Orb_Regret": ("undecided", "U-7, U-9"),
    "SPOE_Orb_Jeweller": ("undecided", "U-7, U-9"),
}

# 분류별 기본 첫 분류(FIRST_PLAN에 없는 항목)
CATEGORY_DEFAULT_PLAN = {
    "proxy": ("keep", ""),
    "effect": ("keep-candidate", "P4b"),
    "token": ("undecided", "U-3, P5"),
    "belt": ("undecided", "P5"),
    "jewel": ("undecided", "P5"),
    "charm": ("undecided", "U-15"),
    "map": ("undecided", "U-10"),
    "tribute": ("undecided", "U-10"),
}


@dataclass
class EspRecord:
    form_id: int
    local_id: int
    signature: str
    flags: int
    editor_id: str | None
    full_name: str | None
    global_type: str | None
    global_value: float | None


@dataclass
class Esp:
    flags: int
    masters: list[str]
    records: list[EspRecord]


def subrecords(body: bytes) -> Iterator[tuple[bytes, bytes]]:
    pos = 0
    extended_size = None
    while pos < len(body):
        if pos + 6 > len(body):
            raise ValueError("서브레코드 머리가 레코드 끝을 넘음")
        signature, size = struct.unpack_from("<4sH", body, pos)
        pos += 6
        if extended_size is not None:
            size = extended_size
            extended_size = None
        end = pos + size
        if end > len(body):
            raise ValueError(f"서브레코드 {signature!r}가 레코드 끝을 넘음")
        payload = body[pos:end]
        pos = end
        if signature == b"XXXX":
            if size != 4:
                raise ValueError("XXXX 서브레코드 크기 이상")
            extended_size, = struct.unpack("<I", payload)
        else:
            yield signature, payload
    if extended_size is not None:
        raise ValueError("XXXX 뒤에 서브레코드가 없음")


def read_esp(path: Path) -> Esp:
    """TES4 머리와 그룹의 모든 레코드를 중복까지 보존해 읽는다."""
    data = path.read_bytes()
    if len(data) < 24 or data[:4] != b"TES4":
        raise ValueError("TES4 머리가 없음")
    header_size, header_flags = struct.unpack_from("<II", data, 4)
    header_end = 24 + header_size
    if header_end > len(data):
        raise ValueError("TES4 레코드가 파일 끝을 넘음")
    if header_flags & COMPRESSED_FLAG:
        raise ValueError("압축 TES4 레코드는 지원하지 않음")
    masters = [
        value.rstrip(b"\x00").decode("utf-8")
        for signature, value in subrecords(data[24:header_end])
        if signature == b"MAST"
    ]
    records: list[EspRecord] = []

    def walk(pos: int, end: int) -> None:
        while pos < end:
            if pos + 24 > end:
                raise ValueError(f"레코드 머리가 구조 경계를 넘음(위치 {pos})")
            signature, size, flags, form_id = struct.unpack_from("<4sIII", data, pos)
            if signature == b"GRUP":
                if size < 24 or pos + size > end:
                    raise ValueError(f"그룹 크기 이상(위치 {pos})")
                walk(pos + 24, pos + size)
                pos += size
                continue
            record_end = pos + 24 + size
            if record_end > end:
                raise ValueError(f"레코드가 구조 경계를 넘음: {form_id:08X}")
            if signature == b"TES4":
                raise ValueError("TES4 머리 중복")
            if flags & COMPRESSED_FLAG:
                raise ValueError(f"압축 레코드 {form_id:08X}는 지원하지 않음")
            fields = {}
            for field, value in subrecords(data[pos + 24:record_end]):
                if field not in (b"EDID", b"FULL") and (
                    signature != b"GLOB" or field not in (b"FNAM", b"FLTV")
                ):
                    continue
                if field in fields:
                    raise ValueError(f"서브레코드 {field!r} 중복: {form_id:08X}")
                fields[field] = value
            global_type = fields.get(b"FNAM")
            global_value = fields.get(b"FLTV")
            if global_type is not None and len(global_type) != 1:
                raise ValueError(f"GLOB FNAM 크기 이상: {form_id:08X}")
            if global_value is not None and len(global_value) != 4:
                raise ValueError(f"GLOB FLTV 크기 이상: {form_id:08X}")
            editor_id = fields.get(b"EDID")
            full_name = fields.get(b"FULL")
            records.append(EspRecord(
                form_id=form_id,
                local_id=form_id & 0x00FFFFFF,
                signature=signature.decode("ascii"),
                flags=flags,
                editor_id=editor_id.rstrip(b"\x00").decode("utf-8") if editor_id is not None else None,
                full_name=full_name.rstrip(b"\x00").decode("utf-8") if full_name is not None else None,
                global_type=global_type.decode("ascii") if global_type is not None else None,
                global_value=struct.unpack("<f", global_value)[0] if global_value is not None else None,
            ))
            pos = record_end

    walk(header_end, len(data))
    return Esp(header_flags, masters, records)


def category_of(editor_id: str) -> str:
    for prefix, cat in CATEGORY_PREFIXES:
        if editor_id.startswith(prefix):
            return cat
    raise ValueError(f"분류를 정하지 못한 EditorID: {editor_id}")


def read_gem_ids(baseline: dict[int, dict[str, Any]]) -> dict[int, str]:
    """동결 기준선의 젬 키를 세대 0 팩의 gemId와 양방향으로 대조한다."""
    catalog = read_json(GEN0_CORE)["gems"]
    gem_ids = {}
    for form_id, entry in baseline.items():
        if entry.get("category") != "gem":
            continue
        key = entry.get("legacyKey")
        if not isinstance(key, str) or not key.startswith("gem:") or key[4:] not in catalog:
            raise ValueError(f"배포 기준선 gen0.json의 젬 legacyKey 오류: {entry['editorId']} {key!r}")
        gem_id = key[4:]
        if catalog[gem_id]["id"] != gem_id:
            raise ValueError(f"세대 0 젬 팩의 gemId 불일치: {gem_id}")
        gem_ids[form_id] = gem_id
    if len(gem_ids) != len(catalog) or set(gem_ids.values()) != catalog.keys():
        raise ValueError("배포 기준선과 세대 0 젬 팩의 gemId 왕복 불일치(누락 또는 중복)")
    return gem_ids


# 세이브 키와 ESP EditorID의 이름이 다른 레거시 젬.
GEM_EDITOR_SUFFIXES = {
    "SupportCastOnCriticalStrike": "CastOnCrit",
    "SupportIncreasedCriticalStrikes": "SupIncCrit",
    "SupportMeleePhysicalDamage": "SupMeleePhys",
    "SupportIncreasedAreaOfEffect": "SupIncAoE",
    "SupportConcentratedEffect": "SupConcEffect",
    "SupportGreaterMultipleProjectiles": "SupGMP",
}
TOKEN_SLOTS = (
    "OneHandWeapon", "TwoHandOrRangedWeapon", "Shield", "Helmet", "Body", "Gloves", "Boots", "Jewelry",
)
TOKEN_EDITOR_SUFFIXES = {"OneHandWeapon": "OneHand", "TwoHandOrRangedWeapon": "TwoHand"}


def legacy_gem_or_token(form_id: int, gem_ids: dict[int, str]) -> tuple[str, str, str] | None:
    """도입 당시의 (분류, EditorID, 세이브 키)를 로컬 ID에서 찾는다."""
    if form_id in gem_ids:
        gem_id = gem_ids[form_id]
        suffix = GEM_EDITOR_SUFFIXES.get(gem_id, gem_id.replace("Support", "Sup", 1))
        return "gem", f"SPOE_Gem_{suffix}", f"gem:{gem_id}"
    if form_id == 0x8AC:
        return "token", "SPOE_Tok_Unique", "token:Unique"
    if 0x82B <= form_id <= 0x87A:
        tier_index, offset = divmod(form_id - 0x82B, 16)
        rarity_index, slot_index = divmod(offset, 8)
        slot = TOKEN_SLOTS[slot_index]
    elif 0x895 <= form_id <= 0x89E:
        tier_index, rarity_index = divmod(form_id - 0x895, 2)
        slot = "Belt"
    else:
        return None
    tier = f"T{tier_index + 1}"
    rarity = ("Magic", "Rare")[rarity_index]
    slot_suffix = TOKEN_EDITOR_SUFFIXES.get(slot, slot)
    return "token", f"SPOE_Tok_{tier}_{rarity}_{slot_suffix}", f"token:{tier}:{rarity}:{slot}"


# FormIds.h의 시작 번호와 CurrencyKind·CharmKind의 저장 순서를 사용한다.
LEGACY_INDEXED_KEYS = {
    "currency": (0x81F, ("ScrollOfWisdom", "Transmutation", "Augmentation", "Alteration", "Regal",
                         "Alchemy", "Chaos", "Exalted", "Scouring", "Divine", "Regret", "Jeweller")),
    "belt": (0x890, tuple(range(5))),
    "jewel": (0x89F, ("Crimson", "Viridian", "Cobalt")),
    "charm": (0x8A2, ("Ruby", "Sapphire", "Topaz", "Amethyst", "Purifying", "Bastion")),
    "map": (0x8AE, tuple(f"T{tier}" for tier in range(1, 17))),
    "tribute": (0x8BE, ("Ascendancy4",)),
    "proxySpell": (0x802, tuple(range(1, 10))),
    "proxyPower": (0x80B, tuple(range(1, 10))),
}


def create_legacy_key(form_id: int, category: str, editor_id: str, gem_ids: dict[int, str]) -> str | None:
    identity = legacy_gem_or_token(form_id, gem_ids)
    if identity is not None:
        expected_category, expected_editor_id, legacy_key = identity
        if (category, editor_id) != (expected_category, expected_editor_id):
            raise ValueError(f"세대 0 EditorID 불일치: {editor_id} / {expected_editor_id}")
        return legacy_key
    if category == "proxy" and form_id not in (0x800, 0x801):
        category = "proxySpell" if form_id <= 0x80A else "proxyPower"
    if category in LEGACY_INDEXED_KEYS:
        start, keys = LEGACY_INDEXED_KEYS[category]
        if 0 <= form_id - start < len(keys):
            return f"{category}:{keys[form_id - start]}"
    if category == "effect" or (category == "proxy" and form_id in (0x800, 0x801)):
        return None
    raise ValueError(f"세대 0 legacyKey를 정할 수 없음: 0x{form_id:06X} {editor_id}")


def create_ledger(esp_path: Path) -> dict[str, Any]:
    esp = read_esp(esp_path)
    baseline = read_json(DEFAULT_PUBLISHED / "gen0.json")
    problems: list[str] = []
    baseline_entries = index_entries(baseline, "배포 기준선 gen0.json", problems)
    if problems:
        raise ValueError(problems[0])
    gem_ids = read_gem_ids(baseline_entries)
    if esp.masters != baseline.get("masters"):
        raise ValueError("ESP의 마스터 목록이 세대 0 기준선과 다름")

    entries = []
    seen = set()
    for rec in sorted(esp.records, key=lambda record: record.local_id):
        old = baseline_entries.get(rec.local_id)
        if old is None:
            continue
        if rec.local_id in seen:
            raise ValueError(f"ESP에 세대 0 FormID 중복: 0x{rec.local_id:06X}")
        seen.add(rec.local_id)
        if (rec.editor_id, rec.signature) != (old["editorId"], old["type"]):
            raise ValueError(f"ESP 폼이 세대 0 기준선과 다름: 0x{rec.local_id:06X} {rec.editor_id}")
        editor_id = old["editorId"]
        cat = category_of(editor_id)
        plan, decided_in = FIRST_PLAN.get(editor_id, CATEGORY_DEFAULT_PLAN.get(cat, ("undecided", "")))
        entries.append({
            "formId": f"0x{rec.local_id:06X}",
            "editorId": editor_id,
            "type": rec.signature,
            "category": cat,
            "contentId": None,
            "legacyKey": create_legacy_key(rec.local_id, cat, editor_id, gem_ids),
            "status": "active",
            "introducedGeneration": 0,
            "retiredGeneration": None,
            "firstPlan": plan,
            "decidedIn": decided_in,
        })
    if seen != baseline_entries.keys():
        raise ValueError("ESP에 세대 0 기준선 폼이 빠져 있음")
    return {**baseline, "entries": entries}


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ValueError(f"{path.name} 읽기 실패: {exc}") from exc


def index_entries(ledger: Any, label: str, problems: list[str]) -> dict[int, dict[str, Any]]:
    if not isinstance(ledger, dict) or not isinstance(ledger.get("entries"), list):
        problems.append(f"{label}의 entries 목록이 없음")
        return {}

    by_id = {}
    seen_edids = set()
    for index, entry in enumerate(ledger["entries"]):
        if not isinstance(entry, dict):
            problems.append(f"{label}의 entries[{index}]가 객체가 아님")
            continue
        form_id = entry.get("formId")
        editor_id = entry.get("editorId")
        signature = entry.get("type")
        if not isinstance(form_id, str) or not FORM_ID_PATTERN.fullmatch(form_id):
            problems.append(f"{label} FormID 형식 오류: {form_id!r}")
            continue
        if not isinstance(editor_id, str) or not editor_id:
            problems.append(f"{label} EditorID가 없음: {form_id}")
            continue
        if not isinstance(signature, str) or not re.fullmatch(r"[A-Z0-9_]{4}", signature):
            problems.append(f"{label} 타입 형식 오류: {editor_id} {signature!r}")
        if entry.get("status") not in ("active", "retired"):
            problems.append(f"{label} 상태 오류: {editor_id} {entry.get('status')!r}")
        if "legacyKey" not in entry or (
            entry["legacyKey"] is not None and not isinstance(entry["legacyKey"], str)
        ):
            problems.append(f"{label} legacyKey 형식 오류: {editor_id}")
        local_id = int(form_id, 16)
        if local_id in by_id:
            problems.append(f"{label} FormID 중복: {form_id}")
        if editor_id in seen_edids:
            problems.append(f"{label} EditorID 중복: {editor_id}")
        by_id.setdefault(local_id, entry)
        seen_edids.add(editor_id)
    return by_id


def check_ranges(ranges: Any, problems: list[str]) -> dict[str, str]:
    if not isinstance(ranges, dict) or ranges.keys() != FORM_RANGES.keys():
        problems.append("장부 범위 목록이 계약 표와 다름")
    statuses = {}
    for name, (start, end, _) in FORM_RANGES.items():
        row = ranges.get(name, {}) if isinstance(ranges, dict) else {}
        if not isinstance(row, dict):
            row = {}
        if (row.get("start"), row.get("end")) != (f"0x{start:06X}", f"0x{end:06X}"):
            problems.append(f"장부 범위 경계가 계약 표와 다름: {name}")
        status = row.get("status")
        if status != "approved":
            problems.append(f"장부 범위 상태 오류: {name} {status!r}")
        else:
            statuses[name] = status
    return statuses


def check_entry(
    form_id: int, entry: dict[str, Any], generation: int | None,
    range_statuses: dict[str, str], gen0_entries: dict[int, dict[str, Any]],
    gem_ids: dict[int, str], problems: list[str],
) -> None:
    editor_id = entry["editorId"]
    introduced = entry.get("introducedGeneration")
    retired = entry.get("retiredGeneration")
    stage7_start, stage7_end, _ = FORM_RANGES["stage7"]
    in_stage7 = stage7_start <= form_id <= stage7_end
    staged = (
        generation is not None and introduced == generation + 1
        and form_id not in gen0_entries and in_stage7
        and entry.get("status") == "active" and entry.get("legacyKey") is None
    )
    if (
        type(introduced) is not int or introduced < 0
        or (generation is not None and introduced > generation and not staged)
        or (introduced == 0 and in_stage7)
    ):
        problems.append(f"도입 세대 오류: {editor_id} {introduced!r}")
    if entry.get("status") == "active" and ("retiredGeneration" not in entry or retired is not None):
        problems.append(f"active 항목의 폐기 세대는 null이어야 함: {editor_id}")
    if entry.get("status") == "retired" and (
        type(retired) is not int or retired < 0
        or (type(introduced) is int and retired < introduced)
        or (generation is not None and retired > generation)
    ):
        problems.append(f"폐기 세대 오류: {editor_id} {retired!r}")
    if entry.get("firstPlan") not in FIRST_PLANS:
        problems.append(f"firstPlan 오류: {editor_id} {entry.get('firstPlan')!r}")
    if "contentId" not in entry or (
        entry["contentId"] is not None and not isinstance(entry["contentId"], str)
    ):
        problems.append(f"contentId 형식 오류: {editor_id}")
    if not isinstance(entry.get("decidedIn"), str):
        problems.append(f"decidedIn 형식 오류: {editor_id}")

    category = entry.get("category")
    if category not in CATEGORIES:
        problems.append(f"category 오류: {editor_id} {category!r}")
    for name, (start, end, categories) in FORM_RANGES.items():
        if start <= form_id <= end:
            if category not in categories:
                problems.append(f"분류에 맞지 않는 범위 {name}: {entry['formId']} {editor_id} {category!r}")
            if range_statuses.get(name) != "approved":
                problems.append(f"승인되지 않은 범위 {name}: {entry['formId']} {editor_id}")
            if name == "legacy":
                identity = legacy_gem_or_token(form_id, gem_ids)
                if identity is None and category in ("gem", "token"):
                    problems.append(f"legacyKey의 세대 0 젬·토큰 번호가 아님: {entry['formId']} {editor_id}")
                elif identity is not None and (category, editor_id, entry.get("legacyKey")) != identity:
                    problems.append(
                        f"legacyKey·EditorID 검산 불일치: {entry['formId']} {editor_id} "
                        f"{entry.get('legacyKey')!r}, 기대 {identity[1]} {identity[2]}"
                    )
            break
    else:
        problems.append(f"ESL 범위(0x800~0xFFF) 밖: {entry['formId']} {editor_id}")


def check_records_json(records: Any, entries: dict[int, dict[str, Any]], problems: list[str]) -> None:
    if not isinstance(records, list):
        problems.append("records.json이 레코드 목록이 아님")
        return
    seen_edids = set()
    rec_types: dict[str, str] = {}
    for index, record in enumerate(records):
        editor_id = record.get("editorid") if isinstance(record, dict) else None
        if not isinstance(editor_id, str) or not editor_id:
            problems.append(f"records.json의 [{index}] EditorID가 없음")
            continue
        if editor_id in seen_edids:
            problems.append(f"records.json에 EditorID 중복: {editor_id}")
        seen_edids.add(editor_id)
        record_type = record.get("record_type")
        signature = RECORD_TYPE_SIGNATURES.get(record_type) if isinstance(record_type, str) else None
        if signature is None:
            problems.append(f"records.json의 모르는 타입 {record_type!r}: {editor_id}")
            continue
        rec_types.setdefault(editor_id, signature)
    ledger_types = {entry["editorId"]: entry.get("type") for entry in entries.values()}
    for edid, sig in rec_types.items():
        if edid not in ledger_types:
            problems.append(f"records.json에만 있음: {edid}")
        elif ledger_types[edid] != sig:
            problems.append(f"records.json 타입 다름: {edid} {sig} / 장부 {ledger_types[edid]}")
    for edid in ledger_types:
        if edid not in rec_types:
            problems.append(f"장부에만 있음(records.json에 없음): {edid}")


def read_published(published_dir: Path, problems: list[str]) -> dict[str, dict[int, dict[str, Any]]]:
    paths = sorted(published_dir.glob("gen*.json"))
    if not paths:
        problems.append(f"배포 기준선 gen*.json이 없음: {published_dir}")
    baselines = {}
    for path in paths:
        label = f"배포 기준선 위반 ({path.name})"
        try:
            baseline = read_json(path)
        except ValueError as exc:
            problems.append(f"{label}: {exc}")
            continue
        baselines[path.name] = index_entries(baseline, label, problems)
    return baselines


def check_published(
    baselines: dict[str, dict[int, dict[str, Any]]], entries: dict[int, dict[str, Any]], problems: list[str],
) -> None:
    for name, baseline in baselines.items():
        label = f"배포 기준선 위반 ({name})"
        for form_id, old in baseline.items():
            current = entries.get(form_id)
            if current is None:
                problems.append(f"{label}: 항목 삭제 {old['formId']} {old['editorId']}")
                continue
            for field in ("editorId", "type", "legacyKey", "introducedGeneration"):
                if current.get(field) != old.get(field):
                    problems.append(
                        f"{label}: {old['formId']} {field} 변경 {old.get(field)!r} → {current.get(field)!r}"
                    )
            if old.get("status") == "retired" and current.get("status") == "active":
                problems.append(f"{label}: retired → active {old['formId']} {old['editorId']}")


def valid_gem_content_id(value: Any) -> bool:
    # 허용 문자가 ASCII이므로 문자 수와 바이트 수가 같다.
    return isinstance(value, str) and len(value) <= 64 and GEM_ID_PATTERN.fullmatch(value) is not None


def check_gem_content_ids(
    bundle: Any, entries: dict[int, dict[str, Any]], gen0_entries: dict[int, dict[str, Any]],
    problems: list[str],
) -> None:
    seen_ids = set()
    ledger_gems = {}
    for form_id, entry in entries.items():
        content_id = entry.get("contentId")
        if not isinstance(content_id, str):
            continue
        if content_id in seen_ids:
            problems.append(f"장부 contentId 중복: {content_id}")
        seen_ids.add(content_id)
        if entry.get("category") == "gem":
            if not valid_gem_content_id(content_id):
                problems.append(f"장부 젬 contentId 문법 오류: {entry['editorId']} {content_id!r}")
            ledger_gems[form_id] = (content_id, entry["editorId"])

    if not isinstance(bundle, list):
        problems.append("젬 묶음 파일이 행 목록이 아님")
        return
    if len(bundle) != 43:
        problems.append(f"젬 묶음 파일 행 수가 43과 다름: {len(bundle)}")
    seen_ids.clear()
    bundle_gems = {}
    for row in bundle:
        content_id = row.get("contentId") if isinstance(row, dict) else None
        if not valid_gem_content_id(content_id):
            problems.append(f"젬 묶음 contentId 문법 오류: {content_id!r}")
            continue
        if content_id in seen_ids:
            problems.append(f"젬 묶음 contentId 중복: {content_id}")
        seen_ids.add(content_id)
        form = row.get("form")
        local_id = form.get("formId") if isinstance(form, dict) else None
        if not isinstance(local_id, str) or not FORM_ID_PATTERN.fullmatch(local_id):
            problems.append(f"젬 묶음 FormID 형식 오류: {content_id} {local_id!r}")
            continue
        form_id = int(local_id, 16)
        if form_id in bundle_gems:
            problems.append(f"젬 묶음 FormID 중복: {local_id}")
        expected_plan = "keep" if form_id in gen0_entries else "new"
        if form.get("plan") != expected_plan:
            problems.append(f"젬 묶음 폼 계획 불일치: {content_id} {local_id}, 기대 {expected_plan}")
        bundle_gems[form_id] = (content_id, form.get("editorId"))

    for form_id, (content_id, editor_id) in bundle_gems.items():
        if ledger_gems.get(form_id) != (content_id, editor_id):
            problems.append(f"젬 묶음과 장부 contentId·EditorID 불일치: 0x{form_id:06X} {content_id}")
    for form_id in ledger_gems.keys() - bundle_gems.keys():
        problems.append(f"젬 묶음 밖 장부 contentId: 0x{form_id:06X} {ledger_gems[form_id][0]}")


def check(
    ledger_path: Path, esp_path: Path, records_path: Path, published_dir: Path = DEFAULT_PUBLISHED,
    gem_bundle_path: Path = DEFAULT_GEM_BUNDLE,
) -> list[str]:
    """어긋난 점의 목록을 돌려준다(비어 있으면 통과)."""
    try:
        ledger = read_json(ledger_path)
        esp = read_esp(esp_path)
        records = read_json(records_path)
        gem_bundle = read_json(gem_bundle_path)
    except (OSError, ValueError, struct.error) as exc:
        return [str(exc)]
    if not isinstance(ledger, dict):
        return ["장부가 JSON 객체가 아님"]

    problems: list[str] = []
    version = ledger.get("version")
    if type(version) is not int or version != LEDGER_VERSION:
        problems.append(f"장부 판이 {version!r}이고 {LEDGER_VERSION}이 아님")
    if ledger.get("plugin") != PLUGIN_NAME:
        problems.append(f"장부 plugin 다름: {ledger.get('plugin')!r}, 기대 {PLUGIN_NAME}")
    if ledger.get("masters") != esp.masters:
        problems.append(f"마스터 목록 다름: 장부 {ledger.get('masters')!r} / ESP {esp.masters}")
    if not isinstance(ledger.get("note"), str):
        problems.append("장부 note가 문자열이 아님")
    if not esp.flags & ESL_FLAG:
        problems.append("TES4 ESL 플래그가 꺼져 있음")
    generation = ledger.get("contentGeneration")
    if type(generation) is not int or generation < 0:
        problems.append(f"contentGeneration은 0 이상의 정수여야 함: {generation!r}")
        generation = None

    baselines = read_published(published_dir, problems)
    gen0_entries = baselines.get("gen0.json")
    if gen0_entries is None:
        return [*problems, "동결 세대 0 기준선 gen0.json이 없음"]
    try:
        gem_ids = read_gem_ids(gen0_entries)
    except (KeyError, TypeError, ValueError) as exc:
        return [*problems, f"구판 젬 키 검산 실패: {exc}"]

    range_statuses = check_ranges(ledger.get("ranges"), problems)
    entries = index_entries(ledger, "장부", problems)
    esp_by_id = {}
    esp_edids = set()
    generation_forms = []
    for record in esp.records:
        if record.local_id in esp_by_id:
            problems.append(f"ESP에 로컬 FormID 중복: 0x{record.local_id:06X}")
        esp_by_id.setdefault(record.local_id, record)
        if record.editor_id is not None:
            if record.editor_id in esp_edids:
                problems.append(f"ESP에 EditorID 중복: {record.editor_id}")
            esp_edids.add(record.editor_id)
        if record.form_id >> 24 != len(esp.masters):
            problems.append(
                f"ESP 소유 바이트 다름: 0x{record.form_id:08X} {record.editor_id} / 마스터 수 {len(esp.masters)}"
            )
        if record.local_id not in entries:
            problems.append(f"장부 밖 ESP 폼: 0x{record.local_id:06X} {record.editor_id}")
        elif record.flags & DELETED_FLAG:
            problems.append(f"Deleted 플래그가 켜짐: 0x{record.form_id:08X} {record.editor_id}")
        if record.editor_id == GENERATION_EDITOR_ID:
            generation_forms.append(record)

    for form_id, entry in entries.items():
        check_entry(form_id, entry, generation, range_statuses, gen0_entries, gem_ids, problems)
        record = esp_by_id.get(form_id)
        if record is None:
            problems.append(f"ESP에 없음: {entry['formId']} {entry['editorId']}")
            continue
        if record.editor_id != entry["editorId"]:
            problems.append(f"EditorID 다름: {entry['formId']} 장부 {entry['editorId']} / ESP {record.editor_id}")
        if record.signature != entry.get("type"):
            problems.append(f"타입 다름: {entry['formId']} 장부 {entry.get('type')} / ESP {record.signature}")
        if entry.get("status") == "retired" and not (record.full_name or "").startswith("(구판) "):
            problems.append(f'폐기 폼 FULL이 "(구판) "으로 시작하지 않음: {entry["editorId"]}')

    marker_ids = [
        form_id for form_id, entry in entries.items()
        if entry.get("category") == "marker" and entry["editorId"] == GENERATION_EDITOR_ID
    ]
    if generation == 0 and (generation_forms or marker_ids):
        problems.append(f"세대 0에 {GENERATION_EDITOR_ID} GLOB이 있음")
    elif generation is not None and generation > 0:
        if len(generation_forms) != 1:
            problems.append(f"{GENERATION_EDITOR_ID} GLOB이 정확히 하나 있어야 함")
        else:
            marker = generation_forms[0]
            if len(marker_ids) != 1:
                problems.append(f"장부에 {GENERATION_EDITOR_ID} marker 항목이 정확히 하나 있어야 함")
            elif marker.local_id != marker_ids[0]:
                problems.append(
                    f"{GENERATION_EDITOR_ID} FormID가 장부와 다름: "
                    f"ESP 0x{marker.local_id:06X} / 장부 0x{marker_ids[0]:06X}"
                )
            if marker.signature != "GLOB":
                problems.append(f"{GENERATION_EDITOR_ID}는 GLOB이어야 함")
            if marker.global_type != "s":
                problems.append(f"세대 GLOB FNAM 타입 다름: {marker.global_type!r}, 기대 's'")
            if marker.global_value != generation:
                problems.append(f"세대 GLOB FLTV 값 다름: {marker.global_value!r}, 기대 {generation}")

    check_records_json(records, entries, problems)
    check_published(baselines, entries, problems)
    check_gem_content_ids(gem_bundle, entries, gen0_entries, problems)
    return problems


def write_new_json(path: Path, value: Any) -> None:
    contents = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    with path.open("x", encoding="utf-8", newline="\n") as file:
        file.write(contents)


def main() -> int:
    for s in (sys.stdout, sys.stderr):
        if hasattr(s, "reconfigure"):
            s.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description="SkyrimPoE.esp FormID 장부")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    c.add_argument("--esp", type=Path, default=DEFAULT_ESP)
    c.add_argument("--records", type=Path, default=DEFAULT_RECORDS)
    c.add_argument("--published-dir", type=Path, default=DEFAULT_PUBLISHED)
    m = sub.add_parser("create")
    m.add_argument("--esp", type=Path, default=DEFAULT_ESP)
    m.add_argument("--out", type=Path, required=True)
    f = sub.add_parser("freeze")
    f.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    f.add_argument("--published-dir", type=Path, default=DEFAULT_PUBLISHED)
    f.add_argument("--generation", type=int, required=True)
    args = ap.parse_args()

    try:
        if args.cmd == "create":
            ledger = create_ledger(args.esp)
            write_new_json(args.out, ledger)
            print(f"만듦: {args.out} ({len(ledger['entries'])}폼)")
            return 0
        if args.cmd == "freeze":
            ledger = read_json(args.ledger)
            version = ledger.get("version") if isinstance(ledger, dict) else None
            if type(version) is not int or version != LEDGER_VERSION:
                raise ValueError(f"배포 기준선에는 판 {LEDGER_VERSION} 장부가 필요함")
            generation = ledger.get("contentGeneration")
            if type(generation) is not int or generation < 0 or generation != args.generation:
                raise ValueError(f"freeze 세대 {args.generation}와 장부 contentGeneration {generation!r} 불일치")
            args.published_dir.mkdir(parents=True, exist_ok=True)
            out = args.published_dir / f"gen{generation}.json"
            write_new_json(out, ledger)
            print(f"기준선 고정: {out}")
            return 0
    except FileExistsError as exc:
        print(f"이미 있음, 덮어쓰지 않음: {exc.filename}", file=sys.stderr)
        return 1
    except (OSError, ValueError, struct.error) as exc:
        print(f"실패: {exc}", file=sys.stderr)
        return 1

    problems = check(args.ledger, args.esp, args.records, args.published_dir)
    if problems:
        for problem in problems:
            print(f"불일치: {problem}")
        print(f"실패: {len(problems)}건")
        return 1
    print("통과: 장부·ESP·records.json·배포 기준선 일치")
    return 0


if __name__ == "__main__":
    sys.exit(main())
