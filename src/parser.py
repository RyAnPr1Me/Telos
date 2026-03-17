"""Recursive-descent parser for the Telos language."""

from __future__ import annotations

from typing import List, Optional

from .lexer import Token, tokenize
from .ast_nodes import (
    Expr, IntLiteral, FloatLiteral, Identifier, BinaryOp, UnaryOp,
    Call, Assignment,
    Stmt, VarDecl, ExprStmt, Return, Block, If, For, While,
    Param, Function, Program,
)

TYPE_KEYWORDS = frozenset({"int", "float", "double", "void", "bool"})
ASSIGN_OPS = frozenset({"=", "+=", "-=", "*=", "/=", "%="})


class ParseError(Exception):
    pass


class Parser:
    """Recursive-descent parser that builds an AST from a token list."""

    def __init__(self, tokens: List[Token]) -> None:
        self._tokens = tokens
        self._pos = 0

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    @property
    def _cur(self) -> Token:
        return self._tokens[self._pos]

    def _peek(self, offset: int = 1) -> Token:
        idx = self._pos + offset
        if idx >= len(self._tokens):
            return self._tokens[-1]
        return self._tokens[idx]

    def _advance(self) -> Token:
        tok = self._tokens[self._pos]
        if tok.kind != "EOF":
            self._pos += 1
        return tok

    def _expect(self, kind: str, value: Optional[str] = None) -> Token:
        tok = self._cur
        if tok.kind != kind:
            raise ParseError(
                f"Expected token kind {kind!r} but got {tok.kind!r} "
                f"({tok.value!r}) at line {tok.line}:{tok.col}"
            )
        if value is not None and tok.value != value:
            raise ParseError(
                f"Expected {value!r} but got {tok.value!r} "
                f"at line {tok.line}:{tok.col}"
            )
        return self._advance()

    def _match(self, kind: str, value: Optional[str] = None) -> bool:
        """Consume and return True if current token matches; otherwise False."""
        tok = self._cur
        if tok.kind != kind:
            return False
        if value is not None and tok.value != value:
            return False
        self._advance()
        return True

    # ------------------------------------------------------------------
    # Program / functions
    # ------------------------------------------------------------------

    def parse_program(self) -> Program:
        fns: List[Function] = []
        while self._cur.kind != "EOF":
            fns.append(self._parse_function())
        return Program(fns)

    def _parse_function(self) -> Function:
        tok = self._cur
        if tok.kind != "KEYWORD" or tok.value not in TYPE_KEYWORDS:
            raise ParseError(
                f"Expected return type at line {tok.line}:{tok.col}, "
                f"got {tok.value!r}"
            )
        ret_type = self._advance().value
        name = self._expect("ID").value
        self._expect("PUNCT", "(")
        params = self._parse_params()
        self._expect("PUNCT", ")")
        body = self._parse_block()
        return Function(ret_type, name, params, body)

    def _parse_params(self) -> List[Param]:
        params: List[Param] = []
        if self._cur.value == ")":
            return params
        while True:
            tok = self._cur
            if tok.kind != "KEYWORD" or tok.value not in TYPE_KEYWORDS:
                raise ParseError(
                    f"Expected parameter type at line {tok.line}:{tok.col}"
                )
            ptype = self._advance().value
            pname = self._expect("ID").value
            params.append(Param(ptype, pname))
            if not self._match("PUNCT", ","):
                break
        return params

    # ------------------------------------------------------------------
    # Statements
    # ------------------------------------------------------------------

    def _parse_block(self) -> Block:
        self._expect("PUNCT", "{")
        stmts: List[Stmt] = []
        while self._cur.value != "}" and self._cur.kind != "EOF":
            stmts.append(self._parse_stmt())
        self._expect("PUNCT", "}")
        return Block(stmts)

    def _parse_stmt(self) -> Stmt:
        tok = self._cur

        # Variable declaration: starts with a type keyword
        if tok.kind == "KEYWORD" and tok.value in TYPE_KEYWORDS:
            return self._parse_var_decl()

        # return
        if tok.kind == "KEYWORD" and tok.value == "return":
            return self._parse_return()

        # for
        if tok.kind == "KEYWORD" and tok.value == "for":
            return self._parse_for()

        # while
        if tok.kind == "KEYWORD" and tok.value == "while":
            return self._parse_while()

        # if
        if tok.kind == "KEYWORD" and tok.value == "if":
            return self._parse_if()

        # nested block
        if tok.value == "{":
            return self._parse_block()

        # expression statement (assignment, call, ++/--, ...)
        expr = self._parse_expr()
        self._expect("PUNCT", ";")
        return ExprStmt(expr)

    def _parse_var_decl(self) -> VarDecl:
        type_name = self._advance().value
        name = self._expect("ID").value
        init: Optional[Expr] = None
        if self._match("OP", "="):
            init = self._parse_expr()
        self._expect("PUNCT", ";")
        return VarDecl(type_name, name, init)

    def _parse_return(self) -> Return:
        self._expect("KEYWORD", "return")
        if self._cur.value == ";":
            self._advance()
            return Return(None)
        val = self._parse_expr()
        self._expect("PUNCT", ";")
        return Return(val)

    def _parse_for(self) -> For:
        self._expect("KEYWORD", "for")
        self._expect("PUNCT", "(")

        # init clause
        init: Optional[Stmt] = None
        if self._cur.value != ";":
            if self._cur.kind == "KEYWORD" and self._cur.value in TYPE_KEYWORDS:
                # var decl without trailing ';' (we eat the ';' below)
                type_name = self._advance().value
                name = self._expect("ID").value
                iexpr: Optional[Expr] = None
                if self._match("OP", "="):
                    iexpr = self._parse_expr()
                init = VarDecl(type_name, name, iexpr)
            else:
                init = ExprStmt(self._parse_expr())
        self._expect("PUNCT", ";")

        # condition clause
        cond: Optional[Expr] = None
        if self._cur.value != ";":
            cond = self._parse_expr()
        self._expect("PUNCT", ";")

        # update clause
        update: Optional[Expr] = None
        if self._cur.value != ")":
            update = self._parse_expr()
        self._expect("PUNCT", ")")

        body = self._parse_block()
        return For(init, cond, update, body)

    def _parse_while(self) -> While:
        self._expect("KEYWORD", "while")
        self._expect("PUNCT", "(")
        cond = self._parse_expr()
        self._expect("PUNCT", ")")
        body = self._parse_block()
        return While(cond, body)

    def _parse_if(self) -> If:
        self._expect("KEYWORD", "if")
        self._expect("PUNCT", "(")
        cond = self._parse_expr()
        self._expect("PUNCT", ")")
        then_branch = self._parse_block()
        else_branch: Optional[Block] = None
        if self._match("KEYWORD", "else"):
            if self._cur.kind == "KEYWORD" and self._cur.value == "if":
                else_branch = Block([self._parse_if()])
            else:
                else_branch = self._parse_block()
        return If(cond, then_branch, else_branch)

    # ------------------------------------------------------------------
    # Expressions  (precedence: low → high)
    # ------------------------------------------------------------------

    def _parse_expr(self) -> Expr:
        return self._parse_assign()

    def _parse_assign(self) -> Expr:
        left = self._parse_or()
        if self._cur.kind == "OP" and self._cur.value in ASSIGN_OPS:
            op = self._advance().value
            if not isinstance(left, Identifier):
                raise ParseError(
                    f"Assignment target must be an identifier "
                    f"at line {self._cur.line}:{self._cur.col}"
                )
            right = self._parse_assign()   # right-associative
            return Assignment(left.name, op, right)
        return left

    def _parse_or(self) -> Expr:
        left = self._parse_and()
        while self._cur.kind == "OP" and self._cur.value == "||":
            op = self._advance().value
            right = self._parse_and()
            left = BinaryOp(op, left, right)
        return left

    def _parse_and(self) -> Expr:
        left = self._parse_eq()
        while self._cur.kind == "OP" and self._cur.value == "&&":
            op = self._advance().value
            right = self._parse_eq()
            left = BinaryOp(op, left, right)
        return left

    def _parse_eq(self) -> Expr:
        left = self._parse_cmp()
        while self._cur.kind == "OP" and self._cur.value in ("==", "!="):
            op = self._advance().value
            right = self._parse_cmp()
            left = BinaryOp(op, left, right)
        return left

    def _parse_cmp(self) -> Expr:
        left = self._parse_add()
        while self._cur.kind == "OP" and self._cur.value in ("<", ">", "<=", ">="):
            op = self._advance().value
            right = self._parse_add()
            left = BinaryOp(op, left, right)
        return left

    def _parse_add(self) -> Expr:
        left = self._parse_mul()
        while self._cur.kind == "OP" and self._cur.value in ("+", "-"):
            op = self._advance().value
            right = self._parse_mul()
            left = BinaryOp(op, left, right)
        return left

    def _parse_mul(self) -> Expr:
        left = self._parse_unary()
        while self._cur.kind == "OP" and self._cur.value in ("*", "/", "%"):
            op = self._advance().value
            right = self._parse_unary()
            left = BinaryOp(op, left, right)
        return left

    def _parse_unary(self) -> Expr:
        tok = self._cur
        # Prefix minus, logical not
        if tok.kind == "OP" and tok.value in ("-", "!"):
            op = self._advance().value
            operand = self._parse_unary()
            return UnaryOp(op, operand)
        # Prefix increment / decrement
        if tok.kind == "OP" and tok.value in ("++", "--"):
            op = self._advance().value
            operand = self._parse_postfix()
            return UnaryOp(op, operand, postfix=False)
        return self._parse_postfix()

    def _parse_postfix(self) -> Expr:
        expr = self._parse_primary()
        while True:
            tok = self._cur
            if tok.kind == "OP" and tok.value in ("++", "--"):
                op = self._advance().value
                expr = UnaryOp(op, expr, postfix=True)
            else:
                break
        return expr

    def _parse_primary(self) -> Expr:
        tok = self._cur

        if tok.kind == "INT_LIT":
            self._advance()
            return IntLiteral(int(tok.value))

        if tok.kind == "FLOAT_LIT":
            self._advance()
            return FloatLiteral(float(tok.value))

        if tok.kind == "KEYWORD" and tok.value in ("true", "false"):
            self._advance()
            return IntLiteral(1 if tok.value == "true" else 0)

        if tok.kind == "ID":
            name = self._advance().value
            if self._cur.value == "(":   # function call
                self._advance()
                args: List[Expr] = []
                if self._cur.value != ")":
                    args.append(self._parse_expr())
                    while self._match("PUNCT", ","):
                        args.append(self._parse_expr())
                self._expect("PUNCT", ")")
                return Call(name, args)
            return Identifier(name)

        if tok.value == "(":            # parenthesised expression
            self._advance()
            expr = self._parse_expr()
            self._expect("PUNCT", ")")
            return expr

        raise ParseError(
            f"Unexpected token {tok.kind!r} ({tok.value!r}) "
            f"at line {tok.line}:{tok.col}"
        )


def parse(source: str) -> Program:
    """Parse a Telos source string and return the Program AST."""
    tokens = tokenize(source)
    parser = Parser(tokens)
    return parser.parse_program()
