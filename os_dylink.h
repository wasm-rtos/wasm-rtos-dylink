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
 * bytes stay the caller's and must stay valid while programs use them; the
 * module of them is parsed once and shared by the processes linked with it
 * (decision 2), freed when the last of them is. A name given again is refused.
 */
OsStatus os_dylink_add_library(const char* name, const uint8_t* bytes, uint32_t size);

#if OS_HAS_FS
/* Library files open at once, shared by the processes linked with them. */
#ifndef OS_DYLINK_MAX_FILES
#define OS_DYLINK_MAX_FILES 8U
#endif
/*
 * The directory of libraries (decision 1): a program, or a library, needing
 * libui.so gets <path>/libui.so, unless a library was added by bytes under
 * that name. The file is opened when a program links it, mapped where the
 * file system can (run in place) and read into RAM otherwise; processes
 * linked with a file of the same contents share it, each holds it until its
 * process is freed (swapped out or ended). A needed name with '/' in it, "."
 * or ".." is not opened. The path, absolute, stays the caller's; NULL forgets
 * it; a new one is for programs linked after.
 */
OsStatus os_dylink_set_directory(const char* path);
#endif

#ifdef __cplusplus
}
#endif

#endif
