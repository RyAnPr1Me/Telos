"""Goal graph: dependency analysis for goal-directed compilation.

The GoalGraph builds an explicit dependency graph from a ConstraintGraph,
enabling:

- **Liveness analysis**: which constraints are needed to compute the return value
- **Dead code elimination**: skip constraints whose results are never used by
  any live constraint
- **Goal-directed planning**: start from the *return goal* and trace backwards
  through the constraint graph to find the minimal set of computations needed

This is a key part of the Telos compilation model.  Rather than compiling
every statement that appears in the source, the compiler works backwards
from the *goal* (the return value) through the constraint graph:

* ``ReturnConstraint`` → root goal (always live)
* Variables it reads → find their last writer → mark it live
* Repeat for each live constraint's consumed variables → until fixpoint

Dead stores are automatically eliminated.  For example, when a
``ReductionConstraint`` overwrites ``s`` via a ``ClosedFormPlan`` or
``ConstantPlan``, any earlier ``AssignConstraint(s, 0)`` is dropped because
it is never the *last writer* of ``s`` that any live node depends on.

Public API
----------
``GoalGraph(graph).live_nodes()``
    Return the live constraints in original program order.
``vars_produced(node)`` / ``vars_consumed(node)``
    Helper functions that decode the variable I/O of a constraint node.
"""

from __future__ import annotations

from typing import Dict, List, Optional, Set

from ..ir.graph import ConstraintGraph
from ..ir.nodes import (
    AssignConstraint,
    CondBranchConstraint,
    ConstraintNode,
    InvariantConstraint,
    IRBinOp,
    IRCall,
    IRConst,
    IRExpr,
    IRUnaryOp,
    IRVar,
    ReductionConstraint,
    ReturnConstraint,
)


# ---------------------------------------------------------------------------
# Variable read / write analysis for IR expressions and constraint nodes
# ---------------------------------------------------------------------------

def vars_in_expr(expr: IRExpr) -> Set[str]:
    """Return the set of variable names referenced in *expr*."""
    if isinstance(expr, IRVar):
        return {expr.name}
    if isinstance(expr, IRConst):
        return set()
    if isinstance(expr, IRBinOp):
        return vars_in_expr(expr.left) | vars_in_expr(expr.right)
    if isinstance(expr, IRUnaryOp):
        return vars_in_expr(expr.operand)
    if isinstance(expr, IRCall):
        result: Set[str] = set()
        for arg in expr.args:
            result |= vars_in_expr(arg)
        return result
    return set()


def vars_produced(node: ConstraintNode) -> Set[str]:
    """Return the set of variable names *written* by *node*.

    ``ReturnConstraint`` and ``CondBranchConstraint`` produce no new
    variable bindings visible to the outer scope.
    """
    if isinstance(node, InvariantConstraint):
        return {node.var}
    if isinstance(node, AssignConstraint):
        return {node.var}
    if isinstance(node, ReductionConstraint):
        return {node.accumulator}
    # ReturnConstraint / CondBranchConstraint
    return set()


def vars_consumed(node: ConstraintNode) -> Set[str]:
    """Return the set of variable names *read* by *node*.

    For a ``ReductionConstraint`` the loop variable is locally scoped and is
    therefore *not* treated as a consumed external variable.  Crucially, the
    *accumulator itself* is also **not** treated as consumed: every plan type
    (``ClosedFormPlan``, ``ConstantPlan``, ``LoopPlan``) initialises the
    accumulator from its own ``init_val`` rather than reading the prior value
    from the stack.  This is what allows the goal graph to correctly mark
    earlier ``AssignConstraint(acc, …)`` nodes as dead.
    """
    if isinstance(node, InvariantConstraint):
        return vars_in_expr(node.expr) if node.expr is not None else set()
    if isinstance(node, AssignConstraint):
        return vars_in_expr(node.expr)
    if isinstance(node, ReductionConstraint):
        consumed = (
            vars_in_expr(node.start)
            | vars_in_expr(node.end)
            | vars_in_expr(node.body)
            | vars_in_expr(node.init_val)
        )
        consumed.discard(node.loop_var)    # loop_var is locally scoped
        consumed.discard(node.accumulator) # accumulator is overwritten, not read
        return consumed
    if isinstance(node, ReturnConstraint):
        return vars_in_expr(node.expr)
    if isinstance(node, CondBranchConstraint):
        consumed = vars_in_expr(node.cond)
        for sub in node.then_constraints + node.else_constraints:
            consumed |= vars_consumed(sub)
        return consumed
    return set()


# ---------------------------------------------------------------------------
# GoalGraph
# ---------------------------------------------------------------------------

class GoalGraph:
    """Dependency graph derived from a ``ConstraintGraph``.

    Encodes the *goal* perspective: starting from the function's
    ``ReturnConstraint`` (the root goal), the ``GoalGraph`` identifies which
    constraints are actually *live* (needed) and in what order they must be
    evaluated.

    Parameters
    ----------
    graph:
        The ``ConstraintGraph`` to analyse.

    Examples
    --------
    For ``int sum(int n) { int s = 0; for (...) { s += i; } return s; }``
    the constraint graph contains four nodes::

        [0] InvariantConstraint(n)           produces: n
        [1] AssignConstraint(s, 0)           produces: s  consumes: {}
        [2] ReductionConstraint(s, add, …)   produces: s  consumes: {n}
        [3] ReturnConstraint(s)              produces: {} consumes: {s}

    The last writer of ``s`` is node 2 (ReductionConstraint), not node 1.
    So node 1 is **dead** and excluded from ``live_nodes()``.  The returned
    list is [node 0, node 2, node 3].
    """

    def __init__(self, graph: ConstraintGraph) -> None:
        self._graph = graph
        self._live_ids: Optional[Set[int]] = None

        # last_writer[var] = the constraint that most recently writes 'var'
        # (in program / AST order — later writes shadow earlier ones).
        self._last_writer: Dict[str, ConstraintNode] = {}
        for node in graph.constraints:
            for var in vars_produced(node):
                self._last_writer[var] = node

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def live_nodes(self) -> List[ConstraintNode]:
        """Return the live constraints in original program order.

        A constraint is *live* if its produced variable(s) are consumed
        (directly or transitively) by the ``ReturnConstraint``.
        ``InvariantConstraint`` nodes (function parameters) are always live.

        The list is in the same order as ``graph.constraints``, which is
        already a valid topological schedule for execution.
        """
        if self._live_ids is None:
            self._compute_liveness()
        return [n for n in self._graph.constraints if n.node_id in self._live_ids]

    def is_live(self, node: ConstraintNode) -> bool:
        """Return ``True`` if *node* is live."""
        if self._live_ids is None:
            self._compute_liveness()
        return node.node_id in self._live_ids

    def __repr__(self) -> str:  # pragma: no cover
        if self._live_ids is None:
            self._compute_liveness()
        lines = [f"GoalGraph({self._graph.name!r}):"]
        for node in self._graph.constraints:
            status = "LIVE" if node.node_id in self._live_ids else "dead"
            prod = vars_produced(node)
            cons = vars_consumed(node)
            lines.append(
                f"  [{node.node_id:2d}] {status:4s}  "
                f"produces={prod}  consumes={cons}  {node}"
            )
        return "\n".join(lines)

    # ------------------------------------------------------------------
    # Internal: backward liveness analysis
    # ------------------------------------------------------------------

    def _compute_liveness(self) -> None:
        """Backward reachability from ReturnConstraint → populate _live_ids."""
        graph = self._graph
        live_ids: Set[int] = set()
        seen_vars: Set[str] = set()

        # Seed from every ReturnConstraint (root goals)
        return_nodes = [
            n for n in graph.constraints if isinstance(n, ReturnConstraint)
        ]
        if not return_nodes:
            # No return at all: conservatively mark everything live
            self._live_ids = {n.node_id for n in graph.constraints}
            return

        worklist: List[str] = []
        for rn in return_nodes:
            live_ids.add(rn.node_id)
            for v in vars_consumed(rn):
                if v not in seen_vars:
                    seen_vars.add(v)
                    worklist.append(v)

        # BFS backward through the "last writer" map
        while worklist:
            var = worklist.pop()
            writer = self._last_writer.get(var)
            if writer is None:
                # Variable is a function parameter — handled by InvariantConstraint
                continue
            if writer.node_id in live_ids:
                continue  # already processed
            live_ids.add(writer.node_id)
            for dep_var in vars_consumed(writer):
                if dep_var not in seen_vars:
                    seen_vars.add(dep_var)
                    worklist.append(dep_var)

        # Parameters (InvariantConstraints) are always live
        for node in graph.constraints:
            if isinstance(node, InvariantConstraint):
                live_ids.add(node.node_id)

        self._live_ids = live_ids
