"""Constraint graph data structure."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List

from .nodes import ConstraintNode, InvariantConstraint


@dataclass
class ConstraintGraph:
    """Represents a single function as a graph of semantic constraints.

    Nodes encode *what* must be computed; edges (implicit through shared
    variable names) encode data dependencies.  The execution order is not
    fixed – the optimizer decides how to schedule/implement each node.
    """

    name: str
    params: List[str]
    constraints: List[ConstraintNode] = field(default_factory=list)
    _next_id: int = field(default=0, repr=False)

    def add(self, node: ConstraintNode) -> ConstraintNode:
        """Append a node to the graph, assigning it a fresh id."""
        node.node_id = self._next_id
        self._next_id += 1
        self.constraints.append(node)
        return node

    def param_invariants(self) -> List[InvariantConstraint]:
        """Return all parameter invariant nodes."""
        return [
            c for c in self.constraints
            if isinstance(c, InvariantConstraint) and c.source == "param"
        ]

    def __repr__(self) -> str:
        lines = [f"ConstraintGraph({self.name!r}, params={self.params}):"]
        for c in self.constraints:
            lines.append(f"  [{c.node_id:2d}] {c}")
        return "\n".join(lines)
