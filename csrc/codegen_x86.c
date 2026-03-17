/*
 * codegen_x86.c — x86-64 machine code generator for the Telos compiler.
 *
 * Translates a FunctionPlan into raw x86-64 machine code bytes using
 * the System V AMD64 ABI (Linux / macOS).
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "telos.h"

/* -----------------------------------------------------------------------
 * Generator state
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t *code;
    int      code_size;
    int      code_cap;
    /* variable → stack-slot mapping */
    char     slot_names[256][64];
    int      slot_offsets[256];
    int      n_slots;
    int      next_slot;           /* grows downward (total bytes allocated) */
} X86Gen;

/* -----------------------------------------------------------------------
 * System V AMD64 ABI: parameter register table
 * ----------------------------------------------------------------------- */

typedef struct { int reg_code; bool needs_rex_r; } ParamReg;

static const ParamReg PARAM_REGS[] = {
    {7, false},   /* rdi */
    {6, false},   /* rsi */
    {2, false},   /* rdx */
    {1, false},   /* rcx */
    {0, true},    /* r8  */
    {1, true},    /* r9  */
};

#define MAX_PARAM_REGS 6

/* -----------------------------------------------------------------------
 * Accumulation operator byte sequences
 * ----------------------------------------------------------------------- */

static const uint8_t ACC_ADD[] = {0x48, 0x01, 0xC8};          /* add rax, rcx */
static const uint8_t ACC_MUL[] = {0x48, 0x0F, 0xAF, 0xC1};   /* imul rax, rcx */
static const uint8_t ACC_SUB[] = {0x48, 0x29, 0xC8};          /* sub rax, rcx */

/* -----------------------------------------------------------------------
 * Byte buffer helpers
 * ----------------------------------------------------------------------- */

static void emit(X86Gen *g, const uint8_t *bytes, int n)
{
    while (g->code_size + n > g->code_cap) {
        g->code_cap = g->code_cap ? g->code_cap * 2 : 256;
        g->code = realloc(g->code, g->code_cap);
    }
    memcpy(g->code + g->code_size, bytes, n);
    g->code_size += n;
}

static void emit1(X86Gen *g, uint8_t b)
{
    emit(g, &b, 1);
}

static void emit2(X86Gen *g, uint8_t a, uint8_t b)
{
    uint8_t t[2] = {a, b};
    emit(g, t, 2);
}
static void emit3(X86Gen *g, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t t[3] = {a, b, c};
    emit(g, t, 3);
}
static void emit4(X86Gen *g, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    uint8_t t[4] = {a, b, c, d};
    emit(g, t, 4);
}

static void emit_i32(X86Gen *g, int32_t v)
{
    uint8_t buf[4];
    memcpy(buf, &v, 4);   /* little-endian on x86 */
    emit(g, buf, 4);
}

static void emit_i64(X86Gen *g, int64_t v)
{
    uint8_t buf[8];
    memcpy(buf, &v, 8);
    emit(g, buf, 8);
}

static void patch_i32(X86Gen *g, int pos, int32_t v)
{
    memcpy(g->code + pos, &v, 4);
}

/* -----------------------------------------------------------------------
 * Stack-slot management
 * ----------------------------------------------------------------------- */

static int slot_lookup(X86Gen *g, const char *name)
{
    for (int i = 0; i < g->n_slots; i++)
        if (strcmp(g->slot_names[i], name) == 0)
            return g->slot_offsets[i];
    return 0; /* should not happen if prescan is correct */
}

static int alloc_slot(X86Gen *g, const char *name)
{
    g->next_slot += 8;
    int off = -g->next_slot;
    strncpy(g->slot_names[g->n_slots], name, 63);
    g->slot_names[g->n_slots][63] = '\0';
    g->slot_offsets[g->n_slots] = off;
    g->n_slots++;
    return off;
}

static int get_or_alloc(X86Gen *g, const char *name)
{
    for (int i = 0; i < g->n_slots; i++)
        if (strcmp(g->slot_names[i], name) == 0)
            return g->slot_offsets[i];
    return alloc_slot(g, name);
}

static void prescan(X86Gen *g, const ExecutionPlan *step)
{
    switch (step->kind) {
    case PLAN_CONSTANT:
        get_or_alloc(g, step->as.constant.accumulator);
        break;
    case PLAN_CLOSED_FORM:
        get_or_alloc(g, step->as.closed_form.accumulator);
        break;
    case PLAN_ASSIGN:
        get_or_alloc(g, step->as.assign.var);
        break;
    case PLAN_LOOP: {
        get_or_alloc(g, step->as.loop.accumulator);
        get_or_alloc(g, step->as.loop.loop_var);
        char end_name[128];
        snprintf(end_name, sizeof(end_name), "_end_%s", step->as.loop.loop_var);
        get_or_alloc(g, end_name);
        break;
    }
    }
}

/* -----------------------------------------------------------------------
 * ModRM encoding for [RBP + offset]
 * ----------------------------------------------------------------------- */

static void emit_modrm_rbp(X86Gen *g, int reg, int offset)
{
    if (offset >= -128 && offset <= 127) {
        /* mod=01, rm=rbp(101), disp8 */
        uint8_t modrm = (0x01 << 6) | ((reg & 7) << 3) | 0x05;
        emit1(g, modrm);
        emit1(g, (uint8_t)(offset & 0xFF));
    } else {
        /* mod=10, rm=rbp(101), disp32 */
        uint8_t modrm = (0x02 << 6) | ((reg & 7) << 3) | 0x05;
        emit1(g, modrm);
        emit_i32(g, (int32_t)offset);
    }
}

/* -----------------------------------------------------------------------
 * Instruction emitters
 * ----------------------------------------------------------------------- */

static void emit_push_rbp(X86Gen *g)       { emit1(g, 0x55); }
static void emit_pop_rbp(X86Gen *g)        { emit1(g, 0x5D); }
static void emit_push_rax(X86Gen *g)       { emit1(g, 0x50); }
static void emit_pop_rcx(X86Gen *g)        { emit1(g, 0x59); }
static void emit_ret(X86Gen *g)            { emit1(g, 0xC3); }
static void emit_mov_rbp_rsp(X86Gen *g)    { emit3(g, 0x48, 0x89, 0xE5); }
static void emit_xor_rax_rax(X86Gen *g)    { emit3(g, 0x48, 0x31, 0xC0); }

static void emit_sub_rsp(X86Gen *g, int n)
{
    if (n <= 127)
        emit4(g, 0x48, 0x83, 0xEC, (uint8_t)n);
    else {
        emit3(g, 0x48, 0x81, 0xEC);
        emit_i32(g, (int32_t)n);
    }
}

static void emit_add_rsp(X86Gen *g, int n)
{
    if (n <= 127)
        emit4(g, 0x48, 0x83, 0xC4, (uint8_t)n);
    else {
        emit3(g, 0x48, 0x81, 0xC4);
        emit_i32(g, (int32_t)n);
    }
}

static void emit_mov_rax_imm(X86Gen *g, int64_t value)
{
    if (value >= -(1LL << 31) && value <= (1LL << 31) - 1) {
        emit3(g, 0x48, 0xC7, 0xC0);
        emit_i32(g, (int32_t)value);
    } else {
        emit2(g, 0x48, 0xB8);
        emit_i64(g, value);
    }
}

static void emit_load_slot_to_rax(X86Gen *g, int offset)
{
    emit2(g, 0x48, 0x8B);
    emit_modrm_rbp(g, 0, offset);
}

static void emit_store_rax_to_slot(X86Gen *g, int offset)
{
    emit2(g, 0x48, 0x89);
    emit_modrm_rbp(g, 0, offset);
}

static void emit_store_reg_to_slot(X86Gen *g, int reg_code, bool needs_rex_r,
                                   int offset)
{
    uint8_t rex = 0x48 | (needs_rex_r ? 0x04 : 0x00);
    emit2(g, rex, 0x89);
    emit_modrm_rbp(g, reg_code, offset);
}

static void emit_add_slot_imm8(X86Gen *g, int offset, uint8_t imm)
{
    emit2(g, 0x48, 0x83);
    emit_modrm_rbp(g, 0, offset);
    emit1(g, imm);
}

/* -----------------------------------------------------------------------
 * SETcc helper
 * ----------------------------------------------------------------------- */

static void emit_setcc(X86Gen *g, const char *op)
{
    uint8_t cc;
    if      (strcmp(op, "<")  == 0) cc = 0x9C;
    else if (strcmp(op, ">")  == 0) cc = 0x9F;
    else if (strcmp(op, "<=") == 0) cc = 0x9E;
    else if (strcmp(op, ">=") == 0) cc = 0x9D;
    else if (strcmp(op, "==") == 0) cc = 0x94;
    else if (strcmp(op, "!=") == 0) cc = 0x95;
    else {
        fprintf(stderr, "codegen_x86: unsupported comparison op '%s'\n", op);
        cc = 0x94;
    }
    emit3(g, 0x0F, cc, 0xC0);                 /* setcc al        */
    emit4(g, 0x48, 0x0F, 0xB6, 0xC0);         /* movzx rax, al   */
}

/* -----------------------------------------------------------------------
 * Binary-op emitter  (rcx = left, rax = right → result in rax)
 * ----------------------------------------------------------------------- */

static void emit_binop(X86Gen *g, const char *op)
{
    if (strcmp(op, "+") == 0) {
        emit3(g, 0x48, 0x01, 0xC8);                   /* add rax, rcx   */

    } else if (strcmp(op, "-") == 0) {
        emit3(g, 0x48, 0x29, 0xC1);                   /* sub rcx, rax   */
        emit3(g, 0x48, 0x89, 0xC8);                   /* mov rax, rcx   */

    } else if (strcmp(op, "*") == 0) {
        emit4(g, 0x48, 0x0F, 0xAF, 0xC1);             /* imul rax, rcx  */

    } else if (strcmp(op, "/") == 0 || strcmp(op, "//") == 0) {
        emit3(g, 0x48, 0x87, 0xC8);                   /* xchg rax, rcx  */
        emit2(g, 0x48, 0x99);                          /* cqo            */
        emit3(g, 0x48, 0xF7, 0xF9);                   /* idiv rcx       */

    } else if (strcmp(op, "%") == 0) {
        emit3(g, 0x48, 0x87, 0xC8);                   /* xchg rax, rcx  */
        emit2(g, 0x48, 0x99);                          /* cqo            */
        emit3(g, 0x48, 0xF7, 0xF9);                   /* idiv rcx       */
        emit3(g, 0x48, 0x89, 0xD0);                   /* mov rax, rdx   */

    } else if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
               strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0 ||
               strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
        emit3(g, 0x48, 0x39, 0xC1);                   /* cmp rcx, rax   */
        emit_setcc(g, op);

    } else {
        fprintf(stderr, "codegen_x86: unsupported binop '%s'\n", op);
    }
}

/* -----------------------------------------------------------------------
 * Expression emitter  (result in RAX)
 * ----------------------------------------------------------------------- */

static void emit_expr(X86Gen *g, IRExpr *raw_expr)
{
    IRExpr *expr = ir_simplify(ir_clone(raw_expr));

    switch (expr->kind) {
    case IR_CONST:
        emit_mov_rax_imm(g, (int64_t)expr->as.const_val);
        break;

    case IR_VAR:
        emit_load_slot_to_rax(g, slot_lookup(g, expr->as.var_name));
        break;

    case IR_BINOP:
        emit_expr(g, expr->as.binop.left);
        emit_push_rax(g);
        emit_expr(g, expr->as.binop.right);
        emit_pop_rcx(g);
        emit_binop(g, expr->as.binop.op);
        break;

    case IR_UNARYOP:
        emit_expr(g, expr->as.unaryop.operand);
        if (strcmp(expr->as.unaryop.op, "-") == 0) {
            emit3(g, 0x48, 0xF7, 0xD8);               /* neg rax        */
        } else if (strcmp(expr->as.unaryop.op, "!") == 0) {
            emit3(g, 0x48, 0x85, 0xC0);               /* test rax, rax  */
            emit3(g, 0x0F, 0x94, 0xC0);               /* setz al        */
            emit4(g, 0x48, 0x0F, 0xB6, 0xC0);         /* movzx rax, al  */
        } else {
            fprintf(stderr, "codegen_x86: unsupported unary op '%s'\n",
                    expr->as.unaryop.op);
        }
        break;

    case IR_CALL:
        fprintf(stderr, "codegen_x86: IR_CALL not supported in codegen\n");
        break;
    }

    ir_free(expr);
}

/* -----------------------------------------------------------------------
 * Loop emitter
 * ----------------------------------------------------------------------- */

static void emit_loop(X86Gen *g, const ExecutionPlan *step)
{
    int acc_slot = slot_lookup(g, step->as.loop.accumulator);
    int i_slot   = slot_lookup(g, step->as.loop.loop_var);

    char end_name[128];
    snprintf(end_name, sizeof(end_name), "_end_%s", step->as.loop.loop_var);
    int end_slot = slot_lookup(g, end_name);

    /* acc = init_val */
    emit_expr(g, step->as.loop.init_val);
    emit_store_rax_to_slot(g, acc_slot);

    /* i = start */
    emit_expr(g, step->as.loop.start);
    emit_store_rax_to_slot(g, i_slot);

    /* _end = end */
    emit_expr(g, step->as.loop.end);
    emit_store_rax_to_slot(g, end_slot);

    /* ---- loop top ---- */
    int loop_top = g->code_size;

    /* load i, push; load _end; pop rcx; cmp rcx, rax */
    emit_load_slot_to_rax(g, i_slot);
    emit_push_rax(g);
    emit_load_slot_to_rax(g, end_slot);
    emit_pop_rcx(g);
    emit3(g, 0x48, 0x39, 0xC1);               /* cmp rcx, rax   */

    /* jge loop_end (forward, patched later) */
    emit2(g, 0x0F, 0x8D);
    int jge_patch = g->code_size;
    emit_i32(g, 0);

    /* evaluate body expression */
    emit_expr(g, step->as.loop.body);

    /* accumulate: mov rcx, rax; load acc; acc_op; store acc */
    emit3(g, 0x48, 0x89, 0xC1);               /* mov rcx, rax   */
    emit_load_slot_to_rax(g, acc_slot);

    const char *op = step->as.loop.op;
    if (strcmp(op, "add") == 0)
        emit(g, ACC_ADD, sizeof(ACC_ADD));
    else if (strcmp(op, "mul") == 0)
        emit(g, ACC_MUL, sizeof(ACC_MUL));
    else if (strcmp(op, "sub") == 0)
        emit(g, ACC_SUB, sizeof(ACC_SUB));
    else
        fprintf(stderr, "codegen_x86: unsupported acc op '%s'\n", op);

    emit_store_rax_to_slot(g, acc_slot);

    /* i += 1 */
    emit_add_slot_imm8(g, i_slot, 1);

    /* jmp loop_top (backward) */
    emit1(g, 0xE9);
    int jmp_patch = g->code_size;
    emit_i32(g, 0);

    /* ---- loop end ---- */
    int loop_end = g->code_size;

    patch_i32(g, jge_patch, (int32_t)(loop_end - (jge_patch + 4)));
    patch_i32(g, jmp_patch, (int32_t)(loop_top  - (jmp_patch + 4)));

    /* reload accumulator into rax */
    emit_load_slot_to_rax(g, acc_slot);
}

/* -----------------------------------------------------------------------
 * Step emitter
 * ----------------------------------------------------------------------- */

static void emit_step(X86Gen *g, const ExecutionPlan *step)
{
    switch (step->kind) {
    case PLAN_CONSTANT:
        emit_mov_rax_imm(g, step->as.constant.value);
        emit_store_rax_to_slot(g, slot_lookup(g, step->as.constant.accumulator));
        break;

    case PLAN_CLOSED_FORM:
        emit_expr(g, step->as.closed_form.formula);
        emit_store_rax_to_slot(g,
            slot_lookup(g, step->as.closed_form.accumulator));
        break;

    case PLAN_ASSIGN:
        emit_expr(g, step->as.assign.expr);
        emit_store_rax_to_slot(g, slot_lookup(g, step->as.assign.var));
        break;

    case PLAN_LOOP:
        emit_loop(g, step);
        break;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

MachineCode telos_gen_x86_64(FunctionPlan *plan)
{
    X86Gen g;
    memset(&g, 0, sizeof(g));

    /* ---- Allocate stack slots: params first, then prescan steps ---- */
    for (int i = 0; i < plan->n_params; i++)
        alloc_slot(&g, plan->param_names[i]);
    for (int i = 0; i < plan->n_steps; i++)
        prescan(&g, plan->steps[i]);

    int frame_size = ((g.next_slot + 15) / 16) * 16;

    /* ---- Prologue ---- */
    emit_push_rbp(&g);
    emit_mov_rbp_rsp(&g);
    if (frame_size)
        emit_sub_rsp(&g, frame_size);

    /* ---- Save ABI parameter registers to stack slots ---- */
    for (int i = 0; i < plan->n_params && i < MAX_PARAM_REGS; i++) {
        int slot = slot_lookup(&g, plan->param_names[i]);
        emit_store_reg_to_slot(&g, PARAM_REGS[i].reg_code,
                               PARAM_REGS[i].needs_rex_r, slot);
    }

    /* ---- Steps ---- */
    for (int i = 0; i < plan->n_steps; i++)
        emit_step(&g, plan->steps[i]);

    /* ---- Return expression ---- */
    if (plan->return_expr)
        emit_expr(&g, plan->return_expr);
    else
        emit_xor_rax_rax(&g);

    /* ---- Epilogue ---- */
    if (frame_size)
        emit_add_rsp(&g, frame_size);
    emit_pop_rbp(&g);
    emit_ret(&g);

    MachineCode mc;
    mc.code = g.code;
    mc.size = g.code_size;
    return mc;
}
