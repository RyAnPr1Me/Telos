"""C source code generator for the Telos compiler.

Translates a ``FunctionPlan`` (produced by the optimizer) into a C source
string that can be compiled to real machine code with ``gcc`` or ``clang``.

All variables are typed as ``long long`` (64-bit signed integer) to match
the semantics used by the x86-64 backend and the System V AMD64 ABI.

The generated code is clean, portable C — no runtime required beyond libc.

* A ``ConstantPlan`` becomes a ``long long var = <literal>;`` declaration.
* A ``ClosedFormPlan`` becomes a single algebraic expression.
* A ``LoopPlan`` becomes a ``for`` loop (the safe fallback).
* An ``AssignPlan`` becomes a simple declaration/assignment.

All IR expressions are passed through the simplifier before emission, so
trivial subexpressions such as ``n - 0`` are collapsed to ``n``.
"""

from __future__ import annotations

import subprocess
import ctypes
import os
import tempfile
from typing import Any, Dict, List, Set

from ..ir.nodes import (
    IRBinOp,
    IRCall,
    IRConst,
    IRExpr,
    IRUnaryOp,
    IRVar,
)
from ..ir.simplify import simplify
from ..optimizer.plans import (
    AssignPlan,
    ClosedFormPlan,
    ConstantPlan,
    ExecutionPlan,
    FunctionPlan,
    LoopPlan,
)

# Operators that map 1-to-1 from IR to C
_BINOP_MAP: Dict[str, str] = {
    "+": "+",
    "-": "-",
    "*": "*",
    "/": "/",
    "//": "/",   # integer division — both operands are long long
    "%": "%",
    "<": "<",
    ">": ">",
    "<=": "<=",
    ">=": ">=",
    "==": "==",
    "!=": "!=",
    "&&": "&&",
    "||": "||",
}


class CodegenError(Exception):
    pass


class CGenerator:
    """Generates C source code from a FunctionPlan."""

    def generate(self, plan: FunctionPlan) -> str:
        """Return a complete C function definition as a string."""
        self._declared: Set[str] = set(plan.param_names)

        params = ", ".join(f"long long {p}" for p in plan.param_names)
        if not params:
            params = "void"

        lines: List[str] = [f"long long {plan.name}({params}) {{"]

        body_lines = self._generate_body(plan)
        lines.extend(f"    {line}" for line in body_lines)
        lines.append("}")

        return "\n".join(lines) + "\n"

    def _generate_body(self, plan: FunctionPlan) -> List[str]:
        lines: List[str] = []
        for step in plan.steps:
            lines.extend(self._generate_step(step))
        if plan.return_expr is not None:
            lines.append(f"return {self._emit_expr(simplify(plan.return_expr))};")
        else:
            lines.append("return 0LL;")
        return lines

    def _generate_step(self, step: ExecutionPlan) -> List[str]:
        if isinstance(step, ConstantPlan):
            return self._emit_constant(step)
        if isinstance(step, ClosedFormPlan):
            return self._emit_closed_form(step)
        if isinstance(step, LoopPlan):
            return self._emit_loop(step)
        if isinstance(step, AssignPlan):
            return self._emit_assign(step)
        raise CodegenError(f"Unsupported plan type: {type(step).__name__}")

    # ------------------------------------------------------------------
    # Per-plan emitters
    # ------------------------------------------------------------------

    def _emit_constant(self, step: ConstantPlan) -> List[str]:
        """Emit a single compile-time constant declaration."""
        v = step.value
        val_str = str(int(v)) if isinstance(v, float) and v == int(v) else str(v)
        decl = self._decl_prefix(step.accumulator)
        return [f"{decl}{step.accumulator} = {val_str}LL; /* compile-time constant */"]

    def _emit_closed_form(self, step: ClosedFormPlan) -> List[str]:
        """Emit a single algebraic formula."""
        expr_str = self._emit_expr(simplify(step.formula))
        comment = f" /* {step.description} */" if step.description else ""
        decl = self._decl_prefix(step.accumulator)
        return [f"{decl}{step.accumulator} = {expr_str};{comment}"]

    def _emit_loop(self, step: LoopPlan) -> List[str]:
        """Emit a for-loop (safe fallback)."""
        init_val = self._emit_expr(step.init_val)
        start = self._emit_expr(step.start)
        end = self._emit_expr(step.end)
        body = self._emit_expr(step.body)
        acc = step.accumulator
        var = step.loop_var

        op_map = {"add": "+=", "sub": "-=", "mul": "*="}
        op = op_map.get(step.op, "+=")

        decl = self._decl_prefix(acc)
        # The loop variable is declared in the for-initialiser; mark it declared.
        self._declared.add(var)

        return [
            f"{decl}{acc} = {init_val}; /* loop fallback */",
            f"for (long long {var} = {start}; {var} < {end}; {var}++) {{",
            f"    {acc} {op} {body};",
            "}",
        ]

    def _emit_assign(self, step: AssignPlan) -> List[str]:
        decl = self._decl_prefix(step.var)
        return [f"{decl}{step.var} = {self._emit_expr(step.expr)};"]

    # ------------------------------------------------------------------
    # Expression emitter
    # ------------------------------------------------------------------

    def _emit_expr(self, expr: IRExpr) -> str:
        if isinstance(expr, IRConst):
            v = expr.value
            if isinstance(v, float) and v == int(v):
                return f"{int(v)}LL"
            return str(v)

        if isinstance(expr, IRVar):
            return expr.name

        if isinstance(expr, IRBinOp):
            c_op = _BINOP_MAP.get(expr.op, expr.op)
            left = self._emit_expr(expr.left)
            right = self._emit_expr(expr.right)
            return f"({left} {c_op} {right})"

        if isinstance(expr, IRUnaryOp):
            operand = self._emit_expr(expr.operand)
            if expr.op == "-":
                return f"(-{operand})"
            if expr.op == "!":
                return f"(!{operand})"
            return f"({expr.op}{operand})"

        if isinstance(expr, IRCall):
            args_str = ", ".join(self._emit_expr(a) for a in expr.args)
            return f"{expr.name}({args_str})"

        raise CodegenError(f"Unsupported IR expression: {type(expr).__name__}")

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _decl_prefix(self, name: str) -> str:
        """Return 'long long ' for the first assignment, '' thereafter."""
        if name not in self._declared:
            self._declared.add(name)
            return "long long "
        return ""


def generate_c(plan: FunctionPlan) -> str:
    """Convenience wrapper: generate C source code for *plan*."""
    return CGenerator().generate(plan)


class _CompiledLibrary:
    """Holds a loaded shared library and exposes its functions as callables.

    The library object is kept alive as an attribute so the GC cannot
    unload it while the callables are still in use.
    """

    def __init__(self, lib: ctypes.CDLL, callables: Dict[str, Any]) -> None:
        self._lib = lib
        self._callables = callables

    def __getitem__(self, name: str):
        return self._callables[name]

    def __contains__(self, name: str) -> bool:
        return name in self._callables

    def keys(self):
        return self._callables.keys()

    def values(self):
        return self._callables.values()

    def items(self):
        return self._callables.items()


def compile_c_source(
    c_source: str,
    func_names: List[str],
    n_params_map: Dict[str, int],
) -> "_CompiledLibrary":
    """Compile *c_source* with gcc and return a :class:`_CompiledLibrary`.

    Parameters
    ----------
    c_source:
        Complete C source code (may contain multiple function definitions).
    func_names:
        Names of the functions to extract from the compiled library.
    n_params_map:
        Maps each function name to its parameter count.

    Returns
    -------
    _CompiledLibrary
        A mapping-like object where each key is a function name and each
        value is a ctypes callable backed by real gcc-compiled machine code.
        The underlying shared library is kept alive as long as this object
        exists.
    """
    with tempfile.TemporaryDirectory() as tmpdir:
        src_path = os.path.join(tmpdir, "telos_out.c")
        so_path = os.path.join(tmpdir, "telos_out.so")

        with open(src_path, "w", encoding="utf-8") as fh:
            fh.write(c_source)

        result = subprocess.run(
            [
                "gcc",
                "-O2",
                "-shared",
                "-fPIC",
                "-o", so_path,
                src_path,
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise CodegenError(
                f"gcc compilation failed:\n{result.stderr}"
            )

        # Load the shared library.  The file must be read before the
        # TemporaryDirectory context manager deletes the directory, so we
        # copy the bytes into a permanent location via mmap.
        import mmap as _mmap
        with open(so_path, "rb") as fh:
            so_bytes = fh.read()

    # Write the shared object to a second temp file outside the deleted dir
    # so that the OS can keep the file open for symbol resolution.
    import shutil
    fd, persistent_so = tempfile.mkstemp(suffix=".so", prefix="telos_")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(so_bytes)
        os.chmod(persistent_so, 0o700)
        lib = ctypes.CDLL(persistent_so)
    finally:
        # Remove after loading; the open file descriptor in the OS keeps
        # the library mapped even after the path is deleted (Linux/macOS).
        try:
            os.unlink(persistent_so)
        except OSError:
            pass

    callables: Dict[str, Any] = {}
    for name in func_names:
        fn = getattr(lib, name)
        n = n_params_map.get(name, 0)
        fn.restype = ctypes.c_longlong
        fn.argtypes = [ctypes.c_longlong] * n
        callables[name] = fn

    return _CompiledLibrary(lib, callables)
