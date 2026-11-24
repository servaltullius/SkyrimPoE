"""Small raw packs with one known difference per regression case."""

import unittest

from test_extract import ScratchTest, read_json
from extract import json_bytes
from luadata import parse


def lua(source):
    return parse("return " + source)[0].value


def entry(key, source):
    return {"key": key, "value": lua(source)}


def tree(source):
    return lua("{ nodes = { [17] = " + source + " } }")


class ComparisonTests(ScratchTest):
    def folder(self, name, pack):
        directory = self.scratch / name
        directory.mkdir()
        for kind, value in pack.items():
            (directory / f"{kind}.json").write_bytes(json_bytes(value))
        return directory

    def compare(self, old, new):
        output = self.scratch / "diff.json"
        self.cli("diffraw.py", "--old", old, "--new", new, "--out", output)
        return read_json(output)["kinds"]

    def expect_one(self, file, before, after, kind, key, category, fields):
        old = self.folder("old", {file: before})
        new = self.folder("new", {file: after})
        report = self.compare(old, new)
        self.assertEqual(set(report), {kind})
        data = report[kind]
        self.assertEqual({name: n for name, n in data["counts"].items() if n}, {category: 1})
        self.assertEqual(data["items"], [
            {"key": key, "classification": category, "changedFields": fields},
        ])
        self.assertEqual(data["possibleRename"], [])

    def test_number_change(self):
        self.expect_one("gems", [entry("a", "{ cost = 1.5 }")], [entry("a", "{ cost = 2 }")],
                        "gems", "a", "numericChange", ["/cost"])

    def test_removed_entry(self):
        self.expect_one("essences", lua("{ a = {} }"), lua("{}"),
                        "essences", "a", "removed", [])

    def test_same_key_with_different_name_and_stats(self):
        self.expect_one("tree", tree('{ name = "Before", stats = { "Adds one" } }'),
                        tree('{ name = "After", stats = { "Adds two" } }'),
                        "tree", 17, "meaningChange", ["/name", "/stats/$array/1"])

    def test_different_connection_target(self):
        self.expect_one("tree", tree("{ connections = { [1] = { id = 1, orbit = 0 } } }"),
                        tree("{ connections = { [1] = { id = 2, orbit = 0 } } }"),
                        "tree", 17, "meaningChange", ["/connections/1"])

    def test_connection_order_and_duplicate_targets_do_not_change_meaning(self):
        self.expect_one("tree", tree("{ connections = { 1, 2 } }"), tree("{ connections = { 2, 1, 1 } }"),
                        "tree", 17, "unchanged", [])

    def test_absent_and_empty_connection_lists_are_the_same_set(self):
        self.expect_one("tree", tree("{ connections = {} }"), tree("{ }"),
                        "tree", 17, "unchanged", [])

    def test_weight_key_order_change(self):
        self.expect_one("mods", {"ModItem": lua('{ a = { weightKey = { "axe", "default" } } }')},
                        {"ModItem": lua('{ a = { weightKey = { "default", "axe" } } }')},
                        "ModItem", "a", "meaningChange", ["/weightKey/$array/1", "/weightKey/$array/2"])

    def test_additional_effect_reference_change(self):
        self.expect_one("gems", [entry("a", '{ additionalStatSet1 = "A" }')],
                        [entry("a", '{ additionalStatSet1 = "B" }')],
                        "gems", "a", "meaningChange", ["/additionalStatSet1"])

    def test_stat_order_numeric_change_is_meaning_change(self):
        self.expect_one("mods", {"ModRunes": lua("{ a = { statOrder = { 99 } } }")},
                        {"ModRunes": lua("{ a = { statOrder = { 100 } } }")},
                        "ModRunes", "a", "meaningChange", ["/statOrder/$array/1"])

    def test_numeric_range_order_change(self):
        self.expect_one("misc", [entry("a", "{ lower = 1, upper = 3 }")],
                        [entry("a", "{ lower = 3, upper = 1 }")],
                        "misc", "a", "meaningChange", ["/lower", "/upper"])

    def test_layout_only(self):
        self.expect_one("tree", tree("{ x = 1, orbitIndex = 1 }"), tree("{ x = 7, orbitIndex = 2 }"),
                        "tree", 17, "layoutChange", ["/x", "/orbitIndex"])

    def test_comparing_a_folder_to_itself(self):
        pack = self.folder("identical", {
            "gems": [entry("g", "{ a = 1 }")],
            "skills": {"entries": [entry("s", "{ a = function() end }")], "fileReturns": {}},
            "tree": tree("{ }"),
            "bases": [{**entry("b", "{ }"), "loaded": False}],
            "mods": {"ModItem": lua("{ m = {} }"), "ModRune": lua("{ r = {} }")},
            "uniques": [{"name": "U", "base": "C", "text": "\nU\nC\n"}],
            "misc": [entry("q", "{ }")],
            "essences": lua("{ e = {} }"),
        })
        groups = self.compare(pack, pack)
        self.assertEqual(set(groups), {
            "gems", "skills", "tree", "bases", "ModItem", "ModRune", "uniques", "misc", "essences",
        })
        for kind, group in groups.items():
            with self.subTest(kind=kind):
                self.assertEqual({name: n for name, n in group["counts"].items() if n}, {"unchanged": 1})
                self.assertEqual([item["changedFields"] for item in group["items"]], [[]])
                self.assertEqual(group["possibleRename"], [])

    def test_same_fingerprint_is_a_possible_rename(self):
        old = self.folder("old", {"gems": [entry("old", '{ name = "Same", damage = 10 }')]})
        new = self.folder("new", {"gems": [entry("new", '{ name = "Same", damage = 11 }')]})
        data = self.compare(old, new)["gems"]
        self.assertEqual({name: n for name, n in data["counts"].items() if n}, {"removed": 1, "added": 1})
        self.assertEqual(data["possibleRename"], [{"removed": {"key": "old"}, "added": {"key": "new"}}])

    def test_duplicate_removal_keeps_effective_declaration_aligned(self):
        older = entry("a", "{ value = 5 }")
        winner = entry("a", "{ value = 10 }")
        old = self.folder("old", {"gems": [older, winner]})
        new = self.folder("new", {"gems": [winner]})
        group = self.compare(old, new)["gems"]
        self.assertEqual(group["items"], [
            {"key": "a", "occurrenceFromEnd": 2, "classification": "removed", "changedFields": []},
            {"key": "a", "classification": "unchanged", "changedFields": []},
        ])


if __name__ == "__main__":
    unittest.main()
