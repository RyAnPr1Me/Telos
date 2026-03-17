"""End-to-end compiler tests.

These tests compile Telos source to native x86-64 machine code, execute
the resulting functions, and verify that the output matches the expected
mathematical results.

They are the primary correctness guarantee: regardless of which
optimization plan is chosen, the observable output must be identical.
"""

import pytest
from src.compiler import compile_telos, run_telos
from src.codegen.executable import NativeFunction


# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------

def run(src: str) -> dict:
    """Compile and run *src*, returning native function callables."""
    return run_telos(src)


# ---------------------------------------------------------------------------
# Sum of integers
# ---------------------------------------------------------------------------

class TestSum:
    SRC = """
    int sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i;
        }
        return s;
    }
    """

    def setup_method(self):
        self.ns = run(self.SRC)

    def test_sum_zero(self):
        assert self.ns["sum"](0) == 0

    def test_sum_one(self):
        assert self.ns["sum"](1) == 0

    def test_sum_small(self):
        assert self.ns["sum"](5) == 10   # 0+1+2+3+4

    def test_sum_ten(self):
        assert self.ns["sum"](10) == 45

    def test_sum_large(self):
        n = 1000
        assert self.ns["sum"](n) == n * (n - 1) // 2

    def test_emits_machine_code_not_python(self):
        """compile_telos must return bytes, not a Python source string."""
        mc = compile_telos(self.SRC)
        assert isinstance(mc, dict)
        assert "sum" in mc
        assert isinstance(mc["sum"], bytes)
        assert len(mc["sum"]) > 0


# ---------------------------------------------------------------------------
# Sum of squares
# ---------------------------------------------------------------------------

class TestSumSquares:
    SRC = """
    int sum_sq(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i * i;
        }
        return s;
    }
    """

    def setup_method(self):
        self.ns = run(self.SRC)

    def test_sum_sq_zero(self):
        assert self.ns["sum_sq"](0) == 0

    def test_sum_sq_five(self):
        assert self.ns["sum_sq"](5) == 30   # 0+1+4+9+16

    def test_sum_sq_large(self):
        n = 100
        expected = sum(i * i for i in range(n))
        assert self.ns["sum_sq"](n) == expected


# ---------------------------------------------------------------------------
# Compile-time constant folding
# ---------------------------------------------------------------------------

class TestConstantFolding:
    SRC = """
    int fixed_sum() {
        int s = 0;
        for (int i = 0; i < 10; i++) {
            s += i;
        }
        return s;
    }
    """

    def setup_method(self):
        self.ns = run(self.SRC)

    def test_correct_value(self):
        assert self.ns["fixed_sum"]() == 45

    def test_emits_machine_code(self):
        mc = compile_telos(self.SRC)
        assert isinstance(mc["fixed_sum"], bytes)
        assert len(mc["fixed_sum"]) > 0


# ---------------------------------------------------------------------------
# Linear combination: Σ(2i+3)
# ---------------------------------------------------------------------------

class TestLinearCombination:
    SRC = """
    int linear_combo(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += 2 * i + 3;
        }
        return s;
    }
    """

    def setup_method(self):
        self.ns = run(self.SRC)

    def test_values(self):
        for n in range(0, 20):
            expected = sum(2 * i + 3 for i in range(n))
            assert self.ns["linear_combo"](n) == expected, f"n={n}"


# ---------------------------------------------------------------------------
# Cube sum: Σ i³
# ---------------------------------------------------------------------------

class TestCubeSum:
    SRC = """
    int cube_sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i * i * i;
        }
        return s;
    }
    """

    def setup_method(self):
        self.ns = run(self.SRC)

    def test_values(self):
        for n in range(0, 15):
            expected = sum(i ** 3 for i in range(n))
            assert self.ns["cube_sum"](n) == expected, f"n={n}"


# ---------------------------------------------------------------------------
# Inclusive range (i <= n)
# ---------------------------------------------------------------------------

class TestInclusiveRange:
    SRC = """
    int sum_inc(int n) {
        int s = 0;
        for (int i = 0; i <= n; i++) {
            s += i;
        }
        return s;
    }
    """

    def setup_method(self):
        self.ns = run(self.SRC)

    def test_values(self):
        for n in range(0, 15):
            expected = sum(range(n + 1))
            assert self.ns["sum_inc"](n) == expected, f"n={n}"


# ---------------------------------------------------------------------------
# Multiple functions in one file
# ---------------------------------------------------------------------------

class TestMultipleFunctions:
    SRC = """
    int a(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) { s += i; }
        return s;
    }
    int b(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) { s += i * i; }
        return s;
    }
    """

    def setup_method(self):
        self.ns = run(self.SRC)

    def test_a(self):
        assert self.ns["a"](10) == 45

    def test_b(self):
        n = 10
        assert self.ns["b"](n) == sum(i * i for i in range(n))

    def test_both_functions_compiled(self):
        mc = compile_telos(self.SRC)
        assert "a" in mc and "b" in mc
        assert isinstance(mc["a"], bytes) and isinstance(mc["b"], bytes)


# ---------------------------------------------------------------------------
# Simple function (no loops)
# ---------------------------------------------------------------------------

class TestSimpleFunction:
    def test_identity(self):
        ns = run("int id(int x) { return x; }")
        assert ns["id"](7) == 7

    def test_add(self):
        ns = run("int add(int a, int b) { return a + b; }")
        assert ns["add"](3, 4) == 7

    def test_constant(self):
        ns = run("int answer() { return 42; }")
        assert ns["answer"]() == 42

    def test_subtract(self):
        ns = run("int sub(int a, int b) { return a - b; }")
        assert ns["sub"](10, 3) == 7

    def test_multiply(self):
        ns = run("int mul(int a, int b) { return a * b; }")
        assert ns["mul"](6, 7) == 42


# ---------------------------------------------------------------------------
# Machine code output validation
# ---------------------------------------------------------------------------

class TestMachineCodeOutput:
    PROGRAMS = [
        "int sum(int n) { int s = 0; for (int i = 0; i < n; i++) { s += i; } return s; }",
        "int sum_sq(int n) { int s = 0; for (int i = 0; i < n; i++) { s += i * i; } return s; }",
        "int fixed() { int s = 0; for (int i = 0; i < 10; i++) { s += i; } return s; }",
        "int f(int a, int b) { return a + b; }",
    ]

    def test_all_compile_to_bytes(self):
        """compile_telos must produce non-empty bytes for every function."""
        for src in self.PROGRAMS:
            mc = compile_telos(src)
            for name, code in mc.items():
                assert isinstance(code, bytes), (
                    f"{src}: expected bytes, got {type(code)}"
                )
                assert len(code) > 0, f"{src}: {name} produced empty code"

    def test_output_is_not_python_source(self):
        """The compiler must NOT emit Python source strings."""
        for src in self.PROGRAMS:
            mc = compile_telos(src)
            assert isinstance(mc, dict), "compile_telos must return a dict"
            # None of the values should be strings
            for name, code in mc.items():
                assert not isinstance(code, str), (
                    f"{src}: {name} returned a Python string, expected bytes"
                )

    def test_native_functions_are_callable(self):
        """run_telos must return NativeFunction instances."""
        for src in self.PROGRAMS:
            funcs = run_telos(src)
            for name, fn in funcs.items():
                assert isinstance(fn, NativeFunction), (
                    f"{src}: {name} is {type(fn)}, expected NativeFunction"
                )
                assert callable(fn)


# ---------------------------------------------------------------------------
# C backend tests
# ---------------------------------------------------------------------------

from src.compiler import compile_telos_c, run_telos_c
from src.codegen.c_gen import generate_c


class TestCGenerator:
    """Tests for the C source code generator."""

    SRC_SUM = """
    int sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i;
        }
        return s;
    }
    """

    def test_c_source_is_string(self):
        """compile_telos_c must return a dict of str values."""
        result = compile_telos_c(self.SRC_SUM)
        assert isinstance(result, dict)
        assert "sum" in result
        assert isinstance(result["sum"], str)

    def test_c_source_contains_long_long(self):
        """Generated C must use long long for 64-bit integers."""
        result = compile_telos_c(self.SRC_SUM)
        assert "long long" in result["sum"]

    def test_c_source_is_not_python(self):
        """Generated C must not contain Python-specific syntax."""
        result = compile_telos_c(self.SRC_SUM)
        src = result["sum"]
        assert "def " not in src
        assert "return" in src

    def test_c_source_has_function_name(self):
        """Generated C must contain the function name."""
        result = compile_telos_c(self.SRC_SUM)
        assert "sum" in result["sum"]


class TestCBackendExecution:
    """End-to-end tests: Telos → C source → gcc → real machine code."""

    SRC_SUM = """
    int sum(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            s += i;
        }
        return s;
    }
    """

    SRC_FIXED = """
    int fixed_sum() {
        int s = 0;
        for (int i = 0; i < 10; i++) {
            s += i;
        }
        return s;
    }
    """

    SRC_ADD      = "int add(int a, int b) { return a + b; }"
    SRC_IDENTITY = "int id(int x) { return x; }"

    def test_sum_via_c(self):
        """sum compiled via C backend must return correct values."""
        fns = run_telos_c(self.SRC_SUM)
        assert fns["sum"](0)  == 0
        assert fns["sum"](1)  == 0
        assert fns["sum"](5)  == 10
        assert fns["sum"](10) == 45
        n = 1000
        assert fns["sum"](n) == n * (n - 1) // 2

    def test_fixed_sum_via_c(self):
        """Constant-folded function compiled via C returns 45."""
        fns = run_telos_c(self.SRC_FIXED)
        assert fns["fixed_sum"]() == 45

    def test_add_via_c(self):
        """Simple two-argument function works via C backend."""
        fns = run_telos_c(self.SRC_ADD)
        assert fns["add"](3, 4) == 7
        assert fns["add"](-1, 1) == 0

    def test_identity_via_c(self):
        """Identity function works via C backend."""
        fns = run_telos_c(self.SRC_IDENTITY)
        assert fns["id"](42) == 42

    def test_c_callables_are_callable(self):
        """run_telos_c must return callable objects."""
        fns = run_telos_c(self.SRC_SUM)
        assert callable(fns["sum"])

    def test_c_and_x86_agree(self):
        """Both backends must produce identical results for all test inputs."""
        src = self.SRC_SUM
        x86_fns = run_telos(src)
        c_fns   = run_telos_c(src)
        for n in range(0, 20):
            assert x86_fns["sum"](n) == c_fns["sum"](n), f"Mismatch at n={n}"

    def test_sum_squares_via_c(self):
        """Sum of squares compiled via C backend must return correct values."""
        src = """
        int sum_sq(int n) {
            int s = 0;
            for (int i = 0; i < n; i++) {
                s += i * i;
            }
            return s;
        }
        """
        fns = run_telos_c(src)
        for n in range(0, 15):
            expected = sum(i * i for i in range(n))
            assert fns["sum_sq"](n) == expected, f"n={n}"
