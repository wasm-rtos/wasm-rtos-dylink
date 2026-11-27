/*
 * dylink.0 (WebAssembly/tool-conventions, DynamicLinking.md) on
 * wasm-rtos-vm's public API: a position-independent program (clang -pie)
 * and the shared libraries it needs (clang -shared) are instances in one
 * runtime, sharing one linear memory and one function table, like an ELF
 * process. Calls between them go straight in, so fuel and stops work across
 * modules as within one.
 *
 * Built in two steps, as the interpreter instantiates: dylink_build makes
 * and links every instance and runs none of their code; dylink_start runs
 * their start functions, relocations and constructors, dependencies first.
 * The same program and libraries build the same instances, so a snapshot of
 * the program's instance reads back into a process built again.
 */
#ifndef DYLINK_H
#define DYLINK_H

#include "wm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Dylink Dylink;

/* Finds a library the program needs (a dylink.0 "needed" name), loaded; the process then owns it. */
typedef WmResult (*DylinkFind)(void* context, const char* name, WmModule** out);

typedef struct DylinkOptions
{
    void* context;
    DylinkFind find;
    /*
     * Asked first for every function import of every module: a host
     * function, or out->as.func left NULL for the modules to define it.
     * Other kinds of imports are the convention's.
     */
    WmResolver host;
    void* host_context;
    /* The stack in linear memory, after every module's data; 0 for 64 KiB. */
    uint32_t stack_size;
} DylinkOptions;

/* Whether the module has a dylink.0 section. */
int dylink_is(const WmModule* module);

/*
 * The program and the libraries it needs, instantiated and linked, none of
 * their code run. The program stays the caller's; the libraries belong to
 * the process, which is kept on failure too, so that dylink_free releases
 * it. The options need not outlive the call.
 */
WmResult dylink_build(Dylink** out, WmModule* program, const DylinkOptions* options);
/* Start functions, data relocations and constructors, dependencies first; once. */
WmResult dylink_start(Dylink* process, WmRuntime* runtime);
/* The program's instance. */
WmInstance* dylink_instance(const Dylink* process);
void dylink_free(Dylink* process);

#ifdef __cplusplus
}
#endif

#endif
