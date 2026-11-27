/*
 * dylink.0 programs on the wasm-rtos kernel: a loader (kernel decision 16)
 * that builds a program and the shared libraries it needs into its task.
 * A build that calls os_dylink_add has it; one that does not, none of it.
 */
#ifndef OS_DYLINK_H
#define OS_DYLINK_H

#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Libraries os_dylink_add_library keeps at most. */
#ifndef OS_DYLINK_MAX_LIBRARIES
#define OS_DYLINK_MAX_LIBRARIES 16U
#endif
/* The stack in each process's linear memory, after every module's data. */
#ifndef OS_DYLINK_STACK_SIZE
#define OS_DYLINK_STACK_SIZE (64U * 1024U)
#endif

/* The loader joins the kernel (after os_init: os_shutdown forgets loaders). */
OsStatus os_dylink_add(void);
/*
 * A shared library under the name programs list as needed. The name and the
 * bytes stay the caller's and must stay valid while programs use them; each
 * process parses its own module of them. A name given again is refused.
 */
OsStatus os_dylink_add_library(const char* name, const uint8_t* bytes, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
