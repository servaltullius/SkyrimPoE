import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "package" / "stamp.py"
PLUGIN = Path("SKSE/Plugins/SkyrimPoE")
PACKS = PLUGIN / "packs"
UI = Path("PrismaUI/views/SkyrimPoE")
MANIFEST = PLUGIN / "manifest.json"
LEGACY_PREP_GEMS = PACKS / "gen1/gems.json"
ESP = "SkyrimPoE.esp"
PACK_NAMES = ("core.json", "tree.json", "items.json", "rules.json")


def field(signature: bytes, payload: bytes) -> bytes:
    return struct.pack("<4sH", signature, len(payload)) + payload


def add_generation_marker(path: Path, value: float, value_type: bytes = b"s") -> None:
    # As in test_formid_ledger.py, use a real GLOB record in a top-level group.
    data = bytearray(path.read_bytes())
    hedr = 24
    assert data[hedr:hedr + 4] == b"HEDR"
    form_id = 0xC00
    count_offset = hedr + 6 + 4
    count, next_id = struct.unpack_from("<II", data, count_offset)
    struct.pack_into("<II", data, count_offset, count + 2, max(next_id, form_id + 1))
    body = (
        field(b"EDID", b"SPOE_ContentGeneration\0")
        + field(b"FNAM", value_type)
        + field(b"FLTV", struct.pack("<f", value))
    )
    record = struct.pack("<4sIIIII", b"GLOB", len(body), 0, 0x01000000 | form_id, 0, 44) + body
    group = struct.pack("<4sI4sIII", b"GRUP", 24 + len(record), b"GLOB", 0, 0, 0)
    path.write_bytes(data + group + record)


def read_json(path: Path) -> dict:
    return json.loads(path.read_bytes())


class TestStamp(unittest.TestCase):
    def setUp(self) -> None:
        scratch = ROOT / "build" / "package-tests"
        scratch.mkdir(parents=True, exist_ok=True)
        directory = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(directory.cleanup)
        self.temp = Path(directory.name)
        self.package = self.temp / "패키지 산출물"
        (self.package / PACKS).mkdir(parents=True)
        (self.package / UI).mkdir(parents=True)
        # 고정값은 세대 0 패키지라 결정 246의 얼린 세대 0 ESP(GLOB 없음)를 쓴다.
        shutil.copyfile(ROOT / "esp" / "gen0" / ESP, self.package / ESP)
        (self.package / UI / "index.html").write_bytes(b"<main></main>\n")
        (self.package / UI / "style.css").write_bytes(b"body {}\n")
        oar = self.package / "meshes/animations/OAR/config.json"
        oar.parent.mkdir(parents=True)
        oar.write_bytes(b"{}\n")
        for name in PACK_NAMES:
            kind = Path(name).stem.split("_")[0]
            pack = {
                "meta": {"schema": f"skyrimpoe.{kind}"},
                "label": "한글 </script>",
            }
            if kind != "rules":
                pack["meta"].update(game="poe1", gameVersion="3.30", contentGeneration=0)
            options = {"indent": 2} if kind in ("core", "rules") else {"separators": (",", ":")}
            ending = "" if kind == "tree" else "\n"
            payload = json.dumps(pack, ensure_ascii=False, **options)
            (self.package / PACKS / name).write_bytes((payload + ending).encode("utf-8"))

    def run_cli(self, command: str, *args: str, package: Path | None = None):
        return subprocess.run(
            [sys.executable, "-I", "-B", str(SCRIPT), command,
             "--package", str(package or self.package), *args],
            capture_output=True, encoding="utf-8", check=False,
        )

    def assert_success(self, result: subprocess.CompletedProcess) -> None:
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_failure(self, result: subprocess.CompletedProcess, *messages: str) -> None:
        self.assertNotEqual(result.returncode, 0, result.stdout)
        for message in messages:
            self.assertIn(message, result.stderr)

    def stamp(self, package: Path | None = None):
        return self.run_cli("stamp", "--esp-name", ESP, package=package)

    def completed(self, package: Path | None = None) -> None:
        self.assert_success(self.stamp(package))
        for command in ("wrap-tree", "manifest", "verify"):
            self.assert_success(self.run_cli(command, package=package))

    def change_meta(self, name: str, key: str, value: object, *, package: Path | None = None) -> None:
        path = (package or self.package) / PACKS / name
        pack = read_json(path)
        pack["meta"][key] = value
        path.write_text(json.dumps(pack, ensure_ascii=False), encoding="utf-8")

    def deployed_copy(self) -> Path:
        self.completed()
        target = self.temp / "배치 대상"
        shutil.copytree(self.package, target)
        return target

    def test_release_package_has_no_legacy_prep_files(self) -> None:
        self.completed()
        self.assertFalse((self.package / LEGACY_PREP_GEMS.parent).exists())
        self.assertNotIn("staged", read_json(self.package / MANIFEST))
        self.assert_success(self.run_cli("verify", "--release"))

    def test_deploy_clean_removes_leftover_legacy_prep_gems(self) -> None:
        target = self.deployed_copy()
        (target / LEGACY_PREP_GEMS.parent).mkdir()
        (target / LEGACY_PREP_GEMS).write_bytes(b"{}\n")
        self.assert_success(self.run_cli("deploy-clean", "--target", str(target)))
        self.assertFalse((target / LEGACY_PREP_GEMS).exists())
        self.assert_success(self.run_cli("verify", "--release", package=target))

    def test_verify_rejects_unlisted_legacy_prep_gems(self) -> None:
        target = self.deployed_copy()
        (target / LEGACY_PREP_GEMS.parent).mkdir()
        (target / LEGACY_PREP_GEMS).write_bytes(b"{}\n")
        self.assert_failure(self.run_cli("verify", "--release", package=target), "옛 준비 파일", "목록 밖 파일")

    def test_verify_rejects_manifest_with_staged_field(self) -> None:
        target = self.deployed_copy()
        manifest_path = target / MANIFEST
        manifest = read_json(manifest_path)
        manifest["staged"] = []
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        self.assert_failure(self.run_cli("verify", package=target), "허용되지 않는 staged 항목")

    def test_manifest_rejects_nested_preparation_tree(self) -> None:
        self.completed()
        (self.package / LEGACY_PREP_GEMS.parent).mkdir()
        (self.package / LEGACY_PREP_GEMS.with_name("tree.json")).write_bytes(b"{}")
        self.assert_failure(self.run_cli("manifest"), "gen1/tree.json", "허용되지 않은 추가 팩")

    def test_fifth_json_pack_is_rejected(self) -> None:
        extra = self.package / PACKS / "old.json"
        extra.write_text("{}", encoding="utf-8")
        self.assert_failure(self.stamp(), "old.json", "허용되지 않은 추가 팩")
        extra.unlink()
        self.completed()
        extra.write_text("{}", encoding="utf-8")
        self.assert_failure(self.run_cli("verify"), "old.json", "허용되지 않은 추가 팩")

    def test_package_is_deterministic_and_manifest_covers_all_files(self) -> None:
        second = self.temp / "또 다른 패키지"
        shutil.copytree(self.package, second)
        pristine = {
            path.relative_to(self.package).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in self.package.rglob("*") if path.is_file()
        }
        payload = "".join(f"{path}\0{digest}\n" for path, digest in sorted(pristine.items()))
        expected_id = hashlib.sha256(payload.encode("utf-8")).hexdigest()[:16]

        self.completed(self.package)
        self.completed(second)
        manifest = read_json(self.package / MANIFEST)
        self.assertEqual(manifest["buildId"], expected_id)
        self.assertEqual(manifest["game"], "poe1")
        self.assertEqual(manifest["contentGeneration"], 0)
        self.assertEqual((self.package / MANIFEST).read_bytes(), (second / MANIFEST).read_bytes())
        for name in PACK_NAMES:
            path = self.package / PACKS / name
            self.assertEqual(path.read_bytes(), (second / PACKS / name).read_bytes())
            self.assertEqual(read_json(path)["meta"]["buildId"], expected_id)
        core, tree, items, rules = (self.package / PACKS / name for name in PACK_NAMES)
        rules_meta = read_json(rules)["meta"]
        self.assertEqual(rules_meta["game"], "poe1")
        self.assertEqual(rules_meta["gameVersion"], "3.30")
        self.assertEqual(rules_meta["contentGeneration"], 0)
        self.assertTrue(core.read_bytes().startswith(b'{\n  "meta":'))
        self.assertFalse(tree.read_bytes().endswith(b"\n"))
        self.assertTrue(items.read_bytes().startswith(b'{"meta":'))
        self.assertTrue(items.read_bytes().endswith(b"\n"))
        wrapper = self.package / UI / "tree-data.js"
        wrapped = wrapper.read_text(encoding="utf-8")
        self.assertIn(r"<\/script>", wrapped)
        self.assertEqual(json.loads(wrapped.removeprefix("window.POE_TREE = ").removesuffix(";\n")), read_json(tree))
        expected_paths = sorted([*pristine, (UI / "tree-data.js").as_posix()])
        self.assertEqual([entry["path"] for entry in manifest["files"]], expected_paths)
        for entry in manifest["files"]:
            data = (self.package / entry["path"]).read_bytes()
            self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())
            self.assertEqual(entry["size"], len(data))

    def test_build_id_changes_with_ui_bytes(self) -> None:
        second = self.temp / "변경한 패키지"
        shutil.copytree(self.package, second)
        (second / UI / "index.html").write_bytes(b"<main>changed</main>\n")
        self.assert_success(self.stamp())
        self.assert_success(self.stamp(second))
        name = PACKS / PACK_NAMES[0]
        self.assertNotEqual(
            read_json(self.package / name)["meta"]["buildId"],
            read_json(second / name)["meta"]["buildId"],
        )

    def test_stamp_rejects_mixed_generations(self) -> None:
        self.change_meta("items.json", "contentGeneration", 1)
        self.assert_failure(self.stamp(), "items.json", "contentGeneration 불일치")
        self.assertNotIn("buildId", read_json(self.package / PACKS / PACK_NAMES[0])["meta"])

    def test_stamp_rejects_mixed_games(self) -> None:
        self.change_meta("tree.json", "game", "poe2")
        self.assert_failure(self.stamp(), "tree.json", "game 불일치")

    def test_stamp_rejects_mismatched_esp_generation(self) -> None:
        add_generation_marker(self.package / ESP, 1)
        self.assert_failure(self.stamp(), ESP, "contentGeneration 불일치", "ESP 1, 팩 0")

    def test_stamp_rejects_non_integer_esp_generation(self) -> None:
        add_generation_marker(self.package / ESP, 0.5)
        self.assert_failure(self.stamp(), ESP, "FLTV")

    def test_verify_rejects_changed_file(self) -> None:
        self.completed()
        (self.package / UI / "style.css").write_bytes(b"body []\n")
        self.assert_failure(self.run_cli("verify"), "style.css: sha256 불일치")

    def test_verify_rejects_unlisted_file(self) -> None:
        self.completed()
        (self.package / "extra.txt").touch()
        self.assert_failure(self.run_cli("verify"), "목록 밖 파일: extra.txt")

    def test_verify_rejects_missing_file(self) -> None:
        self.completed()
        (self.package / UI / "index.html").unlink()
        self.assert_failure(self.run_cli("verify"), f"누락 파일: {(UI / 'index.html').as_posix()}")

    def test_verify_rejects_changed_pack_build_id(self) -> None:
        self.completed()
        self.change_meta("tree.json", "buildId", "0" * 16)
        self.assert_failure(self.run_cli("verify"), "tree.json: buildId 불일치")

    def test_rules_pack_is_required_for_stamp_and_verify(self) -> None:
        path = self.package / PACKS / "rules.json"
        pristine = path.read_bytes()
        path.unlink()
        self.assert_failure(self.stamp(), "rules.json", "필수 팩 파일")
        path.write_bytes(pristine)
        self.completed()
        path.unlink()
        self.assert_failure(self.run_cli("verify"), "rules.json", "필수 팩 파일")

    def test_rules_pack_version_is_checked_after_stamping(self) -> None:
        self.completed()
        self.change_meta("rules.json", "gameVersion", "0.5")
        self.assert_failure(self.run_cli("verify"), "rules.json", "gameVersion 불일치")

    def test_verify_reports_pack_identity_and_file_mismatches(self) -> None:
        self.completed()
        self.change_meta("tree.json", "buildId", "0" * 16)
        self.change_meta("items.json", "contentGeneration", 1)
        (self.package / "extra.txt").touch()
        self.assert_failure(
            self.run_cli("verify"), "tree.json: buildId 불일치",
            "items.json: contentGeneration 불일치", "tree.json: sha256 불일치",
            "목록 밖 파일: extra.txt",
        )

    def test_deploy_clean_removes_only_managed_unlisted_files(self) -> None:
        target = self.deployed_copy()
        old_pack = PACKS / "tree_3_29.json"
        old_ui = UI / "옛 도구.js"
        unmanaged = Path("SKSE/Plugins/SkyrimPoE_other/남길 파일.txt")
        (target / unmanaged).parent.mkdir(parents=True)
        for path in (old_pack, old_ui, unmanaged):
            (target / path).touch()
        result = self.run_cli("deploy-clean", "--target", str(target))
        self.assert_success(result)
        for path in (old_pack, old_ui):
            self.assertFalse((target / path).exists())
            self.assertIn(f"삭제: {path.as_posix()}", result.stdout)
        self.assertTrue((target / unmanaged).is_file())
        self.assertIn(f"남은 파일: {unmanaged.as_posix()}", result.stdout)

    def test_deploy_clean_without_manifest_leaves_files(self) -> None:
        target = self.deployed_copy()
        old_pack = target / PACKS / "tree_3_29.json"
        old_pack.touch()
        for manifest in (self.package / MANIFEST, target / MANIFEST):
            with self.subTest(manifest=manifest):
                payload = manifest.read_bytes()
                manifest.unlink()
                self.assert_failure(self.run_cli("deploy-clean", "--target", str(target)), "manifest.json")
                self.assertTrue(old_pack.is_file())
                manifest.write_bytes(payload)

    def test_deploy_clean_without_target_fails(self) -> None:
        self.completed()
        target = self.temp / "없는 대상"
        self.assert_failure(self.run_cli("deploy-clean", "--target", str(target)), "배치 대상 폴더")
        self.assertFalse(target.exists())

    def test_deploy_clean_verifies_target_hashes(self) -> None:
        target = self.deployed_copy()
        (target / UI / "style.css").write_bytes(b"body []\n")
        self.assert_failure(
            self.run_cli("deploy-clean", "--target", str(target)), "style.css: sha256 불일치",
        )


if __name__ == "__main__":
    unittest.main()
