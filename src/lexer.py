"""Lexer / tokenizer for the Telos language."""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import List

# Reserved words
KEYWORDS = frozenset({
    "int", "float", "double", "void", "bool",
    "return", "if", "else", "for", "while", "break", "continue",
    "true", "false",
})

# Two-character operators that must be matched before single-char ones
TWO_CHAR_OPS = frozenset({
    "++", "--", "+=", "-=", "*=", "/=", "%=",
    "<=", ">=", "==", "!=", "&&", "||", "->", "<<", ">>",
})


@dataclass
class Token:
    """A single lexical token."""
    kind: str    # INT_LIT | FLOAT_LIT | ID | KEYWORD | OP | PUNCT | EOF
    value: str
    line: int
    col: int

    def __repr__(self) -> str:
        return f"Token({self.kind}, {self.value!r}, {self.line}:{self.col})"


class LexError(Exception):
    pass


def tokenize(source: str) -> List[Token]:
    """Tokenize a Telos source string into a list of Tokens."""
    tokens: List[Token] = []
    i = 0
    line = 1
    col = 1

    while i < len(source):
        start_line = line
        start_col = col
        ch = source[i]

        # --- Whitespace ---
        if ch in " \t\r":
            i += 1
            col += 1
            continue

        if ch == "\n":
            i += 1
            line += 1
            col = 1
            continue

        # --- Single-line comment ---
        if source[i : i + 2] == "//":
            while i < len(source) and source[i] != "\n":
                i += 1
                col += 1
            continue

        # --- Multi-line comment ---
        if source[i : i + 2] == "/*":
            i += 2
            col += 2
            while i < len(source) - 1 and source[i : i + 2] != "*/":
                if source[i] == "\n":
                    i += 1
                    line += 1
                    col = 1
                else:
                    i += 1
                    col += 1
            i += 2   # skip */
            col += 2
            continue

        # --- Float literal (before int, to catch '3.14') ---
        m = re.match(r"\d+\.\d*([eE][+-]?\d+)?|\.\d+([eE][+-]?\d+)?", source[i:])
        if m:
            tokens.append(Token("FLOAT_LIT", m.group(), start_line, start_col))
            i += len(m.group())
            col += len(m.group())
            continue

        # --- Integer literal ---
        m = re.match(r"\d+", source[i:])
        if m:
            tokens.append(Token("INT_LIT", m.group(), start_line, start_col))
            i += len(m.group())
            col += len(m.group())
            continue

        # --- Identifier / keyword ---
        m = re.match(r"[a-zA-Z_]\w*", source[i:])
        if m:
            word = m.group()
            kind = "KEYWORD" if word in KEYWORDS else "ID"
            tokens.append(Token(kind, word, start_line, start_col))
            i += len(word)
            col += len(word)
            continue

        # --- Two-character operators ---
        two = source[i : i + 2]
        if two in TWO_CHAR_OPS:
            tokens.append(Token("OP", two, start_line, start_col))
            i += 2
            col += 2
            continue

        # --- Single-character operators ---
        if ch in "+-*/%<>!=&|^~":
            tokens.append(Token("OP", ch, start_line, start_col))
            i += 1
            col += 1
            continue

        # --- Punctuation ---
        if ch in "(){}[];,":
            tokens.append(Token("PUNCT", ch, start_line, start_col))
            i += 1
            col += 1
            continue

        raise LexError(
            f"Unexpected character {ch!r} at line {line}, col {col}"
        )

    tokens.append(Token("EOF", "", line, col))
    return tokens
