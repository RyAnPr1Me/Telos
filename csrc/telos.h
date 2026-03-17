/*
 * telos.h — Main header for the Telos compiler (C implementation).
 *
 * Contains all type definitions: tokens, AST nodes, IR nodes,
 * constraint graphs, execution plans, and public API declarations.
 */

#ifndef TELOS_H
#define TELOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */

typedef struct Token        Token;
typedef struct Expr         Expr;
typedef struct Stmt         Stmt;
typedef struct Param        Param;
typedef struct Function     Function;
typedef struct Program      Program;
typedef struct IRExpr       IRExpr;
typedef struct ConstraintNode ConstraintNode;
typedef struct ConstraintGraph ConstraintGraph;
typedef struct ExecutionPlan ExecutionPlan;
typedef struct FunctionPlan FunctionPlan;

/* -----------------------------------------------------------------------
 * Dynamic array macros
 * ----------------------------------------------------------------------- */

#define DA_INIT_CAP 8

#define DA_TYPEDEF(T, Name) \
    typedef struct { T *items; int count; int cap; } Name;

#define da_init(da) do { \
    (da)->items = NULL; (da)->count = 0; (da)->cap = 0; \
} while(0)

#define da_push(da, item) do { \
    if ((da)->count >= (da)->cap) { \
        (da)->cap = (da)->cap ? (da)->cap * 2 : DA_INIT_CAP; \
        (da)->items = realloc((da)->items, (da)->cap * sizeof(*(da)->items)); \
    } \
    (da)->items[(da)->count++] = (item); \
} while(0)

#define da_free(da) do { \
    free((da)->items); (da)->items = NULL; \
    (da)->count = 0; (da)->cap = 0; \
} while(0)

/* -----------------------------------------------------------------------
 * Token types
 * ----------------------------------------------------------------------- */

typedef enum {
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_ID,
    TOK_KEYWORD,
    TOK_OP,
    TOK_PUNCT,
    TOK_EOF,
} TokenKind;

struct Token {
    TokenKind kind;
    char      value[64];
    int       line;
    int       col;
};

DA_TYPEDEF(Token, TokenArray)

/* -----------------------------------------------------------------------
 * AST node types
 * ----------------------------------------------------------------------- */

typedef enum {
    EXPR_INT_LIT,
    EXPR_FLOAT_LIT,
    EXPR_IDENTIFIER,
    EXPR_BINARY_OP,
    EXPR_UNARY_OP,
    EXPR_CALL,
    EXPR_ASSIGNMENT,
} ExprKind;

struct Expr {
    ExprKind kind;
    union {
        int64_t int_val;          /* EXPR_INT_LIT */
        double  float_val;        /* EXPR_FLOAT_LIT */
        char    name[64];         /* EXPR_IDENTIFIER */
        struct {                  /* EXPR_BINARY_OP */
            char  op[4];
            Expr *left;
            Expr *right;
        } binop;
        struct {                  /* EXPR_UNARY_OP */
            char  op[4];
            Expr *operand;
            bool  postfix;
        } unaryop;
        struct {                  /* EXPR_CALL */
            char  name[64];
            Expr **args;
            int   n_args;
        } call;
        struct {                  /* EXPR_ASSIGNMENT */
            char  target[64];
            char  op[4];
            Expr *value;
        } assign;
    } as;
};

typedef enum {
    STMT_VAR_DECL,
    STMT_EXPR_STMT,
    STMT_RETURN,
    STMT_BLOCK,
    STMT_IF,
    STMT_FOR,
    STMT_WHILE,
} StmtKind;

struct Stmt {
    StmtKind kind;
    union {
        struct {                  /* STMT_VAR_DECL */
            char  type_name[16];
            char  name[64];
            Expr *init;           /* may be NULL */
        } var_decl;
        struct {                  /* STMT_EXPR_STMT */
            Expr *expr;
        } expr_stmt;
        struct {                  /* STMT_RETURN */
            Expr *value;          /* may be NULL */
        } ret;
        struct {                  /* STMT_BLOCK */
            Stmt **stmts;
            int    n_stmts;
        } block;
        struct {                  /* STMT_IF */
            Expr *cond;
            Stmt *then_branch;    /* must be STMT_BLOCK */
            Stmt *else_branch;    /* may be NULL, STMT_BLOCK */
        } if_stmt;
        struct {                  /* STMT_FOR */
            Stmt *init;           /* may be NULL */
            Expr *cond;           /* may be NULL */
            Expr *update;         /* may be NULL */
            Stmt *body;           /* must be STMT_BLOCK */
        } for_stmt;
        struct {                  /* STMT_WHILE */
            Expr *cond;
            Stmt *body;           /* must be STMT_BLOCK */
        } while_stmt;
    } as;
};

struct Param {
    char type_name[16];
    char name[64];
};

struct Function {
    char     return_type[16];
    char     name[64];
    Param   *params;
    int      n_params;
    Stmt    *body;               /* STMT_BLOCK */
};

struct Program {
    Function *functions;
    int       n_functions;
};

/* -----------------------------------------------------------------------
 * IR expression types
 * ----------------------------------------------------------------------- */

typedef enum {
    IR_CONST,
    IR_VAR,
    IR_BINOP,
    IR_UNARYOP,
    IR_CALL,
} IRExprKind;

struct IRExpr {
    IRExprKind kind;
    union {
        double  const_val;        /* IR_CONST */
        char    var_name[64];     /* IR_VAR */
        struct {                  /* IR_BINOP */
            char    op[4];
            IRExpr *left;
            IRExpr *right;
        } binop;
        struct {                  /* IR_UNARYOP */
            char    op[4];
            IRExpr *operand;
        } unaryop;
        struct {                  /* IR_CALL */
            char     name[64];
            IRExpr **args;
            int      n_args;
        } call;
    } as;
};

/* -----------------------------------------------------------------------
 * Constraint node types
 * ----------------------------------------------------------------------- */

typedef enum {
    CN_INVARIANT,
    CN_ASSIGN,
    CN_REDUCTION,
    CN_RETURN,
    CN_COND_BRANCH,
} ConstraintKind;

struct ConstraintNode {
    ConstraintKind kind;
    int            node_id;
    union {
        struct {                  /* CN_INVARIANT */
            char    var[64];
            char    source[16];   /* "param" | "computed" */
            IRExpr *expr;         /* may be NULL */
        } invariant;
        struct {                  /* CN_ASSIGN */
            char    var[64];
            IRExpr *expr;
        } assign;
        struct {                  /* CN_REDUCTION */
            char    accumulator[64];
            char    op[8];        /* "add" | "sub" | "mul" */
            char    loop_var[64];
            IRExpr *start;
            IRExpr *end;
            IRExpr *body;
            IRExpr *init_val;
        } reduction;
        struct {                  /* CN_RETURN */
            IRExpr *expr;
        } ret;
        struct {                  /* CN_COND_BRANCH */
            IRExpr          *cond;
            ConstraintNode **then_nodes;
            int              n_then;
            ConstraintNode **else_nodes;
            int              n_else;
        } cond_branch;
    } as;
};

struct ConstraintGraph {
    char             name[64];
    char           **param_names;
    int              n_params;
    ConstraintNode **constraints;
    int              n_constraints;
    int              cap_constraints;
    int              next_id;
};

/* -----------------------------------------------------------------------
 * Execution plan types
 * ----------------------------------------------------------------------- */

typedef enum {
    PLAN_LOOP,
    PLAN_CLOSED_FORM,
    PLAN_CONSTANT,
    PLAN_ASSIGN,
} PlanKind;

struct ExecutionPlan {
    PlanKind kind;
    char     cost_class[24];
    union {
        struct {                  /* PLAN_LOOP */
            char    accumulator[64];
            char    op[8];
            char    loop_var[64];
            IRExpr *start;
            IRExpr *end;
            IRExpr *body;
            IRExpr *init_val;
        } loop;
        struct {                  /* PLAN_CLOSED_FORM */
            char    accumulator[64];
            IRExpr *formula;
            char    description[128];
        } closed_form;
        struct {                  /* PLAN_CONSTANT */
            char    accumulator[64];
            int64_t value;
        } constant;
        struct {                  /* PLAN_ASSIGN */
            char    var[64];
            IRExpr *expr;
        } assign;
    } as;
};

struct FunctionPlan {
    char            name[64];
    char          **param_names;
    int             n_params;
    ExecutionPlan **steps;
    int             n_steps;
    int             cap_steps;
    IRExpr         *return_expr;  /* may be NULL */
};

/* -----------------------------------------------------------------------
 * Public API: Lexer
 * ----------------------------------------------------------------------- */

int  telos_tokenize(const char *source, TokenArray *out);

/* -----------------------------------------------------------------------
 * Public API: Parser
 * ----------------------------------------------------------------------- */

Program *telos_parse(const char *source);
void     telos_free_program(Program *prog);

/* -----------------------------------------------------------------------
 * Public API: IR
 * ----------------------------------------------------------------------- */

IRExpr *ir_const(double val);
IRExpr *ir_var(const char *name);
IRExpr *ir_binop(const char *op, IRExpr *left, IRExpr *right);
IRExpr *ir_unaryop(const char *op, IRExpr *operand);
IRExpr *ir_call(const char *name, IRExpr **args, int n_args);
IRExpr *ir_clone(const IRExpr *e);
void    ir_free(IRExpr *e);
IRExpr *ir_simplify(IRExpr *expr);

/* -----------------------------------------------------------------------
 * Public API: Constraint Graph
 * ----------------------------------------------------------------------- */

ConstraintGraph *cg_create(const char *name, char **params, int n_params);
ConstraintNode  *cg_add(ConstraintGraph *g, ConstraintNode *node);
void             cg_free(ConstraintGraph *g);

/* -----------------------------------------------------------------------
 * Public API: Semantic Lifting
 * ----------------------------------------------------------------------- */

typedef struct {
    ConstraintGraph **graphs;
    char            **names;
    int               count;
} LiftResult;

LiftResult telos_lift_program(Program *prog);
void       telos_free_lift_result(LiftResult *r);

/* -----------------------------------------------------------------------
 * Public API: Optimizer / Planner
 * ----------------------------------------------------------------------- */

FunctionPlan *telos_plan_function(ConstraintGraph *graph);
void          telos_free_function_plan(FunctionPlan *plan);

/* Cost model */
int            plan_cost(const ExecutionPlan *plan);
ExecutionPlan *plan_cheapest(ExecutionPlan **plans, int count);

/* -----------------------------------------------------------------------
 * Public API: Goal Graph (liveness analysis)
 * ----------------------------------------------------------------------- */

typedef struct {
    bool *is_live;        /* indexed by node_id */
    int   max_id;
} LivenessResult;

LivenessResult telos_compute_liveness(ConstraintGraph *graph);

/* -----------------------------------------------------------------------
 * Public API: Code generators
 * ----------------------------------------------------------------------- */

/* x86-64 machine code generator */
typedef struct {
    uint8_t *code;
    int      size;
} MachineCode;

MachineCode telos_gen_x86_64(FunctionPlan *plan);

/* C source code generator */
char *telos_gen_c(FunctionPlan *plan);

/* -----------------------------------------------------------------------
 * Public API: Executable wrapper
 * ----------------------------------------------------------------------- */

typedef int64_t (*TelosFunc)();
typedef int64_t (*TelosFunc1)(int64_t);
typedef int64_t (*TelosFunc2)(int64_t, int64_t);
typedef int64_t (*TelosFunc3)(int64_t, int64_t, int64_t);

typedef struct {
    void    *mem;         /* mmap'd region */
    int      mem_size;
    void    *func_ptr;    /* function pointer */
    int      n_params;
} NativeFunction;

NativeFunction telos_make_native(const uint8_t *code, int size, int n_params);
int64_t        telos_call_native(NativeFunction *fn, int64_t *args, int n_args);
void           telos_free_native(NativeFunction *fn);

/* -----------------------------------------------------------------------
 * Public API: Compiler (high-level)
 * ----------------------------------------------------------------------- */

typedef struct {
    char        **names;
    MachineCode  *codes;
    int           count;
} CompileResult;

typedef struct {
    char            **names;
    NativeFunction   *funcs;
    int               count;
} RunResult;

typedef struct {
    char **names;
    char **sources;
    int    count;
} CompileCResult;

CompileResult  telos_compile(const char *source);
RunResult      telos_run(const char *source);
CompileCResult telos_compile_c(const char *source);

void telos_free_compile_result(CompileResult *r);
void telos_free_run_result(RunResult *r);
void telos_free_compile_c_result(CompileCResult *r);

/* -----------------------------------------------------------------------
 * Constant evaluation helper (used by planner, exported for tests)
 * ----------------------------------------------------------------------- */

typedef struct {
    const char *names[64];
    double      values[64];
    int         count;
} EvalEnv;

bool ir_eval_const(const IRExpr *expr, const EvalEnv *env, double *out);

#endif /* TELOS_H */
