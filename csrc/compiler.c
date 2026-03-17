/*
 * compiler.c — High-level compiler API and CLI entry point for Telos.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "telos.h"

/* ----------------------------------------------------------------------- */
/*  High-level API                                                         */
/* ----------------------------------------------------------------------- */

CompileResult telos_compile(const char *source)
{
    CompileResult result = {0};

    Program *prog = telos_parse(source);
    if (!prog) return result;

    LiftResult lifted = telos_lift_program(prog);

    result.count = lifted.count;
    result.names = malloc(sizeof(char *) * lifted.count);
    result.codes = malloc(sizeof(MachineCode) * lifted.count);

    for (int i = 0; i < lifted.count; i++) {
        result.names[i] = strdup(lifted.names[i]);

        FunctionPlan *plan = telos_plan_function(lifted.graphs[i]);
        result.codes[i] = telos_gen_x86_64(plan);
        telos_free_function_plan(plan);
    }

    telos_free_lift_result(&lifted);
    telos_free_program(prog);

    return result;
}

RunResult telos_run(const char *source)
{
    RunResult result = {0};

    Program *prog = telos_parse(source);
    if (!prog) return result;

    CompileResult compiled = telos_compile(source);

    result.count = compiled.count;
    result.names = malloc(sizeof(char *) * compiled.count);
    result.funcs = malloc(sizeof(NativeFunction) * compiled.count);

    for (int i = 0; i < compiled.count; i++) {
        result.names[i] = strdup(compiled.names[i]);

        /* Find matching function in the program to get param count */
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

    telos_free_compile_result(&compiled);
    telos_free_program(prog);

    return result;
}

CompileCResult telos_compile_c(const char *source)
{
    CompileCResult result = {0};

    Program *prog = telos_parse(source);
    if (!prog) return result;

    LiftResult lifted = telos_lift_program(prog);

    result.count = lifted.count;
    result.names = malloc(sizeof(char *) * lifted.count);
    result.sources = malloc(sizeof(char *) * lifted.count);

    for (int i = 0; i < lifted.count; i++) {
        result.names[i] = strdup(lifted.names[i]);

        FunctionPlan *plan = telos_plan_function(lifted.graphs[i]);
        result.sources[i] = telos_gen_c(plan);
        telos_free_function_plan(plan);
    }

    telos_free_lift_result(&lifted);
    telos_free_program(prog);

    return result;
}

/* ----------------------------------------------------------------------- */
/*  Free functions                                                         */
/* ----------------------------------------------------------------------- */

void telos_free_compile_result(CompileResult *r)
{
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        free(r->names[i]);
        free(r->codes[i].code);
    }
    free(r->names);
    free(r->codes);
    r->names = NULL;
    r->codes = NULL;
    r->count = 0;
}

void telos_free_run_result(RunResult *r)
{
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        free(r->names[i]);
        telos_free_native(&r->funcs[i]);
    }
    free(r->names);
    free(r->funcs);
    r->names = NULL;
    r->funcs = NULL;
    r->count = 0;
}

void telos_free_compile_c_result(CompileCResult *r)
{
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        free(r->names[i]);
        free(r->sources[i]);
    }
    free(r->names);
    free(r->sources);
    r->names = NULL;
    r->sources = NULL;
    r->count = 0;
}

/* ----------------------------------------------------------------------- */
/*  Hex dump / display                                                     */
/* ----------------------------------------------------------------------- */

static void compile_and_show(const char *source)
{
    CompileResult result = telos_compile(source);

    for (int i = 0; i < result.count; i++) {
        printf("============================================================\n");
        printf("Function: %s  (%d bytes of x86-64 machine code)\n",
               result.names[i], result.codes[i].size);
        printf("============================================================\n");

        const uint8_t *code = result.codes[i].code;
        int size = result.codes[i].size;

        for (int off = 0; off < size; off += 16) {
            printf("  %04x ", off);
            for (int j = 0; j < 16 && off + j < size; j++)
                printf(" %02x", code[off + j]);
            printf("\n");
        }
    }

    telos_free_compile_result(&result);
}

/* ----------------------------------------------------------------------- */
/*  CLI entry point                                                        */
/* ----------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: telos <file.telos>\n");
        return 1;
    }

    FILE *fp = fopen(argv[1], "r");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s'\n", argv[1]);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *source = malloc(len + 1);
    size_t nread = fread(source, 1, len, fp);
    (void)nread;
    source[len] = '\0';
    fclose(fp);

    compile_and_show(source);

    free(source);
    return 0;
}
