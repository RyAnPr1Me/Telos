/*
 * codegen_c.c — C source code generator for Telos.
 *
 * Translates a FunctionPlan into a portable C function definition
 * that uses `long long` for all integer values (64-bit).
 */

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "telos.h"

/* ----------------------------------------------------------------------- */
/* String builder                                                          */
/* ----------------------------------------------------------------------- */

typedef struct {
    char *buf;
    int   len;
    int   cap;
    /* Track declared variables */
    char  declared[256][64];
    int   n_declared;
} CGenState;

static void cg_init(CGenState *s)
{
    s->cap = 8192;
    s->buf = malloc(s->cap);
    s->buf[0] = '\0';
    s->len = 0;
    s->n_declared = 0;
}

static void cg_ensure(CGenState *s, int extra)
{
    while (s->len + extra + 1 > s->cap) {
        s->cap *= 2;
        s->buf = realloc(s->buf, s->cap);
    }
}

static void cg_append(CGenState *s, const char *text)
{
    int n = (int)strlen(text);
    cg_ensure(s, n);
    memcpy(s->buf + s->len, text, n + 1);
    s->len += n;
}

static void cg_appendf(CGenState *s, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    cg_ensure(s, n);
    memcpy(s->buf + s->len, tmp, n + 1);
    s->len += n;
}

/* ----------------------------------------------------------------------- */
/* Declaration tracking                                                    */
/* ----------------------------------------------------------------------- */

static int cg_is_declared(CGenState *s, const char *name)
{
    for (int i = 0; i < s->n_declared; i++) {
        if (strcmp(s->declared[i], name) == 0)
            return 1;
    }
    return 0;
}

static void cg_mark_declared(CGenState *s, const char *name)
{
    if (cg_is_declared(s, name)) return;
    if (s->n_declared < 256)
        strncpy(s->declared[s->n_declared++], name,
                sizeof(s->declared[0]) - 1);
}

/* Returns "long long " for the first assignment, "" thereafter. */
static const char *cg_decl_prefix(CGenState *s, const char *name)
{
    if (!cg_is_declared(s, name)) {
        cg_mark_declared(s, name);
        return "long long ";
    }
    return "";
}

/* ----------------------------------------------------------------------- */
/* Expression emitter                                                      */
/* ----------------------------------------------------------------------- */

static void emit_expr(CGenState *s, const IRExpr *expr);

static void emit_expr(CGenState *s, const IRExpr *expr)
{
    if (!expr) {
        cg_append(s, "0LL");
        return;
    }

    switch (expr->kind) {
    case IR_CONST: {
        double v = expr->as.const_val;
        long long iv = (long long)v;
        if ((double)iv == v) {
            cg_appendf(s, "%lldLL", iv);
        } else {
            cg_appendf(s, "%g", v);
        }
        break;
    }
    case IR_VAR:
        cg_append(s, expr->as.var_name);
        break;

    case IR_BINOP: {
        const char *op = expr->as.binop.op;
        const char *c_op = op;
        if (strcmp(op, "//") == 0) c_op = "/";

        cg_append(s, "(");
        emit_expr(s, expr->as.binop.left);
        cg_appendf(s, " %s ", c_op);
        emit_expr(s, expr->as.binop.right);
        cg_append(s, ")");
        break;
    }
    case IR_UNARYOP: {
        const char *op = expr->as.unaryop.op;
        if (strcmp(op, "-") == 0) {
            cg_append(s, "(-");
            emit_expr(s, expr->as.unaryop.operand);
            cg_append(s, ")");
        } else if (strcmp(op, "!") == 0) {
            cg_append(s, "(!");
            emit_expr(s, expr->as.unaryop.operand);
            cg_append(s, ")");
        } else {
            cg_appendf(s, "(%s", op);
            emit_expr(s, expr->as.unaryop.operand);
            cg_append(s, ")");
        }
        break;
    }
    case IR_CALL: {
        cg_appendf(s, "%s(", expr->as.call.name);
        for (int i = 0; i < expr->as.call.n_args; i++) {
            if (i > 0) cg_append(s, ", ");
            emit_expr(s, expr->as.call.args[i]);
        }
        cg_append(s, ")");
        break;
    }
    }
}

/* Simplify an expression, emit it into the buffer, then free the clone. */
static void emit_simplified(CGenState *s, IRExpr *raw)
{
    IRExpr *expr = ir_simplify(ir_clone(raw));
    emit_expr(s, expr);
    ir_free(expr);
}

/* ----------------------------------------------------------------------- */
/* Step emitters                                                           */
/* ----------------------------------------------------------------------- */

static void emit_constant(CGenState *s, ExecutionPlan *step)
{
    const char *pfx = cg_decl_prefix(s, step->as.constant.accumulator);
    cg_appendf(s, "    %s%s = %lldLL; /* compile-time constant */\n",
               pfx, step->as.constant.accumulator,
               (long long)step->as.constant.value);
}

static void emit_closed_form(CGenState *s, ExecutionPlan *step)
{
    const char *pfx = cg_decl_prefix(s, step->as.closed_form.accumulator);
    cg_appendf(s, "    %s%s = ", pfx, step->as.closed_form.accumulator);
    emit_simplified(s, step->as.closed_form.formula);
    cg_append(s, ";");
    if (step->as.closed_form.description[0] != '\0')
        cg_appendf(s, " /* %s */", step->as.closed_form.description);
    cg_append(s, "\n");
}

static void emit_loop(CGenState *s, ExecutionPlan *step)
{
    const char *acc = step->as.loop.accumulator;
    const char *var = step->as.loop.loop_var;
    const char *pfx = cg_decl_prefix(s, acc);

    /* init value */
    cg_appendf(s, "    %s%s = ", pfx, acc);
    emit_simplified(s, step->as.loop.init_val);
    cg_append(s, "; /* loop fallback */\n");

    /* Mark loop variable as declared (it's declared in for-init). */
    cg_mark_declared(s, var);

    /* for header */
    cg_appendf(s, "    for (long long %s = ", var);
    emit_simplified(s, step->as.loop.start);
    cg_appendf(s, "; %s < ", var);
    emit_simplified(s, step->as.loop.end);
    cg_appendf(s, "; %s++) {\n", var);

    /* accumulation op */
    const char *op_str = "+=";
    if (strcmp(step->as.loop.op, "sub") == 0) op_str = "-=";
    else if (strcmp(step->as.loop.op, "mul") == 0) op_str = "*=";

    cg_appendf(s, "        %s %s ", acc, op_str);
    emit_simplified(s, step->as.loop.body);
    cg_append(s, ";\n");

    cg_append(s, "    }\n");
}

static void emit_assign(CGenState *s, ExecutionPlan *step)
{
    const char *pfx = cg_decl_prefix(s, step->as.assign.var);
    cg_appendf(s, "    %s%s = ", pfx, step->as.assign.var);
    emit_simplified(s, step->as.assign.expr);
    cg_append(s, ";\n");
}

/* ----------------------------------------------------------------------- */
/* Public API                                                              */
/* ----------------------------------------------------------------------- */

char *telos_gen_c(FunctionPlan *plan)
{
    if (!plan) return NULL;

    CGenState s;
    cg_init(&s);

    /* Mark parameters as already declared. */
    for (int i = 0; i < plan->n_params; i++)
        cg_mark_declared(&s, plan->param_names[i]);

    /* Function signature */
    cg_appendf(&s, "long long %s(", plan->name);
    if (plan->n_params == 0) {
        cg_append(&s, "void");
    } else {
        for (int i = 0; i < plan->n_params; i++) {
            if (i > 0) cg_append(&s, ", ");
            cg_appendf(&s, "long long %s", plan->param_names[i]);
        }
    }
    cg_append(&s, ") {\n");

    /* Steps */
    for (int i = 0; i < plan->n_steps; i++) {
        ExecutionPlan *step = plan->steps[i];
        switch (step->kind) {
        case PLAN_CONSTANT:   emit_constant(&s, step);    break;
        case PLAN_CLOSED_FORM: emit_closed_form(&s, step); break;
        case PLAN_LOOP:       emit_loop(&s, step);        break;
        case PLAN_ASSIGN:     emit_assign(&s, step);      break;
        }
    }

    /* Return statement */
    if (plan->return_expr) {
        cg_append(&s, "    return ");
        emit_simplified(&s, plan->return_expr);
        cg_append(&s, ";\n");
    } else {
        cg_append(&s, "    return 0LL;\n");
    }

    cg_append(&s, "}\n");

    /* Shrink to fit and return */
    char *result = realloc(s.buf, s.len + 1);
    return result ? result : s.buf;
}
