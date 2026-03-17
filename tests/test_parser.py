"""Tests for the Telos parser."""

import pytest
from src.parser import parse, ParseError
from src.ast_nodes import (
    Program, Function, Param, Block,
    VarDecl, Return, For, If, While, ExprStmt,
    IntLiteral, FloatLiteral, Identifier, BinaryOp, UnaryOp, Call, Assignment,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def parse_fn(src: str) -> Function:
    """Parse source that contains exactly one function; return it."""
    prog = parse(src)
    assert len(prog.functions) == 1
    return prog.functions[0]


# ---------------------------------------------------------------------------
# Function signatures
# ---------------------------------------------------------------------------

class TestFunctionSignatures:
    def test_no_params(self):
        fn = parse_fn("int foo() { return 1; }")
        assert fn.name == "foo"
        assert fn.return_type == "int"
        assert fn.params == []

    def test_single_param(self):
        fn = parse_fn("int id(int x) { return x; }")
        assert len(fn.params) == 1
        assert fn.params[0].name == "x"
        assert fn.params[0].type_name == "int"

    def test_multiple_params(self):
        fn = parse_fn("int add(int a, int b) { return a + b; }")
        names = [p.name for p in fn.params]
        assert names == ["a", "b"]

    def test_void_return(self):
        fn = parse_fn("void noop() {}")
        assert fn.return_type == "void"


# ---------------------------------------------------------------------------
# Statements
# ---------------------------------------------------------------------------

class TestVarDecl:
    def test_with_init(self):
        fn = parse_fn("int f() { int x = 0; return x; }")
        decl = fn.body.stmts[0]
        assert isinstance(decl, VarDecl)
        assert decl.name == "x"
        assert isinstance(decl.init, IntLiteral)
        assert decl.init.value == 0

    def test_without_init(self):
        fn = parse_fn("int f() { int x; return 0; }")
        decl = fn.body.stmts[0]
        assert isinstance(decl, VarDecl)
        assert decl.init is None


class TestReturn:
    def test_return_expr(self):
        fn = parse_fn("int f() { return 42; }")
        ret = fn.body.stmts[0]
        assert isinstance(ret, Return)
        assert isinstance(ret.value, IntLiteral)
        assert ret.value.value == 42

    def test_return_void(self):
        fn = parse_fn("void f() { return; }")
        ret = fn.body.stmts[0]
        assert isinstance(ret, Return)
        assert ret.value is None


class TestForLoop:
    def test_basic_for(self):
        fn = parse_fn(
            "int f(int n) {"
            "  for (int i = 0; i < n; i++) { s += i; }"
            "  return s;"
            "}"
        )
        for_stmt = fn.body.stmts[0]
        assert isinstance(for_stmt, For)
        assert isinstance(for_stmt.init, VarDecl)
        assert for_stmt.init.name == "i"
        assert isinstance(for_stmt.cond, BinaryOp)
        assert for_stmt.cond.op == "<"


class TestIfElse:
    def test_if_only(self):
        fn = parse_fn("int f(int x) { if (x > 0) { return 1; } return 0; }")
        if_stmt = fn.body.stmts[0]
        assert isinstance(if_stmt, If)
        assert if_stmt.else_branch is None

    def test_if_else(self):
        fn = parse_fn(
            "int f(int x) { if (x > 0) { return 1; } else { return -1; } }"
        )
        if_stmt = fn.body.stmts[0]
        assert if_stmt.else_branch is not None


# ---------------------------------------------------------------------------
# Expressions
# ---------------------------------------------------------------------------

class TestExpressions:
    def test_binary_precedence(self):
        # 2 + 3 * 4  should parse as  2 + (3 * 4)
        fn = parse_fn("int f() { return 2 + 3 * 4; }")
        ret = fn.body.stmts[0]
        assert isinstance(ret.value, BinaryOp)
        assert ret.value.op == "+"
        assert isinstance(ret.value.right, BinaryOp)
        assert ret.value.right.op == "*"

    def test_assignment_expr(self):
        fn = parse_fn("int f() { int x = 0; x = 5; return x; }")
        assign_stmt = fn.body.stmts[1]
        assert isinstance(assign_stmt, ExprStmt)
        assert isinstance(assign_stmt.expr, Assignment)
        assert assign_stmt.expr.target == "x"
        assert assign_stmt.expr.op == "="

    def test_compound_assign(self):
        fn = parse_fn("int f() { int s = 0; s += 1; return s; }")
        stmt = fn.body.stmts[1]
        assert isinstance(stmt.expr, Assignment)
        assert stmt.expr.op == "+="

    def test_call(self):
        fn = parse_fn("int f(int n) { return g(n, 1); }")
        ret = fn.body.stmts[0]
        assert isinstance(ret.value, Call)
        assert ret.value.name == "g"
        assert len(ret.value.args) == 2

    def test_unary_minus(self):
        fn = parse_fn("int f() { return -1; }")
        ret = fn.body.stmts[0]
        assert isinstance(ret.value, UnaryOp)
        assert ret.value.op == "-"

    def test_postfix_increment(self):
        fn = parse_fn("int f() { int i = 0; i++; return i; }")
        stmt = fn.body.stmts[1]
        assert isinstance(stmt.expr, UnaryOp)
        assert stmt.expr.op == "++"
        assert stmt.expr.postfix is True

    def test_parenthesised(self):
        fn = parse_fn("int f() { return (1 + 2) * 3; }")
        ret = fn.body.stmts[0]
        assert isinstance(ret.value, BinaryOp)
        assert ret.value.op == "*"

    def test_float_literal(self):
        fn = parse_fn("float f() { return 3.14; }")
        ret = fn.body.stmts[0]
        assert isinstance(ret.value, FloatLiteral)

    def test_comparison_chain(self):
        fn = parse_fn("int f(int a, int b) { return a <= b; }")
        ret = fn.body.stmts[0]
        assert isinstance(ret.value, BinaryOp)
        assert ret.value.op == "<="


# ---------------------------------------------------------------------------
# Multiple functions
# ---------------------------------------------------------------------------

class TestMultipleFunctions:
    def test_two_functions(self):
        prog = parse("int a() { return 1; } int b() { return 2; }")
        assert len(prog.functions) == 2
        assert prog.functions[0].name == "a"
        assert prog.functions[1].name == "b"


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------

class TestParseErrors:
    def test_missing_closing_brace(self):
        with pytest.raises(ParseError):
            parse("int f() { return 1;")

    def test_missing_semicolon(self):
        with pytest.raises((ParseError, Exception)):
            parse("int f() { int x = 0 return x; }")
