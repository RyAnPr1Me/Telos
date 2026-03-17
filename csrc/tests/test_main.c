/*
 * test_main.c — Comprehensive test suite for the Telos C compiler.
 *
 * Covers: lexer, parser, semantic lifting, optimizer, and end-to-end
 * compilation (source → parse → lift → plan → codegen → execute).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../telos.h"

/* -----------------------------------------------------------------------
 * Test framework
 * ----------------------------------------------------------------------- */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static int test_failed_flag = 0;

#define TEST(name) static void name(void)

#define RUN_TEST(name) do { \
    printf("  %-50s", #name); \
    tests_run++; \
    test_failed_flag = 0; \
    name(); \
    if (!test_failed_flag) { \
        tests_passed++; \
        printf(" PASS\n"); \
    } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf(" FAIL\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        tests_failed++; \
        test_failed_flag = 1; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        printf(" FAIL\n    Expected %lld == %lld\n    at %s:%d\n", \
               _a, _b, __FILE__, __LINE__); \
        tests_failed++; \
        test_failed_flag = 1; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf(" FAIL\n    Expected \"%s\" == \"%s\"\n    at %s:%d\n", \
               (a), (b), __FILE__, __LINE__); \
        tests_failed++; \
        test_failed_flag = 1; \
        return; \
    } \
} while(0)

/* -----------------------------------------------------------------------
 * Compiler API helpers
 *
 * compiler.o is excluded from the test build (it contains main()),
 * so we reimplement the thin wrapper functions here.
 * ----------------------------------------------------------------------- */

static CompileResult test_compile(const char *source)
{
    CompileResult result = {0};
    Program *prog = telos_parse(source);
    if (!prog) return result;

    LiftResult lifted = telos_lift_program(prog);

    result.count = lifted.count;
    result.names = malloc(sizeof(char *) * (size_t)lifted.count);
    result.codes = malloc(sizeof(MachineCode) * (size_t)lifted.count);

    for (int i = 0; i < lifted.count; i++) {
        result.names[i] = strdup(lifted.names[i]);
        FunctionPlan *p = telos_plan_function(lifted.graphs[i]);
        result.codes[i] = telos_gen_x86_64(p);
        telos_free_function_plan(p);
    }

    telos_free_lift_result(&lifted);
    telos_free_program(prog);
    return result;
}

static RunResult test_run(const char *source)
{
    RunResult result = {0};
    Program *prog = telos_parse(source);
    if (!prog) return result;

    CompileResult compiled = test_compile(source);

    result.count = compiled.count;
    result.names = malloc(sizeof(char *) * (size_t)compiled.count);
    result.funcs = malloc(sizeof(NativeFunction) * (size_t)compiled.count);

    for (int i = 0; i < compiled.count; i++) {
        result.names[i] = strdup(compiled.names[i]);

        int n_params = 0;
        for (int j = 0; j < prog->n_functions; j++) {
            if (strcmp(prog->functions[j].name, compiled.names[i]) == 0) {
                n_params = prog->functions[j].n_params;
                break;
            }
        }

        result.funcs[i] = telos_make_native(
            compiled.codes[i].code, compiled.codes[i].size, n_params);
    }

    /* Free the intermediate CompileResult */
    for (int i = 0; i < compiled.count; i++) {
        free(compiled.names[i]);
        free(compiled.codes[i].code);
    }
    free(compiled.names);
    free(compiled.codes);

    telos_free_program(prog);
    return result;
}

static CompileCResult test_compile_c(const char *source)
{
    CompileCResult result = {0};
    Program *prog = telos_parse(source);
    if (!prog) return result;

    LiftResult lifted = telos_lift_program(prog);

    result.count = lifted.count;
    result.names = malloc(sizeof(char *) * (size_t)lifted.count);
    result.sources = malloc(sizeof(char *) * (size_t)lifted.count);

    for (int i = 0; i < lifted.count; i++) {
        result.names[i] = strdup(lifted.names[i]);
        FunctionPlan *p = telos_plan_function(lifted.graphs[i]);
        result.sources[i] = telos_gen_c(p);
        telos_free_function_plan(p);
    }

    telos_free_lift_result(&lifted);
    telos_free_program(prog);
    return result;
}

static void free_run_result(RunResult *r)
{
    for (int i = 0; i < r->count; i++) {
        free(r->names[i]);
        telos_free_native(&r->funcs[i]);
    }
    free(r->names);
    free(r->funcs);
}

static void free_compile_result(CompileResult *r)
{
    for (int i = 0; i < r->count; i++) {
        free(r->names[i]);
        free(r->codes[i].code);
    }
    free(r->names);
    free(r->codes);
}

static void free_compile_c_result(CompileCResult *r)
{
    for (int i = 0; i < r->count; i++) {
        free(r->names[i]);
        free(r->sources[i]);
    }
    free(r->names);
    free(r->sources);
}

/* -----------------------------------------------------------------------
 * Lookup / calling helpers
 * ----------------------------------------------------------------------- */

static NativeFunction *find_func(RunResult *r, const char *name)
{
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->names[i], name) == 0) return &r->funcs[i];
    return NULL;
}

static char *find_c_source(CompileCResult *r, const char *name)
{
    for (int i = 0; i < r->count; i++)
        if (strcmp(r->names[i], name) == 0) return r->sources[i];
    return NULL;
}

static int64_t call0(NativeFunction *fn)
{
    return telos_call_native(fn, NULL, 0);
}

static int64_t call1(NativeFunction *fn, int64_t a)
{
    int64_t args[] = {a};
    return telos_call_native(fn, args, 1);
}

static int64_t call2(NativeFunction *fn, int64_t a, int64_t b)
{
    int64_t args[] = {a, b};
    return telos_call_native(fn, args, 2);
}

/* Return the first non-AssignPlan step from a FunctionPlan */
static ExecutionPlan *first_nontrivial_step(FunctionPlan *fp)
{
    for (int i = 0; i < fp->n_steps; i++)
        if (fp->steps[i]->kind != PLAN_ASSIGN)
            return fp->steps[i];
    return fp->n_steps > 0 ? fp->steps[0] : NULL;
}

/* -----------------------------------------------------------------------
 * Source strings
 * ----------------------------------------------------------------------- */

static const char *SRC_SUM =
    "int sum(int n) {"
    "    int s = 0;"
    "    for (int i = 0; i < n; i++) {"
    "        s += i;"
    "    }"
    "    return s;"
    "}";

static const char *SRC_SUM_SQ =
    "int sum_sq(int n) {"
    "    int s = 0;"
    "    for (int i = 0; i < n; i++) {"
    "        s += i * i;"
    "    }"
    "    return s;"
    "}";

static const char *SRC_FIXED_SUM =
    "int fixed_sum() {"
    "    int s = 0;"
    "    for (int i = 0; i < 10; i++) {"
    "        s += i;"
    "    }"
    "    return s;"
    "}";

static const char *SRC_LINEAR_COMBO =
    "int linear_combo(int n) {"
    "    int s = 0;"
    "    for (int i = 0; i < n; i++) {"
    "        s += 2 * i + 3;"
    "    }"
    "    return s;"
    "}";

static const char *SRC_CUBE_SUM =
    "int cube_sum(int n) {"
    "    int s = 0;"
    "    for (int i = 0; i < n; i++) {"
    "        s += i * i * i;"
    "    }"
    "    return s;"
    "}";

static const char *SRC_INCLUSIVE =
    "int sum_inc(int n) {"
    "    int s = 0;"
    "    for (int i = 0; i <= n; i++) {"
    "        s += i;"
    "    }"
    "    return s;"
    "}";

/* ======================================================================= *
 * 1. Lexer Tests                                                          *
 * ======================================================================= */

TEST(test_integer_literal)
{
    TokenArray tokens;
    da_init(&tokens);
    telos_tokenize("42", &tokens);
    ASSERT(tokens.count >= 2);
    ASSERT_EQ(tokens.items[0].kind, TOK_INT_LIT);
    ASSERT_STR_EQ(tokens.items[0].value, "42");
    da_free(&tokens);
}

TEST(test_float_literal)
{
    TokenArray tokens;
    da_init(&tokens);
    telos_tokenize("3.14", &tokens);
    ASSERT(tokens.count >= 2);
    ASSERT_EQ(tokens.items[0].kind, TOK_FLOAT_LIT);
    ASSERT_STR_EQ(tokens.items[0].value, "3.14");
    da_free(&tokens);
}

TEST(test_identifier)
{
    TokenArray tokens;
    da_init(&tokens);
    telos_tokenize("myVar", &tokens);
    ASSERT(tokens.count >= 2);
    ASSERT_EQ(tokens.items[0].kind, TOK_ID);
    ASSERT_STR_EQ(tokens.items[0].value, "myVar");
    da_free(&tokens);
}

TEST(test_keyword)
{
    TokenArray tokens;
    da_init(&tokens);
    telos_tokenize("int", &tokens);
    ASSERT(tokens.count >= 2);
    ASSERT_EQ(tokens.items[0].kind, TOK_KEYWORD);
    ASSERT_STR_EQ(tokens.items[0].value, "int");
    da_free(&tokens);
}

TEST(test_single_char_ops)
{
    const char *ops[] = {"+", "-", "*", "/", "%", "<", ">"};
    for (int k = 0; k < 7; k++) {
        TokenArray tokens;
        da_init(&tokens);
        telos_tokenize(ops[k], &tokens);
        ASSERT(tokens.count >= 2);
        ASSERT_EQ(tokens.items[0].kind, TOK_OP);
        ASSERT_STR_EQ(tokens.items[0].value, ops[k]);
        da_free(&tokens);
    }
}

TEST(test_two_char_ops)
{
    const char *ops[] = {
        "++", "--", "+=", "-=", "<=", ">=", "==", "!=", "&&", "||"
    };
    for (int k = 0; k < 10; k++) {
        TokenArray tokens;
        da_init(&tokens);
        telos_tokenize(ops[k], &tokens);
        ASSERT(tokens.count >= 2);
        ASSERT_EQ(tokens.items[0].kind, TOK_OP);
        ASSERT_STR_EQ(tokens.items[0].value, ops[k]);
        da_free(&tokens);
    }
}

TEST(test_punctuation)
{
    TokenArray tokens;
    da_init(&tokens);
    telos_tokenize("(){};", &tokens);
    ASSERT(tokens.count >= 6);
    ASSERT_EQ(tokens.items[0].kind, TOK_PUNCT);
    ASSERT_STR_EQ(tokens.items[0].value, "(");
    ASSERT_EQ(tokens.items[1].kind, TOK_PUNCT);
    ASSERT_STR_EQ(tokens.items[1].value, ")");
    ASSERT_EQ(tokens.items[2].kind, TOK_PUNCT);
    ASSERT_STR_EQ(tokens.items[2].value, "{");
    ASSERT_EQ(tokens.items[3].kind, TOK_PUNCT);
    ASSERT_STR_EQ(tokens.items[3].value, "}");
    ASSERT_EQ(tokens.items[4].kind, TOK_PUNCT);
    ASSERT_STR_EQ(tokens.items[4].value, ";");
    da_free(&tokens);
}

TEST(test_comments)
{
    TokenArray tokens;
    da_init(&tokens);
    telos_tokenize("42 // comment\n99", &tokens);
    /* Expect: INT_LIT "42", INT_LIT "99", EOF */
    int non_eof = 0;
    for (int i = 0; i < tokens.count; i++)
        if (tokens.items[i].kind != TOK_EOF) non_eof++;
    ASSERT_EQ(non_eof, 2);
    ASSERT_EQ(tokens.items[0].kind, TOK_INT_LIT);
    ASSERT_STR_EQ(tokens.items[0].value, "42");
    ASSERT_EQ(tokens.items[1].kind, TOK_INT_LIT);
    ASSERT_STR_EQ(tokens.items[1].value, "99");
    da_free(&tokens);
}

TEST(test_eof)
{
    TokenArray tokens;
    da_init(&tokens);
    telos_tokenize("x", &tokens);
    ASSERT(tokens.count >= 1);
    ASSERT_EQ(tokens.items[tokens.count - 1].kind, TOK_EOF);
    da_free(&tokens);
}

/* ======================================================================= *
 * 2. Parser Tests                                                         *
 * ======================================================================= */

TEST(test_no_params_fn)
{
    Program *prog = telos_parse("int foo() { return 1; }");
    ASSERT(prog != NULL);
    ASSERT_EQ(prog->n_functions, 1);
    ASSERT_STR_EQ(prog->functions[0].name, "foo");
    ASSERT_STR_EQ(prog->functions[0].return_type, "int");
    ASSERT_EQ(prog->functions[0].n_params, 0);
    telos_free_program(prog);
}

TEST(test_single_param)
{
    Program *prog = telos_parse("int id(int x) { return x; }");
    ASSERT(prog != NULL);
    ASSERT_EQ(prog->functions[0].n_params, 1);
    ASSERT_STR_EQ(prog->functions[0].params[0].name, "x");
    ASSERT_STR_EQ(prog->functions[0].params[0].type_name, "int");
    telos_free_program(prog);
}

TEST(test_multiple_params)
{
    Program *prog = telos_parse("int add(int a, int b) { return a + b; }");
    ASSERT(prog != NULL);
    ASSERT_EQ(prog->functions[0].n_params, 2);
    ASSERT_STR_EQ(prog->functions[0].params[0].name, "a");
    ASSERT_STR_EQ(prog->functions[0].params[1].name, "b");
    telos_free_program(prog);
}

TEST(test_var_decl)
{
    Program *prog = telos_parse("int f() { int x = 0; return x; }");
    ASSERT(prog != NULL);
    Stmt *body = prog->functions[0].body;
    ASSERT(body != NULL);
    ASSERT_EQ(body->kind, STMT_BLOCK);
    ASSERT(body->as.block.n_stmts >= 2);
    Stmt *decl = body->as.block.stmts[0];
    ASSERT_EQ(decl->kind, STMT_VAR_DECL);
    ASSERT_STR_EQ(decl->as.var_decl.name, "x");
    ASSERT(decl->as.var_decl.init != NULL);
    ASSERT_EQ(decl->as.var_decl.init->kind, EXPR_INT_LIT);
    ASSERT_EQ(decl->as.var_decl.init->as.int_val, 0);
    telos_free_program(prog);
}

TEST(test_for_loop)
{
    Program *prog = telos_parse(
        "int f(int n) {"
        "    for (int i = 0; i < n; i++) { }"
        "    return 0;"
        "}");
    ASSERT(prog != NULL);
    Stmt *body = prog->functions[0].body;
    ASSERT_EQ(body->kind, STMT_BLOCK);
    ASSERT(body->as.block.n_stmts >= 1);
    Stmt *for_stmt = body->as.block.stmts[0];
    ASSERT_EQ(for_stmt->kind, STMT_FOR);
    ASSERT(for_stmt->as.for_stmt.init != NULL);
    ASSERT_EQ(for_stmt->as.for_stmt.init->kind, STMT_VAR_DECL);
    ASSERT_STR_EQ(for_stmt->as.for_stmt.init->as.var_decl.name, "i");
    ASSERT(for_stmt->as.for_stmt.cond != NULL);
    ASSERT_EQ(for_stmt->as.for_stmt.cond->kind, EXPR_BINARY_OP);
    ASSERT_STR_EQ(for_stmt->as.for_stmt.cond->as.binop.op, "<");
    telos_free_program(prog);
}

TEST(test_binary_precedence)
{
    /* 2 + 3 * 4 should parse as BinaryOp(+, 2, BinaryOp(*, 3, 4)) */
    Program *prog = telos_parse("int f() { return 2 + 3 * 4; }");
    ASSERT(prog != NULL);
    Stmt *body = prog->functions[0].body;
    Stmt *ret  = body->as.block.stmts[0];
    ASSERT_EQ(ret->kind, STMT_RETURN);
    Expr *e = ret->as.ret.value;
    ASSERT_EQ(e->kind, EXPR_BINARY_OP);
    ASSERT_STR_EQ(e->as.binop.op, "+");
    ASSERT_EQ(e->as.binop.right->kind, EXPR_BINARY_OP);
    ASSERT_STR_EQ(e->as.binop.right->as.binop.op, "*");
    telos_free_program(prog);
}

TEST(test_two_functions)
{
    Program *prog = telos_parse(
        "int a() { return 1; } int b() { return 2; }");
    ASSERT(prog != NULL);
    ASSERT_EQ(prog->n_functions, 2);
    ASSERT_STR_EQ(prog->functions[0].name, "a");
    ASSERT_STR_EQ(prog->functions[1].name, "b");
    telos_free_program(prog);
}

/* ======================================================================= *
 * 3. Lifting Tests                                                        *
 * ======================================================================= */

TEST(test_param_invariant)
{
    Program *prog = telos_parse("int f(int n) { return n; }");
    ASSERT(prog != NULL);
    LiftResult lifted = telos_lift_program(prog);
    ASSERT_EQ(lifted.count, 1);

    ConstraintGraph *g = lifted.graphs[0];
    int n_invariants = 0;
    for (int i = 0; i < g->n_constraints; i++) {
        if (g->constraints[i]->kind == CN_INVARIANT &&
            strcmp(g->constraints[i]->as.invariant.source, "param") == 0) {
            ASSERT_STR_EQ(g->constraints[i]->as.invariant.var, "n");
            n_invariants++;
        }
    }
    ASSERT_EQ(n_invariants, 1);

    telos_free_lift_result(&lifted);
    telos_free_program(prog);
}

TEST(test_reduction_detection)
{
    Program *prog = telos_parse(SRC_SUM);
    ASSERT(prog != NULL);
    LiftResult lifted = telos_lift_program(prog);
    ASSERT_EQ(lifted.count, 1);

    ConstraintGraph *g = lifted.graphs[0];
    int n_reductions = 0;
    for (int i = 0; i < g->n_constraints; i++)
        if (g->constraints[i]->kind == CN_REDUCTION)
            n_reductions++;
    ASSERT_EQ(n_reductions, 1);

    telos_free_lift_result(&lifted);
    telos_free_program(prog);
}

TEST(test_reduction_attributes)
{
    Program *prog = telos_parse(SRC_SUM);
    ASSERT(prog != NULL);
    LiftResult lifted = telos_lift_program(prog);

    ConstraintGraph *g = lifted.graphs[0];
    ConstraintNode *red = NULL;
    for (int i = 0; i < g->n_constraints; i++) {
        if (g->constraints[i]->kind == CN_REDUCTION) {
            red = g->constraints[i];
            break;
        }
    }
    ASSERT(red != NULL);
    ASSERT_STR_EQ(red->as.reduction.accumulator, "s");
    ASSERT_STR_EQ(red->as.reduction.op, "add");
    ASSERT_STR_EQ(red->as.reduction.loop_var, "i");

    telos_free_lift_result(&lifted);
    telos_free_program(prog);
}

TEST(test_return_constraint)
{
    Program *prog = telos_parse("int f() { return 42; }");
    ASSERT(prog != NULL);
    LiftResult lifted = telos_lift_program(prog);
    ASSERT_EQ(lifted.count, 1);

    ConstraintGraph *g = lifted.graphs[0];
    int n_returns = 0;
    for (int i = 0; i < g->n_constraints; i++) {
        if (g->constraints[i]->kind == CN_RETURN) {
            ASSERT(g->constraints[i]->as.ret.expr != NULL);
            ASSERT_EQ(g->constraints[i]->as.ret.expr->kind, IR_CONST);
            ASSERT_EQ((int)g->constraints[i]->as.ret.expr->as.const_val, 42);
            n_returns++;
        }
    }
    ASSERT_EQ(n_returns, 1);

    telos_free_lift_result(&lifted);
    telos_free_program(prog);
}

/* ======================================================================= *
 * 4. Optimizer Tests                                                      *
 * ======================================================================= */

TEST(test_constant_fold)
{
    /* for(i=0; i<10; i++) s+=i  with no runtime params → ConstantPlan(45) */
    Program *prog = telos_parse(SRC_FIXED_SUM);
    ASSERT(prog != NULL);
    LiftResult lifted = telos_lift_program(prog);
    FunctionPlan *fp = telos_plan_function(lifted.graphs[0]);

    ExecutionPlan *step = first_nontrivial_step(fp);
    ASSERT(step != NULL);
    ASSERT_EQ(step->kind, PLAN_CONSTANT);
    ASSERT_EQ(step->as.constant.value, 45);

    telos_free_function_plan(fp);
    telos_free_lift_result(&lifted);
    telos_free_program(prog);
}

TEST(test_closed_form)
{
    /* for(i=0; i<n; i++) s+=i  with runtime n → ClosedFormPlan */
    Program *prog = telos_parse(SRC_SUM);
    ASSERT(prog != NULL);
    LiftResult lifted = telos_lift_program(prog);
    FunctionPlan *fp = telos_plan_function(lifted.graphs[0]);

    ExecutionPlan *step = first_nontrivial_step(fp);
    ASSERT(step != NULL);
    ASSERT_EQ(step->kind, PLAN_CLOSED_FORM);

    telos_free_function_plan(fp);
    telos_free_lift_result(&lifted);
    telos_free_program(prog);
}

TEST(test_closed_form_correct)
{
    /* Evaluate the closed-form formula for n=0..19, verify == n*(n-1)/2 */
    Program *prog = telos_parse(SRC_SUM);
    ASSERT(prog != NULL);
    LiftResult lifted = telos_lift_program(prog);
    FunctionPlan *fp = telos_plan_function(lifted.graphs[0]);

    ExecutionPlan *step = first_nontrivial_step(fp);
    ASSERT(step != NULL);
    ASSERT_EQ(step->kind, PLAN_CLOSED_FORM);

    IRExpr *formula = step->as.closed_form.formula;
    for (int n = 0; n < 20; n++) {
        EvalEnv env = {0};
        env.names[0]  = "n";
        env.values[0] = (double)n;
        env.count     = 1;
        double result;
        bool ok = ir_eval_const(formula, &env, &result);
        ASSERT(ok);
        int64_t expected = (int64_t)n * ((int64_t)n - 1) / 2;
        ASSERT_EQ((long long)result, (long long)expected);
    }

    telos_free_function_plan(fp);
    telos_free_lift_result(&lifted);
    telos_free_program(prog);
}

TEST(test_cost_constant_preferred)
{
    /* When all inputs are compile-time constants, ConstantPlan wins */
    const char *src =
        "int f() {"
        "    int s = 0;"
        "    for (int i = 0; i < 5; i++) { s += i; }"
        "    return s;"
        "}";
    Program *prog = telos_parse(src);
    ASSERT(prog != NULL);
    LiftResult lifted = telos_lift_program(prog);
    FunctionPlan *fp = telos_plan_function(lifted.graphs[0]);

    ExecutionPlan *step = first_nontrivial_step(fp);
    ASSERT(step != NULL);
    ASSERT_EQ(step->kind, PLAN_CONSTANT);

    telos_free_function_plan(fp);
    telos_free_lift_result(&lifted);
    telos_free_program(prog);
}

TEST(test_cost_closed_preferred)
{
    /* For runtime n, ClosedFormPlan beats LoopPlan */
    Program *prog = telos_parse(SRC_SUM);
    ASSERT(prog != NULL);
    LiftResult lifted = telos_lift_program(prog);
    FunctionPlan *fp = telos_plan_function(lifted.graphs[0]);

    ExecutionPlan *step = first_nontrivial_step(fp);
    ASSERT(step != NULL);
    ASSERT_EQ(step->kind, PLAN_CLOSED_FORM);

    telos_free_function_plan(fp);
    telos_free_lift_result(&lifted);
    telos_free_program(prog);
}

/* ======================================================================= *
 * 5. End-to-End Compiler Tests                                            *
 * ======================================================================= */

/* --- Sum of integers --------------------------------------------------- */

TEST(test_sum_zero)
{
    RunResult rr = test_run(SRC_SUM);
    NativeFunction *fn = find_func(&rr, "sum");
    ASSERT(fn != NULL);
    ASSERT_EQ(call1(fn, 0), 0);
    free_run_result(&rr);
}

TEST(test_sum_one)
{
    RunResult rr = test_run(SRC_SUM);
    NativeFunction *fn = find_func(&rr, "sum");
    ASSERT(fn != NULL);
    ASSERT_EQ(call1(fn, 1), 0);
    free_run_result(&rr);
}

TEST(test_sum_five)
{
    RunResult rr = test_run(SRC_SUM);
    NativeFunction *fn = find_func(&rr, "sum");
    ASSERT(fn != NULL);
    ASSERT_EQ(call1(fn, 5), 10);   /* 0+1+2+3+4 */
    free_run_result(&rr);
}

TEST(test_sum_ten)
{
    RunResult rr = test_run(SRC_SUM);
    NativeFunction *fn = find_func(&rr, "sum");
    ASSERT(fn != NULL);
    ASSERT_EQ(call1(fn, 10), 45);
    free_run_result(&rr);
}

TEST(test_sum_large)
{
    RunResult rr = test_run(SRC_SUM);
    NativeFunction *fn = find_func(&rr, "sum");
    ASSERT(fn != NULL);
    int64_t n = 1000;
    ASSERT_EQ(call1(fn, n), n * (n - 1) / 2);   /* 499500 */
    free_run_result(&rr);
}

/* --- Sum of squares ---------------------------------------------------- */

TEST(test_sum_sq_zero)
{
    RunResult rr = test_run(SRC_SUM_SQ);
    NativeFunction *fn = find_func(&rr, "sum_sq");
    ASSERT(fn != NULL);
    ASSERT_EQ(call1(fn, 0), 0);
    free_run_result(&rr);
}

TEST(test_sum_sq_five)
{
    RunResult rr = test_run(SRC_SUM_SQ);
    NativeFunction *fn = find_func(&rr, "sum_sq");
    ASSERT(fn != NULL);
    ASSERT_EQ(call1(fn, 5), 30);   /* 0+1+4+9+16 */
    free_run_result(&rr);
}

TEST(test_sum_sq_large)
{
    RunResult rr = test_run(SRC_SUM_SQ);
    NativeFunction *fn = find_func(&rr, "sum_sq");
    ASSERT(fn != NULL);
    /* sum(i*i for i in range(100)) = 99*100*199/6 = 328350 */
    ASSERT_EQ(call1(fn, 100), 328350);
    free_run_result(&rr);
}

/* --- Constant folding -------------------------------------------------- */

TEST(test_constant_folding)
{
    RunResult rr = test_run(SRC_FIXED_SUM);
    NativeFunction *fn = find_func(&rr, "fixed_sum");
    ASSERT(fn != NULL);
    ASSERT_EQ(call0(fn), 45);
    free_run_result(&rr);
}

/* --- Linear combination: Σ(2i+3) -------------------------------------- */

TEST(test_linear_combo)
{
    RunResult rr = test_run(SRC_LINEAR_COMBO);
    NativeFunction *fn = find_func(&rr, "linear_combo");
    ASSERT(fn != NULL);
    for (int n = 0; n < 20; n++) {
        int64_t expected = 0;
        for (int i = 0; i < n; i++) expected += 2 * i + 3;
        ASSERT_EQ(call1(fn, (int64_t)n), expected);
    }
    free_run_result(&rr);
}

/* --- Cube sum: Σ(i³) -------------------------------------------------- */

TEST(test_cube_sum)
{
    RunResult rr = test_run(SRC_CUBE_SUM);
    NativeFunction *fn = find_func(&rr, "cube_sum");
    ASSERT(fn != NULL);
    for (int n = 0; n < 15; n++) {
        int64_t expected = 0;
        for (int i = 0; i < n; i++)
            expected += (int64_t)i * i * i;
        ASSERT_EQ(call1(fn, (int64_t)n), expected);
    }
    free_run_result(&rr);
}

/* --- Inclusive range (i <= n) ------------------------------------------ */

TEST(test_inclusive_range)
{
    RunResult rr = test_run(SRC_INCLUSIVE);
    NativeFunction *fn = find_func(&rr, "sum_inc");
    ASSERT(fn != NULL);
    for (int n = 0; n < 15; n++) {
        int64_t expected = (int64_t)n * (n + 1) / 2;
        ASSERT_EQ(call1(fn, (int64_t)n), expected);
    }
    free_run_result(&rr);
}

/* --- Simple functions (no loops) --------------------------------------- */

TEST(test_identity)
{
    RunResult rr = test_run("int id(int x) { return x; }");
    NativeFunction *fn = find_func(&rr, "id");
    ASSERT(fn != NULL);
    ASSERT_EQ(call1(fn, 7), 7);
    free_run_result(&rr);
}

TEST(test_add)
{
    RunResult rr = test_run("int add(int a, int b) { return a + b; }");
    NativeFunction *fn = find_func(&rr, "add");
    ASSERT(fn != NULL);
    ASSERT_EQ(call2(fn, 3, 4), 7);
    free_run_result(&rr);
}

TEST(test_constant_42)
{
    RunResult rr = test_run("int answer() { return 42; }");
    NativeFunction *fn = find_func(&rr, "answer");
    ASSERT(fn != NULL);
    ASSERT_EQ(call0(fn), 42);
    free_run_result(&rr);
}

TEST(test_subtract)
{
    RunResult rr = test_run("int sub(int a, int b) { return a - b; }");
    NativeFunction *fn = find_func(&rr, "sub");
    ASSERT(fn != NULL);
    ASSERT_EQ(call2(fn, 10, 3), 7);
    free_run_result(&rr);
}

TEST(test_multiply)
{
    RunResult rr = test_run("int mul(int a, int b) { return a * b; }");
    NativeFunction *fn = find_func(&rr, "mul");
    ASSERT(fn != NULL);
    ASSERT_EQ(call2(fn, 6, 7), 42);
    free_run_result(&rr);
}

/* --- C codegen output -------------------------------------------------- */

TEST(test_c_gen_output)
{
    CompileCResult cr = test_compile_c(SRC_SUM);
    ASSERT(cr.count >= 1);
    char *csrc = find_c_source(&cr, "sum");
    ASSERT(csrc != NULL);
    ASSERT(strstr(csrc, "long long") != NULL);
    free_compile_c_result(&cr);
}

TEST(test_c_gen_not_python)
{
    CompileCResult cr = test_compile_c(SRC_SUM);
    ASSERT(cr.count >= 1);
    char *csrc = find_c_source(&cr, "sum");
    ASSERT(csrc != NULL);
    ASSERT(strstr(csrc, "def ") == NULL);
    free_compile_c_result(&cr);
}

/* --- Machine code output ----------------------------------------------- */

TEST(test_machine_code_bytes)
{
    CompileResult cr = test_compile(SRC_SUM);
    ASSERT(cr.count >= 1);
    ASSERT(cr.codes[0].size > 0);
    ASSERT(cr.codes[0].code != NULL);
    free_compile_result(&cr);
}

/* ======================================================================= *
 * Main — run all tests and print summary                                  *
 * ======================================================================= */

int main(void)
{
    printf("=== Telos C Test Suite ===\n\n");

    printf("Lexer Tests:\n");
    RUN_TEST(test_integer_literal);
    RUN_TEST(test_float_literal);
    RUN_TEST(test_identifier);
    RUN_TEST(test_keyword);
    RUN_TEST(test_single_char_ops);
    RUN_TEST(test_two_char_ops);
    RUN_TEST(test_punctuation);
    RUN_TEST(test_comments);
    RUN_TEST(test_eof);

    printf("\nParser Tests:\n");
    RUN_TEST(test_no_params_fn);
    RUN_TEST(test_single_param);
    RUN_TEST(test_multiple_params);
    RUN_TEST(test_var_decl);
    RUN_TEST(test_for_loop);
    RUN_TEST(test_binary_precedence);
    RUN_TEST(test_two_functions);

    printf("\nLifting Tests:\n");
    RUN_TEST(test_param_invariant);
    RUN_TEST(test_reduction_detection);
    RUN_TEST(test_reduction_attributes);
    RUN_TEST(test_return_constraint);

    printf("\nOptimizer Tests:\n");
    RUN_TEST(test_constant_fold);
    RUN_TEST(test_closed_form);
    RUN_TEST(test_closed_form_correct);
    RUN_TEST(test_cost_constant_preferred);
    RUN_TEST(test_cost_closed_preferred);

    printf("\nEnd-to-End Tests:\n");
    RUN_TEST(test_sum_zero);
    RUN_TEST(test_sum_one);
    RUN_TEST(test_sum_five);
    RUN_TEST(test_sum_ten);
    RUN_TEST(test_sum_large);
    RUN_TEST(test_sum_sq_zero);
    RUN_TEST(test_sum_sq_five);
    RUN_TEST(test_sum_sq_large);
    RUN_TEST(test_constant_folding);
    RUN_TEST(test_linear_combo);
    RUN_TEST(test_cube_sum);
    RUN_TEST(test_inclusive_range);
    RUN_TEST(test_identity);
    RUN_TEST(test_add);
    RUN_TEST(test_constant_42);
    RUN_TEST(test_subtract);
    RUN_TEST(test_multiply);
    RUN_TEST(test_c_gen_output);
    RUN_TEST(test_c_gen_not_python);
    RUN_TEST(test_machine_code_bytes);

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
