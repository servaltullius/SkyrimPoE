import json
import unittest
from pathlib import Path

from compile_tree import (
    BASE_ASCENDANCY_SET,
    KIND_CLASS_START,
    KIND_KEYSTONE,
    KIND_MASTERY,
    SUPPORTED_KEYSTONE_NAMES,
    compile_tree,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
TREE_JSON = REPO_ROOT / "data" / "packs" / "tree_3_30.json"
DEFAULT_POB_DIR = Path(r"G:\poe\PathOfBuilding-2.68.1")
DEFAULT_KO_DIR = Path(r"G:\poe\POE1 한글 POB (2025.10.31)\Data\Translate\ko-KR")


class TestTreeCompiler(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        # T-7: PoB 원본이 있으면 compile_tree()를 메모리에서 직접 실행해 실제 컴파일러를 검증(저장소 파일 미오염)
        if DEFAULT_POB_DIR.is_dir():
            ko_dir = DEFAULT_KO_DIR if DEFAULT_KO_DIR.is_dir() else None
            cls.data = compile_tree(DEFAULT_POB_DIR, ko_dir, "3_30")
        elif TREE_JSON.is_file():
            cls.data = json.loads(TREE_JSON.read_text(encoding="utf-8"))
        else:
            raise unittest.SkipTest(f"Neither {DEFAULT_POB_DIR} nor {TREE_JSON} exists")
        cls.nodes_by_id = {n["id"]: n for n in cls.data["nodes"]}

    def test_classes_and_19_base_ascendancies(self) -> None:
        classes = self.data["classes"]
        self.assertEqual(len(classes), 7)
        asc_ids = []
        for cls in classes:
            self.assertGreater(cls["startNodeId"], 0)
            self.assertGreater(len(cls["startNeighbors"]), 0, f"Class {cls['name']} missing startNeighbors")
            for asc in cls["ascendancies"]:
                asc_ids.append(asc["id"])
                self.assertGreater(asc["startNodeId"], 0, f"Ascendancy {asc['id']} missing startNodeId")
        self.assertEqual(len(asc_ids), 19)
        self.assertEqual(set(asc_ids), BASE_ASCENDANCY_SET)
        # Verify Raider internal id -> Warden display name
        ranger = next(c for c in classes if c["name"] == "Ranger")
        raider = next(a for a in ranger["ascendancies"] if a["id"] == "Raider")
        self.assertEqual(raider["name"], "Warden")
        # Verify Scion has 6 start neighbors (in-only edges in PoB 3.30)
        scion = next(c for c in classes if c["name"] == "Scion")
        self.assertEqual(len(scion["startNeighbors"]), 6)

    def test_no_forbidden_or_league_nodes(self) -> None:
        for n in self.data["nodes"]:
            asc = n.get("asc", "")
            if asc:
                self.assertIn(asc, BASE_ASCENDANCY_SET)
            # No multiline \n remaining in normalized stats
            for s in n.get("stats", []):
                self.assertNotIn("\n", s)
        for eff in self.data["masteryEffects"].values():
            for s in eff.get("stats", []):
                self.assertNotIn("\n", s)

    def test_graph_symmetry_and_no_class_start_passthrough(self) -> None:
        class_start_ids = {c["startNodeId"] for c in self.data["classes"]}
        for n in self.data["nodes"]:
            nid = n["id"]
            if n["k"] in (KIND_CLASS_START, KIND_MASTERY):
                self.assertEqual(n["adj"], [], f"Node {nid} (kind {n['k']}) must have empty rule adj")
                continue
            for mid in n["adj"]:
                self.assertNotIn(mid, class_start_ids, "classStart must never appear inside rule adj")
                self.assertIn(mid, self.nodes_by_id)
                self.assertIn(nid, self.nodes_by_id[mid]["adj"], f"Asymmetric edge {nid} <-> {mid}")

    def test_multiple_choice_hubs_and_scion_mechanics(self) -> None:
        mc_hubs = [n for n in self.data["nodes"] if n.get("mc")]
        mc_opts = [n for n in self.data["nodes"] if n.get("mco")]
        self.assertEqual(len(mc_hubs), 7)
        self.assertEqual(len(mc_opts), 20)
        hub_ids = {n["id"] for n in mc_hubs}
        for opt in mc_opts:
            self.assertIn(opt.get("mcParent", 0), hub_ids)

        grant_starts = [n for n in self.data["nodes"] if "grantStart" in n]
        self.assertEqual(len(grant_starts), 6)
        grant_pts = [n for n in self.data["nodes"] if n.get("grantPts", 0) > 0]
        self.assertEqual(len(grant_pts), 18)

    def test_mastery_normalization_and_korean_coverage(self) -> None:
        self.assertEqual(len(self.data["masteryEffects"]), 353)
        self.assertGreater(self.data["meta"]["koNameHits"], 500)
        self.assertGreater(self.data["meta"]["statLinesKo"], 2000)
        # Every mastery node must belong to a group with >= 1 notable
        mg = self.data["masteryGroups"]
        for n in self.data["nodes"]:
            if n["k"] == KIND_MASTERY:
                gkey = str(n["g"])
                self.assertIn(gkey, mg)
                self.assertGreater(len(mg[gkey]), 0)
                for eid in n.get("effects", []):
                    self.assertIn(str(eid), self.data["masteryEffects"])

    def test_all_non_mastery_nodes_reachable(self) -> None:
        # Reachable from 7 class startNeighbors + 19 ascendancy start nodes
        seeds = []
        for c in self.data["classes"]:
            seeds.extend(c["startNeighbors"])
            for a in c["ascendancies"]:
                seeds.append(a["startNodeId"])
        visited = set(seeds)
        stack = list(seeds)
        while stack:
            cur = stack.pop()
            for nb in self.nodes_by_id[cur]["adj"]:
                if nb not in visited:
                    visited.add(nb)
                    stack.append(nb)
        for n in self.data["nodes"]:
            if n["k"] in (KIND_CLASS_START, KIND_MASTERY):
                continue
            self.assertIn(n["id"], visited, f"Unreachable non-mastery node {n['id']} ({n['name']})")

    def test_deferred_badge_marks_pure_minion_mine_totem_nodes(self) -> None:
        # Pure minion / mine / totem nodes must be marked deferred even when line mentions Life/Crit/Area
        for nid in (4247, 1722, 6685, 6233, 7136, 544, 18663, 41970):
            self.assertTrue(
                self.nodes_by_id[nid].get("deferred", False),
                f"Expected deferred=True on node {nid} ({self.nodes_by_id[nid]['name']})",
            )
        # Normal life/damage notables must NOT be deferred
        self.assertFalse(self.nodes_by_id[57279].get("deferred", False))  # Blood Magic
        self.assertFalse(self.nodes_by_id[3533].get("deferred", False))   # Life and Cost Efficiency
        # C-29: 미지원 키스톤은 모두 deferred=True여야 하며, 지원 키스톤 10종만 deferred=False
        keystones = [n for n in self.data["nodes"] if n["k"] == KIND_KEYSTONE]
        supported_ks = [n for n in keystones if not n.get("deferred", False)]
        self.assertEqual({n["name"] for n in supported_ks}, SUPPORTED_KEYSTONE_NAMES)


if __name__ == "__main__":
    unittest.main()
