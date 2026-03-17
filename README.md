# Telos

**Telos** is a high-performance experimental programming language and compiler whose
primary goal is:

> The programmer specifies *what must be true*. The compiler finds the **fastest
> physically possible way** to achieve it at runtime.

---

## Core Concept

Traditional compilers translate a fixed sequence of instructions.  Telos is
different: it deconstructs programs into a **graph of semantic goals and
constraints**, then has maximum freedom to choose the fastest valid execution
strategy.

```
Source code
    │  parse
    ▼
Abstract Syntax Tree (AST)
    │  semantic lifting
    ▼
Constraint Graph  ←─── "what must be true"
    │  plan generation
    ▼
Candidate Plans   (loop / closed-form formula / compile-time constant)
    │  cost model
    ▼
Optimal Plan      ←─── "fastest valid strategy"
    │  code generation
    ▼
Executable Python
```

---

## Key Transformation

Input Telos:

```c
int sum(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += i;
    }
    return s;
}
```

Generated Python (O(1) instead of O(n)):

```python
def sum(n):
    s = 0
    s = ((n * (n - 1)) // 2)  # Closed-form sum of degree-1 polynomial
    return s
```

Compile-time constant example:

```c
int fixed_sum() {
    int s = 0;
    for (int i = 0; i < 10; i++) {  // all inputs constant
        s += i;
    }
    return s;
}
```

Generated Python (constant literal):

```python
def fixed_sum():
    s = 0
    s = 45  # compile-time constant
    return s
```

---

## Repository Layout

```
Telos/
├── spec/
│   └── language.md          Language specification
├── src/
│   ├── compiler.py          Main entry point (compile_telos / run_telos)
│   ├── lexer.py             Tokenizer
│   ├── parser.py            Recursive-descent parser
│   ├── ast_nodes.py         AST node types
│   ├── ir/
│   │   ├── nodes.py         Constraint IR node types
│   │   ├── graph.py         ConstraintGraph data structure
│   │   └── simplify.py      IR expression simplifier
│   ├── lifting/
│   │   └── semantic_lift.py AST → Constraint IR (pattern detection)
│   ├── optimizer/
│   │   ├── plans.py         Execution plan types
│   │   ├── cost_model.py    Cost scoring
│   │   └── planner.py       Plan generation & selection
│   └── codegen/
│       └── python_gen.py    Python source code generator
├── tests/
│   ├── test_lexer.py
│   ├── test_parser.py
│   ├── test_lifting.py
│   ├── test_optimizer.py
│   └── test_compiler.py     End-to-end correctness tests
└── examples/
    ├── sum.telos
    ├── sum_sq.telos
    ├── fixed_sum.telos
    ├── linear_combo.telos
    └── multi_fn.telos
```

---

## Quick Start

### Requirements

* Python 3.9+
* `pytest` (for tests)

```bash
pip install pytest
```

### Run the compiler on an example

```bash
python -m src.compiler examples/sum.telos
```

### Use as a library

```python
from src.compiler import compile_telos, run_telos

source = """
int sum(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += i;
    }
    return s;
}
"""

# Inspect the generated Python
print(compile_telos(source))

# Or compile and call directly
funcs = run_telos(source)
print(funcs["sum"](100))   # → 4950
```

### Run the test suite

```bash
python -m pytest tests/ -v
```

---

## Architecture

### 1. Lexer (`src/lexer.py`)

Tokenises the C-like surface syntax into a flat list of `Token` objects.
Handles comments, multi-char operators, keywords, identifiers, and numeric
literals.

### 2. Parser (`src/parser.py`)

Recursive-descent parser that produces a typed AST.  Supports the full
language: functions, typed variables, for/while loops, if/else, all
expression precedences, function calls, compound assignment operators.

### 3. AST nodes (`src/ast_nodes.py`)

Dataclass hierarchy:
```
Expr → IntLiteral | FloatLiteral | Identifier | BinaryOp | UnaryOp | Call | Assignment
Stmt → VarDecl | ExprStmt | Return | Block | If | For | While
Program → [Function]
```

### 4. Semantic Lifting (`src/lifting/semantic_lift.py`)

This is where programs stop being instruction sequences and become
**semantic constraint graphs**.

Key pattern detected — **accumulation loop**:

```c
for (int i = start; i < end; i++) {
    acc op= f(i);
}
```

is lifted to a single `ReductionConstraint` node:

```
ReductionConstraint(
    accumulator="acc",
    op="add",
    loop_var="i",
    start=start,
    end=end,
    body=f(i),
    init_val=0
)
```

The compiler now knows *what* the loop computes, not *how* it was written.

### 5. Constraint IR (`src/ir/`)

A `ConstraintGraph` holds a list of typed constraint nodes:

| Node                  | Meaning                                        |
|-----------------------|------------------------------------------------|
| `InvariantConstraint` | A parameter or loop-invariant value            |
| `AssignConstraint`    | `var = expr`                                   |
| `ReductionConstraint` | Reduction over a range (see above)             |
| `ReturnConstraint`    | The function's observable return value         |
| `CondBranchConstraint`| Conditional with two sub-graphs               |

IR expressions (`IRConst`, `IRVar`, `IRBinOp`, `IRUnaryOp`, `IRCall`) are
symbolic — they can represent runtime-variable sub-computations.

### 6. Optimizer / Planner (`src/optimizer/`)

For each `ReductionConstraint`, the planner generates **multiple candidate
execution plans** and picks the cheapest:

| Plan             | When chosen                              | Cost     |
|------------------|------------------------------------------|----------|
| `ConstantPlan`   | All inputs are compile-time constants    | O(1) = 0 |
| `ClosedFormPlan` | Body is a polynomial in the loop variable| O(1) = 1 |
| `LoopPlan`       | Fallback (always valid)                  | O(n) = 100 |

**Polynomial detection** (`_as_polynomial`): expresses the body as
`c₀ + c₁·i + c₂·i² + …`.

**Closed-form formulas** used:

| Degree | Body        | Formula                         |
|--------|-------------|---------------------------------|
| 0      | constant c  | `c * n`                         |
| 1      | `a + b·i`   | `a*n + b*n*(n-1)/2`             |
| 2      | `… + c·i²`  | `… + c*n*(n-1)*(2n-1)/6`        |
| 3      | `… + d·i³`  | `… + d*[n*(n-1)/2]²`            |

All formulas are applied after a **shift** so they work for any
`[start, end)` range, not just `[0, n)`.

**Constant folding**: if `start` and `end` are both compile-time
constants and the iteration count is ≤ 10,000, the reduction is
evaluated entirely at compile time.

### 7. Code Generator (`src/codegen/python_gen.py`)

Translates the chosen `FunctionPlan` to Python source.  All IR expressions
are passed through the **simplifier** (`src/ir/simplify.py`) first to
eliminate trivial sub-expressions (`x - 0 → x`, `x * 1 → x`, constant
folding, etc.).

---

## Safety Guarantees

* A `LoopPlan` (direct loop) is **always** generated as a fallback.
* The closed-form or constant plan is only selected when it is
  **provably equivalent** to the loop.
* Side-effecting code (function calls with unknown purity) is never
  reordered or eliminated.

---

## Optimizations Demonstrated

| Input pattern                              | Optimization              | Output complexity |
|--------------------------------------------|---------------------------|-------------------|
| `for (i=0; i<n; i++) s += i`               | Gauss formula             | O(1)              |
| `for (i=0; i<n; i++) s += i*i`             | Sum-of-squares formula    | O(1)              |
| `for (i=0; i<n; i++) s += i*i*i`           | Sum-of-cubes formula      | O(1)              |
| `for (i=0; i<n; i++) s += a*i + b`         | Linear sum formula        | O(1)              |
| `for (i=0; i<10; i++) s += i` (constant n) | Compile-time evaluation   | constant = 45     |
| `for (i=0; i<=n; i++) s += i`              | Inclusive range handling  | O(1)              |

---

## Language Reference

See [`spec/language.md`](spec/language.md) for the full language specification.

### Supported syntax (quick reference)

```c
// Types
int   float   void   bool

// Variable declaration
int x = 0;
float y;

// Control flow
if (cond) { ... } else { ... }
for (int i = 0; i < n; i++) { ... }
while (cond) { ... }

// Functions
int add(int a, int b) {
    return a + b;
}

// Operators
+  -  *  /  %           // arithmetic
+=  -=  *=  /=  %=      // compound assignment
++  --                   // increment/decrement (prefix and postfix)
<  >  <=  >=  ==  !=    // comparison
&&  ||  !                // logical
```

---

## Running the Examples

```bash
# Sum of integers (closed-form Gauss formula)
python -m src.compiler examples/sum.telos

# Sum of squares (closed-form n*(n-1)*(2n-1)/6)
python -m src.compiler examples/sum_sq.telos

# All-constant loop (literal 45)
python -m src.compiler examples/fixed_sum.telos

# Linear combination Σ(2i+3)
python -m src.compiler examples/linear_combo.telos

# Multiple functions in one file
python -m src.compiler examples/multi_fn.telos
```