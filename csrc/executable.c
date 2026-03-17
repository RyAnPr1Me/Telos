/*
 * executable.c — Native code execution via mmap + function pointer.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "telos.h"

NativeFunction telos_make_native(const uint8_t *code, int size, int n_params)
{
    NativeFunction fn;
    fn.mem      = NULL;
    fn.mem_size = 0;
    fn.func_ptr = NULL;
    fn.n_params = n_params;

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "telos_make_native: mmap failed\n");
        return fn;
    }

    memcpy(mem, code, size);

    fn.mem      = mem;
    fn.mem_size = size;
    fn.func_ptr = mem;

    return fn;
}

int64_t telos_call_native(NativeFunction *fn, int64_t *args, int n_args)
{
    (void)n_args;

    switch (fn->n_params) {
    case 0:
        return ((int64_t (*)())fn->func_ptr)();
    case 1:
        return ((int64_t (*)(int64_t))fn->func_ptr)(args[0]);
    case 2:
        return ((int64_t (*)(int64_t, int64_t))fn->func_ptr)(args[0], args[1]);
    case 3:
        return ((int64_t (*)(int64_t, int64_t, int64_t))fn->func_ptr)(
            args[0], args[1], args[2]);
    case 4:
        return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))fn->func_ptr)(
            args[0], args[1], args[2], args[3]);
    case 5:
        return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t,
                             int64_t))fn->func_ptr)(
            args[0], args[1], args[2], args[3], args[4]);
    case 6:
        return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t,
                             int64_t, int64_t))fn->func_ptr)(
            args[0], args[1], args[2], args[3], args[4], args[5]);
    default:
        fprintf(stderr, "telos_call_native: unsupported param count %d\n",
                fn->n_params);
        return 0;
    }
}

void telos_free_native(NativeFunction *fn)
{
    if (fn->mem != NULL && fn->mem != MAP_FAILED) {
        munmap(fn->mem, fn->mem_size);
    }
    fn->mem      = NULL;
    fn->func_ptr = NULL;
}
