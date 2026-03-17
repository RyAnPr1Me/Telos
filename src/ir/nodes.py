"""Constraint IR node definitions for the Telos compiler.

The IR represents a program not as a sequence of instructions but as a
**graph of semantic constraints / goals**.  The key property is that the
execution order is NOT fixed – the optimizer has freedom to choose any
valid strategy.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Union


# ---------------------------------------------------------------------------
# IR-level expressions (symbolic, can contain runtime variables)
# ---------------------------------------------------------------------------

class IRExpr:
    """Base class for IR-level symbolic expressions."""


@dataclass
class IRConst(IRExpr):
    """A compile-time numeric constant."""
    value: Union[int, float]

    def __repr__(self) -> str:
        return repr(self.value)


@dataclass
class IRVar(IRExpr):
    """A runtime variable (parameter or local)."""
    name: str

    def __repr__(self) -> str:
        return self.name


@dataclass
class IRBinOp(IRExpr):
    """Binary arithmetic or comparison operator."""
    op: str    # +, -, *, /, //, %, <, >, <=, >=, ==, !=, &&, ||
    left: IRExpr
    right: IRExpr

    def __repr__(self) -> str:
        return f"({self.left!r} {self.op} {self.right!r})"


@dataclass
class IRUnaryOp(IRExpr):
    """Unary operator."""
    op: str    # -, !
    operand: IRExpr

    def __repr__(self) -> str:
        return f"({self.op}{self.operand!r})"


@dataclass
class IRCall(IRExpr):
    """Call to a named function."""
    name: str
    args: List[IRExpr]

    def __repr__(self) -> str:
        args_str = ", ".join(repr(a) for a in self.args)
        return f"{self.name}({args_str})"


# ---------------------------------------------------------------------------
# Constraint nodes
# ---------------------------------------------------------------------------

class ConstraintNode:
    """Base class for semantic constraint / goal nodes."""
    node_id: int = 0


@dataclass
class InvariantConstraint(ConstraintNode):
    """A value that is fixed throughout the function call.

    Parameters have ``source='param'``.  Loop-invariant values computed
    inside the function have ``source='computed'``.
    """
    var: str
    source: str              # 'param' | 'computed'
    expr: Optional[IRExpr] = None
    node_id: int = field(default=0)


@dataclass
class AssignConstraint(ConstraintNode):
    """Simple binding: ``var = expr``."""
    var: str
    expr: IRExpr
    node_id: int = field(default=0)


@dataclass
class ReductionConstraint(ConstraintNode):
    """Reduction over a range.

    Semantics::

        accumulator = init_val
        for loop_var in range(start, end):
            accumulator = combine(op, accumulator, body)

    The compiler is free to replace this with any mathematically
    equivalent strategy (closed form, vectorisation, etc.).
    """
    accumulator: str
    op: str          # 'add' | 'sub' | 'mul' | 'min' | 'max'
    loop_var: str
    start: IRExpr
    end: IRExpr      # exclusive upper bound
    body: IRExpr     # expression in terms of loop_var (and possibly other vars)
    init_val: IRExpr
    node_id: int = field(default=0)


@dataclass
class ReturnConstraint(ConstraintNode):
    """The observable result of a function."""
    expr: IRExpr
    node_id: int = field(default=0)


@dataclass
class CondBranchConstraint(ConstraintNode):
    """Conditional computation: if cond then A else B."""
    cond: IRExpr
    then_constraints: List[ConstraintNode]
    else_constraints: List[ConstraintNode]
    node_id: int = field(default=0)
