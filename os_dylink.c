/* The kernel's dylink.0 loader: dylink.c behind OsLoader. */
#include "os_dylink.h"
#include "dylink.h"

#include <stdlib.h>
#include <string.h>

#if OS_HAS_FS
#include "fs.h"
#endif

typedef struct Library
{
    const char* name;
    const uint8_t* bytes;
    uint32_t size;
} Library;

static Library g_libraries[OS_DYLINK_MAX_LIBRARIES];
static uint32_t g_library_count;

static const char missing[] = "unlinkable: dylink.0 dependency is missing";

#if OS_HAS_FS
/* A library file open while processes are linked with it (decision 1), shared by its contents. */
typedef struct File
{
    OsFsImage image;
    uint32_t references;
} File;

typedef char files_fit_a_mask[OS_DYLINK_MAX_FILES <= 32U ? 1 : -1];

static File g_files[OS_DYLINK_MAX_FILES];
static const char* g_directory;
#endif

typedef struct Process Process;
/* Set by os_dylink_set_directory: a build that never calls it carries none of the files' code (kernel decision 3). */
static WmResult (*g_find_file)(Process* process, const char* name, WmModule** out);
static void (*g_release_files)(Process* process);

/* The loader's process: the linked instances, and the library files they run from. */
struct Process
{
    Dylink* dylink;
    uint32_t files;
};

#if OS_HAS_FS
static void file_release(uint32_t index)
{
    if (g_files[index].references > 0U && --g_files[index].references == 0U)
        os_fs_image_close(&g_files[index].image);
}

/* Not "", ".", ".." nor a path: a name stays inside the directory. */
static int is_file_name(const char* name)
{
    return name[0] != '\0' && strchr(name, '/') == NULL && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

/* The directory's file of that name: an open one of the same contents, or opened now. */
static WmResult find_file(Process* process, const char* name, WmModule** out)
{
    char path[OS_FS_PATH_MAX];
    size_t directory = strlen(g_directory), length = strlen(name);
    int slash = directory == 0U || g_directory[directory - 1U] != '/';
    OsFsStat stat;
    uint32_t index = OS_DYLINK_MAX_FILES, free_index = OS_DYLINK_MAX_FILES;
    WmResult result;

    if (!is_file_name(name) || directory + (size_t)slash + length >= sizeof(path))
        return missing;
    memcpy(path, g_directory, directory);
    if (slash)
        path[directory] = '/';
    memcpy(path + directory + (size_t)slash, name, length + 1U);
    if (os_fs_stat(path, &stat) != OS_STATUS_OK || stat.type != OS_FS_TYPE_FILE)
        return missing;

    for (uint32_t i = 0; i < OS_DYLINK_MAX_FILES && index == OS_DYLINK_MAX_FILES; ++i)
    {
        if (g_files[i].references == 0U)
        {
            if (free_index == OS_DYLINK_MAX_FILES)
                free_index = i;
        }
        else if (g_files[i].image.size == stat.size &&
                 os_fs_file_matches(path, g_files[i].image.bytes, g_files[i].image.size))
            index = i;
    }
    if (index == OS_DYLINK_MAX_FILES)
    {
        if (free_index == OS_DYLINK_MAX_FILES)
            return "unlinkable: too many dylink.0 library files open";
        if (os_fs_image_open(path, &g_files[free_index].image) != OS_STATUS_OK)
            return missing;
        index = free_index;
    }
    ++g_files[index].references;
    result = wm_module_load(out, g_files[index].image.bytes, g_files[index].image.size);
    if (result != NULL || (process->files & (1UL << index)) != 0U)
        file_release(index); /* Failed, or the process holds this file already. */
    else
        process->files |= 1UL << index;
    return result;
}

static void release_files(Process* process)
{
    for (uint32_t i = 0; i < OS_DYLINK_MAX_FILES; ++i)
        if ((process->files & (1UL << i)) != 0U)
            file_release(i);
}
#endif

static WmResult find(void* context, const char* name, WmModule** out)
{
    *out = NULL;
    for (uint32_t i = 0; i < g_library_count; ++i)
        if (strcmp(g_libraries[i].name, name) == 0)
            return wm_module_load(out, g_libraries[i].bytes, g_libraries[i].size);
    if (g_find_file != NULL)
        return g_find_file((Process*)context, name, out);
    return missing;
}

static WmResult build(void* context, WmModule* program, WmMemory* memory, WmResolver host, void* host_context,
                      void** out_process, WmInstance** out_instance)
{
    DylinkOptions options;
    Process* process;
    WmResult result;
    (void)context;
    *out_process = NULL;
    *out_instance = NULL;
    if (!dylink_is(program))
        return os_not_mine;
    process = (Process*)calloc(1U, sizeof(*process));
    if (process == NULL)
        return "exhaustion: out of memory";
    memset(&options, 0, sizeof(options));
    /* Back from swap: built on the program's memory kept apart (kernel decision 17). */
    options.memory = memory;
    options.context = process;
    options.find = find;
    options.host = host;
    options.host_context = host_context;
    options.stack_size = OS_DYLINK_STACK_SIZE;
    result = dylink_build(&process->dylink, program, &options);
    *out_process = process;
    *out_instance = process->dylink != NULL ? dylink_instance(process->dylink) : NULL;
    return result;
}

static WmResult start(void* process, WmRuntime* runtime)
{
    return dylink_start(((Process*)process)->dylink, runtime);
}

/* The instances first, then the files their modules were read from. */
static void release(void* opaque)
{
    Process* process = (Process*)opaque;
    if (process == NULL)
        return;
    dylink_free(process->dylink);
    if (g_release_files != NULL)
        g_release_files(process);
    free(process);
}

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

#if OS_HAS_FS
OsStatus os_dylink_set_directory(const char* path)
{
    if (path != NULL && (path[0] != '/' || strlen(path) >= OS_FS_PATH_MAX))
        return OS_STATUS_INVALID_ARGUMENT;
    g_directory = path;
    /* Files stay held by the processes linked before; they are released as those are freed. */
    g_find_file = path != NULL ? find_file : NULL;
    g_release_files = release_files;
    return OS_STATUS_OK;
}
#endif
