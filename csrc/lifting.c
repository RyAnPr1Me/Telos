/*
 * lifting.c — Semantic lifting: convert AST to Constraint IR.
 *
 * Extracts high-level semantic intent from the surface syntax and encodes
 * it as semantic constraint nodes in a ConstraintGraph.
 *
 * Key patterns detected:
 *   - Accumulation loops (for-loop with single compound assignment body)
 *     → ReductionConstraint
 *   - Simple assignments → AssignConstraint
 *   - Parameters → InvariantConstraint
 *   - Conditional branches → CondBranchConstraint
 */

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "telos.h"

/* -----------------------------------------------------------------------
 * Compound operator mappings
 * ----------------------------------------------------------------------- */

/* Map compound assignment operator to reduction op name. */
static const char *accum_op(const char *op)
{
    if (strcmp(op, "+=") == 0) return "add";
    if (strcmp(op, "-=") == 0) return "sub";
    if (strcmp(op, "*=") == 0) return "mul";
    return NULL;
}

/* Map compound assignment operator to binary operator. */
static const char *compound_to_binop(const char *op)
{
    if (strcmp(op, "+=") == 0) return "+";
    if (strcmp(op, "-=") == 0) return "-";
    if (strcmp(op, "*=") == 0) return "*";
    if (strcmp(op, "/=") == 0) return "/";
    if (strcmp(op, "%=") == 0) return "%";
    return NULL;
}

/* -----------------------------------------------------------------------
 * Simple string-keyed environment (variable origin tracking)
 * ----------------------------------------------------------------------- */

#define ENV_MAX 128

typedef struct {
    char keys[ENV_MAX][64];
    char vals[ENV_MAX][16];   /* "param", "local", "loop_var" */
    int  count;
} LiftEnv;

static void env_init(LiftEnv *e) { e->count = 0; }

static void env_set(LiftEnv *e, const char *key, const char *val)
{
    /* Update existing entry if present. */
    for (int i = 0; i < e->count; i++) {
        if (strcmp(e->keys[i], key) == 0) {
            strncpy(e->vals[i], val, sizeof(e->vals[i]) - 1);
            e->vals[i][sizeof(e->vals[i]) - 1] = '\0';
            return;
        }
    }
    if (e->count >= ENV_MAX) return;
    strncpy(e->keys[e->count], key, sizeof(e->keys[0]) - 1);
    e->keys[e->count][sizeof(e->keys[0]) - 1] = '\0';
    strncpy(e->vals[e->count], val, sizeof(e->vals[0]) - 1);
    e->vals[e->count][sizeof(e->vals[0]) - 1] = '\0';
    e->count++;
}

static void env_copy(LiftEnv *dst, const LiftEnv *src)
{
    memcpy(dst, src, sizeof(LiftEnv));
}

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */

static void    lift_stmt(Stmt *stmt, ConstraintGraph *g, LiftEnv *env);
static void    lift_block(Stmt *block, ConstraintGraph *g, LiftEnv *env);
static IRExpr *lift_expr(Expr *expr, LiftEnv *env);

/* -----------------------------------------------------------------------
 * Pattern-detection helpers for reduction matching
 * ----------------------------------------------------------------------- */

/*
 * _detect_init: extract loop variable name and start expression from
 * the for-loop init clause.
 * Returns true on success, setting *loop_var and *start_expr.
 */
static bool detect_init(Stmt *init, const char **loop_var, Expr **start_expr)
{
    if (!init) return false;

    if (init->kind == STMT_VAR_DECL && init->as.var_decl.init != NULL) {
        *loop_var   = init->as.var_decl.name;
        *start_expr = init->as.var_decl.init;
        return true;
    }

    /* ExprStmt wrapping Assignment with "=" */
    if (init->kind == STMT_EXPR_STMT) {
        Expr *e = init->as.expr_stmt.expr;
        if (e && e->kind == EXPR_ASSIGNMENT && strcmp(e->as.assign.op, "=") == 0) {
            *loop_var   = e->as.assign.target;
            *start_expr = e->as.assign.value;
            return true;
        }
    }

    return false;
}

/*
 * _detect_cond: check that the condition is `loop_var < end` or
 * `loop_var <= end`.  Sets *end_expr and *inclusive accordingly.
 */
static bool detect_cond(Expr *cond, const char *loop_var,
                         Expr **end_expr, bool *inclusive)
{
    if (!cond || cond->kind != EXPR_BINARY_OP)
        return false;

    Expr *left = cond->as.binop.left;
    if (!left || left->kind != EXPR_IDENTIFIER)
        return false;
    if (strcmp(left->as.name, loop_var) != 0)
        return false;

    if (strcmp(cond->as.binop.op, "<") == 0) {
        *end_expr  = cond->as.binop.right;
        *inclusive  = false;
        return true;
    }
    if (strcmp(cond->as.binop.op, "<=") == 0) {
        *end_expr  = cond->as.binop.right;
        *inclusive  = true;
        return true;
    }
    return false;
}

/*
 * _detect_increment: return true if `update` increments `loop_var` by 1.
 * Accepts: i++, ++i, i += 1.
 */
static bool detect_increment(Expr *update, const char *loop_var)
{
    if (!update) return false;

    if (update->kind == EXPR_UNARY_OP) {
        if (strcmp(update->as.unaryop.op, "++") != 0)
            return false;
        Expr *operand = update->as.unaryop.operand;
        return operand && operand->kind == EXPR_IDENTIFIER
            && strcmp(operand->as.name, loop_var) == 0;
    }

    if (update->kind == EXPR_ASSIGNMENT) {
        return strcmp(update->as.assign.target, loop_var) == 0
            && strcmp(update->as.assign.op, "+=") == 0
            && update->as.assign.value != NULL
            && update->as.assign.value->kind == EXPR_INT_LIT
            && update->as.assign.value->as.int_val == 1;
    }

    return false;
}

/*
 * _detect_accumulation: match body statement `acc op= f(loop_var)`.
 * Returns true on success, setting *accumulator, *op_name, *body_expr.
 */
static bool detect_accumulation(Stmt *stmt,
                                 const char **accumulator,
                                 const char **op_name,
                                 Expr       **body_expr)
{
    if (!stmt || stmt->kind != STMT_EXPR_STMT)
        return false;

    Expr *e = stmt->as.expr_stmt.expr;
    if (!e || e->kind != EXPR_ASSIGNMENT)
        return false;

    const char *aop = accum_op(e->as.assign.op);
    if (!aop)
        return false;

    *accumulator = e->as.assign.target;
    *op_name     = aop;
    *body_expr   = e->as.assign.value;
    return true;
}

/* -----------------------------------------------------------------------
 * try_reduction: attempt to match a for-loop as a single accumulation
 * ----------------------------------------------------------------------- */

static bool try_reduction(Stmt *for_stmt, ConstraintGraph *g, LiftEnv *env)
{
    Stmt *init   = for_stmt->as.for_stmt.init;
    Expr *cond   = for_stmt->as.for_stmt.cond;
    Expr *update = for_stmt->as.for_stmt.update;
    Stmt *body   = for_stmt->as.for_stmt.body;

    if (!init || !cond || !update)
        return false;

    /* Detect loop variable and start value. */
    const char *loop_var  = NULL;
    Expr       *start_ast = NULL;
    if (!detect_init(init, &loop_var, &start_ast))
        return false;

    /* Detect upper bound and inclusivity. */
    Expr *end_ast    = NULL;
    bool  inclusive  = false;
    if (!detect_cond(cond, loop_var, &end_ast, &inclusive))
        return false;

    /* Increment must be by 1. */
    if (!detect_increment(update, loop_var))
        return false;

    /* Body must be exactly one accumulation statement. */
    if (!body || body->kind != STMT_BLOCK || body->as.block.n_stmts != 1)
        return false;

    const char *accumulator = NULL;
    const char *op_name     = NULL;
    Expr       *body_ast    = NULL;
    if (!detect_accumulation(body->as.block.stmts[0],
                             &accumulator, &op_name, &body_ast))
        return false;

    /* Build IR expressions. */
    LiftEnv loop_env;
    env_copy(&loop_env, env);
    env_set(&loop_env, loop_var, "loop_var");

    IRExpr *ir_start = lift_expr(start_ast, env);
    IRExpr *ir_end   = lift_expr(end_ast, env);
    if (inclusive)
        ir_end = ir_binop("+", ir_end, ir_const(1));

    IRExpr *ir_body = lift_expr(body_ast, &loop_env);

    /* Determine initial value based on reduction op. */
    IRExpr *init_val;
    if (strcmp(op_name, "mul") == 0)
        init_val = ir_const(1);
    else
        init_val = ir_const(0);

    /* Build and add the ReductionConstraint. */
    ConstraintNode node;
    memset(&node, 0, sizeof(node));
    node.kind = CN_REDUCTION;
    strncpy(node.as.reduction.accumulator, accumulator,
            sizeof(node.as.reduction.accumulator) - 1);
    strncpy(node.as.reduction.op, op_name,
            sizeof(node.as.reduction.op) - 1);
    strncpy(node.as.reduction.loop_var, loop_var,
            sizeof(node.as.reduction.loop_var) - 1);
    node.as.reduction.start    = ir_start;
    node.as.reduction.end      = ir_end;
    node.as.reduction.body     = ir_body;
    node.as.reduction.init_val = init_val;

    cg_add(g, &node);
    return true;
}

/* -----------------------------------------------------------------------
 * Expression lifter
 * ----------------------------------------------------------------------- */

static IRExpr *lift_expr(Expr *expr, LiftEnv *env)
{
    (void)env;  /* env available for future use */

    if (!expr) return ir_const(0);

    switch (expr->kind) {
    case EXPR_INT_LIT:
        return ir_const((double)expr->as.int_val);

    case EXPR_FLOAT_LIT:
        return ir_const(expr->as.float_val);

    case EXPR_IDENTIFIER:
        return ir_var(expr->as.name);

    case EXPR_BINARY_OP:
        return ir_binop(expr->as.binop.op,
                        lift_expr(expr->as.binop.left, env),
                        lift_expr(expr->as.binop.right, env));

    case EXPR_UNARY_OP:
        return ir_unaryop(expr->as.unaryop.op,
                          lift_expr(expr->as.unaryop.operand, env));

    case EXPR_CALL: {
        int n = expr->as.call.n_args;
        IRExpr **args = NULL;
        if (n > 0) {
            args = malloc(sizeof(IRExpr *) * n);
            if (!args) return ir_const(0);
            for (int i = 0; i < n; i++)
                args[i] = lift_expr(expr->as.call.args[i], env);
        }
        IRExpr *result = ir_call(expr->as.call.name, args, n);
        free(args);
        return result;
    }

    case EXPR_ASSIGNMENT:
        fprintf(stderr, "lifting: assignment as expression not supported\n");
        return ir_const(0);
    }

    return ir_const(0);
}

/* -----------------------------------------------------------------------
 * Expression-statement lifter (assignments, ++/--)
 * ----------------------------------------------------------------------- */

static void lift_expr_stmt(Expr *expr, ConstraintGraph *g, LiftEnv *env)
{
    if (!expr) return;

    if (expr->kind == EXPR_ASSIGNMENT) {
        IRExpr *rhs = lift_expr(expr->as.assign.value, env);

        if (strcmp(expr->as.assign.op, "=") == 0) {
            /* Simple assignment: var = expr */
            ConstraintNode node;
            memset(&node, 0, sizeof(node));
            node.kind = CN_ASSIGN;
            strncpy(node.as.assign.var, expr->as.assign.target,
                    sizeof(node.as.assign.var) - 1);
            node.as.assign.expr = rhs;
            cg_add(g, &node);
        } else {
            /* Compound: x op= y → x = x binop y */
            const char *binop = compound_to_binop(expr->as.assign.op);
            if (!binop) binop = "+";
            IRExpr *combined = ir_binop(binop,
                                        ir_var(expr->as.assign.target),
                                        rhs);
            ConstraintNode node;
            memset(&node, 0, sizeof(node));
            node.kind = CN_ASSIGN;
            strncpy(node.as.assign.var, expr->as.assign.target,
                    sizeof(node.as.assign.var) - 1);
            node.as.assign.expr = combined;
            cg_add(g, &node);
        }
        return;
    }

    if (expr->kind == EXPR_UNARY_OP &&
        (strcmp(expr->as.unaryop.op, "++") == 0 ||
         strcmp(expr->as.unaryop.op, "--") == 0))
    {
        Expr *operand = expr->as.unaryop.operand;
        if (operand && operand->kind == EXPR_IDENTIFIER) {
            double delta = (strcmp(expr->as.unaryop.op, "++") == 0)
                            ? 1.0 : -1.0;
            IRExpr *combined = ir_binop("+",
                                        ir_var(operand->as.name),
                                        ir_const(delta));
            ConstraintNode node;
            memset(&node, 0, sizeof(node));
            node.kind = CN_ASSIGN;
            strncpy(node.as.assign.var, operand->as.name,
                    sizeof(node.as.assign.var) - 1);
            node.as.assign.expr = combined;
            cg_add(g, &node);
        }
    }
}

/* -----------------------------------------------------------------------
 * Statement lifter
 * ----------------------------------------------------------------------- */

static void lift_block(Stmt *block, ConstraintGraph *g, LiftEnv *env)
{
    if (!block || block->kind != STMT_BLOCK) return;
    for (int i = 0; i < block->as.block.n_stmts; i++)
        lift_stmt(block->as.block.stmts[i], g, env);
}

static void lift_stmt(Stmt *stmt, ConstraintGraph *g, LiftEnv *env)
{
    if (!stmt) return;

    switch (stmt->kind) {
    case STMT_VAR_DECL:
        env_set(env, stmt->as.var_decl.name, "local");
        if (stmt->as.var_decl.init != NULL) {
            ConstraintNode node;
            memset(&node, 0, sizeof(node));
            node.kind = CN_ASSIGN;
            strncpy(node.as.assign.var, stmt->as.var_decl.name,
                    sizeof(node.as.assign.var) - 1);
            node.as.assign.expr = lift_expr(stmt->as.var_decl.init, env);
            cg_add(g, &node);
        }
        break;

    case STMT_EXPR_STMT:
        lift_expr_stmt(stmt->as.expr_stmt.expr, g, env);
        break;

    case STMT_FOR:
        if (!try_reduction(stmt, g, env)) {
            /* Fallback: lift init, then body statements. */
            LiftEnv new_env;
            env_copy(&new_env, env);
            if (stmt->as.for_stmt.init != NULL)
                lift_stmt(stmt->as.for_stmt.init, g, &new_env);
            lift_block(stmt->as.for_stmt.body, g, &new_env);
        }
        break;

    case STMT_WHILE:
        lift_block(stmt->as.while_stmt.body, g, env);
        break;

    case STMT_RETURN: {
        IRExpr *val = stmt->as.ret.value
                        ? lift_expr(stmt->as.ret.value, env)
                        : ir_const(0);
        ConstraintNode node;
        memset(&node, 0, sizeof(node));
        node.kind = CN_RETURN;
        node.as.ret.expr = val;
        cg_add(g, &node);
        break;
    }

    case STMT_IF: {
        IRExpr *cond_ir = lift_expr(stmt->as.if_stmt.cond, env);

        /* Build then sub-graph. */
        ConstraintGraph *then_g = cg_create("_then", NULL, 0);
        LiftEnv then_env;
        env_copy(&then_env, env);
        lift_block(stmt->as.if_stmt.then_branch, then_g, &then_env);

        /* Build else sub-graph. */
        ConstraintGraph *else_g = cg_create("_else", NULL, 0);
        if (stmt->as.if_stmt.else_branch != NULL) {
            LiftEnv else_env;
            env_copy(&else_env, env);
            lift_block(stmt->as.if_stmt.else_branch, else_g, &else_env);
        }

        /* Build the CondBranchConstraint. */
        ConstraintNode node;
        memset(&node, 0, sizeof(node));
        node.kind = CN_COND_BRANCH;
        node.as.cond_branch.cond = cond_ir;

        /* Transfer ownership of constraint nodes from sub-graphs. */
        node.as.cond_branch.n_then     = then_g->n_constraints;
        node.as.cond_branch.then_nodes = then_g->constraints;
        node.as.cond_branch.n_else     = else_g->n_constraints;
        node.as.cond_branch.else_nodes = else_g->constraints;

        cg_add(g, &node);

        /*
         * Free the sub-graph shells without freeing their constraint
         * arrays — ownership was transferred to the node copy made
         * by cg_add.  We must NULL out the pointers before calling
         * cg_free so they are not double-freed.
         */
        then_g->constraints     = NULL;
        then_g->n_constraints   = 0;
        then_g->cap_constraints = 0;
        cg_free(then_g);

        else_g->constraints     = NULL;
        else_g->n_constraints   = 0;
        else_g->cap_constraints = 0;
        cg_free(else_g);
        break;
    }

    case STMT_BLOCK:
        lift_block(stmt, g, env);
        break;
    }
}

/* -----------------------------------------------------------------------
 * lift_function: top-level function lifter
 * ----------------------------------------------------------------------- */

static ConstraintGraph *lift_function(Function *fn)
{
    /* Collect parameter names. */
    char **param_names = NULL;
    if (fn->n_params > 0) {
        param_names = malloc(sizeof(char *) * fn->n_params);
        for (int i = 0; i < fn->n_params; i++)
            param_names[i] = fn->params[i].name;
    }

    ConstraintGraph *graph = cg_create(fn->name, param_names, fn->n_params);
    free(param_names);

    /* Add InvariantConstraint for each parameter. */
    for (int i = 0; i < fn->n_params; i++) {
        ConstraintNode node;
        memset(&node, 0, sizeof(node));
        node.kind = CN_INVARIANT;
        strncpy(node.as.invariant.var, fn->params[i].name,
                sizeof(node.as.invariant.var) - 1);
        strncpy(node.as.invariant.source, "param",
                sizeof(node.as.invariant.source) - 1);
        node.as.invariant.expr = NULL;
        cg_add(graph, &node);
    }

    /* Initialize environment with parameters. */
    LiftEnv env;
    env_init(&env);
    for (int i = 0; i < fn->n_params; i++)
        env_set(&env, fn->params[i].name, "param");

    /* Lift function body. */
    lift_block(fn->body, graph, &env);

    return graph;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

LiftResult telos_lift_program(Program *prog)
{
    LiftResult result;
    result.graphs = NULL;
    result.names  = NULL;
    result.count  = 0;

    if (!prog || prog->n_functions == 0)
        return result;

    result.count  = prog->n_functions;
    result.graphs = malloc(sizeof(ConstraintGraph *) * result.count);
    result.names  = malloc(sizeof(char *)            * result.count);

    for (int i = 0; i < prog->n_functions; i++) {
        result.graphs[i] = lift_function(&prog->functions[i]);
        result.names[i]  = strdup(prog->functions[i].name);
    }

    return result;
}

void telos_free_lift_result(LiftResult *r)
{
    if (!r) return;

    for (int i = 0; i < r->count; i++) {
        cg_free(r->graphs[i]);
        free(r->names[i]);
    }
    free(r->graphs);
    free(r->names);

    r->graphs = NULL;
    r->names  = NULL;
    r->count  = 0;
}
