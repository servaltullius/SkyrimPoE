"""FormID 장부와 ESP 바이트 대조 시험(7단계 P0 계약 5장)."""
from __future__ import annotations

import copy
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from collections import Counter
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import formid_ledger  # noqa: E402


HEADER_SIZE = 24
GENERATION_EDITOR_ID = "SPOE_ContentGeneration"
# 젬 묶음에 없는 세대 0 젬(G1에서 폐기). 0x816 IceNova는 G1에서 유지한다.
RETIRED_FORM_ID = 0x817


def rewrite_esp(data: bytes, edit_record: Callable[[bytes], bytes]) -> bytes:
    """파일을 순회하며 레코드와 그 레코드를 담은 모든 그룹의 크기를 다시 쓴다."""
    result = bytearray()
    pos = 0
    while pos < len(data):
        signature, size = struct.unpack_from("<4sI", data, pos)
        if signature == b"GRUP":
            end = pos + size
            body = rewrite_esp(data[pos + HEADER_SIZE:end], edit_record)
            group = bytearray(data[pos:pos + HEADER_SIZE])
            struct.pack_into("<I", group, 4, HEADER_SIZE + len(body))
            result.extend(group + body)
        else:
            end = pos + HEADER_SIZE + size
            result.extend(edit_record(data[pos:end]))
        pos = end
    return bytes(result)


def field(signature: bytes, payload: bytes) -> bytes:
    return struct.pack("<4sH", signature, len(payload)) + payload


def zstring(value: str) -> bytes:
    return value.encode("utf-8") + b"\x00"


def edit_field(record: bytes, signature: bytes, edit: Callable[[bytes], bytes]) -> bytes:
    pos = HEADER_SIZE
    while pos < len(record):
        name, size = struct.unpack_from("<4sH", record, pos)
        end = pos + 6 + size
        if name == signature:
            body = record[HEADER_SIZE:pos] + field(signature, edit(record[pos + 6:end])) + record[end:]
            return record[:4] + struct.pack("<I", len(body)) + record[8:HEADER_SIZE] + body
        pos = end
    raise AssertionError(f"테스트 레코드에 {signature!r} 필드가 없음")


def change_word(record: bytes, offset: int, value: int) -> bytes:
    return record[:offset] + struct.pack("<I", value) + record[offset + 4:]


def game_record(signature: bytes, local_id: int, body: bytes) -> bytes:
    return struct.pack("<4sIIIII", signature, len(body), 0, 0x01000000 | local_id, 0, 44) + body


def add_record(data: bytes, record: bytes, *, separate_group: bool = False) -> bytes:
    """레코드를 같은 종류의 그룹에 붙이거나 새 최상위 그룹을 만든다."""
    signature = record[:4]
    form_id = struct.unpack_from("<I", record, 12)[0] & 0x00FFFFFF
    header_end = HEADER_SIZE + struct.unpack_from("<I", data, 4)[0]
    pos = header_end
    while pos < len(data) and not separate_group:
        _, size, group_label = struct.unpack_from("<4sI4s", data, pos)
        if group_label == signature:
            group = change_word(data[pos:pos + size], 4, size + len(record))
            groups = data[header_end:pos] + group + record + data[pos + size:]
            count_added = 1
            break
        pos += size
    else:
        groups = data[header_end:] + struct.pack("<4sI4sIII", b"GRUP", HEADER_SIZE + len(record),
                                                 signature, 0, 0, 0) + record
        count_added = 2

    def update_hedr(payload: bytes) -> bytes:
        version, count, next_id = struct.unpack("<fII", payload)
        return struct.pack("<fII", version, count + count_added, max(next_id, form_id + 1))

    return edit_field(data[:header_end], b"HEDR", update_hedr) + groups


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


class TestFormIdLedger(unittest.TestCase):
    def setUp(self) -> None:
        cache = Path(__file__).resolve().parent / "__pycache__"
        cache.mkdir(exist_ok=True)
        temp_dir = tempfile.TemporaryDirectory(dir=cache)
        self.addCleanup(temp_dir.cleanup)
        self.temp_path = Path(temp_dir.name)
        self.ledger_path = self.temp_path / "formid-ledger.json"
        self.esp_path = self.temp_path / "SkyrimPoE.esp"
        self.records_path = self.temp_path / "records.json"
        # 기본 고정값은 G1 전 세대 0 상태다(결정 246으로 얼린 ESP와 그때의 장부·records.json).
        self.ledger = json.loads(formid_ledger.DEFAULT_GEN0_LEDGER.read_text(encoding="utf-8"))
        self.esp_bytes = formid_ledger.DEFAULT_GEN0_ESP.read_bytes()
        self.records = json.loads(formid_ledger.DEFAULT_GEN0_RECORDS.read_text(encoding="utf-8"))

    def entry(self, form_id: int) -> dict:
        return next(row for row in self.ledger["entries"] if row["formId"] == f"0x{form_id:06X}")

    def persist(self) -> None:
        write_json(self.ledger_path, self.ledger)
        self.esp_path.write_bytes(self.esp_bytes)
        write_json(self.records_path, self.records)

    def check_copy(self, published_dir: Path = formid_ledger.DEFAULT_PUBLISHED) -> list[str]:
        self.persist()
        # 고정값 자체가 얼린 세대 0 ESP라서, 얼린 ESP 대조는 그 시험에서만 따로 건다.
        return formid_ledger.check(self.ledger_path, self.esp_path, self.records_path, published_dir,
                                   gen0_dir=None)

    def assert_problem(self, expected: str) -> None:
        problems = self.check_copy()
        self.assertTrue(any(expected in problem for problem in problems), problems)

    def edit_esp_record(self, form_id: int, edit: Callable[[bytes], bytes]) -> None:
        def edit_target(record: bytes) -> bytes:
            record_id = struct.unpack_from("<I", record, 12)[0]
            return edit(record) if record_id == (0x01000000 | form_id) else record

        self.esp_bytes = rewrite_esp(self.esp_bytes, edit_target)

    def add_form(
        self, form_id: int, editor_id: str, signature: str, category: str, record_type: str,
        fields: bytes, *, content_id: str | None = None,
    ) -> None:
        body = field(b"EDID", zstring(editor_id)) + fields
        self.esp_bytes = add_record(self.esp_bytes, game_record(signature.encode("ascii"), form_id, body))
        self.records.append({"record_type": record_type, "editorid": editor_id, "ops": []})
        self.ledger["entries"].append({
            "formId": f"0x{form_id:06X}",
            "editorId": editor_id,
            "type": signature,
            "category": category,
            "contentId": content_id,
            "legacyKey": None,
            "status": "active",
            "introducedGeneration": self.ledger["contentGeneration"],
            "retiredGeneration": None,
            "firstPlan": "undecided",
            "decidedIn": "",
        })

    def add_generation_marker(
        self, value: float = 1, value_type: bytes = b"s", form_id: int = 0xC00,
    ) -> None:
        self.add_form(form_id, GENERATION_EDITOR_ID, "GLOB", "marker", "GlobalShort",
                      field(b"FNAM", value_type) + field(b"FLTV", struct.pack("<f", value)))
        spec = next(row for row in self.records if row["editorid"] == GENERATION_EDITOR_ID)
        spec["ops"] = [{"field_path": "Data", "value": f"{value:g}"}]

    def rename_form(self, form_id: int, name: str) -> None:
        self.edit_esp_record(form_id, lambda record: edit_field(record, b"FULL", lambda _: zstring(name)))
        editor_id = self.entry(form_id)["editorId"]
        spec = next(row for row in self.records if row["editorid"] == editor_id)
        next(op for op in spec["ops"] if op["field_path"] == "Name")["value"] = name

    def promote_to_generation_one(self) -> None:
        """G1 전 고정값을 세대 1 규칙에 맞게 올린다: 지도·공물 ID(결정 253), 유지 젬 이름(5.1)."""
        self.ledger["contentGeneration"] = 1
        for entry in self.ledger["entries"]:
            if entry["category"] in ("map", "tribute"):
                entry["contentId"] = entry["legacyKey"].replace(":", ".")
        self.rename_form(0x816, "스킬 젬: 얼음 폭발")
        self.rename_form(0x883, "보조 젬: 갈래")

    def retire_gem(self, *, update_name: bool = True) -> None:
        self.entry(RETIRED_FORM_ID).update(status="retired", retiredGeneration=1)
        if update_name:
            self.edit_esp_record(RETIRED_FORM_ID, lambda record: edit_field(
                record, b"FULL", lambda name: "(구판) ".encode("utf-8") + name,
            ))
            editor_id = self.entry(RETIRED_FORM_ID)["editorId"]
            record = next(row for row in self.records if row["editorid"] == editor_id)
            for op in record["ops"]:
                if op["field_path"] == "Name":
                    op["value"] = "(구판) " + op["value"]

    def add_new_currency(self) -> None:
        self.add_form(0x8E2, "SPOE_Orb_NewCurrency", "MISC", "currency", "MiscItem",
                      field(b"OBND", bytes(12)) + field(b"FULL", zstring("신규 화폐"))
                      + field(b"DATA", struct.pack("<If", 1, 0.0)), content_id="currency.NewCurrency")
        spec = next(row for row in self.records if row["editorid"] == "SPOE_Orb_NewCurrency")
        spec["ops"] = [{"field_path": "Name", "value": "신규 화폐"}]

    def run_cli(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-I", str(Path(formid_ledger.__file__).resolve()), *args],
            cwd=formid_ledger.ROOT, encoding="utf-8", capture_output=True, check=False,
        )

    def test_repository_ledger_matches_esp_records_and_published(self) -> None:
        self.assertEqual(formid_ledger.check(
            formid_ledger.DEFAULT_LEDGER, formid_ledger.DEFAULT_ESP, formid_ledger.DEFAULT_RECORDS,
        ), [])

    def test_read_esp_preserves_owner_and_utf8_full_after_edid(self) -> None:
        esp = formid_ledger.read_esp(formid_ledger.DEFAULT_ESP)
        self.assertEqual(esp.flags, 0x200)
        self.assertEqual(esp.masters, ["Skyrim.esm"])
        gem = next(record for record in esp.records if record.local_id == 0x816)
        self.assertEqual((gem.form_id, gem.flags, gem.signature, gem.editor_id),
                         (0x01000816, 0, "MISC", "SPOE_Gem_IceNova"))
        self.assertTrue(any(ord(char) > 127 for char in gem.full_name))

    def test_ledger_counts_by_category(self) -> None:
        counts = Counter(entry["category"] for entry in self.ledger["entries"])
        self.assertEqual(sum(counts.values()), 226)
        self.assertEqual(counts, {
            "gem": 62, "currency": 12, "token": 91, "map": 16, "proxy": 20, "effect": 10,
            "belt": 5, "jewel": 3, "charm": 6, "tribute": 1,
        })

    def test_generation_zero_plan_and_keys(self) -> None:
        counts = Counter(entry["firstPlan"] for entry in self.ledger["entries"]
                         if entry["introducedGeneration"] == 0)
        self.assertEqual(counts, {"keep": 20, "keep-candidate": 26, "retire-candidate": 11, "undecided": 135})
        examples = {
            0x800: (None, "keep", ""),
            0x804: ("proxySpell:3", "keep", ""),
            0x813: ("proxyPower:9", "keep", ""),
            0x815: ("gem:SupportCastOnCriticalStrike", "undecided", "U-6"),
            0x816: ("gem:IceNova", "keep-candidate", "U-4"),
            0x81C: ("gem:SupportConcentratedEffect", "keep-candidate", "U-4, U-5"),
            0x81D: (None, "keep-candidate", "P4b"),
            0x81F: ("currency:ScrollOfWisdom", "undecided", "U-7, U-9"),
            0x825: ("currency:Chaos", "undecided", "U-7"),
            0x82B: ("token:T1:Magic:OneHandWeapon", "undecided", "U-3, P5"),
            0x856: ("token:T3:Rare:Helmet", "undecided", "U-3, P5"),
            0x888: ("gem:Discipline", "undecided", "U-4"),
            0x88C: ("gem:PurityOfElements", "retire-candidate", "U-4"),
            0x892: ("belt:2", "undecided", "P5"),
            0x897: ("token:T2:Magic:Belt", "undecided", "U-3, P5"),
            0x8A1: ("jewel:Cobalt", "undecided", "P5"),
            0x8A4: ("charm:Topaz", "undecided", "U-15"),
            0x8AC: ("token:Unique", "undecided", "U-3, P5"),
            0x8B4: ("map:T7", "undecided", "U-10"),
            0x8BE: ("tribute:Ascendancy4", "undecided", "U-10"),
        }
        for form_id, expected in examples.items():
            with self.subTest(form_id=hex(form_id)):
                entry = self.entry(form_id)
                self.assertEqual((entry["legacyKey"], entry["firstPlan"], entry["decidedIn"]), expected)

    def test_create_reproduces_published_gen0_and_ignores_new_forms(self) -> None:
        result = self.run_cli("create", "--out", str(self.ledger_path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        created = self.ledger_path.read_bytes()
        self.assertEqual(created, (formid_ledger.DEFAULT_PUBLISHED / "gen0.json").read_bytes())
        self.assertEqual(len(json.loads(created)["entries"]), 192)
        # 226폼 + G1의 세대 GLOB(0x8E2). create는 세대 0 기준선 폼만 다시 만든다.
        self.assertEqual(len(formid_ledger.read_esp(formid_ledger.DEFAULT_ESP).records), 227)

    def test_swapping_two_form_ids_in_esp_and_ledger_breaks_published_baseline(self) -> None:
        first, second = 0x802, 0x803
        first_entry, second_entry = self.entry(first), self.entry(second)
        first_entry["formId"], second_entry["formId"] = second_entry["formId"], first_entry["formId"]
        swaps = {0x01000000 | first: 0x01000000 | second, 0x01000000 | second: 0x01000000 | first}

        def swap(record: bytes) -> bytes:
            form_id = struct.unpack_from("<I", record, 12)[0]
            return change_word(record, 12, swaps[form_id]) if form_id in swaps else record

        self.esp_bytes = rewrite_esp(self.esp_bytes, swap)
        problems = self.check_copy()
        self.assertEqual(len(problems), 4, problems)
        self.assertTrue(all("배포 기준선 위반 (gen0.json)" in problem for problem in problems), problems)

    def test_rejects_non_self_owned_record(self) -> None:
        self.edit_esp_record(0x802, lambda record: change_word(record, 12, 0x00000802))
        self.assert_problem("ESP 소유 바이트 다름: 0x00000802")

    def test_rejects_deleted_record(self) -> None:
        self.edit_esp_record(0x802, lambda record: change_word(record, 8, 0x20))
        self.assert_problem("Deleted 플래그가 켜짐")

    def test_rejects_compressed_record(self) -> None:
        self.edit_esp_record(0x802, lambda record: change_word(record, 8, 0x40000))
        self.assert_problem("압축 레코드 01000802는 지원하지 않음")

    def test_rejects_tes4_without_esl_flag(self) -> None:
        self.esp_bytes = change_word(self.esp_bytes, 8, 0)
        self.assert_problem("TES4 ESL 플래그가 꺼져 있음")

    def test_rejects_master_mismatch(self) -> None:
        self.ledger["masters"] = ["Other.esm"]
        self.assert_problem("마스터 목록 다름")

    def test_reports_esp_duplicates_even_across_top_level_groups(self) -> None:
        duplicates = []

        def capture_record(record: bytes) -> bytes:
            duplicates.append(record)
            return record

        self.edit_esp_record(0x802, capture_record)
        self.assertEqual(len(duplicates), 1)
        self.esp_bytes = add_record(self.esp_bytes, duplicates[0], separate_group=True)
        problems = self.check_copy()
        self.assertIn("ESP에 로컬 FormID 중복: 0x000802", problems)
        self.assertIn("ESP에 EditorID 중복: SPOE_ProxySpell_1", problems)
        self.assertEqual(len(formid_ledger.read_esp(self.esp_path).records), 227)

    def test_rejects_duplicate_records_json_editor_id(self) -> None:
        self.records.append(self.records[0].copy())
        self.assert_problem("records.json에 EditorID 중복")

    def test_rejects_tampered_editor_id(self) -> None:
        self.entry(0x800)["editorId"] = "SPOE_Wrong"
        self.assert_problem("EditorID 다름")

    def test_rejects_removing_a_published_entry(self) -> None:
        removed = self.entry(0x8BF)
        self.ledger["entries"].remove(removed)
        problems = self.check_copy()
        self.assertTrue(any("장부 밖 ESP 폼" in problem and removed["editorId"] in problem for problem in problems))
        self.assertTrue(any("배포 기준선 위반" in problem and "항목 삭제" in problem for problem in problems))

    def test_rejects_changing_legacy_keys(self) -> None:
        self.entry(0x816)["legacyKey"] = "gem:Spark"
        self.entry(0x856)["legacyKey"] = "token:T3:Magic:Helmet"
        problems = self.check_copy()
        self.assertTrue(any("legacyKey·EditorID 검산" in problem and "SPOE_Gem_IceNova" in problem
                            for problem in problems), problems)
        self.assertTrue(any("legacyKey·EditorID 검산" in problem and "SPOE_Tok_T3_Rare_Helmet" in problem
                            for problem in problems), problems)
        self.assertTrue(any("배포 기준선 위반" in problem and "legacyKey 변경" in problem for problem in problems))

    def test_legacy_gem_keys_round_trip_with_gen0_catalog(self) -> None:
        baseline = formid_ledger.read_json(formid_ledger.DEFAULT_PUBLISHED / "gen0.json")
        problems = []
        entries = formid_ledger.index_entries(baseline, "gen0", problems)
        self.assertEqual(problems, [])
        gem_ids = formid_ledger.read_gem_ids(entries)
        form_ids = {gem_id: form_id for form_id, gem_id in gem_ids.items()}
        catalog = formid_ledger.read_json(formid_ledger.GEN0_CORE)["gems"]
        self.assertEqual(len(gem_ids), 28)
        self.assertEqual(form_ids.keys(), catalog.keys())
        for gem_id, gem in catalog.items():
            with self.subTest(gem_id=gem_id):
                form_id = form_ids[gem["id"]]
                self.assertEqual(gem_ids[form_id], gem_id)
                self.assertEqual(self.entry(form_id)["legacyKey"], f"gem:{gem_id}")

    def test_accepts_34_staged_forms_at_generation_zero(self) -> None:
        self.assertEqual(self.ledger["contentGeneration"], 0)
        staged = [entry for entry in self.ledger["entries"] if entry["introducedGeneration"] == 1]
        self.assertEqual(len(staged), 34)
        self.assertEqual(self.check_copy(), [])

    def test_rejects_staged_form_present_in_gen0_baseline(self) -> None:
        self.add_new_currency()
        staged = self.entry(0x8E2)
        staged["introducedGeneration"] = 1
        baseline = formid_ledger.read_json(formid_ledger.DEFAULT_PUBLISHED / "gen0.json")
        baseline["entries"].append(staged.copy())
        baseline_dir = self.temp_path / "published"
        baseline_dir.mkdir()
        write_json(baseline_dir / "gen0.json", baseline)
        self.assertEqual(self.check_copy(baseline_dir), ["도입 세대 오류: SPOE_Orb_NewCurrency 1"])

    def test_rejects_staged_form_with_legacy_key(self) -> None:
        self.entry(0x8C0)["legacyKey"] = "gem:FrostBolt"
        self.assert_problem("도입 세대 오류: SPOE_Gem_FrostBolt 1")

    def test_rejects_staged_form_with_retired_status(self) -> None:
        self.entry(0x8C0).update(status="retired", retiredGeneration=1)
        self.assert_problem("도입 세대 오류: SPOE_Gem_FrostBolt 1")

    def test_rejects_introduced_generation_two_ahead(self) -> None:
        self.entry(0x8C0)["introducedGeneration"] = 2
        self.assert_problem("도입 세대 오류: SPOE_Gem_FrostBolt 2")

    def test_rejects_stage7_form_declared_as_generation_zero(self) -> None:
        self.entry(0x8C0)["introducedGeneration"] = 0
        self.assert_problem("도입 세대 오류: SPOE_Gem_FrostBolt 0")

    def test_rejects_changing_published_introduced_generation(self) -> None:
        self.promote_to_generation_one()
        self.add_generation_marker(form_id=0x8E2)
        self.entry(0x880)["introducedGeneration"] = 1
        self.assertEqual(self.check_copy(), [
            "배포 기준선 위반 (gen0.json): 0x000880 introducedGeneration 변경 0 → 1",
        ])

    def test_rejects_duplicate_content_id(self) -> None:
        self.entry(0x8C0)["contentId"] = "gem.Spark"
        self.assert_problem("장부 contentId 중복: gem.Spark")

    def test_rejects_multiple_content_ids_for_one_form(self) -> None:
        duplicate = {**self.entry(0x8C0), "contentId": "gem.Other"}
        self.ledger["entries"].append(duplicate)
        self.assert_problem("장부 FormID 중복: 0x0008C0")

    def test_rejects_missing_content_id(self) -> None:
        self.entry(0x8C0)["contentId"] = None
        self.assert_problem("젬 묶음과 장부 contentId·EditorID 불일치: 0x0008C0 gem.FrostBolt")

    def test_rejects_invalid_content_id_prefix_characters_or_length(self) -> None:
        for content_id in ("skill.FrostBolt", "gem.서리", "gem.Frost-Bolt", "gem." + "a" * 61):
            with self.subTest(content_id=content_id):
                self.entry(0x8C0)["contentId"] = content_id
                self.assert_problem("장부 젬 contentId 문법 오류: SPOE_Gem_FrostBolt")

    def test_rejects_content_id_on_unbundled_gem(self) -> None:
        self.entry(0x882)["contentId"] = "sup.Chain"
        self.assert_problem("젬 묶음 밖 장부 contentId: 0x000882 sup.Chain")

    def test_rejects_mismatched_gem_bundle_form_and_ledger(self) -> None:
        bundle = formid_ledger.read_json(formid_ledger.DEFAULT_GEM_BUNDLE)
        bundle[0]["form"]["editorId"] = "SPOE_Gem_Wrong"
        path = self.temp_path / "gem-bundle.json"
        write_json(path, bundle)
        self.persist()
        self.assertEqual(formid_ledger.check(
            self.ledger_path, self.esp_path, self.records_path, gem_bundle_path=path,
        ), ["젬 묶음과 장부 contentId·EditorID 불일치: 0x000880 gem.Spark"])

    def test_rejects_token_editor_id_inconsistent_with_decoded_slot(self) -> None:
        entry = self.entry(0x856)
        old_editor_id = entry["editorId"]
        new_editor_id = "SPOE_Tok_T3_Rare_Mismatched"
        entry["editorId"] = new_editor_id
        next(record for record in self.records if record["editorid"] == old_editor_id)["editorid"] = new_editor_id
        self.edit_esp_record(0x856, lambda record: edit_field(record, b"EDID", lambda _: zstring(new_editor_id)))
        self.assert_problem("legacyKey·EditorID 검산 불일치")

    def test_rejects_active_with_retired_generation(self) -> None:
        self.entry(0x800)["retiredGeneration"] = 0
        self.assert_problem("active 항목의 폐기 세대는 null")

    def test_rejects_retirement_without_generation(self) -> None:
        self.entry(0x800)["status"] = "retired"
        self.assert_problem("폐기 세대 오류")

    def test_rejects_negative_and_inconsistent_generations(self) -> None:
        self.ledger["contentGeneration"] = -1
        self.entry(0x800)["introducedGeneration"] = -1
        self.entry(0x801).update(status="retired", introducedGeneration=2, retiredGeneration=1)
        problems = self.check_copy()
        self.assertTrue(any("contentGeneration은 0 이상의 정수" in problem for problem in problems), problems)
        self.assertTrue(any("도입 세대 오류" in problem for problem in problems), problems)
        self.assertTrue(any("폐기 세대 오류" in problem for problem in problems), problems)

    def test_rejects_unknown_category_and_plan(self) -> None:
        self.entry(0x800).update(category="unknown", firstPlan="maybe")
        problems = self.check_copy()
        for expected in ("category 오류", "firstPlan 오류"):
            self.assertTrue(any(expected in problem for problem in problems), problems)

    def test_rejects_missing_published_baseline(self) -> None:
        baseline_dir = self.temp_path / "published"
        baseline_dir.mkdir()
        problems = self.check_copy(baseline_dir)
        self.assertTrue(any("배포 기준선 gen*.json이 없음" in problem for problem in problems), problems)

    def test_rejects_reactivating_form_retired_in_later_baseline(self) -> None:
        self.promote_to_generation_one()
        self.add_generation_marker()
        baseline_dir = self.temp_path / "published"
        baseline_dir.mkdir()
        gen0 = (formid_ledger.DEFAULT_PUBLISHED / "gen0.json").read_bytes()
        (baseline_dir / "gen0.json").write_bytes(gen0)
        gen1 = copy.deepcopy(self.ledger)
        next(entry for entry in gen1["entries"] if entry["formId"] == "0x000816").update(
            status="retired", retiredGeneration=1,
        )
        write_json(baseline_dir / "gen1.json", gen1)
        problems = self.check_copy(baseline_dir)
        self.assertEqual(problems, ["배포 기준선 위반 (gen1.json): retired → active 0x000816 SPOE_Gem_IceNova"])

    def test_rejects_altered_range_bounds(self) -> None:
        self.ledger["ranges"]["stage7"]["start"] = "0x0008C1"
        self.assert_problem("장부 범위 경계가 계약 표와 다름: stage7")

    def test_rejects_new_form_in_proposed_range(self) -> None:
        self.ledger["ranges"]["stage7"]["status"] = "proposed"
        self.assert_problem("승인되지 않은 범위 stage7: 0x0008C0")

    def test_rejects_stage7_category_in_legacy_range(self) -> None:
        self.entry(0x800)["category"] = "marker"
        self.assert_problem("분류에 맞지 않는 범위 legacy: 0x000800")

    def test_rejects_marker_at_generation_zero(self) -> None:
        self.add_generation_marker()
        self.assert_problem("세대 0에 SPOE_ContentGeneration GLOB이 있음")

    def test_rejects_missing_generation_marker(self) -> None:
        self.promote_to_generation_one()
        self.assert_problem("SPOE_ContentGeneration GLOB이 정확히 하나")

    def test_rejects_wrong_generation_marker_id(self) -> None:
        self.promote_to_generation_one()
        self.add_generation_marker(form_id=0x8E2)
        self.edit_esp_record(0x8E2, lambda record: change_word(record, 12, 0x010008E3))
        self.assert_problem("SPOE_ContentGeneration FormID가 장부와 다름: ESP 0x0008E3 / 장부 0x0008E2")

    def test_accepts_generation_marker_with_ledger_id_other_than_c00(self) -> None:
        self.promote_to_generation_one()
        self.add_generation_marker(form_id=0x8E2)
        self.assertEqual(self.check_copy(), [])

    def test_rejects_wrong_generation_global_value(self) -> None:
        self.promote_to_generation_one()
        self.add_generation_marker(value=2)
        self.assert_problem("세대 GLOB FLTV 값 다름")

    def test_rejects_wrong_generation_global_type(self) -> None:
        self.promote_to_generation_one()
        self.add_generation_marker(value_type=b"l")
        self.assert_problem("세대 GLOB FNAM 타입 다름")

    def test_rejects_retired_form_without_name_prefix(self) -> None:
        self.promote_to_generation_one()
        self.add_generation_marker()
        self.retire_gem(update_name=False)
        self.assert_problem('폐기 폼 FULL이 "(구판) "으로 시작하지 않음')

    def test_accepts_retirement_with_name_prefix_and_generation_marker(self) -> None:
        self.promote_to_generation_one()
        self.add_generation_marker()
        self.retire_gem()
        self.assertEqual(self.check_copy(), [])

    def test_accepts_new_form_in_approved_matching_range(self) -> None:
        self.promote_to_generation_one()
        self.add_generation_marker(form_id=0x8E3)
        self.add_new_currency()
        self.assertEqual(self.check_copy(), [])

    def test_create_refuses_to_overwrite(self) -> None:
        self.ledger_path.write_bytes(b"original")
        result = self.run_cli("create", "--out", str(self.ledger_path))
        self.assertEqual(result.returncode, 1)
        self.assertIn("이미 있음, 덮어쓰지 않음", result.stderr)
        self.assertEqual(self.ledger_path.read_bytes(), b"original")

    def test_freeze_creates_snapshot_and_refuses_to_overwrite(self) -> None:
        self.persist()
        baseline_dir = self.temp_path / "published"
        args = ("freeze", "--generation", "0", "--ledger", str(self.ledger_path),
                "--published-dir", str(baseline_dir))
        result = self.run_cli(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        snapshot = baseline_dir / "gen0.json"
        original = snapshot.read_bytes()
        self.assertEqual(json.loads(original), self.ledger)

        self.ledger["note"] = "수정된 장부"
        self.persist()
        result = self.run_cli(*args)
        self.assertEqual(result.returncode, 1)
        self.assertIn("이미 있음, 덮어쓰지 않음", result.stderr)
        self.assertEqual(snapshot.read_bytes(), original)

    def test_freeze_rejects_mismatched_generation(self) -> None:
        self.persist()
        result = self.run_cli("freeze", "--generation", "1", "--ledger", str(self.ledger_path),
                              "--published-dir", str(self.temp_path))
        self.assertEqual(result.returncode, 1)
        self.assertIn("contentGeneration 0 불일치", result.stderr)
        self.assertFalse((self.temp_path / "gen1.json").exists())

    def test_check_accepts_custom_paths(self) -> None:
        self.persist()
        result = self.run_cli("check", "--esp", str(self.esp_path), "--ledger", str(self.ledger_path),
                              "--records", str(self.records_path),
                              "--published-dir", str(formid_ledger.DEFAULT_PUBLISHED))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)



# G1(결정 204·231·246·253)의 세대 1 장부를 장부 밖에서 정한 기대표와 대조한다.
G1_RETIRED = {
    "SPOE_Gem_CastOnCrit", "SPOE_Gem_ShockNova", "SPOE_Gem_SupIncCrit", "SPOE_Gem_SupMeleePhys",
    "SPOE_Gem_SupBrutality", "SPOE_Gem_SupIncAoE", "SPOE_Gem_SupConcEffect", "SPOE_Gem_FreezingPulse",
    "SPOE_Gem_SupGMP", "SPOE_Gem_SupChain", "SPOE_Gem_SupPierce", "SPOE_Gem_Determination",
    "SPOE_Gem_Grace", "SPOE_Gem_Discipline", "SPOE_Gem_Hatred", "SPOE_Gem_Wrath", "SPOE_Gem_Anger",
    "SPOE_Gem_PurityOfElements", "SPOE_Gem_MoltenShell",
}
G1_KEPT = {
    "SPOE_Gem_Cyclone": ("gem.Cyclone", "스킬 젬: 사이클론"),
    "SPOE_Gem_IceNova": ("gem.IceNova", "스킬 젬: 얼음 폭발"),
    "SPOE_Gem_Fireball": ("gem.Fireball", "스킬 젬: 화염구"),
    "SPOE_Gem_Arc": ("gem.Arc", "스킬 젬: 연쇄 번개"),
    "SPOE_Gem_Spark": ("gem.Spark", "스킬 젬: 전기불꽃"),
    "SPOE_Gem_SupFork": ("sup.Fork", "보조 젬: 갈래"),
    "SPOE_Gem_SupSpellEcho": ("sup.SpellEcho", "보조 젬: 주문 메아리"),
    "SPOE_Gem_HeraldOfIce": ("gem.HeraldOfIce", "전령 젬: 얼음의 전령"),
    "SPOE_Gem_HeraldOfAsh": ("gem.HeraldOfAsh", "전령 젬: 재의 전령"),
}
G1_MAPS = {**{f"SPOE_Map_T{tier}": f"map.T{tier}" for tier in range(1, 17)},
           "SPOE_Tribute_Ascendancy4": "tribute.Ascendancy4"}
G1_MARKER_FORM_ID = 0x8E2


class TestGenerationOneLedger(unittest.TestCase):
    def setUp(self) -> None:
        self.ledger = json.loads(formid_ledger.DEFAULT_LEDGER.read_text(encoding="utf-8"))
        self.by_editor = {entry["editorId"]: entry for entry in self.ledger["entries"]}
        self.esp = {record.editor_id: record
                    for record in formid_ledger.read_esp(formid_ledger.DEFAULT_ESP).records}
        self.frozen = {record.editor_id: record
                       for record in formid_ledger.read_esp(formid_ledger.DEFAULT_GEN0_ESP).records}
        cache = Path(__file__).resolve().parent / "__pycache__"
        cache.mkdir(exist_ok=True)
        temp_dir = tempfile.TemporaryDirectory(dir=cache)
        self.addCleanup(temp_dir.cleanup)
        self.temp_path = Path(temp_dir.name)

    def test_repository_check_includes_frozen_generation_zero_esp(self) -> None:
        self.assertEqual(formid_ledger.check(
            formid_ledger.DEFAULT_LEDGER, formid_ledger.DEFAULT_ESP, formid_ledger.DEFAULT_RECORDS,
            gen0_dir=formid_ledger.DEFAULT_GEN0_DIR,
        ), [])

    def test_generation_and_marker(self) -> None:
        self.assertEqual(self.ledger["contentGeneration"], 1)
        marker = self.by_editor[GENERATION_EDITOR_ID]
        self.assertEqual(
            (marker["formId"], marker["type"], marker["category"], marker["status"], marker["introducedGeneration"]),
            (f"0x{G1_MARKER_FORM_ID:06X}", "GLOB", "marker", "active", 1))
        record = self.esp[GENERATION_EDITOR_ID]
        self.assertEqual((record.local_id, record.global_type, record.global_value), (G1_MARKER_FORM_ID, "s", 1.0))
        self.assertNotIn(GENERATION_EDITOR_ID, self.frozen)

    def test_gem_retire_keep_and_new_sets(self) -> None:
        gems = {editor: entry for editor, entry in self.by_editor.items() if entry["category"] == "gem"}
        self.assertEqual(len(gems), 62)
        retired = {editor for editor, entry in gems.items() if entry["status"] == "retired"}
        self.assertEqual(retired, G1_RETIRED)
        for editor in G1_RETIRED:
            with self.subTest(editor=editor):
                entry = gems[editor]
                self.assertEqual((entry["retiredGeneration"], entry["contentId"], entry["introducedGeneration"]),
                                 (1, None, 0))
                self.assertEqual(self.esp[editor].full_name, "(구판) " + self.frozen[editor].full_name)
        for editor, (content_id, name) in G1_KEPT.items():
            with self.subTest(editor=editor):
                entry = gems[editor]
                self.assertEqual((entry["status"], entry["contentId"], entry["introducedGeneration"]),
                                 ("active", content_id, 0))
                self.assertEqual(self.esp[editor].full_name, name)
        new = {editor: entry for editor, entry in gems.items() if entry["introducedGeneration"] == 1}
        self.assertEqual(len(new), 34)
        self.assertTrue(all(entry["status"] == "active" and entry["contentId"] and entry["legacyKey"] is None
                            for entry in new.values()))
        self.assertEqual(len(G1_RETIRED) + len(G1_KEPT) + len(new), len(gems))

    def test_maps_and_tribute_are_kept_with_content_ids(self) -> None:
        kept = {editor: entry for editor, entry in self.by_editor.items()
                if entry["category"] in ("map", "tribute")}
        self.assertEqual({editor: entry["contentId"] for editor, entry in kept.items()}, G1_MAPS)
        for editor, entry in kept.items():
            with self.subTest(editor=editor):
                # 첫 분류는 P0의 이력으로 남는다(P6a 4.1).
                self.assertEqual((entry["status"], entry["introducedGeneration"], entry["firstPlan"]),
                                 ("active", 0, "undecided"))
                self.assertEqual(entry["legacyKey"].replace(":", "."), entry["contentId"])

    def check_with(
        self, *, frozen_esp: bytes | None = None, frozen_ledger: dict | None = None, ledger: dict | None = None,
        records: list | None = None, esp: bytes | None = None,
    ) -> list[str]:
        gen0 = self.temp_path / "gen0"
        gen0.mkdir(exist_ok=True)
        (gen0 / "SkyrimPoE.esp").write_bytes(
            formid_ledger.DEFAULT_GEN0_ESP.read_bytes() if frozen_esp is None else frozen_esp)
        write_json(gen0 / "formid-ledger.json", frozen_ledger if frozen_ledger is not None
                   else json.loads(formid_ledger.DEFAULT_GEN0_LEDGER.read_text(encoding="utf-8")))
        (gen0 / "records.json").write_bytes(formid_ledger.DEFAULT_GEN0_RECORDS.read_bytes())
        ledger_path = formid_ledger.DEFAULT_LEDGER
        if ledger is not None:
            ledger_path = self.temp_path / "ledger.json"
            write_json(ledger_path, ledger)
        records_path = formid_ledger.DEFAULT_RECORDS
        if records is not None:
            records_path = self.temp_path / "records.json"
            write_json(records_path, records)
        esp_path = formid_ledger.DEFAULT_ESP
        if esp is not None:
            esp_path = self.temp_path / "SkyrimPoE.esp"
            esp_path.write_bytes(esp)
        return formid_ledger.check(ledger_path, esp_path, records_path, gen0_dir=gen0)

    def assert_reports(self, problems: list[str], expected: str) -> None:
        self.assertTrue(any(expected in problem for problem in problems), problems)

    def current_records(self) -> list:
        return json.loads(formid_ledger.DEFAULT_RECORDS.read_text(encoding="utf-8"))

    def test_frozen_copy_matches_its_pre_g1_ledger(self) -> None:
        self.assertEqual(self.check_with(), [])
        frozen = json.loads(formid_ledger.DEFAULT_GEN0_LEDGER.read_text(encoding="utf-8"))
        self.assertEqual((frozen["contentGeneration"], len(frozen["entries"])), (0, 226))

    def test_a_later_generation_one_form_does_not_break_the_frozen_check(self) -> None:
        # 세대 1에 폼을 더해도 얼린 ESP의 기대 집합은 G1 전 장부라서 그대로다(검토 지적 1).
        esp = add_record(formid_ledger.DEFAULT_ESP.read_bytes(), game_record(
            b"MISC", 0x8E3, field(b"EDID", zstring("SPOE_Orb_Later")) + field(b"OBND", bytes(12))
            + field(b"FULL", zstring("나중 화폐")) + field(b"DATA", struct.pack("<If", 1, 0.0))))
        ledger = copy.deepcopy(self.ledger)
        ledger["entries"].append({
            "formId": "0x0008E3", "editorId": "SPOE_Orb_Later", "type": "MISC", "category": "currency",
            "contentId": "currency.Later", "legacyKey": None, "status": "active", "introducedGeneration": 1,
            "retiredGeneration": None, "firstPlan": "keep", "decidedIn": "",
        })
        records = self.current_records()
        records.append({"record_type": "MiscItem", "editorid": "SPOE_Orb_Later",
                        "ops": [{"field_path": "Name", "value": "나중 화폐"}]})
        self.assertEqual(self.check_with(ledger=ledger, records=records, esp=esp), [])

    def test_rejects_frozen_esp_that_is_the_generation_one_esp(self) -> None:
        problems = self.check_with(frozen_esp=formid_ledger.DEFAULT_ESP.read_bytes())
        self.assert_reports(problems, "얼린 세대 0 ESP에 GLOB이 있음")
        self.assert_reports(problems, "얼린 세대 0 ESP에 폐기 이름 표시가 있음")
        self.assert_reports(problems, "얼린 세대 0 ESP에만 있음(G1 전 장부에 없음): 0x0008E2")

    def test_rejects_frozen_esp_with_renamed_retired_form(self) -> None:
        def rename(record: bytes) -> bytes:
            if struct.unpack_from("<I", record, 12)[0] != 0x01000815:
                return record
            return edit_field(record, b"FULL", lambda _: zstring("다른 이름"))
        problems = self.check_with(frozen_esp=rewrite_esp(formid_ledger.DEFAULT_GEN0_ESP.read_bytes(), rename))
        self.assert_reports(problems, "폐기 폼 이름이 현재 ESP 이름의 원래 이름과 다름")
        self.assert_reports(problems, "records.json 이름과 ESP FULL 다름: SPOE_Gem_CastOnCrit")

    def test_rejects_frozen_esp_with_a_changed_editor_id(self) -> None:
        def change(record: bytes) -> bytes:
            if struct.unpack_from("<I", record, 12)[0] != 0x01000880:
                return record
            return edit_field(record, b"EDID", lambda _: zstring("SPOE_Other"))
        problems = self.check_with(frozen_esp=rewrite_esp(formid_ledger.DEFAULT_GEN0_ESP.read_bytes(), change))
        self.assert_reports(problems, "얼린 세대 0 ESP의 EditorID·타입 다름: 0x000880")

    def test_rejects_frozen_ledger_that_the_current_ledger_does_not_preserve(self) -> None:
        frozen = json.loads(formid_ledger.DEFAULT_GEN0_LEDGER.read_text(encoding="utf-8"))
        next(entry for entry in frozen["entries"] if entry["formId"] == "0x000880")["legacyKey"] = "gem:Other"
        self.assert_reports(self.check_with(frozen_ledger=frozen), "지금 장부가 G1 전 폼을 보존하지 않음: 0x000880")
        frozen["contentGeneration"] = 1
        self.assert_reports(self.check_with(frozen_ledger=frozen), "얼린 세대 0 ESP의 장부는 세대 0 장부여야 함")

    def test_rejects_missing_frozen_files(self) -> None:
        problems = formid_ledger.check(formid_ledger.DEFAULT_LEDGER, formid_ledger.DEFAULT_ESP,
                                       formid_ledger.DEFAULT_RECORDS, gen0_dir=self.temp_path / "missing")
        self.assert_reports(problems, "얼린 세대 0 ESP·장부·records.json을 읽지 못함")

    def test_rejects_records_json_name_that_differs_or_is_missing(self) -> None:
        records = self.current_records()
        spark = next(row for row in records if row["editorid"] == "SPOE_Gem_Spark")
        next(op for op in spark["ops"] if op["field_path"] == "Name")["value"] = "스킬 젬: 다른 이름"
        problems = self.check_with(records=records)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("records.json 이름과 ESP FULL 다름: SPOE_Gem_Spark", problems[0])
        records = self.current_records()
        shock = next(row for row in records if row["editorid"] == "SPOE_Gem_ShockNova")
        shock["ops"] = [op for op in shock["ops"] if op["field_path"] != "Name"]
        self.assert_reports(self.check_with(records=records), "records.json에 Name이 없음(ESP에는 FULL이 있음): "
                                                                 "SPOE_Gem_ShockNova")

    def test_rejects_records_json_generation_global_that_differs_from_esp(self) -> None:
        for record_type, value, expected in (("Global", "1", "GLOB 타입이 ESP와 다름"),
                                             ("GlobalShort", "0", "GLOB 값이 ESP와 다름")):
            with self.subTest(record_type=record_type, value=value):
                records = self.current_records()
                marker = next(row for row in records if row["editorid"] == GENERATION_EDITOR_ID)
                marker["record_type"] = record_type
                marker["ops"] = [{"field_path": "Data", "value": value}]
                self.assert_reports(self.check_with(records=records), f"records.json {expected}: "
                                                                       f"{GENERATION_EDITOR_ID}")

    def test_rejects_map_content_id_that_is_not_the_legacy_key(self) -> None:
        for value in (None, "gem.T1", "tribute.T1"):
            with self.subTest(value=value):
                ledger = copy.deepcopy(self.ledger)
                next(entry for entry in ledger["entries"] if entry["editorId"] == "SPOE_Map_T1")["contentId"] = value
                self.assert_reports(self.check_with(ledger=ledger),
                                    "지도·공물 contentId가 세대 0 저장 키와 다름: SPOE_Map_T1")

    def test_rejects_gem_full_that_does_not_end_with_the_bundle_name(self) -> None:
        # 신규 34종을 포함한 43종 FULL이 젬 묶음의 nameKo로 끝나야 한다(검토 지적 4·5).
        def rename(record: bytes) -> bytes:
            if struct.unpack_from("<I", record, 12)[0] != 0x010008C0:
                return record
            return edit_field(record, b"FULL", lambda _: zstring("스킬 젬: 다른 젬"))
        records = self.current_records()
        frost = next(row for row in records if row["editorid"] == "SPOE_Gem_FrostBolt")
        next(op for op in frost["ops"] if op["field_path"] == "Name")["value"] = "스킬 젬: 다른 젬"
        problems = self.check_with(records=records, esp=rewrite_esp(formid_ledger.DEFAULT_ESP.read_bytes(), rename))
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("젬 FULL이 젬 묶음 이름으로 끝나지 않음: SPOE_Gem_FrostBolt", problems[0])

    def test_rejects_non_gem_content_id_grammar(self) -> None:
        ledger = copy.deepcopy(self.ledger)
        next(entry for entry in ledger["entries"] if entry["editorId"] == "SPOE_Map_T1")["contentId"] = "Map T1"
        self.assert_reports(self.check_with(ledger=ledger), "장부 contentId 문법 오류: SPOE_Map_T1")


if __name__ == "__main__":
    unittest.main()
