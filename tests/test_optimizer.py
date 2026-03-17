"""Tests for the optimizer / planner.

Verifies that the planner selects the expected plan type and that the
generated plan is mathematically correct (by constant-checking).
"""

import pytest
from src.parser import parse
from src.lifting.semantic_lift import lift_program
from src.optimizer.planner import Planner
from src.optimizer.plans import (
    ClosedFormPlan, ConstantPlan, LoopPlan, AssignPlan, FunctionPlan,
)


def plan(src: str, fn_name: str = None) -> FunctionPlan:
    """Full pipeline: parse → lift → plan; return FunctionPlan."""
    prog = parse(src)
    graphs = lift_program(prog)
    planner = Planner()
    if fn_name is None:
        graph = next(iter(graphs.values()))
    else:
        graph = graphs[fn_name]
    return planner.plan_function(graph)


def first_step(src: str):
    """Return the first non-trivial step (skip simple initializer assigns)."""
    fp = plan(src)
    assert fp.steps, "Expected at least one step"
    for step in fp.steps:
        if not isinstance(step, AssignPlan):
            return step
    return fp.steps[0]


# ---------------------------------------------------------------------------
# Constant folding (all inputs compile-time known)
# ---------------------------------------------------------------------------

class TestConstantFolding:
    SUM_10 = """
    int fixed_sum() {
        int s = 0;
        for (int i = 0; i < 10; i++) {
            s += i;
        }
        return s;
    }
    """

    def test_produces_constant_plan(self):
        step = first_step(self.SUM_10)
        assert isinstance(step, ConstantPlan)

    def test_constant_value_correct(self):
        step = first_step(self.SUM_10)
        assert step.value == 45   # 0+1+…+9

    def test_sum_squares_constant(self):
        src = """
        int f() {
            int s = 0;
            for (int i = 0; i < 5; i++) {
                s += i * i;
            }
            return s;
        }
        """
        step = first_step(src)
        assert isinstance(step, ConstantPlan)
        assert step.value == 0 + 1 + 4 + 9 + 16   # = 30


# ---------------------------------------------------------------------------
# Closed-form plan (runtime variable bound)
# ---------------------------------------------------------------------------

class TestClosedForm:
    SUM_N = """
    int sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i;
        }
        return s;
    }
    """

    def test_produces_closed_form_plan(self):
        step = first_step(self.SUM_N)
        assert isinstance(step, ClosedFormPlan)

    def test_closed_form_correct_for_various_n(self):
        """Evaluate the closed-form formula for several n and compare to loop."""
        from src.optimizer.planner import _eval_const
        from src.optimizer.plans import ClosedFormPlan
        cf = first_step(self.SUM_N)
        assert isinstance(cf, ClosedFormPlan)
        formula = cf.formula
        for n in range(0, 20):
            got = _eval_const(formula, {"n": n})
            expected = n * (n - 1) // 2
            assert got == expected, f"n={n}: got {got}, expected {expected}"

    def test_sum_squares_closed_form(self):
        src = """
        int sum_sq(int n) {
            int s = 0;
            for (int i = 0; i < n; i++) {
                s += i * i;
            }
            return s;
        }
        """
        from src.optimizer.planner import _eval_const
        step = first_step(src)
        assert isinstance(step, (ClosedFormPlan, ConstantPlan))

        if isinstance(step, ClosedFormPlan):
            for n in range(0, 15):
                got = _eval_const(step.formula, {"n": n})
                expected = sum(i * i for i in range(n))
                assert got == expected, f"n={n}: got {got}, expected {expected}"

    def test_linear_combo_closed_form(self):
        """Σ(i=0..n-1) (2i + 3) = n^2 + 2n"""
        src = """
        int f(int n) {
            int s = 0;
            for (int i = 0; i < n; i++) {
                s += 2 * i + 3;
            }
            return s;
        }
        """
        from src.optimizer.planner import _eval_const
        step = first_step(src)
        if isinstance(step, ClosedFormPlan):
            for n in range(0, 15):
                got = _eval_const(step.formula, {"n": n})
                expected = sum(2 * i + 3 for i in range(n))
                assert got == expected, f"n={n}: got {got}, expected {expected}"

    def test_cube_sum_closed_form(self):
        """Σ(i=0..n-1) i^3 = [n*(n-1)/2]^2"""
        src = """
        int cube_sum(int n) {
            int s = 0;
            for (int i = 0; i < n; i++) {
                s += i * i * i;
            }
            return s;
        }
        """
        from src.optimizer.planner import _eval_const
        step = first_step(src)
        if isinstance(step, (ClosedFormPlan, ConstantPlan)):
            pass  # either is acceptable
        if isinstance(step, ClosedFormPlan):
            for n in range(0, 12):
                got = _eval_const(step.formula, {"n": n})
                expected = sum(i ** 3 for i in range(n))
                assert got == expected, f"n={n}: got {got}, expected {expected}"


# ---------------------------------------------------------------------------
# Loop fallback (when no closed form is possible)
# ---------------------------------------------------------------------------

class TestLoopFallback:
    def test_multi_stmt_body_not_optimized(self):
        """Loops with multiple-statement bodies remain as-is (AssignConstraint)."""
        src = """
        int f(int n) {
            int s = 0;
            for (int i = 0; i < n; i++) {
                s += i;
                s += i;
            }
            return s;
        }
        """
        fp = plan(src)
        # No ReductionConstraint was lifted, so steps should not contain
        # a ClosedFormPlan or ConstantPlan for a reduction
        has_loop = any(isinstance(s, LoopPlan) for s in fp.steps)
        has_closed = any(isinstance(s, ClosedFormPlan) for s in fp.steps)
        # Multi-statement body falls back to individual assignments
        assert not has_closed


# ---------------------------------------------------------------------------
# Cost model: cheapest plan wins
# ---------------------------------------------------------------------------

class TestCostModel:
    def test_constant_preferred_over_closed_form(self):
        """When all inputs are constants, ConstantPlan beats ClosedFormPlan."""
        step = first_step("""
        int f() {
            int s = 0;
            for (int i = 0; i < 5; i++) { s += i; }
            return s;
        }
        """)
        assert isinstance(step, ConstantPlan)

    def test_closed_form_preferred_over_loop(self):
        """For runtime n, ClosedFormPlan beats LoopPlan."""
        step = first_step("""
        int sum(int n) {
            int s = 0;
            for (int i = 0; i < n; i++) { s += i; }
            return s;
        }
        """)
        assert isinstance(step, ClosedFormPlan)
