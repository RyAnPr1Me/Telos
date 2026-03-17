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
x86-64 Machine Code  ←─── native binary, executed directly
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

Generated x86-64 machine code (O(1) instead of O(n)):

```
Function: sum  (83 bytes of x86-64 machine code)
  0000  55 48 89 e5 48 83 ec 10 48 89 7d f8 48 c7 c0 00
  0010  00 00 00 48 89 45 f0 48 8b 45 f8 50 48 8b 45 f8
  ...
```

The optimizer selects the Gauss closed-form formula `n*(n-1)/2` and the
x86-64 code generator emits the arithmetic directly as native machine
instructions — no interpreter, no Python runtime.

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

Generated x86-64 machine code (compile-time constant folded to `mov rax, 45`):

```
Function: fixed_sum  (40 bytes of x86-64 machine code)
  0000  55 48 89 e5 48 83 ec 10 48 c7 c0 00 00 00 00 48
  0010  89 45 f8 48 c7 c0 2d 00 00 00 48 89 45 f8 48 8b
  0020  45 f8 48 83 c4 10 5d c3
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
│       ├── x86_64_gen.py    x86-64 machine code generator
│       └── executable.py    Wraps machine code bytes in a ctypes callable
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

# Compile: returns {function_name: x86-64_machine_code_bytes}
machine_code = compile_telos(source)
print(machine_code["sum"].hex())   # raw bytes, e.g. "5548...c3"

# Or compile and call directly via native execution
funcs = run_telos(source)
print(funcs["sum"](100))   # → 4950  (executed as native machine code)
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

### 7. x86-64 Code Generator (`src/codegen/x86_64_gen.py`)

Translates the chosen `FunctionPlan` directly into **x86-64 machine code bytes**
using the System V AMD64 ABI (Linux/macOS).

Key techniques:
* All IR expressions are simplified first (`src/ir/simplify.py`).
* A pre-scan allocates stack slots (8 bytes each, RBP-relative) for every variable.
* Expression evaluation uses a software stack: left operand is saved with `PUSH RAX`,
  right operand is evaluated into RAX, then `POP RCX` restores the left.
* Integer division uses `XCHG RAX,RCX` + `CQO` + `IDIV RCX` (signed).
* The `LoopPlan` fallback emits a full compare-and-branch loop with forward jumps
  patched after the loop body is emitted.

### 8. Executable Wrapper (`src/codegen/executable.py`)

The machine code bytes are copied into a read/write/execute memory region
(allocated via `mmap` with `PROT_READ|PROT_WRITE|PROT_EXEC`) and wrapped in a
`ctypes.CFUNCTYPE` pointer.  The resulting `NativeFunction` object is a normal
Python callable that runs at native speed.

```python
fn = NativeFunction(code_bytes, n_params=1)
result = fn(100)   # executes native x86-64 code, returns Python int
```

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