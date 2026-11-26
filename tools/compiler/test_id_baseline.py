"""세대 1 의미 지문 기준선의 종류별 소유와 형식 시험(G1 계획 4.1 W1·W2·W5)."""
from copy import deepcopy
import json

import pytest

from id_baseline import BASELINE_PATH, GEM_KINDS, KIND_ORDER, TREE_KINDS, baseline_bytes, merge_owned


@pytest.fixture(scope="module")
def baseline():
    return json.loads(BASELINE_PATH.read_bytes())


def test_checked_in_baseline_has_every_kind_once_in_fixed_order(baseline):
    assert baseline["contentGeneration"] == 1
    assert tuple(baseline["entries"]) == KIND_ORDER
    assert baseline_bytes(baseline) == BASELINE_PATH.read_bytes()


def test_merge_replaces_only_owned_kinds_and_keeps_the_others_byte_identical(baseline):
    gems = {kind: deepcopy(baseline["entries"][kind]) for kind in GEM_KINDS}
    gems["gem"]["gem.NewSkill"] = ["New Skill", "0" * 64]
    merged = merge_owned(baseline, GEM_KINDS, gems)
    assert {kind: merged["entries"][kind] for kind in TREE_KINDS} == {
        kind: baseline["entries"][kind] for kind in TREE_KINDS
    }
    assert merged["entries"]["gem"]["gem.NewSkill"] == ["New Skill", "0" * 64]
    tree_only = {kind: baseline["entries"][kind] for kind in TREE_KINDS}
    assert merge_owned(merged, TREE_KINDS, tree_only)["entries"] == merged["entries"]


def test_both_owners_in_either_order_give_the_same_bytes(baseline):
    tree = {kind: baseline["entries"][kind] for kind in TREE_KINDS}
    gems = {kind: baseline["entries"][kind] for kind in GEM_KINDS}
    empty = {"contentGeneration": 1, "entries": {}}
    tree_first = merge_owned(merge_owned(empty, TREE_KINDS, tree), GEM_KINDS, gems)
    gems_first = merge_owned(merge_owned(empty, GEM_KINDS, gems), TREE_KINDS, tree)
    assert baseline_bytes(tree_first) == baseline_bytes(gems_first) == BASELINE_PATH.read_bytes()


@pytest.mark.parametrize("kind,identifier", [("gem", "gem.Spark"), ("sup", "sup.Fork")])
def test_merge_rejects_removed_or_changed_ids(baseline, kind, identifier):
    gems = {owned: deepcopy(baseline["entries"][owned]) for owned in GEM_KINDS}
    del gems[kind][identifier]
    with pytest.raises(ValueError, match="사라졌습니다"):
        merge_owned(baseline, GEM_KINDS, gems)
    gems = {owned: deepcopy(baseline["entries"][owned]) for owned in GEM_KINDS}
    gems[kind][identifier] = [gems[kind][identifier][0], "f" * 64]
    with pytest.raises(ValueError, match="뜻이 바뀌었습니다"):
        merge_owned(baseline, GEM_KINDS, gems)


def test_merge_rejects_kinds_outside_its_ownership(baseline):
    gems = {kind: baseline["entries"][kind] for kind in GEM_KINDS}
    with pytest.raises(ValueError, match="맡은 종류"):
        merge_owned(baseline, GEM_KINDS, {**gems, "node": {}})


def test_writer_rejects_unknown_kinds(baseline):
    changed = deepcopy(baseline)
    changed["entries"]["affix"] = {}
    with pytest.raises(ValueError, match="모르는 종류"):
        baseline_bytes(changed)
