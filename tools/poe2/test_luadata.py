"""Tests for the Lua data subset and its JSON encoding."""

from decimal import Decimal
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from luadata import LuaDataError, long_strings, parse


class LuaDataTests(unittest.TestCase):
    def test_table_keeps_arrays_keys_and_duplicates(self):
        value = parse('return { true; nil, name = false, [3] = "three"; '
                      '["3"] = "string", name = "again", "last", }')[0].value
        self.assertEqual(value, {"$table": {
            "array": [True, None, "last"],
            "fields": [["name", False], [3, "three"], ["3", "string"], ["name", "again"]],
        }})

    def test_local_and_assignment_forms(self):
        lines = parse("local skills, mod, flag, skill = ...\n"
                      'a = 4; data.level = 2\nskills["Title"] = { }')
        self.assertEqual([(line.kind, line.target, line.value, line.line) for line in lines], [
            ("local", ["skills", "mod", "flag", "skill"], [{"$name": "..."}], 1),
            ("assign", ["a"], 4, 2),
            ("assign", ["data", "level"], 2, 2),
            ("assign", ["skills", "Title"], {"$table": {"array": [], "fields": []}}, 3),
        ])

    def test_name_call_and_unary_minus(self):
        value = parse('return mod("Hit", "BASE", nil, bit.bor(Tag.X, Tag.Y), -scale.value, flag(), skill("Arc"))')[0].value
        self.assertEqual(value, {"$call": "mod", "args": [
            "Hit", "BASE", None, {"$call": "bit.bor", "args": [{"$name": "Tag.X"}, {"$name": "Tag.Y"}]},
            {"$neg": {"$name": "scale.value"}}, {"$call": "flag", "args": []}, {"$call": "skill", "args": ["Arc"]},
        ]})

    def test_numbers_preserve_non_integer_spelling(self):
        numbers = parse("return { -12, 0x1F, -0x10, 9.1599998474121, "
                        "-1.2300E-12, .5, 1., 0x1.fP-5, 1e+400 }")[0].value["$table"]["array"]
        self.assertEqual(numbers[:3], [-12, 31, -16])
        spellings = [row["$number"] for row in numbers[3:]]
        self.assertEqual(spellings, ["9.1599998474121", "-1.2300E-12", ".5", "1.", "0x1.fP-5", "1e+400"])
        self.assertEqual(Decimal(spellings[0]), Decimal("9.1599998474121"))
        self.assertEqual(float.fromhex(spellings[4]), 31 / 512)

    def test_numeric_keys_and_arithmetic(self):
        parsed = parse("return { [0.5] = 3/2, [0x20] = 1+2*3, [f(-2)] = -2^2 }")
        self.assertEqual(parsed[0].value["$table"]["fields"], [
            [0.5, {"$binary": "/", "args": [3, 2]}],
            [32, {"$binary": "+", "args": [1, {"$binary": "*", "args": [2, 3]}]}],
            [{"$call": "f", "args": [-2]}, {"$neg": {"$binary": "^", "args": [2, 2]}}],
        ])

    def test_quotes_and_escapes(self):
        source = r'''return { "a\"b\\c\n\t", 'it\'s', "\065\x42\195\169\u{1F600}", "x\z'''
        source += '\r\n \t y", "a\\\r\nb" }'
        self.assertEqual(parse(source)[0].value["$table"]["array"],
                         ['a"b\\c\n\t', "it's", "ABé😀", "xy", "a\nb"])

    def test_long_bracket_levels_comments_and_verbatim_contents(self):
        text = "-- [=[ignored]=]\r\n--[==[ comment ]] return ]==]\nreturn { [==[\r\na]=]b]]c\r\n]==], [[d]], }"
        self.assertEqual(parse(text)[0].value["$table"]["array"], ["a]=]b]]c\n", "d"])
        self.assertEqual(list(long_strings(text, "long.lua")), ["\r\na]=]b]]c\r\n", "d"])

    def test_opaque_function_with_nested_blocks(self):
        function = """function(a)
  if a then
    for x = 1, 2 do
      do
        local inner = function() return "function end" end
      end
    end
  elseif a == false then
    while a do break end
  else
    repeat a = true until a
    --[=[ function end ]=]
    return [==[end]==]
  end
end"""
        source = f"-- header\r\nreturn {{ handler = {function}, after = 42 }}"
        fields = parse(source)[0].value["$table"]["fields"]
        self.assertEqual(fields, [["handler", {"$function": function, "line": 2}], ["after", 42]])

    def test_unsupported_top_level_has_location(self):
        with self.assertRaisesRegex(LuaDataError, r"sample\.lua:2:3:"):
            parse("-- first line\n  do x = 1 end", "sample.lua")
        with self.assertRaisesRegex(LuaDataError, r"sample\.lua:2:1:.*after return"):
            parse("return { }\nx = 1", "sample.lua")

    def test_unterminated_data_has_location(self):
        for source, error in [("return [[hi", "long bracket"), ("return 'hi", "quoted string"),
                              ('return function() if true then end', "function")]:
            with self.subTest(error=error), self.assertRaisesRegex(LuaDataError, f"<input>:1:8:.*{error}"):
                parse(source)


if __name__ == "__main__":
    unittest.main()
