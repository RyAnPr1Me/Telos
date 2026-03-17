"""Python code generator for the Telos compiler.

Translates a ``FunctionPlan`` (produced by the optimizer) into a Python
source string that can be executed directly with ``exec`` / ``eval``.

The generated code is clean Python, not byte-code.  This makes it easy to
inspect the effect of each optimization:

* A ``ConstantPlan`` becomes a single literal assignment.
* A ``ClosedFormPlan`` becomes a single algebraic expression.
* A ``LoopPlan`` becomes a ``for`` loop (the safe fallback).
* An ``AssignPlan`` becomes a simple assignment.

All IR expressions are passed through the simplifier before emission, so
trivial subexpressions such as ``n - 0`` are collapsed to ``n``.
"""

from __future__ import annotations

from typing import Dict, List, Union

from ..ir.nodes import (
    IRBinOp,
    IRCall,
    IRConst,
    IRExpr,
    IRUnaryOp,
    IRVar,
)
from ..ir.simplify import simplify
from ..optimizer.plans import (
    AssignPlan,
    ClosedFormPlan,
    ConstantPlan,
    ExecutionPlan,
    FunctionPlan,
    LoopPlan,
)

# Operators that map 1-to-1 from IR to Python
_BINOP_MAP: Dict[str, str] = {
    "+": "+",
    "-": "-",
    "*": "*",
    "/": "/",
    "//": "//",
    "%": "%",
    "<": "<",
    ">": ">",
    "<=": "<=",
    ">=": ">=",
    "==": "==",
    "!=": "!=",
    "&&": "and",
    "||": "or",
}

# Python identity element per reduction operator (for loop fallback)
_INIT_VALS: Dict[str, str] = {
    "add": "0",
    "sub": "0",
    "mul": "1",
}

# Python operator for each reduction accumulation
_OP_STMTS: Dict[str, str] = {
    "add": "+=",
    "sub": "-=",
    "mul": "*=",
}


class CodegenError(Exception):
    pass


class PythonGenerator:
    """Generates Python source code from a FunctionPlan."""

    def generate(self, plan: FunctionPlan) -> str:
        """Return a complete Python function definition as a string."""
        lines: List[str] = []
        params = ", ".join(plan.param_names)
        lines.append(f"def {plan.name}({params}):")

        body_lines = self._generate_body(plan)
        if not body_lines:
            lines.append("    pass")
        else:
            lines.extend(f"    {line}" for line in body_lines)

        return "\n".join(lines) + "\n"

    def _generate_body(self, plan: FunctionPlan) -> List[str]:
        lines: List[str] = []
        for step in plan.steps:
            lines.extend(self._generate_step(step))
        if plan.return_expr is not None:
            lines.append(f"return {self._emit_expr(simplify(plan.return_expr))}")
        return lines

    def _generate_step(self, step: ExecutionPlan) -> List[str]:
        if isinstance(step, ConstantPlan):
            return self._emit_constant(step)
        if isinstance(step, ClosedFormPlan):
            return self._emit_closed_form(step)
        if isinstance(step, LoopPlan):
            return self._emit_loop(step)
        if isinstance(step, AssignPlan):
            return self._emit_assign(step)
        raise CodegenError(f"Unsupported plan type: {type(step).__name__}")

    # ------------------------------------------------------------------
    # Per-plan emitters
    # ------------------------------------------------------------------

    def _emit_constant(self, step: ConstantPlan) -> List[str]:
        """Emit a single compile-time constant assignment."""
        v = step.value
        val_str = str(int(v)) if isinstance(v, float) and v == int(v) else str(v)
        comment = "  # compile-time constant"
        return [f"{step.accumulator} = {val_str}{comment}"]

    def _emit_closed_form(self, step: ClosedFormPlan) -> List[str]:
        """Emit a single algebraic formula."""
        expr_str = self._emit_expr(simplify(step.formula))
        comment = f"  # {step.description}" if step.description else ""
        return [f"{step.accumulator} = {expr_str}{comment}"]

    def _emit_loop(self, step: LoopPlan) -> List[str]:
        """Emit a for-loop (safe fallback)."""
        init = _OP_STMTS.get(step.op, "+=")
        init_val = self._emit_expr(step.init_val)
        start = self._emit_expr(step.start)
        end = self._emit_expr(step.end)
        body = self._emit_expr(step.body)
        acc = step.accumulator
        var = step.loop_var

        # Substitute the loop variable name in the body expression
        lines = [
            f"{acc} = {init_val}  # loop fallback",
            f"for {var} in range({start}, {end}):",
            f"    {acc} {init} {body}",
        ]
        return lines

    def _emit_assign(self, step: AssignPlan) -> List[str]:
        return [f"{step.var} = {self._emit_expr(step.expr)}"]

    # ------------------------------------------------------------------
    # Expression emitter
    # ------------------------------------------------------------------

    def _emit_expr(self, expr: IRExpr) -> str:
        if isinstance(expr, IRConst):
            v = expr.value
            if isinstance(v, float) and v == int(v):
                return str(int(v))
            return str(v)

        if isinstance(expr, IRVar):
            return expr.name

        if isinstance(expr, IRBinOp):
            py_op = _BINOP_MAP.get(expr.op, expr.op)
            left = self._emit_expr(expr.left)
            right = self._emit_expr(expr.right)
            return f"({left} {py_op} {right})"

        if isinstance(expr, IRUnaryOp):
            operand = self._emit_expr(expr.operand)
            if expr.op == "-":
                return f"(-{operand})"
            if expr.op == "!":
                return f"(not {operand})"
            return f"({expr.op}{operand})"

        if isinstance(expr, IRCall):
            args_str = ", ".join(self._emit_expr(a) for a in expr.args)
            return f"{expr.name}({args_str})"

        raise CodegenError(f"Unsupported IR expression: {type(expr).__name__}")


def generate_python(plan: FunctionPlan) -> str:
    """Convenience wrapper: generate Python source for *plan*."""
    return PythonGenerator().generate(plan)
