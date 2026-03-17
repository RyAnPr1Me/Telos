"""End-to-end compiler tests.

These tests compile Telos source to Python, execute the generated code,
and verify that the output matches the expected mathematical results.

They are the primary correctness guarantee: regardless of which
optimization plan is chosen, the observable output must be identical.
"""

import pytest
from src.compiler import compile_telos, run_telos


# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------

def run(src: str) -> dict:
    """Compile and run *src*, returning the namespace dict."""
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

    def test_compiled_source_contains_formula(self):
        """The optimized output should NOT contain a for-loop (closed form used)."""
        py_src = compile_telos(self.SRC)
        assert "for " not in py_src, (
            "Expected closed-form formula, found a loop:\n" + py_src
        )


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

    def test_compiled_is_constant(self):
        """The optimized output should be a literal 45."""
        py_src = compile_telos(self.SRC)
        assert "45" in py_src
        assert "for " not in py_src


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


# ---------------------------------------------------------------------------
# Regression: generated code must be syntactically valid Python
# ---------------------------------------------------------------------------

class TestCodeValidity:
    PROGRAMS = [
        "int sum(int n) { int s = 0; for (int i = 0; i < n; i++) { s += i; } return s; }",
        "int sum_sq(int n) { int s = 0; for (int i = 0; i < n; i++) { s += i * i; } return s; }",
        "int fixed() { int s = 0; for (int i = 0; i < 10; i++) { s += i; } return s; }",
        "int f(int a, int b) { return a + b; }",
    ]

    def test_all_compile_to_valid_python(self):
        for src in self.PROGRAMS:
            py_src = compile_telos(src)
            try:
                compile(py_src, "<telos>", "exec")
            except SyntaxError as e:
                pytest.fail(f"Invalid Python generated for:\n{src}\n\n{py_src}\n{e}")
