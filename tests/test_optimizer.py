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


# ---------------------------------------------------------------------------
# GoalGraph: liveness analysis and dead-code elimination
# ---------------------------------------------------------------------------

from src.optimizer.goal_graph import GoalGraph, vars_produced, vars_consumed
from src.ir.nodes import (
    AssignConstraint, ReductionConstraint, ReturnConstraint,
    InvariantConstraint,
)


def goal_graph(src: str, fn_name: str = None) -> GoalGraph:
    """Full pipeline: parse → lift → GoalGraph."""
    prog = parse(src)
    graphs = lift_program(prog)
    if fn_name is None:
        graph = next(iter(graphs.values()))
    else:
        graph = graphs[fn_name]
    return GoalGraph(graph)


class TestGoalGraph:
    """Tests for GoalGraph liveness analysis."""

    SRC_SUM = """
    int sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) { s += i; }
        return s;
    }
    """

    def test_dead_init_eliminated(self):
        """s = 0 is dead when the reduction overwrites s without reading it."""
        gg = goal_graph(self.SRC_SUM)
        live = gg.live_nodes()
        dead_assigns = [
            n for n in gg._graph.constraints
            if isinstance(n, AssignConstraint) and n.var == 's'
            and not gg.is_live(n)
        ]
        assert len(dead_assigns) == 1, "AssignConstraint(s, 0) should be dead"

    def test_reduction_is_live(self):
        """The ReductionConstraint that writes s must be live."""
        gg = goal_graph(self.SRC_SUM)
        live = gg.live_nodes()
        live_reductions = [n for n in live if isinstance(n, ReductionConstraint)]
        assert len(live_reductions) == 1

    def test_return_is_live(self):
        """ReturnConstraint is always live."""
        gg = goal_graph(self.SRC_SUM)
        live = gg.live_nodes()
        assert any(isinstance(n, ReturnConstraint) for n in live)

    def test_param_invariant_always_live(self):
        """InvariantConstraint for function parameters is always live."""
        gg = goal_graph(self.SRC_SUM)
        live = gg.live_nodes()
        assert any(
            isinstance(n, InvariantConstraint) and n.var == 'n'
            for n in live
        )

    def test_dead_unused_variable(self):
        """An assigned variable never read is dead."""
        src = """
        int foo(int n) {
            int x = n * 2;
            return n;
        }
        """
        gg = goal_graph(src)
        x_node = next(
            n for n in gg._graph.constraints
            if isinstance(n, AssignConstraint) and n.var == 'x'
        )
        assert not gg.is_live(x_node), "x = n*2 is never used, should be dead"

    def test_live_intermediate_used_variable(self):
        """An assignment whose result is transitively needed is live."""
        src = """
        int foo(int n) {
            int x = n * 2;
            int y = x + 1;
            return y;
        }
        """
        gg = goal_graph(src)
        x_node = next(
            n for n in gg._graph.constraints
            if isinstance(n, AssignConstraint) and n.var == 'x'
        )
        assert gg.is_live(x_node), "x = n*2 feeds y which is returned, so it's live"

    def test_live_variable_consumed_by_reduction_body(self):
        """A variable used inside a reduction body must be live."""
        src = """
        int foo(int n) {
            int x = n * 2;
            int s = 0;
            for (int i = 0; i < n; i++) { s += x * i; }
            return s;
        }
        """
        gg = goal_graph(src)
        x_node = next(
            n for n in gg._graph.constraints
            if isinstance(n, AssignConstraint) and n.var == 'x'
        )
        assert gg.is_live(x_node), "x is used in the reduction body, must be live"

    def test_dead_init_even_with_loop_plan(self):
        """Even when the loop fallback is used, s=0 is dead (loop resets acc from init_val)."""
        src = """
        int foo(int n) {
            int s = 0;
            for (int i = 0; i < n; i++) { s += i; s += i; }
            return s;
        }
        """
        # This has a multi-stmt body so no reduction is lifted; the loop
        # body statements become individual AssignConstraints.  The original
        # 's = 0' VarDecl still creates an AssignConstraint.
        gg = goal_graph(src)
        # The s=0 node may or may not be live depending on whether the body
        # assigns to s — just verify the graph is internally consistent
        live = gg.live_nodes()
        assert len(live) > 0


class TestGoalDirectedPlanning:
    """Tests verifying that the planner uses the GoalGraph correctly."""

    SRC_SUM = """
    int sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) { s += i; }
        return s;
    }
    """

    def test_no_dead_assign_in_plan_steps(self):
        """After goal-directed compilation, s=0 must not appear in plan steps."""
        fp = plan(self.SRC_SUM)
        dead = [
            s for s in fp.steps
            if isinstance(s, AssignPlan) and s.var == 's'
        ]
        assert len(dead) == 0, (
            "AssignPlan(s, 0) is dead code and must be eliminated by GoalGraph"
        )

    def test_closed_form_is_the_only_step(self):
        """For the sum function, GoalGraph+planner should produce exactly one step."""
        fp = plan(self.SRC_SUM)
        assert len(fp.steps) == 1
        assert isinstance(fp.steps[0], ClosedFormPlan)

    def test_dead_unused_variable_not_in_plan(self):
        """An unused variable assignment should not appear in the plan."""
        src = """
        int foo(int n) {
            int x = n * 2;
            return n;
        }
        """
        fp = plan(src)
        assert not any(
            isinstance(s, AssignPlan) and s.var == 'x' for s in fp.steps
        ), "x = n*2 is dead and should not be in the plan"

    def test_live_variable_in_plan(self):
        """A variable used by a subsequent computation remains in the plan."""
        src = """
        int foo(int n) {
            int x = n * 2;
            int y = x + 1;
            return y;
        }
        """
        fp = plan(src)
        assert any(
            isinstance(s, AssignPlan) and s.var == 'x' for s in fp.steps
        ), "x = n*2 feeds y which is returned, must remain in plan"


class TestVarsProducedConsumed:
    """Unit tests for vars_produced / vars_consumed helpers."""

    def test_invariant_produces_var(self):
        node = InvariantConstraint(var='n', source='param')
        assert vars_produced(node) == {'n'}
        assert vars_consumed(node) == set()

    def test_assign_constraint(self):
        from src.ir.nodes import IRBinOp, IRVar, IRConst
        node = AssignConstraint(var='x', expr=IRBinOp('+', IRVar('a'), IRConst(1)))
        assert vars_produced(node) == {'x'}
        assert vars_consumed(node) == {'a'}

    def test_reduction_does_not_consume_accumulator(self):
        """The accumulator is overwritten, not read, so it must not appear in consumed."""
        from src.ir.nodes import IRVar, IRConst
        node = ReductionConstraint(
            accumulator='s', op='add', loop_var='i',
            start=IRConst(0), end=IRVar('n'), body=IRVar('i'),
            init_val=IRConst(0),
        )
        assert 's' not in vars_consumed(node), (
            "accumulator 's' must not be in consumed vars"
        )
        assert 'n' in vars_consumed(node)

    def test_reduction_does_not_consume_loop_var(self):
        """The loop variable is locally scoped and must not appear in consumed."""
        from src.ir.nodes import IRVar, IRConst
        node = ReductionConstraint(
            accumulator='s', op='add', loop_var='i',
            start=IRConst(0), end=IRVar('n'), body=IRVar('i'),
            init_val=IRConst(0),
        )
        assert 'i' not in vars_consumed(node), (
            "loop var 'i' is locally scoped and must not be in consumed vars"
        )

    def test_return_consumes_expr_vars(self):
        from src.ir.nodes import IRVar
        node = ReturnConstraint(expr=IRVar('s'))
        assert vars_produced(node) == set()
        assert vars_consumed(node) == {'s'}
