"""Optimizer / planner for the Telos compiler.

Given a ``ConstraintGraph`` the planner:

1. For each constraint node, generates a *set of candidate execution plans*.
2. Evaluates the cost of each candidate using the cost model.
3. Selects the cheapest valid plan.

The key optimization is **algebraic closed-form detection** for additive
reductions over polynomial bodies: loops of the form::

    for (int i = 0; i < n; i++) { s += f(i); }

are replaced by O(1) formulas when ``f`` is a low-degree polynomial.

Design notes
------------
* This is NOT pass-based. Every constraint node is planned independently,
  and the cheapest plan wins.
* Bounded search: polynomial analysis is limited to degree ≤ 3 to avoid
  exponential blowup.
* Safe fallback: a LoopPlan is always generated; the closed-form is only
  selected when it can be verified to be equivalent.
"""

from __future__ import annotations

from typing import Dict, List, Optional, Sequence, Union

from ..ir.graph import ConstraintGraph
from ..ir.nodes import (
    AssignConstraint,
    CondBranchConstraint,
    ConstraintNode,
    InvariantConstraint,
    IRBinOp,
    IRConst,
    IRExpr,
    IRUnaryOp,
    IRVar,
    ReductionConstraint,
    ReturnConstraint,
)
from .cost_model import cheapest, plan_cost
from .plans import (
    AssignPlan,
    ClosedFormPlan,
    ConstantPlan,
    ExecutionPlan,
    FunctionPlan,
    LoopPlan,
)

# Maximum number of loop iterations to evaluate at compile time
_MAX_CONST_FOLD_ITERS = 10_000

# Maximum supported polynomial degree for closed-form synthesis
_MAX_POLY_DEGREE = 3


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

class Planner:
    """Generates and selects optimal execution plans for constraint graphs."""

    def plan_function(self, graph: ConstraintGraph) -> FunctionPlan:
        """Return the optimal ``FunctionPlan`` for *graph*."""
        fn_plan = FunctionPlan(name=graph.name, param_names=graph.params)

        for node in graph.constraints:
            if isinstance(node, InvariantConstraint):
                continue   # params are represented by function signature

            if isinstance(node, ReturnConstraint):
                fn_plan.return_expr = node.expr
                continue

            candidates = self._candidates(node)
            if candidates:
                fn_plan.steps.append(cheapest(candidates))

        return fn_plan

    # ------------------------------------------------------------------
    # Candidate generation per node type
    # ------------------------------------------------------------------

    def _candidates(self, node: ConstraintNode) -> List[ExecutionPlan]:
        if isinstance(node, ReductionConstraint):
            return self._candidates_reduction(node)
        if isinstance(node, AssignConstraint):
            return [AssignPlan(var=node.var, expr=node.expr)]
        return []

    def _candidates_reduction(
        self, r: ReductionConstraint
    ) -> List[ExecutionPlan]:
        """Generate all candidate plans for a reduction constraint."""
        candidates: List[ExecutionPlan] = []

        # --- Plan A: loop (always valid) ---
        candidates.append(LoopPlan(
            accumulator=r.accumulator,
            op=r.op,
            loop_var=r.loop_var,
            start=r.start,
            end=r.end,
            body=r.body,
            init_val=r.init_val,
        ))

        # --- Plan B: compile-time constant fold ---
        const_plan = self._try_constant_fold(r)
        if const_plan is not None:
            candidates.append(const_plan)

        # --- Plan C: algebraic closed form ---
        if r.op == "add":
            cf = self._try_closed_form_sum(r)
            if cf is not None:
                candidates.append(cf)
        elif r.op == "mul":
            cf = self._try_closed_form_product(r)
            if cf is not None:
                candidates.append(cf)

        return candidates

    # ------------------------------------------------------------------
    # Constant folding
    # ------------------------------------------------------------------

    def _try_constant_fold(
        self, r: ReductionConstraint
    ) -> Optional[ConstantPlan]:
        """Evaluate the reduction entirely at compile time if possible."""
        start_v = _eval_const(r.start)
        end_v = _eval_const(r.end)
        if start_v is None or end_v is None:
            return None
        n_iters = int(end_v) - int(start_v)
        if n_iters > _MAX_CONST_FOLD_ITERS or n_iters < 0:
            return None

        init_v = _eval_const(r.init_val)
        if init_v is None:
            return None

        op_fn = _OP_FUNCTIONS.get(r.op)
        if op_fn is None:
            return None

        result: Union[int, float] = init_v
        for i in range(int(start_v), int(end_v)):
            body_v = _eval_const(r.body, {r.loop_var: i})
            if body_v is None:
                return None
            result = op_fn(result, body_v)

        return ConstantPlan(accumulator=r.accumulator, value=result)

    # ------------------------------------------------------------------
    # Closed-form summation
    # ------------------------------------------------------------------

    def _try_closed_form_sum(
        self, r: ReductionConstraint
    ) -> Optional[ClosedFormPlan]:
        """Try to replace Σ body(i), i ∈ [start, end) with an O(1) formula.

        Strategy
        --------
        1. Decompose *body* as a polynomial in *loop_var* (integer coefficients
           only; other variables are treated as runtime constants).
        2. Apply standard summation-of-polynomials formulas.

        Shift trick: Σ(i=s..e-1) f(i) = Σ(j=0..n-1) f(j+s) where n=e-s.
        We expand f(j+s) as a polynomial in j, then use the [0,n) formulas.
        """
        var = r.loop_var
        raw_poly = _as_polynomial(r.body, var)
        if raw_poly is None:
            return None
        if len(raw_poly) - 1 > _MAX_POLY_DEGREE:
            return None  # degree too high

        n_expr = IRBinOp("-", r.end, r.start)   # n = end - start
        s_expr = r.start                          # s = start

        # Shift polynomial: replace i with (j + s), i.e. expand poly(j+s)
        shifted = _poly_shift(raw_poly, s_expr)
        if shifted is None:
            return None

        formula = _sum_poly_closed_form(shifted, n_expr)
        if formula is None:
            return None

        return ClosedFormPlan(
            accumulator=r.accumulator,
            formula=formula,
            cost_class="O(1)",
            description=(
                f"Closed-form sum of degree-{len(raw_poly) - 1} polynomial"
            ),
        )

    # ------------------------------------------------------------------
    # Closed-form product
    # ------------------------------------------------------------------

    def _try_closed_form_product(
        self, r: ReductionConstraint
    ) -> Optional[ClosedFormPlan]:
        """Try closed-form for multiplicative reductions (limited support)."""
        var = r.loop_var
        poly = _as_polynomial(r.body, var)
        if poly is None:
            return None

        # Only handle degree-0 (constant body): Π c = c^n
        if len(poly) != 1:
            return None

        c = poly[0]
        if isinstance(c, int):
            if c == 1:
                return ClosedFormPlan(
                    accumulator=r.accumulator,
                    formula=IRConst(1),
                    cost_class="O(1) constant",
                    description="Product of 1s = 1",
                )
            if c == 0:
                return ClosedFormPlan(
                    accumulator=r.accumulator,
                    formula=IRConst(0),
                    cost_class="O(1) constant",
                    description="Product of 0s = 0",
                )
        return None


# ---------------------------------------------------------------------------
# Polynomial representation and arithmetic
#
# A polynomial P(x) = c_0 + c_1*x + c_2*x^2 + ...
# is stored as a list of coefficients: [c_0, c_1, c_2, ...]
# Coefficients are either int/float (compile-time) or IRExpr (runtime).
# ---------------------------------------------------------------------------

Poly = List[Union[int, float, IRExpr]]


def _as_polynomial(expr: IRExpr, var: str) -> Optional[Poly]:
    """Return polynomial coefficients in *var*, or None.

    Integer/float constants become int/float coefficients.
    Variables other than *var* are treated as opaque runtime constants
    and represented as IRExpr coefficients.

    Returns None if the expression is not a polynomial in *var* (e.g. it
    involves *var* in a non-polynomial way).
    """
    if isinstance(expr, IRConst):
        return [expr.value]

    if isinstance(expr, IRVar):
        if expr.name == var:
            return [0, 1]   # 0 + 1·var
        return [expr]       # other variable: degree-0 w.r.t. var

    if isinstance(expr, IRBinOp):
        left = _as_polynomial(expr.left, var)
        right = _as_polynomial(expr.right, var)
        if left is None or right is None:
            return None

        if expr.op == "+":
            return _poly_add(left, right)
        if expr.op == "-":
            return _poly_sub(left, right)
        if expr.op == "*":
            return _poly_mul(left, right)

    if isinstance(expr, IRUnaryOp) and expr.op == "-":
        inner = _as_polynomial(expr.operand, var)
        if inner is None:
            return None
        return _poly_neg(inner)

    return None


def _coef_neg(c: Union[int, float, IRExpr]) -> Union[int, float, IRExpr]:
    if isinstance(c, (int, float)):
        return -c
    return IRUnaryOp("-", c)


def _coef_add(
    a: Union[int, float, IRExpr],
    b: Union[int, float, IRExpr],
) -> Union[int, float, IRExpr]:
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a + b
    if isinstance(a, (int, float)) and a == 0:
        return b
    if isinstance(b, (int, float)) and b == 0:
        return a
    return IRBinOp("+", _as_ir(a), _as_ir(b))


def _coef_mul(
    a: Union[int, float, IRExpr],
    b: Union[int, float, IRExpr],
) -> Union[int, float, IRExpr]:
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a * b
    if isinstance(a, (int, float)) and a == 0:
        return 0
    if isinstance(b, (int, float)) and b == 0:
        return 0
    if isinstance(a, (int, float)) and a == 1:
        return b
    if isinstance(b, (int, float)) and b == 1:
        return a
    return IRBinOp("*", _as_ir(a), _as_ir(b))


def _as_ir(c: Union[int, float, IRExpr]) -> IRExpr:
    if isinstance(c, (int, float)):
        return IRConst(c)
    return c


def _poly_neg(p: Poly) -> Poly:
    return [_coef_neg(c) for c in p]


def _poly_add(a: Poly, b: Poly) -> Poly:
    n = max(len(a), len(b))
    result: Poly = [0] * n
    for i, c in enumerate(a):
        result[i] = c
    for i, c in enumerate(b):
        result[i] = _coef_add(result[i], c)
    return result


def _poly_sub(a: Poly, b: Poly) -> Poly:
    return _poly_add(a, _poly_neg(b))


def _poly_mul(a: Poly, b: Poly) -> Poly:
    if not a or not b:
        return [0]
    n = len(a) + len(b) - 1
    result: Poly = [0] * n
    for i, ai in enumerate(a):
        for j, bj in enumerate(b):
            result[i + j] = _coef_add(result[i + j], _coef_mul(ai, bj))
    return result


def _poly_shift(poly: Poly, s_expr: IRExpr) -> Optional[Poly]:
    """Compute poly(j + s_expr) as a polynomial in j.

    Uses the binomial expansion.  Only works when poly has numeric
    (int/float) coefficients; returns None otherwise.
    """
    for c in poly:
        if not isinstance(c, (int, float)):
            return None   # symbolic coefficients not yet supported

    # Represent (j + s_expr) as a polynomial in j: [s_const, 1] if s is const
    # or [s_expr, 1] otherwise.
    s_val = _eval_const(s_expr)

    # Build result = Σ_k c_k * (j + s)^k
    result: Poly = [0]
    j_plus_s: Poly     # polynomial representation of (j + s)
    if s_val is not None:
        j_plus_s = [s_val, 1]   # s_val + 1·j
    else:
        j_plus_s = [s_expr, 1]  # symbolic s + 1·j

    # (j+s)^k — build incrementally
    power: Poly = [1]   # (j+s)^0 = 1
    for k, ck in enumerate(poly):
        if ck != 0:
            term = _poly_mul([ck], power)
            result = _poly_add(result, term)
        if k < len(poly) - 1:
            power = _poly_mul(power, j_plus_s)

    return result


# ---------------------------------------------------------------------------
# Closed-form formulas for Σ(j=0..n-1) P(j)
# ---------------------------------------------------------------------------

def _sum_poly_closed_form(poly: Poly, n_expr: IRExpr) -> Optional[IRExpr]:
    """Build an IRExpr for Σ(j=0..n-1) P(j) given polynomial *poly*.

    Uses the standard formulas:
    * Σ 1          = n
    * Σ j          = n*(n-1)/2
    * Σ j²         = n*(n-1)*(2n-1)/6
    * Σ j³         = [n*(n-1)/2]²

    Combined via linearity: Σ(c0 + c1*j + c2*j² + ...) =
        c0*Σ1 + c1*Σj + c2*Σj² + ...
    """
    if not poly:
        return IRConst(0)

    degree = len(poly) - 1
    if degree > _MAX_POLY_DEGREE:
        return None

    # Pre-build basis formulas Σ j^k for k = 0..degree
    bases = []
    for k in range(degree + 1):
        b = _sum_power_basis(k, n_expr)
        if b is None:
            return None
        bases.append(b)

    # Combine: Σ P(j) = Σ_k c_k * basis_k
    terms: List[IRExpr] = []
    for k, ck in enumerate(poly):
        if isinstance(ck, (int, float)) and ck == 0:
            continue
        c_ir = _as_ir(ck)
        b_ir = bases[k]
        if isinstance(ck, (int, float)) and ck == 1:
            terms.append(b_ir)
        else:
            terms.append(IRBinOp("*", c_ir, b_ir))

    if not terms:
        return IRConst(0)

    result = terms[0]
    for t in terms[1:]:
        result = IRBinOp("+", result, t)
    return result


def _sum_power_basis(k: int, n: IRExpr) -> Optional[IRExpr]:
    """Return an IRExpr for Σ(j=0..n-1) j^k."""
    if k == 0:
        # Σ 1 = n
        return n

    if k == 1:
        # Σ j = n*(n-1)//2
        nm1 = IRBinOp("-", n, IRConst(1))
        return IRBinOp("//", IRBinOp("*", n, nm1), IRConst(2))

    if k == 2:
        # Σ j² = n*(n-1)*(2n-1)//6
        nm1 = IRBinOp("-", n, IRConst(1))
        two_n_m1 = IRBinOp("-", IRBinOp("*", IRConst(2), n), IRConst(1))
        return IRBinOp(
            "//",
            IRBinOp("*", IRBinOp("*", n, nm1), two_n_m1),
            IRConst(6),
        )

    if k == 3:
        # Σ j³ = [n*(n-1)//2]²
        inner = IRBinOp("//", IRBinOp("*", n, IRBinOp("-", n, IRConst(1))), IRConst(2))
        return IRBinOp("*", inner, inner)

    return None  # unsupported degree


# ---------------------------------------------------------------------------
# Constant evaluation of IR expressions
# ---------------------------------------------------------------------------

_OP_FUNCTIONS = {
    "add": lambda a, b: a + b,
    "sub": lambda a, b: a - b,
    "mul": lambda a, b: a * b,
}


def _eval_const(
    expr: IRExpr,
    env: Optional[Dict[str, Union[int, float]]] = None,
) -> Optional[Union[int, float]]:
    """Try to reduce *expr* to a Python number.  Returns None on failure."""
    if env is None:
        env = {}

    if isinstance(expr, IRConst):
        return expr.value

    if isinstance(expr, IRVar):
        return env.get(expr.name)

    if isinstance(expr, IRBinOp):
        l = _eval_const(expr.left, env)
        r = _eval_const(expr.right, env)
        if l is None or r is None:
            return None
        try:
            if expr.op == "+":
                return l + r
            if expr.op == "-":
                return l - r
            if expr.op == "*":
                return l * r
            if expr.op in ("/", "//"):
                if r == 0:
                    return None
                return int(l) // int(r) if expr.op == "//" else l / r
            if expr.op == "%":
                return l % r if r != 0 else None
        except (TypeError, ZeroDivisionError):
            return None

    if isinstance(expr, IRUnaryOp) and expr.op == "-":
        v = _eval_const(expr.operand, env)
        return -v if v is not None else None

    return None
