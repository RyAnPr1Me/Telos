"""Telos compiler – main entry point.

Usage
-----
As a library::

    from src.compiler import compile_telos, run_telos

    code = '''
    int sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i;
        }
        return s;
    }
    '''

    # Compile and get the generated Python source
    source = compile_telos(code)
    print(source)

    # Or compile and immediately execute
    funcs = run_telos(code)
    print(funcs['sum'](100))   # → 4950

As a CLI::

    python -m src.compiler examples/sum.telos

Architecture overview
---------------------
1. **Lexer** (``src.lexer``) – tokenises the source string.
2. **Parser** (``src.parser``) – builds a concrete AST.
3. **Semantic lifter** (``src.lifting.semantic_lift``) – converts the AST
   into a Constraint Graph, detecting high-level patterns such as
   accumulation loops.
4. **Planner / optimizer** (``src.optimizer.planner``) – for each constraint
   node it generates candidate execution plans (loop, closed-form formula,
   compile-time constant) and selects the cheapest valid one.
5. **Code generator** (``src.codegen.python_gen``) – emits clean Python
   source from the chosen plans.
"""

from __future__ import annotations

import sys
from typing import Any, Callable, Dict

from .codegen.python_gen import generate_python, PythonGenerator
from .lifting.semantic_lift import lift_program
from .optimizer.planner import Planner
from .parser import parse


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def compile_telos(source: str) -> str:
    """Compile Telos *source* to a Python source string.

    Parameters
    ----------
    source:
        A string containing one or more Telos function definitions.

    Returns
    -------
    str
        Python source code that defines equivalent functions.
    """
    program = parse(source)
    graphs = lift_program(program)
    planner = Planner()
    gen = PythonGenerator()

    parts = []
    for name, graph in graphs.items():
        plan = planner.plan_function(graph)
        parts.append(gen.generate(plan))

    return "\n".join(parts)


def run_telos(
    source: str,
    globals_dict: Optional[Dict[str, Any]] = None,
) -> Dict[str, Callable]:
    """Compile and execute Telos *source*, returning all defined functions.

    Parameters
    ----------
    source:
        Telos source code.
    globals_dict:
        Optional namespace to use.  If omitted, a fresh dict is used.

    Returns
    -------
    dict
        Maps each function name to the compiled Python callable.
    """
    python_src = compile_telos(source)
    ns: Dict[str, Any] = globals_dict if globals_dict is not None else {}
    exec(python_src, ns)  # noqa: S102

    program = parse(source)
    return {fn.name: ns[fn.name] for fn in program.functions}


def compile_and_show(source: str) -> None:
    """Compile *source* and print the generated Python code with a header."""
    python_src = compile_telos(source)
    print("=" * 60)
    print("Generated Python:")
    print("=" * 60)
    print(python_src)


# ---------------------------------------------------------------------------
# Optional type import (Python 3.9 compat)
# ---------------------------------------------------------------------------

try:
    from typing import Optional
except ImportError:
    Optional = None  # type: ignore


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _cli() -> None:
    """Simple command-line interface: ``python -m src.compiler <file.telos>``."""
    if len(sys.argv) < 2:
        print("Usage: python -m src.compiler <file.telos>", file=sys.stderr)
        sys.exit(1)

    path = sys.argv[1]
    try:
        with open(path, "r", encoding="utf-8") as fh:
            source = fh.read()
    except OSError as exc:
        print(f"Error reading {path!r}: {exc}", file=sys.stderr)
        sys.exit(1)

    compile_and_show(source)


if __name__ == "__main__":
    _cli()
