"""Tests for the Telos lexer."""

import pytest
from src.lexer import tokenize, LexError, Token


def _kinds(source: str):
    return [(t.kind, t.value) for t in tokenize(source) if t.kind != "EOF"]


class TestBasicTokens:
    def test_integer_literal(self):
        toks = _kinds("42")
        assert toks == [("INT_LIT", "42")]

    def test_float_literal(self):
        toks = _kinds("3.14")
        assert toks == [("FLOAT_LIT", "3.14")]

    def test_identifier(self):
        toks = _kinds("myVar")
        assert toks == [("ID", "myVar")]

    def test_keyword(self):
        toks = _kinds("int")
        assert toks == [("KEYWORD", "int")]

    def test_all_keywords(self):
        for kw in ("int", "float", "void", "return", "if", "else",
                   "for", "while", "true", "false"):
            toks = _kinds(kw)
            assert toks == [("KEYWORD", kw)], kw


class TestOperators:
    def test_single_char(self):
        for op in ("+", "-", "*", "/", "%", "<", ">"):
            toks = _kinds(op)
            assert toks == [("OP", op)], op

    def test_two_char(self):
        for op in ("++", "--", "+=", "-=", "<=", ">=", "==", "!=", "&&", "||"):
            toks = _kinds(op)
            assert toks == [("OP", op)], op

    def test_does_not_greedily_merge(self):
        # "< =" should be two tokens, not "<="
        toks = _kinds("< =")
        assert toks == [("OP", "<"), ("OP", "=")]


class TestPunctuation:
    def test_braces_parens(self):
        toks = _kinds("(){}")
        assert toks == [
            ("PUNCT", "("),
            ("PUNCT", ")"),
            ("PUNCT", "{"),
            ("PUNCT", "}"),
        ]

    def test_semicolon_comma(self):
        toks = _kinds(";,")
        assert toks == [("PUNCT", ";"), ("PUNCT", ",")]


class TestComments:
    def test_single_line_comment(self):
        toks = _kinds("42 // this is a comment\n99")
        assert toks == [("INT_LIT", "42"), ("INT_LIT", "99")]

    def test_multi_line_comment(self):
        toks = _kinds("1 /* foo\nbar */ 2")
        assert toks == [("INT_LIT", "1"), ("INT_LIT", "2")]

    def test_comment_only_line(self):
        assert _kinds("// nothing") == []


class TestWhitespace:
    def test_spaces_ignored(self):
        assert _kinds("  1  +  2  ") == [
            ("INT_LIT", "1"), ("OP", "+"), ("INT_LIT", "2")
        ]

    def test_newlines_ignored(self):
        assert _kinds("1\n2\n3") == [
            ("INT_LIT", "1"), ("INT_LIT", "2"), ("INT_LIT", "3")
        ]


class TestLineNumbers:
    def test_line_tracking(self):
        toks = tokenize("a\nb\nc")
        ids = [t for t in toks if t.kind == "ID"]
        assert ids[0].line == 1
        assert ids[1].line == 2
        assert ids[2].line == 3


class TestErrors:
    def test_unknown_character(self):
        with pytest.raises(LexError):
            tokenize("@")


class TestEOF:
    def test_eof_at_end(self):
        toks = tokenize("x")
        assert toks[-1].kind == "EOF"
