"""Cost model for comparing execution plans.

Plans are scored so that the optimizer can select the cheapest valid
strategy.  Lower score = better.
"""

from __future__ import annotations

from typing import Iterable

from .plans import ExecutionPlan

# Map from cost-class string to numeric score
_SCORES: dict = {
    "O(1) constant": 0,
    "O(1)": 1,
    "O(log n)": 10,
    "O(n)": 100,
    "O(n log n)": 500,
    "O(n^2)": 10_000,
}


def plan_cost(plan: ExecutionPlan) -> int:
    """Return a numeric cost estimate for *plan* (lower is better)."""
    cost_class = getattr(plan, "cost_class", "O(n)")
    return _SCORES.get(cost_class, 100)


def cheapest(plans: Iterable[ExecutionPlan]) -> ExecutionPlan:
    """Return the plan with the lowest cost from *plans*.

    Raises ``ValueError`` if the iterable is empty.
    """
    return min(plans, key=plan_cost)
