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

    # Compile: returns {function_name: machine_code_bytes}
    machine_code = compile_telos(code)
    print(machine_code["sum"].hex())

    # Or compile and immediately execute via native machine code
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
5. **x86-64 code generator** (``src.codegen.x86_64_gen``) – emits native
   x86-64 machine code bytes from the chosen plans.
6. **Executable wrapper** (``src.codegen.executable``) – maps the machine
   code bytes into read/write/execute memory and wraps them in a ctypes
   callable.
"""

from __future__ import annotations

import sys
from typing import Any, Callable, Dict, Optional

from .codegen.executable import NativeFunction
from .codegen.x86_64_gen import X86_64Generator
from .lifting.semantic_lift import lift_program
from .optimizer.planner import Planner
from .parser import parse


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def compile_telos(source: str) -> Dict[str, bytes]:
    """Compile Telos *source* to x86-64 machine code.

    Parameters
    ----------
    source:
        A string containing one or more Telos function definitions.

    Returns
    -------
    dict
        Maps each function name to its raw x86-64 machine code bytes.
    """
    program = parse(source)
    graphs = lift_program(program)
    planner = Planner()
    gen = X86_64Generator()

    result: Dict[str, bytes] = {}
    for name, graph in graphs.items():
        plan = planner.plan_function(graph)
        result[name] = gen.generate(plan)

    return result


def run_telos(
    source: str,
) -> Dict[str, Callable]:
    """Compile Telos *source* and return native callables.

    Each returned function is backed by x86-64 machine code loaded into
    executable memory.  It accepts and returns 64-bit signed integers.

    Parameters
    ----------
    source:
        Telos source code.

    Returns
    -------
    dict
        Maps each function name to a :class:`NativeFunction` callable.
    """
    program = parse(source)
    machine_code = compile_telos(source)

    funcs: Dict[str, Callable] = {}
    for fn in program.functions:
        n_params = len(fn.params)
        code = machine_code[fn.name]
        funcs[fn.name] = NativeFunction(code, n_params)

    return funcs


def compile_and_show(source: str) -> None:
    """Compile *source* and print a hex dump of each function's machine code."""
    machine_code = compile_telos(source)
    for name, code in machine_code.items():
        print("=" * 60)
        print(f"Function: {name}  ({len(code)} bytes of x86-64 machine code)")
        print("=" * 60)
        # Print hex dump: 16 bytes per line
        for i in range(0, len(code), 16):
            chunk = code[i : i + 16]
            hex_part = " ".join(f"{b:02x}" for b in chunk)
            print(f"  {i:04x}  {hex_part}")
        print()


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
