/*
 * parser.c — Recursive-descent parser for the Telos language.
 *
 * Parses tokens produced by telos_tokenize() into an AST.
 * Matches the behavior of the Python Parser class in src/parser.py.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "telos.h"

/* -----------------------------------------------------------------------
 * Parser state
 * ----------------------------------------------------------------------- */

typedef struct {
    Token *tokens;
    int    count;
    int    pos;
} ParserState;

/* -----------------------------------------------------------------------
 * Keyword / operator classification helpers
 * ----------------------------------------------------------------------- */

static bool is_type_keyword(const char *val)
{
    return strcmp(val, "int") == 0 || strcmp(val, "float") == 0 ||
           strcmp(val, "double") == 0 || strcmp(val, "void") == 0 ||
           strcmp(val, "bool") == 0;
}

static bool is_assign_op(const char *val)
{
    return strcmp(val, "=") == 0 || strcmp(val, "+=") == 0 ||
           strcmp(val, "-=") == 0 || strcmp(val, "*=") == 0 ||
           strcmp(val, "/=") == 0 || strcmp(val, "%=") == 0;
}

/* -----------------------------------------------------------------------
 * Token access helpers
 * ----------------------------------------------------------------------- */

static Token *cur(ParserState *p)
{
    return &p->tokens[p->pos];
}

static Token *peek(ParserState *p, int offset) __attribute__((unused));
static Token *peek(ParserState *p, int offset)
{
    int idx = p->pos + offset;
    if (idx >= p->count)
        return &p->tokens[p->count - 1];
    return &p->tokens[idx];
}

static Token *advance(ParserState *p)
{
    Token *tok = &p->tokens[p->pos];
    if (tok->kind != TOK_EOF)
        p->pos++;
    return tok;
}

static Token *expect(ParserState *p, TokenKind kind, const char *value)
{
    Token *tok = cur(p);
    if (tok->kind != kind) {
        fprintf(stderr, "ParseError: Expected token kind %d but got %d "
                "('%s') at line %d:%d\n",
                kind, tok->kind, tok->value, tok->line, tok->col);
        return NULL;
    }
    if (value && strcmp(tok->value, value) != 0) {
        fprintf(stderr, "ParseError: Expected '%s' but got '%s' "
                "at line %d:%d\n",
                value, tok->value, tok->line, tok->col);
        return NULL;
    }
    return advance(p);
}

static bool match(ParserState *p, TokenKind kind, const char *value)
{
    Token *tok = cur(p);
    if (tok->kind != kind)
        return false;
    if (value && strcmp(tok->value, value) != 0)
        return false;
    advance(p);
    return true;
}

/* -----------------------------------------------------------------------
 * AST node constructors — Expressions
 * ----------------------------------------------------------------------- */

static Expr *make_int_lit(int64_t val)
{
    Expr *e = malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_INT_LIT;
    e->as.int_val = val;
    return e;
}

static Expr *make_float_lit(double val)
{
    Expr *e = malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_FLOAT_LIT;
    e->as.float_val = val;
    return e;
}

static Expr *make_identifier(const char *name)
{
    Expr *e = malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_IDENTIFIER;
    strncpy(e->as.name, name, sizeof(e->as.name) - 1);
    e->as.name[sizeof(e->as.name) - 1] = '\0';
    return e;
}

static Expr *make_binop(const char *op, Expr *left, Expr *right)
{
    Expr *e = malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_BINARY_OP;
    strncpy(e->as.binop.op, op, sizeof(e->as.binop.op) - 1);
    e->as.binop.op[sizeof(e->as.binop.op) - 1] = '\0';
    e->as.binop.left = left;
    e->as.binop.right = right;
    return e;
}

static Expr *make_unaryop(const char *op, Expr *operand, bool postfix)
{
    Expr *e = malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_UNARY_OP;
    strncpy(e->as.unaryop.op, op, sizeof(e->as.unaryop.op) - 1);
    e->as.unaryop.op[sizeof(e->as.unaryop.op) - 1] = '\0';
    e->as.unaryop.operand = operand;
    e->as.unaryop.postfix = postfix;
    return e;
}

static Expr *make_call(const char *name, Expr **args, int n_args)
{
    Expr *e = malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_CALL;
    strncpy(e->as.call.name, name, sizeof(e->as.call.name) - 1);
    e->as.call.name[sizeof(e->as.call.name) - 1] = '\0';
    e->as.call.args = args;
    e->as.call.n_args = n_args;
    return e;
}

static Expr *make_assignment(const char *target, const char *op, Expr *value)
{
    Expr *e = malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_ASSIGNMENT;
    strncpy(e->as.assign.target, target, sizeof(e->as.assign.target) - 1);
    e->as.assign.target[sizeof(e->as.assign.target) - 1] = '\0';
    strncpy(e->as.assign.op, op, sizeof(e->as.assign.op) - 1);
    e->as.assign.op[sizeof(e->as.assign.op) - 1] = '\0';
    e->as.assign.value = value;
    return e;
}

/* -----------------------------------------------------------------------
 * AST node constructors — Statements
 * ----------------------------------------------------------------------- */

static Stmt *make_var_decl(const char *type, const char *name, Expr *init)
{
    Stmt *s = malloc(sizeof(Stmt));
    if (!s) return NULL;
    s->kind = STMT_VAR_DECL;
    strncpy(s->as.var_decl.type_name, type, sizeof(s->as.var_decl.type_name) - 1);
    s->as.var_decl.type_name[sizeof(s->as.var_decl.type_name) - 1] = '\0';
    strncpy(s->as.var_decl.name, name, sizeof(s->as.var_decl.name) - 1);
    s->as.var_decl.name[sizeof(s->as.var_decl.name) - 1] = '\0';
    s->as.var_decl.init = init;
    return s;
}

static Stmt *make_expr_stmt(Expr *expr)
{
    Stmt *s = malloc(sizeof(Stmt));
    if (!s) return NULL;
    s->kind = STMT_EXPR_STMT;
    s->as.expr_stmt.expr = expr;
    return s;
}

static Stmt *make_return(Expr *value)
{
    Stmt *s = malloc(sizeof(Stmt));
    if (!s) return NULL;
    s->kind = STMT_RETURN;
    s->as.ret.value = value;
    return s;
}

static Stmt *make_block(Stmt **stmts, int n_stmts)
{
    Stmt *s = malloc(sizeof(Stmt));
    if (!s) return NULL;
    s->kind = STMT_BLOCK;
    s->as.block.stmts = stmts;
    s->as.block.n_stmts = n_stmts;
    return s;
}

static Stmt *make_if(Expr *cond, Stmt *then_b, Stmt *else_b)
{
    Stmt *s = malloc(sizeof(Stmt));
    if (!s) return NULL;
    s->kind = STMT_IF;
    s->as.if_stmt.cond = cond;
    s->as.if_stmt.then_branch = then_b;
    s->as.if_stmt.else_branch = else_b;
    return s;
}

static Stmt *make_for(Stmt *init, Expr *cond, Expr *update, Stmt *body)
{
    Stmt *s = malloc(sizeof(Stmt));
    if (!s) return NULL;
    s->kind = STMT_FOR;
    s->as.for_stmt.init = init;
    s->as.for_stmt.cond = cond;
    s->as.for_stmt.update = update;
    s->as.for_stmt.body = body;
    return s;
}

static Stmt *make_while(Expr *cond, Stmt *body)
{
    Stmt *s = malloc(sizeof(Stmt));
    if (!s) return NULL;
    s->kind = STMT_WHILE;
    s->as.while_stmt.cond = cond;
    s->as.while_stmt.body = body;
    return s;
}

/* -----------------------------------------------------------------------
 * Forward declarations for recursive-descent expression parsing
 * ----------------------------------------------------------------------- */

static Expr *parse_expr(ParserState *p);
static Expr *parse_assign(ParserState *p);
static Expr *parse_or(ParserState *p);
static Expr *parse_and(ParserState *p);
static Expr *parse_eq(ParserState *p);
static Expr *parse_cmp(ParserState *p);
static Expr *parse_add(ParserState *p);
static Expr *parse_mul(ParserState *p);
static Expr *parse_unary(ParserState *p);
static Expr *parse_postfix(ParserState *p);
static Expr *parse_primary(ParserState *p);

/* Forward declarations for statement parsing */
static Stmt *parse_block(ParserState *p);
static Stmt *parse_stmt(ParserState *p);

/* -----------------------------------------------------------------------
 * Expression parsing (precedence: low → high)
 * ----------------------------------------------------------------------- */

static Expr *parse_expr(ParserState *p)
{
    return parse_assign(p);
}

static Expr *parse_assign(ParserState *p)
{
    Expr *left = parse_or(p);
    if (!left) return NULL;

    if (cur(p)->kind == TOK_OP && is_assign_op(cur(p)->value)) {
        Token *op_tok = advance(p);
        if (left->kind != EXPR_IDENTIFIER) {
            fprintf(stderr, "ParseError: Assignment target must be an identifier "
                    "at line %d:%d\n", cur(p)->line, cur(p)->col);
            return NULL;
        }
        Expr *right = parse_assign(p);  /* right-associative */
        if (!right) return NULL;
        char name_copy[64];
        strncpy(name_copy, left->as.name, sizeof(name_copy) - 1);
        name_copy[sizeof(name_copy) - 1] = '\0';
        free(left);
        return make_assignment(name_copy, op_tok->value, right);
    }
    return left;
}

static Expr *parse_or(ParserState *p)
{
    Expr *left = parse_and(p);
    if (!left) return NULL;

    while (cur(p)->kind == TOK_OP && strcmp(cur(p)->value, "||") == 0) {
        Token *op_tok = advance(p);
        Expr *right = parse_and(p);
        if (!right) return NULL;
        left = make_binop(op_tok->value, left, right);
        if (!left) return NULL;
    }
    return left;
}

static Expr *parse_and(ParserState *p)
{
    Expr *left = parse_eq(p);
    if (!left) return NULL;

    while (cur(p)->kind == TOK_OP && strcmp(cur(p)->value, "&&") == 0) {
        Token *op_tok = advance(p);
        Expr *right = parse_eq(p);
        if (!right) return NULL;
        left = make_binop(op_tok->value, left, right);
        if (!left) return NULL;
    }
    return left;
}

static Expr *parse_eq(ParserState *p)
{
    Expr *left = parse_cmp(p);
    if (!left) return NULL;

    while (cur(p)->kind == TOK_OP &&
           (strcmp(cur(p)->value, "==") == 0 || strcmp(cur(p)->value, "!=") == 0)) {
        Token *op_tok = advance(p);
        Expr *right = parse_cmp(p);
        if (!right) return NULL;
        left = make_binop(op_tok->value, left, right);
        if (!left) return NULL;
    }
    return left;
}

static Expr *parse_cmp(ParserState *p)
{
    Expr *left = parse_add(p);
    if (!left) return NULL;

    while (cur(p)->kind == TOK_OP &&
           (strcmp(cur(p)->value, "<") == 0 || strcmp(cur(p)->value, ">") == 0 ||
            strcmp(cur(p)->value, "<=") == 0 || strcmp(cur(p)->value, ">=") == 0)) {
        Token *op_tok = advance(p);
        Expr *right = parse_add(p);
        if (!right) return NULL;
        left = make_binop(op_tok->value, left, right);
        if (!left) return NULL;
    }
    return left;
}

static Expr *parse_add(ParserState *p)
{
    Expr *left = parse_mul(p);
    if (!left) return NULL;

    while (cur(p)->kind == TOK_OP &&
           (strcmp(cur(p)->value, "+") == 0 || strcmp(cur(p)->value, "-") == 0)) {
        Token *op_tok = advance(p);
        Expr *right = parse_mul(p);
        if (!right) return NULL;
        left = make_binop(op_tok->value, left, right);
        if (!left) return NULL;
    }
    return left;
}

static Expr *parse_mul(ParserState *p)
{
    Expr *left = parse_unary(p);
    if (!left) return NULL;

    while (cur(p)->kind == TOK_OP &&
           (strcmp(cur(p)->value, "*") == 0 || strcmp(cur(p)->value, "/") == 0 ||
            strcmp(cur(p)->value, "%") == 0)) {
        Token *op_tok = advance(p);
        Expr *right = parse_unary(p);
        if (!right) return NULL;
        left = make_binop(op_tok->value, left, right);
        if (!left) return NULL;
    }
    return left;
}

static Expr *parse_unary(ParserState *p)
{
    Token *tok = cur(p);

    /* Prefix minus, logical not */
    if (tok->kind == TOK_OP &&
        (strcmp(tok->value, "-") == 0 || strcmp(tok->value, "!") == 0)) {
        Token *op_tok = advance(p);
        Expr *operand = parse_unary(p);
        if (!operand) return NULL;
        return make_unaryop(op_tok->value, operand, false);
    }

    /* Prefix increment / decrement */
    if (tok->kind == TOK_OP &&
        (strcmp(tok->value, "++") == 0 || strcmp(tok->value, "--") == 0)) {
        Token *op_tok = advance(p);
        Expr *operand = parse_postfix(p);
        if (!operand) return NULL;
        return make_unaryop(op_tok->value, operand, false);
    }

    return parse_postfix(p);
}

static Expr *parse_postfix(ParserState *p)
{
    Expr *expr = parse_primary(p);
    if (!expr) return NULL;

    while (cur(p)->kind == TOK_OP &&
           (strcmp(cur(p)->value, "++") == 0 || strcmp(cur(p)->value, "--") == 0)) {
        Token *op_tok = advance(p);
        expr = make_unaryop(op_tok->value, expr, true);
        if (!expr) return NULL;
    }
    return expr;
}

static Expr *parse_primary(ParserState *p)
{
    Token *tok = cur(p);

    /* Integer literal */
    if (tok->kind == TOK_INT_LIT) {
        advance(p);
        return make_int_lit(strtoll(tok->value, NULL, 10));
    }

    /* Float literal */
    if (tok->kind == TOK_FLOAT_LIT) {
        advance(p);
        return make_float_lit(strtod(tok->value, NULL));
    }

    /* Boolean keywords → integer 1 or 0 */
    if (tok->kind == TOK_KEYWORD &&
        (strcmp(tok->value, "true") == 0 || strcmp(tok->value, "false") == 0)) {
        bool is_true = strcmp(tok->value, "true") == 0;
        advance(p);
        return make_int_lit(is_true ? 1 : 0);
    }

    /* Identifier (possibly followed by function call) */
    if (tok->kind == TOK_ID) {
        Token *id_tok = advance(p);
        if (strcmp(cur(p)->value, "(") == 0) {
            /* Function call */
            advance(p);  /* consume '(' */

            int arg_cap = 4;
            int n_args = 0;
            Expr **args = malloc(arg_cap * sizeof(Expr *));
            if (!args) return NULL;

            if (strcmp(cur(p)->value, ")") != 0) {
                Expr *arg = parse_expr(p);
                if (!arg) { free(args); return NULL; }
                args[n_args++] = arg;

                while (match(p, TOK_PUNCT, ",")) {
                    if (n_args >= arg_cap) {
                        arg_cap *= 2;
                        Expr **tmp = realloc(args, arg_cap * sizeof(Expr *));
                        if (!tmp) { free(args); return NULL; }
                        args = tmp;
                    }
                    arg = parse_expr(p);
                    if (!arg) { free(args); return NULL; }
                    args[n_args++] = arg;
                }
            }

            if (!expect(p, TOK_PUNCT, ")")) { free(args); return NULL; }
            return make_call(id_tok->value, args, n_args);
        }
        return make_identifier(id_tok->value);
    }

    /* Parenthesized expression */
    if (strcmp(tok->value, "(") == 0) {
        advance(p);
        Expr *expr = parse_expr(p);
        if (!expr) return NULL;
        if (!expect(p, TOK_PUNCT, ")")) return NULL;
        return expr;
    }

    fprintf(stderr, "ParseError: Unexpected token '%s' at line %d:%d\n",
            tok->value, tok->line, tok->col);
    return NULL;
}

/* -----------------------------------------------------------------------
 * Statement parsing
 * ----------------------------------------------------------------------- */

static Stmt *parse_block(ParserState *p)
{
    if (!expect(p, TOK_PUNCT, "{")) return NULL;

    int cap = 8;
    int count = 0;
    Stmt **stmts = malloc(cap * sizeof(Stmt *));
    if (!stmts) return NULL;

    while (strcmp(cur(p)->value, "}") != 0 && cur(p)->kind != TOK_EOF) {
        Stmt *s = parse_stmt(p);
        if (!s) { free(stmts); return NULL; }
        if (count >= cap) {
            cap *= 2;
            Stmt **tmp = realloc(stmts, cap * sizeof(Stmt *));
            if (!tmp) { free(stmts); return NULL; }
            stmts = tmp;
        }
        stmts[count++] = s;
    }

    if (!expect(p, TOK_PUNCT, "}")) { free(stmts); return NULL; }
    return make_block(stmts, count);
}

static Stmt *parse_var_decl_stmt(ParserState *p)
{
    Token *type_tok = advance(p);
    Token *name_tok = expect(p, TOK_ID, NULL);
    if (!name_tok) return NULL;

    Expr *init = NULL;
    if (match(p, TOK_OP, "=")) {
        init = parse_expr(p);
        if (!init) return NULL;
    }
    if (!expect(p, TOK_PUNCT, ";")) return NULL;
    return make_var_decl(type_tok->value, name_tok->value, init);
}

static Stmt *parse_return_stmt(ParserState *p)
{
    if (!expect(p, TOK_KEYWORD, "return")) return NULL;

    if (strcmp(cur(p)->value, ";") == 0) {
        advance(p);
        return make_return(NULL);
    }
    Expr *val = parse_expr(p);
    if (!val) return NULL;
    if (!expect(p, TOK_PUNCT, ";")) return NULL;
    return make_return(val);
}

static Stmt *parse_for_stmt(ParserState *p)
{
    if (!expect(p, TOK_KEYWORD, "for")) return NULL;
    if (!expect(p, TOK_PUNCT, "(")) return NULL;

    /* init clause */
    Stmt *init = NULL;
    if (strcmp(cur(p)->value, ";") != 0) {
        if (cur(p)->kind == TOK_KEYWORD && is_type_keyword(cur(p)->value)) {
            /* Variable declaration (without consuming the trailing ';') */
            Token *type_tok = advance(p);
            Token *name_tok = expect(p, TOK_ID, NULL);
            if (!name_tok) return NULL;
            Expr *iexpr = NULL;
            if (match(p, TOK_OP, "=")) {
                iexpr = parse_expr(p);
                if (!iexpr) return NULL;
            }
            init = make_var_decl(type_tok->value, name_tok->value, iexpr);
        } else {
            Expr *e = parse_expr(p);
            if (!e) return NULL;
            init = make_expr_stmt(e);
        }
        if (!init) return NULL;
    }
    if (!expect(p, TOK_PUNCT, ";")) return NULL;

    /* condition clause */
    Expr *cond = NULL;
    if (strcmp(cur(p)->value, ";") != 0) {
        cond = parse_expr(p);
        if (!cond) return NULL;
    }
    if (!expect(p, TOK_PUNCT, ";")) return NULL;

    /* update clause */
    Expr *update = NULL;
    if (strcmp(cur(p)->value, ")") != 0) {
        update = parse_expr(p);
        if (!update) return NULL;
    }
    if (!expect(p, TOK_PUNCT, ")")) return NULL;

    Stmt *body = parse_block(p);
    if (!body) return NULL;
    return make_for(init, cond, update, body);
}

static Stmt *parse_while_stmt(ParserState *p)
{
    if (!expect(p, TOK_KEYWORD, "while")) return NULL;
    if (!expect(p, TOK_PUNCT, "(")) return NULL;
    Expr *cond = parse_expr(p);
    if (!cond) return NULL;
    if (!expect(p, TOK_PUNCT, ")")) return NULL;
    Stmt *body = parse_block(p);
    if (!body) return NULL;
    return make_while(cond, body);
}

static Stmt *parse_if_stmt(ParserState *p)
{
    if (!expect(p, TOK_KEYWORD, "if")) return NULL;
    if (!expect(p, TOK_PUNCT, "(")) return NULL;
    Expr *cond = parse_expr(p);
    if (!cond) return NULL;
    if (!expect(p, TOK_PUNCT, ")")) return NULL;

    Stmt *then_branch = parse_block(p);
    if (!then_branch) return NULL;

    Stmt *else_branch = NULL;
    if (match(p, TOK_KEYWORD, "else")) {
        if (cur(p)->kind == TOK_KEYWORD && strcmp(cur(p)->value, "if") == 0) {
            /* else if: wrap inner if in a block */
            Stmt *inner = parse_if_stmt(p);
            if (!inner) return NULL;
            Stmt **wrap = malloc(sizeof(Stmt *));
            if (!wrap) return NULL;
            wrap[0] = inner;
            else_branch = make_block(wrap, 1);
        } else {
            else_branch = parse_block(p);
        }
        if (!else_branch) return NULL;
    }
    return make_if(cond, then_branch, else_branch);
}

static Stmt *parse_stmt(ParserState *p)
{
    Token *tok = cur(p);

    /* Variable declaration */
    if (tok->kind == TOK_KEYWORD && is_type_keyword(tok->value))
        return parse_var_decl_stmt(p);

    /* return */
    if (tok->kind == TOK_KEYWORD && strcmp(tok->value, "return") == 0)
        return parse_return_stmt(p);

    /* for */
    if (tok->kind == TOK_KEYWORD && strcmp(tok->value, "for") == 0)
        return parse_for_stmt(p);

    /* while */
    if (tok->kind == TOK_KEYWORD && strcmp(tok->value, "while") == 0)
        return parse_while_stmt(p);

    /* if */
    if (tok->kind == TOK_KEYWORD && strcmp(tok->value, "if") == 0)
        return parse_if_stmt(p);

    /* nested block */
    if (strcmp(tok->value, "{") == 0)
        return parse_block(p);

    /* expression statement */
    Expr *expr = parse_expr(p);
    if (!expr) return NULL;
    if (!expect(p, TOK_PUNCT, ";")) return NULL;
    return make_expr_stmt(expr);
}

/* -----------------------------------------------------------------------
 * Function / program parsing
 * ----------------------------------------------------------------------- */

static Param *parse_params(ParserState *p, int *out_count)
{
    int cap = 4;
    int count = 0;
    Param *params = malloc(cap * sizeof(Param));
    if (!params) { *out_count = 0; return NULL; }

    if (strcmp(cur(p)->value, ")") == 0) {
        *out_count = 0;
        free(params);
        return NULL;
    }

    while (true) {
        Token *tok = cur(p);
        if (tok->kind != TOK_KEYWORD || !is_type_keyword(tok->value)) {
            fprintf(stderr, "ParseError: Expected parameter type at line %d:%d\n",
                    tok->line, tok->col);
            free(params);
            *out_count = -1;
            return NULL;
        }
        Token *type_tok = advance(p);
        Token *name_tok = expect(p, TOK_ID, NULL);
        if (!name_tok) { free(params); *out_count = -1; return NULL; }

        if (count >= cap) {
            cap *= 2;
            Param *tmp = realloc(params, cap * sizeof(Param));
            if (!tmp) { free(params); *out_count = -1; return NULL; }
            params = tmp;
        }

        strncpy(params[count].type_name, type_tok->value, sizeof(params[count].type_name) - 1);
        params[count].type_name[sizeof(params[count].type_name) - 1] = '\0';
        strncpy(params[count].name, name_tok->value, sizeof(params[count].name) - 1);
        params[count].name[sizeof(params[count].name) - 1] = '\0';
        count++;

        if (!match(p, TOK_PUNCT, ","))
            break;
    }

    *out_count = count;
    return params;
}

static Function *parse_function(ParserState *p)
{
    Token *tok = cur(p);
    if (tok->kind != TOK_KEYWORD || !is_type_keyword(tok->value)) {
        fprintf(stderr, "ParseError: Expected return type at line %d:%d, "
                "got '%s'\n", tok->line, tok->col, tok->value);
        return NULL;
    }
    Token *ret_tok = advance(p);
    Token *name_tok = expect(p, TOK_ID, NULL);
    if (!name_tok) return NULL;
    if (!expect(p, TOK_PUNCT, "(")) return NULL;

    int n_params = 0;
    Param *params = parse_params(p, &n_params);
    if (n_params < 0) return NULL;

    if (!expect(p, TOK_PUNCT, ")")) { free(params); return NULL; }

    Stmt *body = parse_block(p);
    if (!body) { free(params); return NULL; }

    Function *fn = malloc(sizeof(Function));
    if (!fn) { free(params); return NULL; }

    strncpy(fn->return_type, ret_tok->value, sizeof(fn->return_type) - 1);
    fn->return_type[sizeof(fn->return_type) - 1] = '\0';
    strncpy(fn->name, name_tok->value, sizeof(fn->name) - 1);
    fn->name[sizeof(fn->name) - 1] = '\0';
    fn->params = params;
    fn->n_params = n_params;
    fn->body = body;
    return fn;
}

static Program *parse_program(ParserState *p)
{
    int cap = 4;
    int count = 0;
    Function *fns = malloc(cap * sizeof(Function));
    if (!fns) return NULL;

    while (cur(p)->kind != TOK_EOF) {
        Function *fn = parse_function(p);
        if (!fn) { free(fns); return NULL; }
        if (count >= cap) {
            cap *= 2;
            Function *tmp = realloc(fns, cap * sizeof(Function));
            if (!tmp) { free(fns); return NULL; }
            fns = tmp;
        }
        fns[count++] = *fn;
        free(fn);
    }

    Program *prog = malloc(sizeof(Program));
    if (!prog) { free(fns); return NULL; }
    prog->functions = fns;
    prog->n_functions = count;
    return prog;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

Program *telos_parse(const char *source)
{
    TokenArray tokens;
    if (telos_tokenize(source, &tokens) != 0)
        return NULL;

    ParserState state;
    state.tokens = tokens.items;
    state.count = tokens.count;
    state.pos = 0;

    Program *prog = parse_program(&state);

    da_free(&tokens);
    return prog;
}

/* -----------------------------------------------------------------------
 * Recursive free helpers
 * ----------------------------------------------------------------------- */

static void free_expr(Expr *e)
{
    if (!e) return;
    switch (e->kind) {
    case EXPR_INT_LIT:
    case EXPR_FLOAT_LIT:
    case EXPR_IDENTIFIER:
        break;
    case EXPR_BINARY_OP:
        free_expr(e->as.binop.left);
        free_expr(e->as.binop.right);
        break;
    case EXPR_UNARY_OP:
        free_expr(e->as.unaryop.operand);
        break;
    case EXPR_CALL:
        for (int i = 0; i < e->as.call.n_args; i++)
            free_expr(e->as.call.args[i]);
        free(e->as.call.args);
        break;
    case EXPR_ASSIGNMENT:
        free_expr(e->as.assign.value);
        break;
    }
    free(e);
}

static void free_stmt(Stmt *s)
{
    if (!s) return;
    switch (s->kind) {
    case STMT_VAR_DECL:
        free_expr(s->as.var_decl.init);
        break;
    case STMT_EXPR_STMT:
        free_expr(s->as.expr_stmt.expr);
        break;
    case STMT_RETURN:
        free_expr(s->as.ret.value);
        break;
    case STMT_BLOCK:
        for (int i = 0; i < s->as.block.n_stmts; i++)
            free_stmt(s->as.block.stmts[i]);
        free(s->as.block.stmts);
        break;
    case STMT_IF:
        free_expr(s->as.if_stmt.cond);
        free_stmt(s->as.if_stmt.then_branch);
        free_stmt(s->as.if_stmt.else_branch);
        break;
    case STMT_FOR:
        free_stmt(s->as.for_stmt.init);
        free_expr(s->as.for_stmt.cond);
        free_expr(s->as.for_stmt.update);
        free_stmt(s->as.for_stmt.body);
        break;
    case STMT_WHILE:
        free_expr(s->as.while_stmt.cond);
        free_stmt(s->as.while_stmt.body);
        break;
    }
    free(s);
}

void telos_free_program(Program *prog)
{
    if (!prog) return;
    for (int i = 0; i < prog->n_functions; i++) {
        free(prog->functions[i].params);
        free_stmt(prog->functions[i].body);
    }
    free(prog->functions);
    free(prog);
}
