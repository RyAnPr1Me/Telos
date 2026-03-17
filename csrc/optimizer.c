/*
 * optimizer.c — Optimizer/planner, goal graph (liveness analysis),
 *               and cost model for the Telos compiler.
 *
 * Given a ConstraintGraph the planner:
 *   1. Computes liveness (backward from ReturnConstraint).
 *   2. For each live node, generates candidate execution plans.
 *   3. Selects the cheapest plan using the cost model.
 *
 * Key optimization: algebraic closed-form detection for additive
 * reductions over polynomial bodies (up to degree 3).
 */

#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "telos.h"

/* Maximum loop iterations for compile-time constant folding */
#define MAX_CONST_FOLD_ITERS 10000

/* Maximum supported polynomial degree for closed-form synthesis */
#define MAX_POLY_DEGREE 3

/* Maximum variables we track in any single set */
#define MAX_VARS 256

/* -----------------------------------------------------------------------
 * Simple string-set (used for variable name tracking)
 * ----------------------------------------------------------------------- */

typedef struct {
    char *names[MAX_VARS];
    int   count;
} VarSet;

static void varset_init(VarSet *s) { s->count = 0; }

static bool varset_contains(const VarSet *s, const char *name)
{
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->names[i], name) == 0) return true;
    return false;
}

static void varset_add(VarSet *s, const char *name)
{
    if (s->count >= MAX_VARS || varset_contains(s, name)) return;
    s->names[s->count++] = strdup(name);
}

static void varset_discard(VarSet *s, const char *name)
{
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->names[i], name) == 0) {
            free(s->names[i]);
            s->names[i] = s->names[--s->count];
            return;
        }
    }
}

static void varset_union(VarSet *dst, const VarSet *src)
{
    for (int i = 0; i < src->count; i++)
        varset_add(dst, src->names[i]);
}

static void varset_free(VarSet *s)
{
    for (int i = 0; i < s->count; i++)
        free(s->names[i]);
    s->count = 0;
}

/* -----------------------------------------------------------------------
 * Variable analysis helpers
 * ----------------------------------------------------------------------- */

static void vars_in_expr(const IRExpr *expr, VarSet *out)
{
    if (!expr) return;
    switch (expr->kind) {
    case IR_VAR:
        varset_add(out, expr->as.var_name);
        break;
    case IR_CONST:
        break;
    case IR_BINOP:
        vars_in_expr(expr->as.binop.left, out);
        vars_in_expr(expr->as.binop.right, out);
        break;
    case IR_UNARYOP:
        vars_in_expr(expr->as.unaryop.operand, out);
        break;
    case IR_CALL:
        for (int i = 0; i < expr->as.call.n_args; i++)
            vars_in_expr(expr->as.call.args[i], out);
        break;
    }
}

static void vars_produced(const ConstraintNode *node, VarSet *out)
{
    switch (node->kind) {
    case CN_INVARIANT:
        varset_add(out, node->as.invariant.var);
        break;
    case CN_ASSIGN:
        varset_add(out, node->as.assign.var);
        break;
    case CN_REDUCTION:
        varset_add(out, node->as.reduction.accumulator);
        break;
    case CN_RETURN:
    case CN_COND_BRANCH:
        break;
    }
}

static void vars_consumed(const ConstraintNode *node, VarSet *out)
{
    switch (node->kind) {
    case CN_INVARIANT:
        if (node->as.invariant.expr)
            vars_in_expr(node->as.invariant.expr, out);
        break;
    case CN_ASSIGN:
        vars_in_expr(node->as.assign.expr, out);
        break;
    case CN_REDUCTION: {
        vars_in_expr(node->as.reduction.start, out);
        vars_in_expr(node->as.reduction.end, out);
        vars_in_expr(node->as.reduction.body, out);
        vars_in_expr(node->as.reduction.init_val, out);
        varset_discard(out, node->as.reduction.loop_var);
        varset_discard(out, node->as.reduction.accumulator);
        break;
    }
    case CN_RETURN:
        vars_in_expr(node->as.ret.expr, out);
        break;
    case CN_COND_BRANCH:
        vars_in_expr(node->as.cond_branch.cond, out);
        for (int i = 0; i < node->as.cond_branch.n_then; i++) {
            VarSet sub;
            varset_init(&sub);
            vars_consumed(node->as.cond_branch.then_nodes[i], &sub);
            varset_union(out, &sub);
            varset_free(&sub);
        }
        for (int i = 0; i < node->as.cond_branch.n_else; i++) {
            VarSet sub;
            varset_init(&sub);
            vars_consumed(node->as.cond_branch.else_nodes[i], &sub);
            varset_union(out, &sub);
            varset_free(&sub);
        }
        break;
    }
}

/* -----------------------------------------------------------------------
 * Cost model
 * ----------------------------------------------------------------------- */

int plan_cost(const ExecutionPlan *plan)
{
    if (!plan) return 100;
    const char *cc = plan->cost_class;
    if (strcmp(cc, "O(1) constant") == 0)  return 0;
    if (strcmp(cc, "O(1)") == 0)           return 1;
    if (strcmp(cc, "O(log n)") == 0)       return 10;
    if (strcmp(cc, "O(n)") == 0)           return 100;
    if (strcmp(cc, "O(n log n)") == 0)     return 500;
    if (strcmp(cc, "O(n^2)") == 0)         return 10000;
    return 100;
}

ExecutionPlan *plan_cheapest(ExecutionPlan **plans, int count)
{
    if (count <= 0) return NULL;
    ExecutionPlan *best = plans[0];
    int best_cost = plan_cost(best);
    for (int i = 1; i < count; i++) {
        int c = plan_cost(plans[i]);
        if (c < best_cost) {
            best = plans[i];
            best_cost = c;
        }
    }
    return best;
}

/* -----------------------------------------------------------------------
 * Liveness analysis
 * ----------------------------------------------------------------------- */

LivenessResult telos_compute_liveness(ConstraintGraph *graph)
{
    LivenessResult result;
    result.max_id = graph->next_id;
    result.is_live = calloc(graph->next_id + 1, sizeof(bool));
    if (graph->next_id == 0) return result;

    /* Find return nodes (root goals) */
    bool has_return = false;
    for (int i = 0; i < graph->n_constraints; i++) {
        if (graph->constraints[i]->kind == CN_RETURN) {
            has_return = true;
            break;
        }
    }

    if (!has_return) {
        /* No return: conservatively mark everything live */
        for (int i = 0; i < graph->n_constraints; i++)
            result.is_live[graph->constraints[i]->node_id] = true;
        return result;
    }

    /* Build last_writer map: last_writer[var] = constraint node */
    typedef struct { char name[64]; ConstraintNode *writer; } WriterEntry;
    WriterEntry *writers = calloc(graph->n_constraints * 4, sizeof(WriterEntry));
    int n_writers = 0;

    for (int i = 0; i < graph->n_constraints; i++) {
        VarSet produced;
        varset_init(&produced);
        vars_produced(graph->constraints[i], &produced);
        for (int j = 0; j < produced.count; j++) {
            /* Update or add entry */
            bool found = false;
            for (int k = 0; k < n_writers; k++) {
                if (strcmp(writers[k].name, produced.names[j]) == 0) {
                    writers[k].writer = graph->constraints[i];
                    found = true;
                    break;
                }
            }
            if (!found) {
                strncpy(writers[n_writers].name, produced.names[j],
                        sizeof(writers[n_writers].name) - 1);
                writers[n_writers].name[sizeof(writers[n_writers].name) - 1] = '\0';
                writers[n_writers].writer = graph->constraints[i];
                n_writers++;
            }
        }
        varset_free(&produced);
    }

    /* Seed worklist from return nodes */
    VarSet seen_vars;
    varset_init(&seen_vars);

    char *worklist[MAX_VARS * 4];
    int wl_count = 0;

    for (int i = 0; i < graph->n_constraints; i++) {
        ConstraintNode *cn = graph->constraints[i];
        if (cn->kind == CN_RETURN) {
            result.is_live[cn->node_id] = true;
            VarSet consumed;
            varset_init(&consumed);
            vars_consumed(cn, &consumed);
            for (int j = 0; j < consumed.count; j++) {
                if (!varset_contains(&seen_vars, consumed.names[j])) {
                    varset_add(&seen_vars, consumed.names[j]);
                    worklist[wl_count++] = strdup(consumed.names[j]);
                }
            }
            varset_free(&consumed);
        }
    }

    /* BFS backward through last_writer map */
    while (wl_count > 0) {
        char *var = worklist[--wl_count];

        /* Find writer for this variable */
        ConstraintNode *writer = NULL;
        for (int k = 0; k < n_writers; k++) {
            if (strcmp(writers[k].name, var) == 0) {
                writer = writers[k].writer;
                break;
            }
        }
        free(var);

        if (!writer) continue;
        if (result.is_live[writer->node_id]) continue;

        result.is_live[writer->node_id] = true;

        VarSet consumed;
        varset_init(&consumed);
        vars_consumed(writer, &consumed);
        for (int j = 0; j < consumed.count; j++) {
            if (!varset_contains(&seen_vars, consumed.names[j])) {
                varset_add(&seen_vars, consumed.names[j]);
                worklist[wl_count++] = strdup(consumed.names[j]);
            }
        }
        varset_free(&consumed);
    }

    /* InvariantConstraints are always live */
    for (int i = 0; i < graph->n_constraints; i++) {
        if (graph->constraints[i]->kind == CN_INVARIANT)
            result.is_live[graph->constraints[i]->node_id] = true;
    }

    varset_free(&seen_vars);
    free(writers);
    return result;
}

/* -----------------------------------------------------------------------
 * Polynomial coefficient type and arithmetic
 *
 * A polynomial P(x) = c0 + c1*x + c2*x^2 + ...
 * Coefficients are either numeric (double) or symbolic (IRExpr*).
 * ----------------------------------------------------------------------- */

typedef struct {
    bool    is_numeric;
    double  num_val;
    IRExpr *ir_val;   /* only valid when !is_numeric; owned by poly */
} PolyCoef;

typedef struct {
    PolyCoef *coeffs;
    int       degree_plus_1;  /* length = degree + 1 */
} Polynomial;

static PolyCoef coef_numeric(double v)
{
    PolyCoef c;
    c.is_numeric = true;
    c.num_val = v;
    c.ir_val = NULL;
    return c;
}

static PolyCoef coef_ir(IRExpr *e)
{
    PolyCoef c;
    c.is_numeric = false;
    c.num_val = 0;
    c.ir_val = e;
    return c;
}

static PolyCoef coef_clone(PolyCoef c)
{
    if (c.is_numeric) return coef_numeric(c.num_val);
    return coef_ir(ir_clone(c.ir_val));
}

static void coef_free(PolyCoef *c)
{
    if (!c->is_numeric && c->ir_val) {
        ir_free(c->ir_val);
        c->ir_val = NULL;
    }
}

static bool coef_is_zero(PolyCoef c)
{
    return c.is_numeric && c.num_val == 0;
}

static bool coef_is_one(PolyCoef c)
{
    return c.is_numeric && c.num_val == 1;
}

static IRExpr *coef_to_ir(PolyCoef c)
{
    if (c.is_numeric) return ir_const(c.num_val);
    return ir_clone(c.ir_val);
}

/* Coefficient arithmetic */

static PolyCoef _coef_neg(PolyCoef c)
{
    if (c.is_numeric) return coef_numeric(-c.num_val);
    return coef_ir(ir_unaryop("-", ir_clone(c.ir_val)));
}

static PolyCoef _coef_add(PolyCoef a, PolyCoef b)
{
    if (a.is_numeric && b.is_numeric)
        return coef_numeric(a.num_val + b.num_val);
    if (a.is_numeric && a.num_val == 0)
        return coef_clone(b);
    if (b.is_numeric && b.num_val == 0)
        return coef_clone(a);
    return coef_ir(ir_binop("+", coef_to_ir(a), coef_to_ir(b)));
}

static PolyCoef _coef_mul(PolyCoef a, PolyCoef b)
{
    if (a.is_numeric && b.is_numeric)
        return coef_numeric(a.num_val * b.num_val);
    if (a.is_numeric && a.num_val == 0)
        return coef_numeric(0);
    if (b.is_numeric && b.num_val == 0)
        return coef_numeric(0);
    if (a.is_numeric && a.num_val == 1)
        return coef_clone(b);
    if (b.is_numeric && b.num_val == 1)
        return coef_clone(a);
    return coef_ir(ir_binop("*", coef_to_ir(a), coef_to_ir(b)));
}

/* Polynomial allocation helpers */

static Polynomial poly_alloc(int len)
{
    Polynomial p;
    p.degree_plus_1 = len;
    p.coeffs = calloc(len, sizeof(PolyCoef));
    for (int i = 0; i < len; i++)
        p.coeffs[i] = coef_numeric(0);
    return p;
}

static void poly_free(Polynomial *p)
{
    if (!p->coeffs) return;
    for (int i = 0; i < p->degree_plus_1; i++)
        coef_free(&p->coeffs[i]);
    free(p->coeffs);
    p->coeffs = NULL;
    p->degree_plus_1 = 0;
}

/* Polynomial arithmetic */

static Polynomial _poly_neg(Polynomial p)
{
    Polynomial r = poly_alloc(p.degree_plus_1);
    for (int i = 0; i < p.degree_plus_1; i++) {
        coef_free(&r.coeffs[i]);
        r.coeffs[i] = _coef_neg(p.coeffs[i]);
    }
    return r;
}

static Polynomial _poly_add(Polynomial a, Polynomial b)
{
    int n = a.degree_plus_1 > b.degree_plus_1
            ? a.degree_plus_1 : b.degree_plus_1;
    Polynomial result = poly_alloc(n);
    for (int i = 0; i < a.degree_plus_1; i++) {
        coef_free(&result.coeffs[i]);
        result.coeffs[i] = coef_clone(a.coeffs[i]);
    }
    for (int i = 0; i < b.degree_plus_1; i++) {
        PolyCoef sum = _coef_add(result.coeffs[i], b.coeffs[i]);
        coef_free(&result.coeffs[i]);
        result.coeffs[i] = sum;
    }
    return result;
}

static Polynomial _poly_sub(Polynomial a, Polynomial b)
{
    Polynomial neg = _poly_neg(b);
    Polynomial result = _poly_add(a, neg);
    poly_free(&neg);
    return result;
}

static Polynomial _poly_mul(Polynomial a, Polynomial b)
{
    if (a.degree_plus_1 == 0 || b.degree_plus_1 == 0)
        return poly_alloc(1);
    int n = a.degree_plus_1 + b.degree_plus_1 - 1;
    Polynomial result = poly_alloc(n);
    for (int i = 0; i < a.degree_plus_1; i++) {
        for (int j = 0; j < b.degree_plus_1; j++) {
            PolyCoef prod = _coef_mul(a.coeffs[i], b.coeffs[j]);
            PolyCoef sum = _coef_add(result.coeffs[i + j], prod);
            coef_free(&prod);
            coef_free(&result.coeffs[i + j]);
            result.coeffs[i + j] = sum;
        }
    }
    return result;
}

/* -----------------------------------------------------------------------
 * Polynomial decomposition: extract poly coefficients from IRExpr
 * ----------------------------------------------------------------------- */

/* Returns success (true) and fills *out, or false if not a polynomial */
static bool _as_polynomial(const IRExpr *expr, const char *var, Polynomial *out)
{
    if (!expr) return false;

    switch (expr->kind) {
    case IR_CONST:
        *out = poly_alloc(1);
        coef_free(&out->coeffs[0]);
        out->coeffs[0] = coef_numeric(expr->as.const_val);
        return true;

    case IR_VAR:
        if (strcmp(expr->as.var_name, var) == 0) {
            /* The variable itself: 0 + 1*var */
            *out = poly_alloc(2);
            coef_free(&out->coeffs[0]);
            out->coeffs[0] = coef_numeric(0);
            coef_free(&out->coeffs[1]);
            out->coeffs[1] = coef_numeric(1);
        } else {
            /* Other variable: opaque runtime constant */
            *out = poly_alloc(1);
            coef_free(&out->coeffs[0]);
            out->coeffs[0] = coef_ir(ir_var(expr->as.var_name));
        }
        return true;

    case IR_BINOP: {
        Polynomial left, right;
        if (!_as_polynomial(expr->as.binop.left, var, &left))
            return false;
        if (!_as_polynomial(expr->as.binop.right, var, &right)) {
            poly_free(&left);
            return false;
        }
        if (strcmp(expr->as.binop.op, "+") == 0) {
            *out = _poly_add(left, right);
        } else if (strcmp(expr->as.binop.op, "-") == 0) {
            *out = _poly_sub(left, right);
        } else if (strcmp(expr->as.binop.op, "*") == 0) {
            *out = _poly_mul(left, right);
        } else {
            poly_free(&left);
            poly_free(&right);
            return false;
        }
        poly_free(&left);
        poly_free(&right);
        return true;
    }

    case IR_UNARYOP:
        if (strcmp(expr->as.unaryop.op, "-") == 0) {
            Polynomial inner;
            if (!_as_polynomial(expr->as.unaryop.operand, var, &inner))
                return false;
            *out = _poly_neg(inner);
            poly_free(&inner);
            return true;
        }
        return false;

    case IR_CALL:
        return false;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Polynomial shift: compute poly(j + s) as polynomial in j
 *
 * Uses binomial expansion. Only works with numeric coefficients.
 * ----------------------------------------------------------------------- */

static bool _poly_shift(Polynomial poly, const IRExpr *s_expr, Polynomial *out)
{
    /* Check that all coefficients are numeric */
    for (int i = 0; i < poly.degree_plus_1; i++) {
        if (!poly.coeffs[i].is_numeric)
            return false;
    }

    /* Determine shift base: (j + s) as polynomial in j */
    Polynomial j_plus_s;
    EvalEnv empty_env;
    empty_env.count = 0;
    double s_val;
    if (ir_eval_const(s_expr, &empty_env, &s_val)) {
        j_plus_s = poly_alloc(2);
        coef_free(&j_plus_s.coeffs[0]);
        j_plus_s.coeffs[0] = coef_numeric(s_val);
        coef_free(&j_plus_s.coeffs[1]);
        j_plus_s.coeffs[1] = coef_numeric(1);
    } else {
        j_plus_s = poly_alloc(2);
        coef_free(&j_plus_s.coeffs[0]);
        j_plus_s.coeffs[0] = coef_ir(ir_clone(s_expr));
        coef_free(&j_plus_s.coeffs[1]);
        j_plus_s.coeffs[1] = coef_numeric(1);
    }

    /* Build result = Σ_k c_k * (j + s)^k */
    Polynomial result = poly_alloc(1);  /* starts as 0 */
    Polynomial power = poly_alloc(1);   /* (j+s)^0 = 1 */
    coef_free(&power.coeffs[0]);
    power.coeffs[0] = coef_numeric(1);

    for (int k = 0; k < poly.degree_plus_1; k++) {
        double ck = poly.coeffs[k].num_val;
        if (ck != 0) {
            Polynomial ck_poly = poly_alloc(1);
            coef_free(&ck_poly.coeffs[0]);
            ck_poly.coeffs[0] = coef_numeric(ck);
            Polynomial term = _poly_mul(ck_poly, power);
            Polynomial new_result = _poly_add(result, term);
            poly_free(&ck_poly);
            poly_free(&term);
            poly_free(&result);
            result = new_result;
        }
        if (k < poly.degree_plus_1 - 1) {
            Polynomial new_power = _poly_mul(power, j_plus_s);
            poly_free(&power);
            power = new_power;
        }
    }

    poly_free(&power);
    poly_free(&j_plus_s);
    *out = result;
    return true;
}

/* -----------------------------------------------------------------------
 * Closed-form summation formulas
 *
 * Σ(j=0..n-1) j^k for standard k values
 * ----------------------------------------------------------------------- */

/* Return IRExpr for Σ(j=0..n-1) j^k, or NULL if unsupported */
static IRExpr *_sum_power_basis(int k, IRExpr *n_expr)
{
    if (k == 0) {
        /* Σ 1 = n */
        return ir_clone(n_expr);
    }
    if (k == 1) {
        /* Σ j = n*(n-1)//2 */
        IRExpr *nm1 = ir_binop("-", ir_clone(n_expr), ir_const(1));
        return ir_binop("//", ir_binop("*", ir_clone(n_expr), nm1), ir_const(2));
    }
    if (k == 2) {
        /* Σ j² = n*(n-1)*(2n-1)//6 */
        IRExpr *nm1 = ir_binop("-", ir_clone(n_expr), ir_const(1));
        IRExpr *two_n_m1 = ir_binop("-",
                                     ir_binop("*", ir_const(2), ir_clone(n_expr)),
                                     ir_const(1));
        return ir_binop("//",
                        ir_binop("*",
                                 ir_binop("*", ir_clone(n_expr), nm1),
                                 two_n_m1),
                        ir_const(6));
    }
    if (k == 3) {
        /* Σ j³ = [n*(n-1)//2]² */
        IRExpr *inner = ir_binop("//",
                                 ir_binop("*",
                                          ir_clone(n_expr),
                                          ir_binop("-", ir_clone(n_expr), ir_const(1))),
                                 ir_const(2));
        return ir_binop("*", ir_clone(inner), inner);
    }
    return NULL;
}

/* Build IRExpr for Σ(j=0..n-1) P(j) given polynomial coefficients */
static IRExpr *_sum_poly_closed_form(Polynomial poly, IRExpr *n_expr)
{
    if (poly.degree_plus_1 == 0)
        return ir_const(0);

    int degree = poly.degree_plus_1 - 1;
    if (degree > MAX_POLY_DEGREE)
        return NULL;

    /* Build basis formulas */
    IRExpr *bases[MAX_POLY_DEGREE + 1];
    for (int k = 0; k <= degree; k++) {
        bases[k] = _sum_power_basis(k, n_expr);
        if (!bases[k]) {
            for (int j = 0; j < k; j++) ir_free(bases[j]);
            return NULL;
        }
    }

    /* Combine: Σ P(j) = Σ_k c_k * basis_k */
    IRExpr *result = NULL;
    for (int k = 0; k <= degree; k++) {
        PolyCoef ck = poly.coeffs[k];
        if (coef_is_zero(ck)) {
            ir_free(bases[k]);
            continue;
        }
        IRExpr *term;
        if (coef_is_one(ck)) {
            term = bases[k];
        } else {
            IRExpr *c_ir = coef_to_ir(ck);
            term = ir_binop("*", c_ir, bases[k]);
        }
        if (result == NULL) {
            result = term;
        } else {
            result = ir_binop("+", result, term);
        }
    }

    if (!result) result = ir_const(0);
    return result;
}

/* -----------------------------------------------------------------------
 * Candidate plan generation
 * ----------------------------------------------------------------------- */

/* Helper: apply op function for constant folding */
static bool _apply_op(const char *op, double acc, double val, double *out)
{
    if (strcmp(op, "add") == 0) { *out = acc + val; return true; }
    if (strcmp(op, "sub") == 0) { *out = acc - val; return true; }
    if (strcmp(op, "mul") == 0) { *out = acc * val; return true; }
    return false;
}

/* Try to evaluate the reduction at compile time */
static ExecutionPlan *_try_constant_fold(const ConstraintNode *r)
{
    EvalEnv empty_env;
    empty_env.count = 0;

    double start_v, end_v, init_v;
    if (!ir_eval_const(r->as.reduction.start, &empty_env, &start_v))
        return NULL;
    if (!ir_eval_const(r->as.reduction.end, &empty_env, &end_v))
        return NULL;

    int n_iters = (int)end_v - (int)start_v;
    if (n_iters > MAX_CONST_FOLD_ITERS || n_iters < 0)
        return NULL;

    if (!ir_eval_const(r->as.reduction.init_val, &empty_env, &init_v))
        return NULL;

    double result = init_v;
    for (int i = (int)start_v; i < (int)end_v; i++) {
        EvalEnv env;
        env.count = 1;
        env.names[0] = r->as.reduction.loop_var;
        env.values[0] = (double)i;

        double body_v;
        if (!ir_eval_const(r->as.reduction.body, &env, &body_v))
            return NULL;
        if (!_apply_op(r->as.reduction.op, result, body_v, &result))
            return NULL;
    }

    ExecutionPlan *plan = calloc(1, sizeof(ExecutionPlan));
    plan->kind = PLAN_CONSTANT;
    strncpy(plan->cost_class, "O(1) constant", sizeof(plan->cost_class) - 1);
    strncpy(plan->as.constant.accumulator, r->as.reduction.accumulator,
            sizeof(plan->as.constant.accumulator) - 1);
    plan->as.constant.value = (int64_t)result;
    return plan;
}

/* Try closed-form summation for additive reductions */
static ExecutionPlan *_try_closed_form_sum(const ConstraintNode *r)
{
    const char *var = r->as.reduction.loop_var;

    Polynomial raw_poly;
    if (!_as_polynomial(r->as.reduction.body, var, &raw_poly))
        return NULL;

    int raw_degree = raw_poly.degree_plus_1 - 1;
    if (raw_degree > MAX_POLY_DEGREE) {
        poly_free(&raw_poly);
        return NULL;
    }

    /* n = end - start */
    IRExpr *n_expr = ir_binop("-",
                               ir_clone(r->as.reduction.end),
                               ir_clone(r->as.reduction.start));
    /* s = start */
    const IRExpr *s_expr = r->as.reduction.start;

    /* Shift polynomial: replace i with (j + s) */
    Polynomial shifted;
    if (!_poly_shift(raw_poly, s_expr, &shifted)) {
        poly_free(&raw_poly);
        ir_free(n_expr);
        return NULL;
    }

    IRExpr *formula = _sum_poly_closed_form(shifted, n_expr);
    poly_free(&raw_poly);
    poly_free(&shifted);
    ir_free(n_expr);

    if (!formula)
        return NULL;

    /* Build description */
    char desc[128];
    snprintf(desc, sizeof(desc),
             "Closed-form sum of degree-%d polynomial",
             raw_degree);

    ExecutionPlan *plan = calloc(1, sizeof(ExecutionPlan));
    plan->kind = PLAN_CLOSED_FORM;
    strncpy(plan->cost_class, "O(1)", sizeof(plan->cost_class) - 1);
    strncpy(plan->as.closed_form.accumulator, r->as.reduction.accumulator,
            sizeof(plan->as.closed_form.accumulator) - 1);
    plan->as.closed_form.formula = formula;
    strncpy(plan->as.closed_form.description, desc,
            sizeof(plan->as.closed_form.description) - 1);
    return plan;
}

/* Try closed-form for multiplicative reductions (limited) */
static ExecutionPlan *_try_closed_form_product(const ConstraintNode *r)
{
    const char *var = r->as.reduction.loop_var;

    Polynomial poly;
    if (!_as_polynomial(r->as.reduction.body, var, &poly))
        return NULL;

    /* Only handle degree-0 (constant body) */
    if (poly.degree_plus_1 != 1 || !poly.coeffs[0].is_numeric) {
        poly_free(&poly);
        return NULL;
    }

    double c = poly.coeffs[0].num_val;
    int c_int = (int)c;
    poly_free(&poly);

    if (c_int == 1) {
        ExecutionPlan *plan = calloc(1, sizeof(ExecutionPlan));
        plan->kind = PLAN_CLOSED_FORM;
        strncpy(plan->cost_class, "O(1) constant", sizeof(plan->cost_class) - 1);
        strncpy(plan->as.closed_form.accumulator, r->as.reduction.accumulator,
                sizeof(plan->as.closed_form.accumulator) - 1);
        plan->as.closed_form.formula = ir_const(1);
        strncpy(plan->as.closed_form.description, "Product of 1s = 1",
                sizeof(plan->as.closed_form.description) - 1);
        return plan;
    }
    if (c_int == 0) {
        ExecutionPlan *plan = calloc(1, sizeof(ExecutionPlan));
        plan->kind = PLAN_CLOSED_FORM;
        strncpy(plan->cost_class, "O(1) constant", sizeof(plan->cost_class) - 1);
        strncpy(plan->as.closed_form.accumulator, r->as.reduction.accumulator,
                sizeof(plan->as.closed_form.accumulator) - 1);
        plan->as.closed_form.formula = ir_const(0);
        strncpy(plan->as.closed_form.description, "Product of 0s = 0",
                sizeof(plan->as.closed_form.description) - 1);
        return plan;
    }
    return NULL;
}

/* Build a LoopPlan (always valid) */
static ExecutionPlan *_make_loop_plan(const ConstraintNode *r)
{
    ExecutionPlan *plan = calloc(1, sizeof(ExecutionPlan));
    plan->kind = PLAN_LOOP;
    strncpy(plan->cost_class, "O(n)", sizeof(plan->cost_class) - 1);
    strncpy(plan->as.loop.accumulator, r->as.reduction.accumulator,
            sizeof(plan->as.loop.accumulator) - 1);
    strncpy(plan->as.loop.op, r->as.reduction.op,
            sizeof(plan->as.loop.op) - 1);
    strncpy(plan->as.loop.loop_var, r->as.reduction.loop_var,
            sizeof(plan->as.loop.loop_var) - 1);
    plan->as.loop.start    = ir_clone(r->as.reduction.start);
    plan->as.loop.end      = ir_clone(r->as.reduction.end);
    plan->as.loop.body     = ir_clone(r->as.reduction.body);
    plan->as.loop.init_val = ir_clone(r->as.reduction.init_val);
    return plan;
}

/* Build an AssignPlan */
static ExecutionPlan *_make_assign_plan(const ConstraintNode *n)
{
    ExecutionPlan *plan = calloc(1, sizeof(ExecutionPlan));
    plan->kind = PLAN_ASSIGN;
    strncpy(plan->cost_class, "O(1)", sizeof(plan->cost_class) - 1);
    strncpy(plan->as.assign.var, n->as.assign.var,
            sizeof(plan->as.assign.var) - 1);
    plan->as.assign.expr = ir_clone(n->as.assign.expr);
    return plan;
}

/* -----------------------------------------------------------------------
 * Planner: main entry point
 * ----------------------------------------------------------------------- */

FunctionPlan *telos_plan_function(ConstraintGraph *graph)
{
    FunctionPlan *fp = calloc(1, sizeof(FunctionPlan));
    strncpy(fp->name, graph->name, sizeof(fp->name) - 1);
    fp->n_params = graph->n_params;
    if (graph->n_params > 0) {
        fp->param_names = malloc(sizeof(char *) * graph->n_params);
        for (int i = 0; i < graph->n_params; i++)
            fp->param_names[i] = strdup(graph->param_names[i]);
    }
    fp->steps = NULL;
    fp->n_steps = 0;
    fp->cap_steps = 0;
    fp->return_expr = NULL;

    /* Compute liveness */
    LivenessResult liveness = telos_compute_liveness(graph);

    /* Plan each live node */
    for (int i = 0; i < graph->n_constraints; i++) {
        ConstraintNode *cn = graph->constraints[i];
        if (!liveness.is_live[cn->node_id])
            continue;

        switch (cn->kind) {
        case CN_INVARIANT:
            /* Params are represented by the function signature */
            continue;

        case CN_RETURN:
            fp->return_expr = ir_clone(cn->as.ret.expr);
            continue;

        case CN_REDUCTION: {
            /* Generate candidates */
            ExecutionPlan *candidates[4];
            int n_candidates = 0;

            /* Plan A: loop (always valid) */
            candidates[n_candidates++] = _make_loop_plan(cn);

            /* Plan B: constant fold */
            ExecutionPlan *cf = _try_constant_fold(cn);
            if (cf) candidates[n_candidates++] = cf;

            /* Plan C: algebraic closed form */
            if (strcmp(cn->as.reduction.op, "add") == 0) {
                ExecutionPlan *closed = _try_closed_form_sum(cn);
                if (closed) candidates[n_candidates++] = closed;
            } else if (strcmp(cn->as.reduction.op, "mul") == 0) {
                ExecutionPlan *closed = _try_closed_form_product(cn);
                if (closed) candidates[n_candidates++] = closed;
            }

            /* Pick cheapest */
            ExecutionPlan *best = plan_cheapest(candidates, n_candidates);

            /* Add to steps (grow array if needed) */
            if (fp->n_steps >= fp->cap_steps) {
                fp->cap_steps = fp->cap_steps ? fp->cap_steps * 2 : 8;
                fp->steps = realloc(fp->steps,
                                    sizeof(ExecutionPlan *) * fp->cap_steps);
            }
            fp->steps[fp->n_steps++] = best;

            /* Free non-selected candidates */
            for (int j = 0; j < n_candidates; j++) {
                if (candidates[j] != best) {
                    /* Free the plan */
                    switch (candidates[j]->kind) {
                    case PLAN_LOOP:
                        ir_free(candidates[j]->as.loop.start);
                        ir_free(candidates[j]->as.loop.end);
                        ir_free(candidates[j]->as.loop.body);
                        ir_free(candidates[j]->as.loop.init_val);
                        break;
                    case PLAN_CLOSED_FORM:
                        ir_free(candidates[j]->as.closed_form.formula);
                        break;
                    case PLAN_CONSTANT:
                    case PLAN_ASSIGN:
                        if (candidates[j]->kind == PLAN_ASSIGN)
                            ir_free(candidates[j]->as.assign.expr);
                        break;
                    }
                    free(candidates[j]);
                }
            }
            break;
        }

        case CN_ASSIGN: {
            ExecutionPlan *ap = _make_assign_plan(cn);
            if (fp->n_steps >= fp->cap_steps) {
                fp->cap_steps = fp->cap_steps ? fp->cap_steps * 2 : 8;
                fp->steps = realloc(fp->steps,
                                    sizeof(ExecutionPlan *) * fp->cap_steps);
            }
            fp->steps[fp->n_steps++] = ap;
            break;
        }

        case CN_COND_BRANCH:
            /* Not yet planned — skipped for now */
            break;
        }
    }

    free(liveness.is_live);
    return fp;
}

/* -----------------------------------------------------------------------
 * Free a FunctionPlan
 * ----------------------------------------------------------------------- */

static void _free_execution_plan(ExecutionPlan *plan)
{
    if (!plan) return;
    switch (plan->kind) {
    case PLAN_LOOP:
        ir_free(plan->as.loop.start);
        ir_free(plan->as.loop.end);
        ir_free(plan->as.loop.body);
        ir_free(plan->as.loop.init_val);
        break;
    case PLAN_CLOSED_FORM:
        ir_free(plan->as.closed_form.formula);
        break;
    case PLAN_CONSTANT:
        break;
    case PLAN_ASSIGN:
        ir_free(plan->as.assign.expr);
        break;
    }
    free(plan);
}

void telos_free_function_plan(FunctionPlan *plan)
{
    if (!plan) return;
    for (int i = 0; i < plan->n_params; i++)
        free(plan->param_names[i]);
    free(plan->param_names);
    for (int i = 0; i < plan->n_steps; i++)
        _free_execution_plan(plan->steps[i]);
    free(plan->steps);
    ir_free(plan->return_expr);
    free(plan);
}
