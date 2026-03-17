"""IR expression simplifier.

Performs algebraic simplification of IR expressions to reduce verbosity
in the generated code.  This is a simple syntactic pass, not a full
algebraic solver.

Rules applied (recursively, bottom-up)
---------------------------------------
* x + 0  →  x
* 0 + x  →  x
* x - 0  →  x
* x * 1  →  x
* 1 * x  →  x
* x * 0  →  0
* 0 * x  →  0
* x // 1 →  x
* Constant folding: evaluate binary ops on two literals
"""

from __future__ import annotations

from typing import Union

from ..ir.nodes import IRBinOp, IRConst, IRExpr, IRUnaryOp, IRVar, IRCall


def simplify(expr: IRExpr) -> IRExpr:
    """Return a simplified (but semantically equivalent) copy of *expr*."""
    if isinstance(expr, (IRConst, IRVar)):
        return expr

    if isinstance(expr, IRUnaryOp):
        inner = simplify(expr.operand)
        if isinstance(inner, IRConst) and expr.op == "-":
            return IRConst(-inner.value)
        return IRUnaryOp(expr.op, inner)

    if isinstance(expr, IRCall):
        return IRCall(expr.name, [simplify(a) for a in expr.args])

    if isinstance(expr, IRBinOp):
        left = simplify(expr.left)
        right = simplify(expr.right)
        return _simplify_binop(expr.op, left, right)

    return expr  # unknown node: return as-is


def _simplify_binop(op: str, left: IRExpr, right: IRExpr) -> IRExpr:
    """Apply algebraic simplification to a binary operation."""
    # --- Constant folding ---
    if isinstance(left, IRConst) and isinstance(right, IRConst):
        v = _fold(op, left.value, right.value)
        if v is not None:
            return IRConst(v)

    # --- Identity / absorbing element rules ---
    lv = left.value if isinstance(left, IRConst) else None
    rv = right.value if isinstance(right, IRConst) else None

    if op == "+":
        if lv == 0:
            return right
        if rv == 0:
            return left

    elif op == "-":
        if rv == 0:
            return left
        if lv == 0:
            return IRUnaryOp("-", right)

    elif op == "*":
        if lv == 0 or rv == 0:
            return IRConst(0)
        if lv == 1:
            return right
        if rv == 1:
            return left

    elif op in ("/", "//"):
        if rv == 1:
            return left
        if lv == 0:
            return IRConst(0)

    elif op == "%":
        if lv == 0:
            return IRConst(0)

    return IRBinOp(op, left, right)


def _fold(op: str, a: Union[int, float], b: Union[int, float]):
    """Try to fold a constant binary operation.  Returns None on failure."""
    try:
        if op == "+":
            return a + b
        if op == "-":
            return a - b
        if op == "*":
            return a * b
        if op == "/":
            if b == 0:
                return None
            return a / b
        if op == "//":
            if b == 0:
                return None
            return int(a) // int(b)
        if op == "%":
            if b == 0:
                return None
            return a % b
    except (TypeError, ZeroDivisionError):
        pass
    return None
