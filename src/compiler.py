"""Telos compiler – main entry point.

Usage
-----
As a library::

    from src.compiler import compile_telos, run_telos, compile_telos_c, run_telos_c

    code = '''
    int sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i;
        }
        return s;
    }
    '''

    # Compile via direct x86-64 emission: returns {function_name: machine_code_bytes}
    machine_code = compile_telos(code)
    print(machine_code["sum"].hex())

    # Or compile and immediately execute via native machine code (x86-64 backend)
    funcs = run_telos(code)
    print(funcs['sum'](100))   # → 4950

    # Compile via C: generates C source, compiles with gcc, loads shared library
    c_src = compile_telos_c(code)       # returns {name: C_source_str}
    funcs_c = run_telos_c(code)         # returns {name: ctypes callable}
    print(funcs_c['sum'](100))          # → 4950  (real gcc-compiled machine code)

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
5. **Code generators** – two backends are available:
   a. **x86-64 code generator** (``src.codegen.x86_64_gen``) – emits native
      x86-64 machine code bytes directly from the chosen plans.
   b. **C code generator** (``src.codegen.c_gen``) – emits C source code
      that is then compiled to real machine code by gcc/clang.
6. **Executable wrapper** (``src.codegen.executable``) – maps the machine
   code bytes into read/write/execute memory and wraps them in a ctypes
   callable (used by the x86-64 backend).
"""

from __future__ import annotations

import sys
from typing import Any, Callable, Dict, Optional

from .codegen.c_gen import CGenerator, compile_c_source, generate_c
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


def compile_telos_c(source: str) -> Dict[str, str]:
    """Compile Telos *source* to C source code.

    This backend generates clean, portable C (using ``long long`` for all
    integer values) rather than emitting x86-64 bytes directly.  The C
    source can be compiled to real machine code with any C compiler.

    Parameters
    ----------
    source:
        A string containing one or more Telos function definitions.

    Returns
    -------
    dict
        Maps each function name to its C source code string.
    """
    program = parse(source)
    graphs = lift_program(program)
    planner = Planner()
    gen = CGenerator()

    result: Dict[str, str] = {}
    for name, graph in graphs.items():
        plan = planner.plan_function(graph)
        result[name] = gen.generate(plan)

    return result


def run_telos_c(source: str) -> Dict[str, Callable]:
    """Compile Telos *source* via C and return native callables.

    Generates C source code for each function, compiles it with ``gcc``
    into a shared library, and returns ctypes callables backed by real
    gcc-compiled machine code.

    Parameters
    ----------
    source:
        Telos source code.

    Returns
    -------
    dict
        Maps each function name to a ctypes callable.  The callable
        accepts and returns 64-bit signed integers (``long long``).
    """
    program = parse(source)
    graphs = lift_program(program)
    planner = Planner()
    gen = CGenerator()

    c_sources: Dict[str, str] = {}
    n_params_map: Dict[str, int] = {}
    for fn in program.functions:
        graph = graphs[fn.name]
        plan = planner.plan_function(graph)
        c_sources[fn.name] = gen.generate(plan)
        n_params_map[fn.name] = len(fn.params)

    combined_c = "\n".join(c_sources.values())
    func_names = list(c_sources.keys())
    callables = compile_c_source(combined_c, func_names, n_params_map)

    return {name: callables[name] for name in func_names}


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
