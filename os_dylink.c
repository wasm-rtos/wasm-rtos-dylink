/* The kernel's dylink.0 loader: dylink.c behind OsLoader. */
#include "os_dylink.h"
#include "dylink.h"

#include <stdlib.h>
#include <string.h>

#if OS_HAS_FS
#include "fs.h"
#endif

/* A library added by bytes, and its module while processes are linked with it (decision 2). */
typedef struct Library
{
    const char* name;
    const uint8_t* bytes;
    uint32_t size;
    WmModule* module;
    uint32_t users;
} Library;

typedef char libraries_fit_a_mask[OS_DYLINK_MAX_LIBRARIES <= 32U ? 1 : -1];

static Library g_libraries[OS_DYLINK_MAX_LIBRARIES];
static uint32_t g_library_count;

static const char missing[] = "unlinkable: dylink.0 dependency is missing";

#if OS_HAS_FS
/* A library file open while processes are linked with it (decision 1), shared by its contents, with its module (decision 2). */
typedef struct File
{
    OsFsImage image;
    WmModule* module;
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

/* The loader's process: the linked instances, and the libraries and files whose modules they share. */
struct Process
{
    Dylink* dylink;
    uint32_t libraries;
    uint32_t files;
};

#if OS_HAS_FS
static void file_release(uint32_t index)
{
    if (g_files[index].references > 0U && --g_files[index].references == 0U)
    {
        wm_module_free(g_files[index].module);
        g_files[index].module = NULL;
        os_fs_image_close(&g_files[index].image);
    }
}

/* Not "", ".", ".." nor a path: a name stays inside the directory. */
static int is_file_name(const char* name)
{
    return name[0] != '\0' && strchr(name, '/') == NULL && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

/* The directory's file of that name: an open one of the same contents, or opened now; its module lent. */
static WmResult find_file(Process* process, const char* name, WmModule** out)
{
    char path[OS_FS_PATH_MAX];
    size_t directory = strlen(g_directory), length = strlen(name);
    int slash = directory == 0U || g_directory[directory - 1U] != '/';
    OsFsStat stat;
    uint32_t index = OS_DYLINK_MAX_FILES, free_index = OS_DYLINK_MAX_FILES;
    File* file;

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
        file = &g_files[free_index];
        if (os_fs_image_open(path, &file->image) != OS_STATUS_OK)
            return missing;
        {
            WmResult result = wm_module_load(&file->module, file->image.bytes, file->image.size);
            if (result != NULL)
            {
                file->module = NULL;
                os_fs_image_close(&file->image);
                return result;
            }
        }
        index = free_index;
    }
    file = &g_files[index];
    /* The same contents under another name in this process: a module of its own, as two libraries. */
    if ((process->files & (1UL << index)) != 0U)
        return wm_module_load(out, file->image.bytes, file->image.size);
    ++file->references;
    process->files |= 1UL << index;
    *out = file->module;
    return NULL;
}

static void release_files(Process* process)
{
    for (uint32_t i = 0; i < OS_DYLINK_MAX_FILES; ++i)
        if ((process->files & (1UL << i)) != 0U)
            file_release(i);
}
#endif

/* A library added by bytes, its module loaded by the first process and lent to the rest; or the directory's file. */
static WmResult find(void* context, const char* name, WmModule** out)
{
    Process* process = (Process*)context;
    *out = NULL;
    for (uint32_t i = 0; i < g_library_count; ++i)
    {
        Library* library = &g_libraries[i];
        if (strcmp(library->name, name) != 0)
            continue;
        if (library->module == NULL)
        {
            WmResult result = wm_module_load(&library->module, library->bytes, library->size);
            if (result != NULL)
            {
                library->module = NULL;
                return result;
            }
        }
        if ((process->libraries & (1UL << i)) == 0U)
        {
            ++library->users;
            process->libraries |= 1UL << i;
        }
        *out = library->module;
        return NULL;
    }
    if (g_find_file != NULL)
        return g_find_file(process, name, out);
    return missing;
}

/* dylink gives a module back: one lent stays with its library or file until release; one of its own is freed. */
static void give_back(void* context, WmModule* module)
{
    (void)context;
    for (uint32_t i = 0; i < g_library_count; ++i)
        if (g_libraries[i].module == module)
            return;
#if OS_HAS_FS
    for (uint32_t i = 0; i < OS_DYLINK_MAX_FILES; ++i)
        if (g_files[i].references > 0U && g_files[i].module == module)
            return;
#endif
    wm_module_free(module);
}

static void library_release(uint32_t index)
{
    Library* library = &g_libraries[index];
    if (library->users > 0U && --library->users == 0U)
    {
        wm_module_free(library->module);
        library->module = NULL;
    }
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
    options.release = give_back;
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

/* The instances first, then the modules they shared and the files those were read from. */
static void release(void* opaque)
{
    Process* process = (Process*)opaque;
    if (process == NULL)
        return;
    dylink_free(process->dylink);
    for (uint32_t i = 0; i < g_library_count; ++i)
        if ((process->libraries & (1UL << i)) != 0U)
            library_release(i);
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
