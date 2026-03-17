"""Execution plan types produced by the Telos optimizer."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Union

from ..ir.nodes import IRExpr


class ExecutionPlan:
    """Base class for all execution plans."""


@dataclass
class LoopPlan(ExecutionPlan):
    """Execute a reduction via a direct loop.

    This is the *always-valid* fallback plan.  Cost class O(n).
    """
    accumulator: str
    op: str
    loop_var: str
    start: IRExpr
    end: IRExpr
    body: IRExpr
    init_val: IRExpr
    cost_class: str = "O(n)"


@dataclass
class ClosedFormPlan(ExecutionPlan):
    """Execute a reduction via a closed-form algebraic formula.

    The ``formula`` IRExpr computes the result directly without any loop.
    Cost class O(1).
    """
    accumulator: str
    formula: IRExpr
    cost_class: str = "O(1)"
    description: str = ""


@dataclass
class ConstantPlan(ExecutionPlan):
    """The result is a value known entirely at compile time.

    Cost class O(1) constant (score 0).
    """
    accumulator: str
    value: Union[int, float]
    cost_class: str = "O(1) constant"


@dataclass
class AssignPlan(ExecutionPlan):
    """A simple variable binding: ``var = expr``."""
    var: str
    expr: IRExpr
    cost_class: str = "O(1)"


@dataclass
class FunctionPlan(ExecutionPlan):
    """The complete execution plan for one function."""
    name: str
    param_names: List[str]
    steps: List[ExecutionPlan] = field(default_factory=list)
    return_expr: Optional[IRExpr] = None
