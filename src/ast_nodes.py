"""AST node definitions for the Telos language."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional


# ---------------------------------------------------------------------------
# Expressions
# ---------------------------------------------------------------------------

class Expr:
    """Base class for all AST expressions."""


@dataclass
class IntLiteral(Expr):
    value: int


@dataclass
class FloatLiteral(Expr):
    value: float


@dataclass
class Identifier(Expr):
    name: str


@dataclass
class BinaryOp(Expr):
    """Binary operation: left op right."""
    op: str   # +, -, *, /, %, <, >, <=, >=, ==, !=, &&, ||
    left: Expr
    right: Expr


@dataclass
class UnaryOp(Expr):
    """Unary operation: op operand (or operand op for postfix)."""
    op: str       # -, !, ++, --
    operand: Expr
    postfix: bool = False   # True for i++, i--


@dataclass
class Call(Expr):
    """Function call: name(args...)."""
    name: str
    args: List[Expr]


@dataclass
class Assignment(Expr):
    """Assignment expression: target op= value."""
    target: str
    op: str     # =, +=, -=, *=, /=, %=
    value: Expr


# ---------------------------------------------------------------------------
# Statements
# ---------------------------------------------------------------------------

class Stmt:
    """Base class for all AST statements."""


@dataclass
class VarDecl(Stmt):
    """Variable declaration: type_name name [= init];"""
    type_name: str
    name: str
    init: Optional[Expr] = None


@dataclass
class ExprStmt(Stmt):
    """Expression used as a statement."""
    expr: Expr


@dataclass
class Return(Stmt):
    """Return statement."""
    value: Optional[Expr] = None


@dataclass
class Block(Stmt):
    """Sequence of statements enclosed in braces."""
    stmts: List[Stmt] = field(default_factory=list)


@dataclass
class If(Stmt):
    """If / else statement."""
    cond: Expr
    then_branch: Block
    else_branch: Optional[Block] = None


@dataclass
class For(Stmt):
    """C-style for loop."""
    init: Optional[Stmt]     # VarDecl or ExprStmt
    cond: Optional[Expr]
    update: Optional[Expr]
    body: Block


@dataclass
class While(Stmt):
    """While loop."""
    cond: Expr
    body: Block


# ---------------------------------------------------------------------------
# Top-level declarations
# ---------------------------------------------------------------------------

@dataclass
class Param:
    """A single function parameter."""
    type_name: str
    name: str


@dataclass
class Function:
    """A top-level function definition."""
    return_type: str
    name: str
    params: List[Param]
    body: Block


@dataclass
class Program:
    """A complete Telos program (list of function definitions)."""
    functions: List[Function] = field(default_factory=list)
