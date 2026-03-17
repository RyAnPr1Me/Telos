"""Tests for the semantic lifter.

The lifter's job is to recognise high-level patterns (especially
accumulation loops) and encode them as ReductionConstraint nodes.
"""

import pytest
from src.parser import parse
from src.lifting.semantic_lift import lift_program
from src.ir.nodes import (
    ReductionConstraint, AssignConstraint, ReturnConstraint,
    InvariantConstraint, IRConst, IRVar,
)


def lift(src: str, fn_name: str = None):
    """Parse *src* and return the ConstraintGraph for *fn_name* (or the only fn)."""
    prog = parse(src)
    graphs = lift_program(prog)
    if fn_name is None:
        assert len(graphs) == 1
        return next(iter(graphs.values()))
    return graphs[fn_name]


# ---------------------------------------------------------------------------
# Parameters → InvariantConstraint
# ---------------------------------------------------------------------------

class TestParameters:
    def test_single_param(self):
        g = lift("int f(int n) { return n; }")
        params = [c for c in g.constraints if isinstance(c, InvariantConstraint)]
        assert len(params) == 1
        assert params[0].var == "n"
        assert params[0].source == "param"

    def test_no_params(self):
        g = lift("int f() { return 0; }")
        params = [c for c in g.constraints if isinstance(c, InvariantConstraint)]
        assert len(params) == 0


# ---------------------------------------------------------------------------
# Simple variable declarations and assignments
# ---------------------------------------------------------------------------

class TestAssignConstraint:
    def test_var_with_init(self):
        g = lift("int f() { int x = 5; return x; }")
        assigns = [c for c in g.constraints if isinstance(c, AssignConstraint)]
        assert any(a.var == "x" for a in assigns)

    def test_assign_stmt(self):
        g = lift("int f() { int x = 0; x = 7; return x; }")
        assigns = [c for c in g.constraints if isinstance(c, AssignConstraint)]
        assert any(a.var == "x" for a in assigns)


# ---------------------------------------------------------------------------
# Reduction detection
# ---------------------------------------------------------------------------

class TestReductionDetection:
    SUM_SRC = """
    int sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i;
        }
        return s;
    }
    """

    def test_sum_produces_reduction(self):
        g = lift(self.SUM_SRC)
        reds = [c for c in g.constraints if isinstance(c, ReductionConstraint)]
        assert len(reds) == 1, "Expected exactly one ReductionConstraint"

    def test_sum_reduction_attributes(self):
        g = lift(self.SUM_SRC)
        red = next(c for c in g.constraints if isinstance(c, ReductionConstraint))
        assert red.accumulator == "s"
        assert red.op == "add"
        assert red.loop_var == "i"

    def test_sum_range_start_zero(self):
        g = lift(self.SUM_SRC)
        red = next(c for c in g.constraints if isinstance(c, ReductionConstraint))
        assert isinstance(red.start, IRConst) and red.start.value == 0

    def test_sum_range_end_is_n(self):
        g = lift(self.SUM_SRC)
        red = next(c for c in g.constraints if isinstance(c, ReductionConstraint))
        assert isinstance(red.end, IRVar) and red.end.name == "n"

    def test_sum_body_is_i(self):
        g = lift(self.SUM_SRC)
        red = next(c for c in g.constraints if isinstance(c, ReductionConstraint))
        assert isinstance(red.body, IRVar) and red.body.name == "i"

    def test_product_reduction(self):
        src = """
        int factorial(int n) {
            int p = 1;
            for (int i = 1; i <= n; i++) {
                p *= i;
            }
            return p;
        }
        """
        g = lift(src)
        reds = [c for c in g.constraints if isinstance(c, ReductionConstraint)]
        assert len(reds) == 1
        assert reds[0].op == "mul"

    def test_body_linear_expression(self):
        src = """
        int f(int n) {
            int s = 0;
            for (int i = 0; i < n; i++) {
                s += 2 * i + 3;
            }
            return s;
        }
        """
        g = lift(src)
        reds = [c for c in g.constraints if isinstance(c, ReductionConstraint)]
        assert len(reds) == 1
        assert reds[0].op == "add"

    def test_multi_stmt_body_not_reduction(self):
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
        g = lift(src)
        reds = [c for c in g.constraints if isinstance(c, ReductionConstraint)]
        # Multi-statement body → fallback, no ReductionConstraint
        assert len(reds) == 0

    def test_inclusive_range(self):
        src = """
        int f(int n) {
            int s = 0;
            for (int i = 1; i <= n; i++) {
                s += i;
            }
            return s;
        }
        """
        g = lift(src)
        reds = [c for c in g.constraints if isinstance(c, ReductionConstraint)]
        assert len(reds) == 1
        # Inclusive: end should be n+1
        from src.ir.nodes import IRBinOp
        assert isinstance(reds[0].end, IRBinOp)
        assert reds[0].end.op == "+"


# ---------------------------------------------------------------------------
# Return constraint
# ---------------------------------------------------------------------------

class TestReturnConstraint:
    def test_return_present(self):
        g = lift("int f() { return 42; }")
        rets = [c for c in g.constraints if isinstance(c, ReturnConstraint)]
        assert len(rets) == 1
        assert isinstance(rets[0].expr, IRConst)
        assert rets[0].expr.value == 42
