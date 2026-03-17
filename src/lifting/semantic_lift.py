"""Semantic lifting: convert AST to Constraint IR.

This module is responsible for the most important transformation in the
Telos compiler: extracting *high-level semantic intent* from the surface
syntax and encoding it as semantic constraint nodes.

Key patterns detected
---------------------
* **Accumulation loops** – a ``for`` loop whose body is a single compound
  assignment ``acc op= f(i)`` is lifted to a ``ReductionConstraint``.
* **Simple assignments** – lifted directly to ``AssignConstraint``.
* **Parameters** – recorded as ``InvariantConstraint`` nodes.
* **Conditional branches** – lifted to ``CondBranchConstraint``.

When a loop cannot be recognised as a clean reduction (e.g. the body has
multiple statements or side effects), the lifter falls back to recording
individual ``AssignConstraint`` nodes, which the optimizer will later try
to execute as a generic loop.
"""

from __future__ import annotations

from typing import Dict, Optional, Tuple

from ..ast_nodes import (
    Assignment, BinaryOp, Block, Call, ExprStmt, FloatLiteral,
    For, Function, Identifier, If, IntLiteral, Program, Return,
    Stmt, UnaryOp, VarDecl, While, Expr,
)
from ..ir.graph import ConstraintGraph
from ..ir.nodes import (
    AssignConstraint, CondBranchConstraint, InvariantConstraint,
    IRBinOp, IRCall, IRConst, IRExpr, IRUnaryOp, IRVar,
    ReductionConstraint, ReturnConstraint,
)

# Mapping from compound assignment operators to reduction op names
_ACCUM_OPS: Dict[str, str] = {
    "+=": "add",
    "-=": "sub",
    "*=": "mul",
}

# Mapping from compound assignment operators to plain binary operators
_COMPOUND_TO_BINOP: Dict[str, str] = {
    "+=": "+",
    "-=": "-",
    "*=": "*",
    "/=": "/",
    "%=": "%",
}


class LiftError(Exception):
    pass


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def lift_program(program: Program) -> Dict[str, ConstraintGraph]:
    """Lift a full Program to a mapping of function name → ConstraintGraph."""
    lifter = SemanticLifter()
    return {fn.name: lifter.lift_function(fn) for fn in program.functions}


# ---------------------------------------------------------------------------
# Lifter implementation
# ---------------------------------------------------------------------------

class SemanticLifter:
    """Converts an AST function into a ConstraintGraph."""

    def lift_function(self, fn: Function) -> ConstraintGraph:
        graph = ConstraintGraph(fn.name, [p.name for p in fn.params])

        # Parameters are invariants
        for p in fn.params:
            graph.add(InvariantConstraint(var=p.name, source="param"))

        env: Dict[str, str] = {p.name: "param" for p in fn.params}
        self._lift_block(fn.body, graph, env)
        return graph

    # ------------------------------------------------------------------
    # Block / statement visitors
    # ------------------------------------------------------------------

    def _lift_block(
        self,
        block: Block,
        graph: ConstraintGraph,
        env: Dict[str, str],
    ) -> None:
        for stmt in block.stmts:
            self._lift_stmt(stmt, graph, env)

    def _lift_stmt(
        self,
        stmt: Stmt,
        graph: ConstraintGraph,
        env: Dict[str, str],
    ) -> None:
        if isinstance(stmt, VarDecl):
            env[stmt.name] = "local"
            if stmt.init is not None:
                graph.add(AssignConstraint(
                    var=stmt.name,
                    expr=self._lift_expr(stmt.init, env),
                ))

        elif isinstance(stmt, ExprStmt):
            self._lift_expr_stmt(stmt.expr, graph, env)

        elif isinstance(stmt, For):
            self._lift_for(stmt, graph, env)

        elif isinstance(stmt, While):
            self._lift_while(stmt, graph, env)

        elif isinstance(stmt, Return):
            expr = (
                self._lift_expr(stmt.value, env)
                if stmt.value is not None
                else IRConst(0)
            )
            graph.add(ReturnConstraint(expr=expr))

        elif isinstance(stmt, If):
            self._lift_if(stmt, graph, env)

        elif isinstance(stmt, Block):
            self._lift_block(stmt, graph, env)

    def _lift_expr_stmt(
        self,
        expr: Expr,
        graph: ConstraintGraph,
        env: Dict[str, str],
    ) -> None:
        """Handle expression statements (assignments, ++/--, calls)."""
        if isinstance(expr, Assignment):
            rhs = self._lift_expr(expr.value, env)
            if expr.op == "=":
                graph.add(AssignConstraint(var=expr.target, expr=rhs))
            else:
                # x op= y  →  x = x binop y
                binop = _COMPOUND_TO_BINOP.get(expr.op, expr.op[0])
                combined = IRBinOp(binop, IRVar(expr.target), rhs)
                graph.add(AssignConstraint(var=expr.target, expr=combined))
        elif isinstance(expr, UnaryOp) and expr.op in ("++", "--"):
            # i++ / ++i / i-- / --i
            if isinstance(expr.operand, Identifier):
                delta = IRConst(1) if expr.op == "++" else IRConst(-1)
                graph.add(AssignConstraint(
                    var=expr.operand.name,
                    expr=IRBinOp("+", IRVar(expr.operand.name), delta),
                ))

    # ------------------------------------------------------------------
    # For-loop lifting
    # ------------------------------------------------------------------

    def _lift_for(
        self,
        stmt: For,
        graph: ConstraintGraph,
        env: Dict[str, str],
    ) -> None:
        """Try to lift a for-loop as a reduction; fall back if not possible."""
        reduction = self._try_reduction(stmt, env)
        if reduction is not None:
            graph.add(reduction)
        else:
            # Fallback: emit the init, then treat body statements individually.
            # The optimizer will generate a LoopPlan for any remaining
            # ReductionConstraint or use a generic loop.
            new_env = dict(env)
            if stmt.init is not None:
                self._lift_stmt(stmt.init, graph, new_env)
            self._lift_block(stmt.body, graph, new_env)

    def _try_reduction(
        self,
        stmt: For,
        env: Dict[str, str],
    ) -> Optional[ReductionConstraint]:
        """Attempt to match the for-loop as a single accumulation reduction.

        Accepted pattern::

            for (<type> i = start; i < end; i++) {
                acc op= f(i);
            }

        Also accepts ``i <= end``, ``++i``, and ``i += 1`` as equivalents.
        """
        if stmt.init is None or stmt.cond is None or stmt.update is None:
            return None

        # --- loop variable and start value ---
        loop_var, start_ast = _detect_init(stmt.init)
        if loop_var is None:
            return None

        # --- upper bound and whether it is inclusive ---
        end_ast, inclusive = _detect_cond(stmt.cond, loop_var)
        if end_ast is None:
            return None

        # --- increment must be by 1 ---
        if not _detect_increment(stmt.update, loop_var):
            return None

        # --- body must be exactly one accumulation statement ---
        if len(stmt.body.stmts) != 1:
            return None
        accumulator, op, body_ast = _detect_accumulation(
            stmt.body.stmts[0], loop_var
        )
        if accumulator is None:
            return None

        # --- build IR expressions ---
        loop_env = {**env, loop_var: "loop_var"}
        ir_start = self._lift_expr(start_ast, env)
        ir_end = self._lift_expr(end_ast, env)
        if inclusive:
            ir_end = IRBinOp("+", ir_end, IRConst(1))

        ir_body = self._lift_expr(body_ast, loop_env)

        _init_vals = {
            "add": IRConst(0),
            "sub": IRConst(0),
            "mul": IRConst(1),
        }
        init_val = _init_vals.get(op, IRConst(0))

        return ReductionConstraint(
            accumulator=accumulator,
            op=op,
            loop_var=loop_var,
            start=ir_start,
            end=ir_end,
            body=ir_body,
            init_val=init_val,
        )

    # ------------------------------------------------------------------
    # While-loop (not optimised, emit as-is)
    # ------------------------------------------------------------------

    def _lift_while(
        self,
        stmt: While,
        graph: ConstraintGraph,
        env: Dict[str, str],
    ) -> None:
        self._lift_block(stmt.body, graph, env)

    # ------------------------------------------------------------------
    # Conditional
    # ------------------------------------------------------------------

    def _lift_if(
        self,
        stmt: If,
        graph: ConstraintGraph,
        env: Dict[str, str],
    ) -> None:
        cond_ir = self._lift_expr(stmt.cond, env)

        then_graph = ConstraintGraph("_then", [])
        self._lift_block(stmt.then_branch, then_graph, dict(env))

        else_graph = ConstraintGraph("_else", [])
        if stmt.else_branch is not None:
            self._lift_block(stmt.else_branch, else_graph, dict(env))

        graph.add(CondBranchConstraint(
            cond=cond_ir,
            then_constraints=then_graph.constraints,
            else_constraints=else_graph.constraints,
        ))

    # ------------------------------------------------------------------
    # Expression lifter
    # ------------------------------------------------------------------

    def _lift_expr(self, expr: Expr, env: Dict[str, str]) -> IRExpr:
        if isinstance(expr, IntLiteral):
            return IRConst(expr.value)
        if isinstance(expr, FloatLiteral):
            return IRConst(expr.value)
        if isinstance(expr, Identifier):
            return IRVar(expr.name)
        if isinstance(expr, BinaryOp):
            return IRBinOp(
                expr.op,
                self._lift_expr(expr.left, env),
                self._lift_expr(expr.right, env),
            )
        if isinstance(expr, UnaryOp):
            return IRUnaryOp(expr.op, self._lift_expr(expr.operand, env))
        if isinstance(expr, Call):
            return IRCall(
                expr.name,
                [self._lift_expr(a, env) for a in expr.args],
            )
        if isinstance(expr, Assignment):
            raise LiftError(
                "Assignment used as expression is not supported in this context"
            )
        raise LiftError(f"Unknown expression type: {type(expr).__name__}")


# ---------------------------------------------------------------------------
# Pattern-detection helpers (module-level, pure functions)
# ---------------------------------------------------------------------------

def _detect_init(
    init: Stmt,
) -> Tuple[Optional[str], Optional[Expr]]:
    """Return (loop_var, start_expr) or (None, None)."""
    if isinstance(init, VarDecl) and init.init is not None:
        return init.name, init.init
    if (
        isinstance(init, ExprStmt)
        and isinstance(init.expr, Assignment)
        and init.expr.op == "="
    ):
        return init.expr.target, init.expr.value
    return None, None


def _detect_cond(
    cond: Expr,
    loop_var: str,
) -> Tuple[Optional[Expr], bool]:
    """Return (end_expr, inclusive) or (None, False).

    ``inclusive=True`` means the loop runs for ``i <= end`` (so the
    effective exclusive bound is ``end + 1``).
    """
    if not isinstance(cond, BinaryOp):
        return None, False
    if not isinstance(cond.left, Identifier):
        return None, False
    if cond.left.name != loop_var:
        return None, False
    if cond.op == "<":
        return cond.right, False
    if cond.op == "<=":
        return cond.right, True
    return None, False


def _detect_increment(update: Expr, loop_var: str) -> bool:
    """Return True if *update* increments *loop_var* by exactly 1."""
    if isinstance(update, UnaryOp):
        return (
            update.op == "++"
            and isinstance(update.operand, Identifier)
            and update.operand.name == loop_var
        )
    if isinstance(update, Assignment):
        return (
            update.target == loop_var
            and update.op == "+="
            and isinstance(update.value, IntLiteral)
            and update.value.value == 1
        )
    return False


def _detect_accumulation(
    stmt: Stmt,
    loop_var: str,
) -> Tuple[Optional[str], Optional[str], Optional[Expr]]:
    """Match ``acc op= f(loop_var)`` and return ``(acc, ir_op, body_expr)``.

    Returns ``(None, None, None)`` if the statement does not match.
    """
    expr = stmt.expr if isinstance(stmt, ExprStmt) else None
    if expr is None:
        return None, None, None

    if isinstance(expr, Assignment) and expr.op in _ACCUM_OPS:
        return expr.target, _ACCUM_OPS[expr.op], expr.value

    return None, None, None
