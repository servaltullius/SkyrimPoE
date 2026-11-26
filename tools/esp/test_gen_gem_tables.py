import json
import pytest

from formid_ledger import DEFAULT_GEN0_LEDGER, DEFAULT_LEDGER
from gen_gem_tables import OUTPUT, render



def test_generated_gem_header_is_current():
    assert OUTPUT.read_bytes() == render()


def test_pre_g1_ledger_renders_the_same_gem_rows_without_the_marker():
    # G1 전 장부는 폐기 젬이 아직 active이고 세대 GLOB이 없다. 젬 행은 그대로, GLOB 줄만 다르다.
    before = render(DEFAULT_GEN0_LEDGER).decode("utf-8").splitlines()
    after = render().decode("utf-8").splitlines()
    differing = [(old, new) for old, new in zip(before, after) if old != new]
    assert len(before) == len(after)
    assert differing == [(
        "    inline constexpr std::optional<std::uint32_t> kGenerationGlobal = std::nullopt;",
        "    inline constexpr std::optional<std::uint32_t> kGenerationGlobal = 0x0008E2;",
    )]


@pytest.mark.parametrize("editor_id,status,retired", [
    ("SPOE_Gem_ShockNova", "active", None),   # 묶음에서 빠졌는데 폐기되지 않음
    ("SPOE_Gem_Spark", "retired", 1),         # 묶음에 있는데 폐기됨
])
def test_rejects_ledger_status_that_disagrees_with_the_bundle(tmp_path, editor_id, status, retired):
    ledger = json.loads(DEFAULT_LEDGER.read_text(encoding="utf-8"))
    entry = next(row for row in ledger["entries"] if row["editorId"] == editor_id)
    entry.update(status=status, retiredGeneration=retired)
    path = tmp_path / "ledger.json"
    path.write_text(json.dumps(ledger, ensure_ascii=False), encoding="utf-8")
    with pytest.raises(ValueError, match=f"장부 상태가 젬 묶음과 맞지 않음: {editor_id}"):
        render(path)
