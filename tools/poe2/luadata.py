"""Read the data subset of Lua without evaluating any Lua code."""

from __future__ import annotations

from bisect import bisect_right
from dataclasses import dataclass
from decimal import Decimal
import re


class LuaDataError(ValueError):
    pass


_LONG_OPEN = re.compile(r"\[(=*)\[")
_NEWLINE = re.compile(r"\r\n|\n\r|\r|\n")
_TOKEN = re.compile(
    r"(?P<space>[ \t\f\v\r\n]+)"
    r"|(?P<comment>--)"
    r"""|(?P<string>"(?:[^"\\\r\n]|\\(?:z[ \t\f\v\r\n]*|\r\n|\n\r|[\s\S]))*"|'(?:[^'\\\r\n]|\\(?:z[ \t\f\v\r\n]*|\r\n|\n\r|[\s\S]))*')"""
    r"|(?P<number>0[xX](?:[0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)(?:[pP][+-]?\d+)?"
    r"|\d+(?:\.(?!\.)\d*)?(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?)"
    r"|(?P<name>[a-zA-Z_][a-zA-Z_0-9]*)"
    r"|(?P<symbol>\.\.\.|[\s\S])"
)
_ESCAPE = re.compile(r"\\(?:z[ \t\f\v\r\n]*|x[0-9a-fA-F]{2}|u\{[0-9a-fA-F]+\}|\d{1,3}|\r\n|\n\r|[\s\S])")
_ESCAPES = dict(zip("abfnrtv\\\"'", "\a\b\f\n\r\t\v\\\"'"))
_KEYWORDS = frozenset(
    "and break do else elseif end false for function goto if in local nil not or "
    "repeat return then true until while".split()
)
_BINARY_PRECEDENCE = {"+": 1, "-": 1, "*": 2, "/": 2, "%": 2, "^": 4}


@dataclass(slots=True)
class Token:
    kind: str
    text: str
    start: int
    end: int


@dataclass(slots=True)
class Statement:
    kind: str
    target: list
    value: object
    line: int


class Source:
    def __init__(self, text: str, filename: str):
        self.text = text
        self.filename = filename
        self.line_starts = [0, *(match.end() for match in _NEWLINE.finditer(text))]

    def line(self, offset: int) -> int:
        return bisect_right(self.line_starts, offset)

    def error(self, offset: int, message: str) -> LuaDataError:
        line = self.line(offset)
        column = offset - self.line_starts[line - 1] + 1
        return LuaDataError(f"{self.filename}:{line}:{column}: {message}")

    def tokens(self, start: int = 0):
        offset = start
        while offset < len(self.text):
            match = _TOKEN.match(self.text, offset)
            kind = match.lastgroup
            end = match.end()
            if kind == "comment" or match[0] == "[":
                opening = _LONG_OPEN.match(self.text, end if kind == "comment" else offset)
                if opening:
                    closing = "]" + opening[1] + "]"
                    close_at = self.text.find(closing, opening.end())
                    if close_at < 0:
                        raise self.error(offset, "unterminated long bracket")
                    end = close_at + len(closing)
                    if kind != "comment":
                        yield Token("long", self.text[opening.end():close_at], offset, end)
                elif kind == "comment":
                    newline = _NEWLINE.search(self.text, end)
                    end = newline.end() if newline else len(self.text)
                else:
                    yield Token(kind, match[0], offset, end)
            elif kind != "space":
                if match[0] in ('"', "'"):
                    raise self.error(offset, "unterminated quoted string")
                yield Token(kind, match[0], offset, end)
            offset = end
        yield Token("eof", "", offset, offset)


def long_strings(text: str, filename: str):
    """Yield verbatim contents of long literals, excluding comments and delimiters."""
    for token in Source(text, filename).tokens():
        if token.kind == "long":
            yield token.text


def short_string(source: Source, token: Token) -> str:
    spelling = token.text[1:-1]
    if "\\" not in spelling:
        return spelling
    result = bytearray()
    start = 0
    for match in _ESCAPE.finditer(spelling):
        result.extend(spelling[start:match.start()].encode("utf-8"))
        escaped = match[0][1:]
        if escaped in _ESCAPES:
            result.extend(_ESCAPES[escaped].encode("utf-8"))
        elif escaped[0] == "z":
            pass
        elif escaped[0] in "\r\n":
            result.append(10)
        elif escaped[0] == "x" and len(escaped) == 3:
            result.append(int(escaped[1:], 16))
        elif escaped.startswith("u{") and int(escaped[2:-1], 16) <= 0x10FFFF:
            result.extend(chr(int(escaped[2:-1], 16)).encode("utf-8", "surrogatepass"))
        elif escaped.isascii() and escaped.isdecimal() and int(escaped) <= 255:
            result.append(int(escaped))
        else:
            raise source.error(token.start + match.start() + 1, f"invalid escape {match[0]!r}")
        start = match.end()
    result.extend(spelling[start:].encode("utf-8"))
    return result.decode("utf-8", "surrogateescape")


def number(spelling: str):
    if spelling.lower().startswith("0x"):
        if "." not in spelling and "p" not in spelling.lower():
            return int(spelling, 16)
    elif spelling.isdecimal():
        return int(spelling)
    return {"$number": spelling}


def numeric_key(value):
    if isinstance(value, dict) and "$number" in value:
        spelling = value["$number"]
        if not spelling.lower().lstrip("-").startswith("0x"):
            decimal = Decimal(spelling)
            approximation = float(decimal)
            if Decimal(str(approximation)) == decimal:
                return approximation
    return value


class Parser:
    def __init__(self, text: str, filename: str = "<input>", start: int = 0):
        self.source = Source(text, filename)
        self.tokens = self.source.tokens(start)
        self.token = next(self.tokens)

    def advance(self) -> Token:
        previous = self.token
        if previous.kind != "eof":
            self.token = next(self.tokens)
        return previous

    def at(self, text: str) -> bool:
        return self.token.kind in ("symbol", "name") and self.token.text == text

    def take(self, text: str) -> bool:
        if self.at(text):
            self.advance()
            return True
        return False

    def expect(self, text: str):
        if not self.take(text):
            raise self.source.error(self.token.start, f"expected {text!r}, got {self.token.text!r}")

    def identifier(self) -> str:
        if self.token.kind != "name" or self.token.text in _KEYWORDS:
            raise self.source.error(self.token.start, f"expected identifier, got {self.token.text!r}")
        return self.advance().text

    def expressions(self) -> list:
        values = [self.expression()]
        while self.take(","):
            values.append(self.expression())
        return values

    def expression(self, min_precedence: int = 1):
        return self.binary_tail(self.primary(), min_precedence)

    def binary_tail(self, left, min_precedence: int = 1):
        while self.token.kind == "symbol":
            operator = self.token.text
            precedence = _BINARY_PRECEDENCE.get(operator, 0)
            if precedence < min_precedence:
                break
            self.advance()
            right = self.expression(precedence if operator == "^" else precedence + 1)
            left = {"$binary": operator, "args": [left, right]}
        return left

    def primary(self):
        token = self.token
        if self.take("{"):
            return self.table()
        if self.take("("):
            value = self.expression()
            self.expect(")")
            return value
        if self.take("-"):
            value = self.expression(3)
            if type(value) is int:
                return -value
            if isinstance(value, dict) and "$number" in value:
                spelling = value["$number"]
                return {"$number": spelling[1:] if spelling.startswith("-") else "-" + spelling}
            return {"$neg": value}
        if token.kind == "string":
            return short_string(self.source, self.advance())
        if token.kind == "long":
            self.advance()
            opening_newline = _NEWLINE.match(token.text)
            contents = token.text[opening_newline.end():] if opening_newline else token.text
            return _NEWLINE.sub("\n", contents)
        if token.kind == "number":
            return number(self.advance().text)
        if self.take("function"):
            return self.function(token.start)
        if self.take("..."):
            return {"$name": "..."}
        if token.kind == "name":
            if token.text in ("true", "false", "nil"):
                self.advance()
                return {"true": True, "false": False, "nil": None}[token.text]
            return self.name_value(self.identifier())
        raise self.source.error(token.start, f"expected data expression, got {token.text!r}")

    def name_value(self, name: str):
        parts = [name]
        while self.take("."):
            parts.append(self.identifier())
        path = ".".join(parts)
        if self.take("("):
            if self.take(")"):
                return {"$call": path, "args": []}
            args = self.expressions()
            self.expect(")")
            return {"$call": path, "args": args}
        return {"$name": path}

    def table(self) -> dict:
        array = []
        fields = []
        while not self.take("}"):
            if self.take("["):
                key = numeric_key(self.expression())
                self.expect("]")
                self.expect("=")
                fields.append([key, self.expression()])
            elif self.token.kind == "name" and self.token.text not in _KEYWORDS:
                name = self.identifier()
                if self.take("="):
                    fields.append([name, self.expression()])
                else:
                    array.append(self.binary_tail(self.name_value(name)))
            else:
                array.append(self.expression())
            if not (self.take(",") or self.take(";")):
                self.expect("}")
                break
        return {"$table": {"array": array, "fields": fields}}

    def function(self, start: int) -> dict:
        self.expect("(")
        blocks = ["function"]
        while self.token.kind != "eof":
            token = self.advance()
            if token.kind != "name":
                continue
            if token.text in ("for", "while"):
                blocks.append("loop")
            elif token.text == "do":
                if blocks[-1] == "loop":
                    blocks[-1] = "do"
                else:
                    blocks.append("do")
            elif token.text in ("function", "if", "repeat"):
                blocks.append(token.text)
            elif token.text in ("end", "until"):
                if (blocks[-1] == "repeat") != (token.text == "until"):
                    raise self.source.error(token.start, f"unexpected {token.text!r} in function")
                blocks.pop()
                if not blocks:
                    return {"$function": self.source.text[start:token.end], "line": self.source.line(start)}
        raise self.source.error(start, "unterminated function")

    def statements(self) -> list[Statement]:
        result = []
        while self.token.kind != "eof":
            if self.take(";"):
                continue
            token = self.token
            line = self.source.line(token.start)
            if self.take("return"):
                result.append(Statement("return", [], self.expression(), line))
                self.take(";")
                if self.token.kind != "eof":
                    raise self.source.error(self.token.start, "unexpected statement after return")
            elif self.take("local"):
                names = [self.identifier()]
                while self.take(","):
                    names.append(self.identifier())
                self.expect("=")
                result.append(Statement("local", names, self.expressions(), line))
            else:
                target = [self.identifier()]
                while True:
                    if self.take("."):
                        target.append(self.identifier())
                    elif self.take("["):
                        target.append(numeric_key(self.expression()))
                        self.expect("]")
                    else:
                        break
                self.expect("=")
                result.append(Statement("assign", target, self.expression(), line))
        return result


def parse(text: str, filename: str = "<input>") -> list[Statement]:
    return Parser(text, filename).statements()


def table_fields(value: dict) -> list:
    return value["$table"]["fields"]


def table_array(value: dict) -> list:
    return value["$table"]["array"]


def field(value: dict, key, default=None):
    for current_key, current_value in reversed(table_fields(value)):
        if current_key == key:
            return current_value
    return default
