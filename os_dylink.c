/* The kernel's dylink.0 loader: dylink.c behind OsLoader. */
#include "os_dylink.h"
#include "dylink.h"

#include <string.h>

typedef struct Library
{
    const char* name;
    const uint8_t* bytes;
    uint32_t size;
} Library;

static Library g_libraries[OS_DYLINK_MAX_LIBRARIES];
static uint32_t g_library_count;

static WmResult find(void* context, const char* name, WmModule** out)
{
    (void)context;
    *out = NULL;
    for (uint32_t i = 0; i < g_library_count; ++i)
        if (strcmp(g_libraries[i].name, name) == 0)
            return wm_module_load(out, g_libraries[i].bytes, g_libraries[i].size);
    return "unlinkable: dylink.0 dependency is missing";
}

static WmResult build(void* context, WmModule* program, WmMemory* memory, WmResolver host, void* host_context,
                      void** out_process, WmInstance** out_instance)
{
    DylinkOptions options;
    Dylink* process = NULL;
    WmResult result;
    (void)context;
    if (!dylink_is(program))
        return os_not_mine;
    /* A memory kept apart is not taken yet: loaded tasks do not swap (os.h, Loaders). */
    if (memory != NULL)
        return "unsupported: a dylink.0 program on a kept memory";
    memset(&options, 0, sizeof(options));
    options.find = find;
    options.host = host;
    options.host_context = host_context;
    options.stack_size = OS_DYLINK_STACK_SIZE;
    result = dylink_build(&process, program, &options);
    *out_process = process;
    *out_instance = dylink_instance(process);
    return result;
}

static WmResult start(void* process, WmRuntime* runtime) { return dylink_start((Dylink*)process, runtime); }

static void release(void* process) { dylink_free((Dylink*)process); }

static const OsLoader g_loader = {build, start, release, NULL};

OsStatus os_dylink_add(void) { return os_loader_add(&g_loader); }

OsStatus os_dylink_add_library(const char* name, const uint8_t* bytes, uint32_t size)
{
    if (name == NULL || name[0] == '\0' || bytes == NULL || size == 0U)
        return OS_STATUS_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < g_library_count; ++i)
        if (strcmp(g_libraries[i].name, name) == 0)
            return OS_STATUS_INVALID_ARGUMENT;
    if (g_library_count >= OS_DYLINK_MAX_LIBRARIES)
        return OS_STATUS_LIMIT_REACHED;
    g_libraries[g_library_count].name = name;
    g_libraries[g_library_count].bytes = bytes;
    g_libraries[g_library_count].size = size;
    ++g_library_count;
    return OS_STATUS_OK;
}
