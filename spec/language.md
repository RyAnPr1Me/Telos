# Telos Language Specification

## Overview

**Telos** is a high-performance experimental programming language designed around a
single principle:

> The programmer specifies *what must be true*. The compiler finds the **fastest
> physically possible way** to achieve it at runtime.

Telos uses a familiar C-like surface syntax, but internally it deconstructs programs
into a **graph of semantic goals and constraints** rather than a fixed sequence of
instructions. The compiler has maximum freedom to reorder, replace, or eliminate
computation as long as observable behaviour is preserved.

---

## 1. Syntax

### 1.1 Source files

Telos source files use the `.telos` extension and are encoded in UTF-8.

### 1.2 Comments

```
// Single-line comment
/* Multi-line
   comment */
```

### 1.3 Types

| Type     | Description                |
|----------|----------------------------|
| `int`    | 64-bit signed integer      |
| `float`  | 64-bit IEEE 754 double     |
| `void`   | No value                   |
| `bool`   | Boolean (true / false)     |

### 1.4 Literals

```
42          // integer literal
3.14        // float literal
true false  // boolean literals
```

### 1.5 Identifiers

An identifier starts with a letter or underscore followed by zero or more letters,
digits, or underscores.

### 1.6 Operators

| Category         | Operators                         |
|------------------|-----------------------------------|
| Arithmetic       | `+  -  *  /  %`                   |
| Compound assign  | `+=  -=  *=  /=  %=`              |
| Comparison       | `<  >  <=  >=  ==  !=`            |
| Logical          | `&&  \|\|  !`                     |
| Increment        | `++  --` (prefix and postfix)     |
| Assignment       | `=`                               |

### 1.7 Statements

```c
// Variable declaration with optional initialiser
int x = 0;
float y;

// Assignment
x = 42;
x += 10;

// If / else
if (x > 0) {
    y = 1.0;
} else {
    y = -1.0;
}

// For loop
for (int i = 0; i < n; i++) {
    s += i;
}

// While loop
while (x > 0) {
    x--;
}

// Return
return s;
```

### 1.8 Functions

```c
int sum(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += i;
    }
    return s;
}
```

---

## 2. Semantics

### 2.1 Execution model

A Telos program is a collection of function definitions. Execution begins by calling
any named function from the host environment. Functions may call other functions.

### 2.2 Purity

A function is **pure** if it:
- Has no writes to memory outside its own local variables.
- Makes no calls to impure functions.

Pure functions may be freely reordered, memoised, or partially evaluated by the
compiler.

### 2.3 Loops as hints

`for` and `while` loops describe *iteration intent*, not a mandatory execution
strategy. The compiler may replace any loop with:

- A closed-form mathematical formula.
- A vectorised computation.
- A lookup table.
- A compile-time constant (if all inputs are known).

---

## 3. Constraint IR

Internally, the compiler lowers the AST into a **Constraint Graph**: a directed graph
of semantic nodes where edges represent data dependencies, not control flow.

### 3.1 Node types

| Node                  | Meaning                                         |
|-----------------------|-------------------------------------------------|
| `InvariantConstraint` | A value that never changes within a function call (parameter or loop-invariant). |
| `AssignConstraint`    | `var = expr`                                    |
| `ReductionConstraint` | `acc = reduce(op, f(i), i ∈ [start, end))`     |
| `ReturnConstraint`    | The observable result of a function.            |
| `CondBranchConstraint`| Conditional split with two sub-graphs.          |

### 3.2 ReductionConstraint

```
ReductionConstraint(
    accumulator = "s",
    op          = "add",
    loop_var    = "i",
    start       = 0,
    end         = n,
    body        = i,
    init_val    = 0
)
```

This says: *compute the additive reduction of `i` over the range `[0, n)`*. The
compiler is free to use any mathematically equivalent strategy.

---

## 4. Optimisation

### 4.1 Plan generation

For each constraint node, the compiler generates a set of *execution plans*:

1. **LoopPlan** — direct loop execution (always valid, O(n)).
2. **ClosedFormPlan** — algebraic closed-form (O(1) when applicable).
3. **ConstantPlan** — compile-time evaluation (O(1) constant).

### 4.2 Cost model

Each plan carries a cost class:

| Cost class      | Score |
|-----------------|-------|
| `O(1) constant` | 0     |
| `O(1)`          | 1     |
| `O(log n)`      | 10    |
| `O(n)`          | 100   |
| `O(n²)`         | 10000 |

The planner selects the plan with the lowest score.

### 4.3 Closed-form summation rules

For additive reductions `Σ f(i), i ∈ [0, n)`:

| Body f(i)       | Closed form                    |
|-----------------|--------------------------------|
| `c` (constant)  | `c * n`                        |
| `i`             | `n * (n − 1) / 2`              |
| `a + b*i`       | `a*n + b * n*(n−1)/2`          |
| `i²`            | `n * (n−1) * (2n−1) / 6`      |
| `a + b*i + c*i²`| combination of above formulas  |

General range `[s, e)` is shifted: let `n = e − s`, then apply the `[0, n)` formula
with `i → j + s`.

---

## 5. Example transformations

### 5.1 Sum of integers (O(n) → O(1))

**Input:**

```c
int sum(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += i;
    }
    return s;
}
```

**Constraint IR:**

```
ReductionConstraint(acc=s, op=add, var=i, range=[0,n), body=i, init=0)
ReturnConstraint(s)
```

**Optimised plan:** ClosedFormPlan  
**Generated code:**

```python
def sum(n):
    s = n * (n - 1) // 2
    return s
```

### 5.2 Sum of squares (O(n) → O(1))

**Input:**

```c
int sum_sq(int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += i * i;
    }
    return s;
}
```

**Generated code:**

```python
def sum_sq(n):
    s = n * (n - 1) * (2 * n - 1) // 6
    return s
```

### 5.3 Constant product (compile-time evaluation)

**Input:**

```c
int fixed_sum() {
    int s = 0;
    for (int i = 0; i < 10; i++) {
        s += i;
    }
    return s;
}
```

**Generated code:**

```python
def fixed_sum():
    s = 45   # evaluated at compile time
    return s
```

---

## 6. Safety guarantees

- The compiler **never** changes observable behaviour.
- If no closed form is found, the compiler falls back to the loop plan.
- Side-effecting code (I/O, external calls) is always executed in source order.
