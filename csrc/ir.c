/*
 * ir.c — IR expression constructors, constraint graph, simplifier,
 *         and constant evaluator for the Telos compiler.
 */

#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "telos.h"

/* -----------------------------------------------------------------------
 * IR Expression constructors
 * ----------------------------------------------------------------------- */

IRExpr *ir_const(double val)
{
    IRExpr *e = calloc(1, sizeof(IRExpr));
    e->kind = IR_CONST;
    e->as.const_val = val;
    return e;
}

IRExpr *ir_var(const char *name)
{
    IRExpr *e = calloc(1, sizeof(IRExpr));
    e->kind = IR_VAR;
    strncpy(e->as.var_name, name, sizeof(e->as.var_name) - 1);
    return e;
}

IRExpr *ir_binop(const char *op, IRExpr *left, IRExpr *right)
{
    IRExpr *e = calloc(1, sizeof(IRExpr));
    e->kind = IR_BINOP;
    strncpy(e->as.binop.op, op, sizeof(e->as.binop.op) - 1);
    e->as.binop.left  = left;
    e->as.binop.right = right;
    return e;
}

IRExpr *ir_unaryop(const char *op, IRExpr *operand)
{
    IRExpr *e = calloc(1, sizeof(IRExpr));
    e->kind = IR_UNARYOP;
    strncpy(e->as.unaryop.op, op, sizeof(e->as.unaryop.op) - 1);
    e->as.unaryop.operand = operand;
    return e;
}

IRExpr *ir_call(const char *name, IRExpr **args, int n_args)
{
    IRExpr *e = calloc(1, sizeof(IRExpr));
    e->kind = IR_CALL;
    strncpy(e->as.call.name, name, sizeof(e->as.call.name) - 1);
    e->as.call.n_args = n_args;
    if (n_args > 0) {
        e->as.call.args = malloc(sizeof(IRExpr *) * n_args);
        for (int i = 0; i < n_args; i++)
            e->as.call.args[i] = args[i];
    } else {
        e->as.call.args = NULL;
    }
    return e;
}

/* -----------------------------------------------------------------------
 * Deep copy / free
 * ----------------------------------------------------------------------- */

IRExpr *ir_clone(const IRExpr *e)
{
    if (!e) return NULL;

    switch (e->kind) {
    case IR_CONST:
        return ir_const(e->as.const_val);

    case IR_VAR:
        return ir_var(e->as.var_name);

    case IR_BINOP:
        return ir_binop(e->as.binop.op,
                        ir_clone(e->as.binop.left),
                        ir_clone(e->as.binop.right));

    case IR_UNARYOP:
        return ir_unaryop(e->as.unaryop.op,
                          ir_clone(e->as.unaryop.operand));

    case IR_CALL: {
        IRExpr **cloned_args = NULL;
        if (e->as.call.n_args > 0) {
            cloned_args = malloc(sizeof(IRExpr *) * e->as.call.n_args);
            for (int i = 0; i < e->as.call.n_args; i++)
                cloned_args[i] = ir_clone(e->as.call.args[i]);
        }
        IRExpr *c = ir_call(e->as.call.name, cloned_args, e->as.call.n_args);
        free(cloned_args);
        return c;
    }
    }
    return NULL;
}

void ir_free(IRExpr *e)
{
    if (!e) return;

    switch (e->kind) {
    case IR_CONST:
    case IR_VAR:
        break;

    case IR_BINOP:
        ir_free(e->as.binop.left);
        ir_free(e->as.binop.right);
        break;

    case IR_UNARYOP:
        ir_free(e->as.unaryop.operand);
        break;

    case IR_CALL:
        for (int i = 0; i < e->as.call.n_args; i++)
            ir_free(e->as.call.args[i]);
        free(e->as.call.args);
        break;
    }
    free(e);
}

/* -----------------------------------------------------------------------
 * Constraint Graph
 * ----------------------------------------------------------------------- */

ConstraintGraph *cg_create(const char *name, char **params, int n_params)
{
    ConstraintGraph *g = calloc(1, sizeof(ConstraintGraph));
    strncpy(g->name, name, sizeof(g->name) - 1);
    g->n_params = n_params;
    if (n_params > 0) {
        g->param_names = malloc(sizeof(char *) * n_params);
        for (int i = 0; i < n_params; i++)
            g->param_names[i] = strdup(params[i]);
    }
    g->constraints     = NULL;
    g->n_constraints   = 0;
    g->cap_constraints = 0;
    g->next_id         = 0;
    return g;
}

ConstraintNode *cg_add(ConstraintGraph *g, ConstraintNode *node)
{
    if (g->n_constraints >= g->cap_constraints) {
        int new_cap = g->cap_constraints ? g->cap_constraints * 2
                                         : DA_INIT_CAP;
        ConstraintNode **tmp = realloc(g->constraints,
                                       sizeof(ConstraintNode *) * new_cap);
        if (!tmp) {
            fprintf(stderr, "cg_add: out of memory\n");
            return NULL;
        }
        g->constraints     = tmp;
        g->cap_constraints = new_cap;
    }

    ConstraintNode *copy = malloc(sizeof(ConstraintNode));
    *copy = *node;
    copy->node_id = g->next_id++;

    g->constraints[g->n_constraints++] = copy;
    return copy;
}

static void cn_free(ConstraintNode *cn)
{
    if (!cn) return;

    switch (cn->kind) {
    case CN_INVARIANT:
        ir_free(cn->as.invariant.expr);
        break;
    case CN_ASSIGN:
        ir_free(cn->as.assign.expr);
        break;
    case CN_REDUCTION:
        ir_free(cn->as.reduction.start);
        ir_free(cn->as.reduction.end);
        ir_free(cn->as.reduction.body);
        ir_free(cn->as.reduction.init_val);
        break;
    case CN_RETURN:
        ir_free(cn->as.ret.expr);
        break;
    case CN_COND_BRANCH:
        ir_free(cn->as.cond_branch.cond);
        for (int i = 0; i < cn->as.cond_branch.n_then; i++)
            cn_free(cn->as.cond_branch.then_nodes[i]);
        free(cn->as.cond_branch.then_nodes);
        for (int i = 0; i < cn->as.cond_branch.n_else; i++)
            cn_free(cn->as.cond_branch.else_nodes[i]);
        free(cn->as.cond_branch.else_nodes);
        break;
    }
    free(cn);
}

void cg_free(ConstraintGraph *g)
{
    if (!g) return;

    for (int i = 0; i < g->n_params; i++)
        free(g->param_names[i]);
    free(g->param_names);

    for (int i = 0; i < g->n_constraints; i++)
        cn_free(g->constraints[i]);
    free(g->constraints);

    free(g);
}

/* -----------------------------------------------------------------------
 * Simplifier helpers (all static)
 * ----------------------------------------------------------------------- */

static bool _is_const(const IRExpr *e, double v)
{
    return e->kind == IR_CONST && e->as.const_val == v;
}

static bool _fold(const char *op, double a, double b, double *out)
{
    if (strcmp(op, "+") == 0)       { *out = a + b; return true; }
    if (strcmp(op, "-") == 0)       { *out = a - b; return true; }
    if (strcmp(op, "*") == 0)       { *out = a * b; return true; }
    if (strcmp(op, "/") == 0) {
        if (b == 0) return false;
        *out = a / b;
        return true;
    }
    if (strcmp(op, "//") == 0) {
        if (b == 0) return false;
        *out = floor(a / b);
        return true;
    }
    if (strcmp(op, "%") == 0) {
        if (b == 0) return false;
        *out = fmod(a, b);
        return true;
    }
    return false;
}

static IRExpr *_simplify_binop(const char *op, IRExpr *left, IRExpr *right)
{
    /* Constant folding */
    if (left->kind == IR_CONST && right->kind == IR_CONST) {
        double v;
        if (_fold(op, left->as.const_val, right->as.const_val, &v)) {
            ir_free(left);
            ir_free(right);
            return ir_const(v);
        }
    }

    /* Identity / absorbing-element rules */
    if (strcmp(op, "+") == 0) {
        if (_is_const(left, 0))  { ir_free(left);  return right; }
        if (_is_const(right, 0)) { ir_free(right); return left;  }
    }
    else if (strcmp(op, "-") == 0) {
        if (_is_const(right, 0)) { ir_free(right); return left;  }
        if (_is_const(left, 0)) {
            ir_free(left);
            return ir_unaryop("-", right);
        }
    }
    else if (strcmp(op, "*") == 0) {
        if (_is_const(left, 0) || _is_const(right, 0)) {
            ir_free(left);
            ir_free(right);
            return ir_const(0);
        }
        if (_is_const(left, 1))  { ir_free(left);  return right; }
        if (_is_const(right, 1)) { ir_free(right); return left;  }
    }
    else if (strcmp(op, "/") == 0 || strcmp(op, "//") == 0) {
        if (_is_const(right, 1)) { ir_free(right); return left;  }
        if (_is_const(left, 0)) {
            ir_free(left);
            ir_free(right);
            return ir_const(0);
        }
    }
    else if (strcmp(op, "%") == 0) {
        if (_is_const(left, 0)) {
            ir_free(left);
            ir_free(right);
            return ir_const(0);
        }
    }

    return ir_binop(op, left, right);
}

/* -----------------------------------------------------------------------
 * Public simplifier — recursive bottom-up, returns new tree
 * ----------------------------------------------------------------------- */

IRExpr *ir_simplify(IRExpr *expr)
{
    if (!expr) return NULL;

    switch (expr->kind) {
    case IR_CONST:
        return ir_clone(expr);

    case IR_VAR:
        return ir_clone(expr);

    case IR_UNARYOP: {
        IRExpr *inner = ir_simplify(expr->as.unaryop.operand);
        if (inner->kind == IR_CONST && strcmp(expr->as.unaryop.op, "-") == 0) {
            double v = -inner->as.const_val;
            ir_free(inner);
            return ir_const(v);
        }
        return ir_unaryop(expr->as.unaryop.op, inner);
    }

    case IR_BINOP: {
        IRExpr *left  = ir_simplify(expr->as.binop.left);
        IRExpr *right = ir_simplify(expr->as.binop.right);
        return _simplify_binop(expr->as.binop.op, left, right);
    }

    case IR_CALL: {
        int n = expr->as.call.n_args;
        IRExpr **args = NULL;
        if (n > 0) {
            args = malloc(sizeof(IRExpr *) * n);
            for (int i = 0; i < n; i++)
                args[i] = ir_simplify(expr->as.call.args[i]);
        }
        IRExpr *c = ir_call(expr->as.call.name, args, n);
        free(args);
        return c;
    }
    }
    return ir_clone(expr);
}

/* -----------------------------------------------------------------------
 * Constant evaluation
 * ----------------------------------------------------------------------- */

bool ir_eval_const(const IRExpr *expr, const EvalEnv *env, double *out)
{
    if (!expr) return false;

    switch (expr->kind) {
    case IR_CONST:
        *out = expr->as.const_val;
        return true;

    case IR_VAR:
        if (env) {
            for (int i = 0; i < env->count; i++) {
                if (strcmp(env->names[i], expr->as.var_name) == 0) {
                    *out = env->values[i];
                    return true;
                }
            }
        }
        return false;

    case IR_BINOP: {
        double a, b;
        if (!ir_eval_const(expr->as.binop.left, env, &a))  return false;
        if (!ir_eval_const(expr->as.binop.right, env, &b)) return false;
        return _fold(expr->as.binop.op, a, b, out);
    }

    case IR_UNARYOP: {
        double v;
        if (!ir_eval_const(expr->as.unaryop.operand, env, &v)) return false;
        if (strcmp(expr->as.unaryop.op, "-") == 0) {
            *out = -v;
            return true;
        }
        return false;
    }

    case IR_CALL:
        return false;
    }
    return false;
}
