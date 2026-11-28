/*
 * dylink.0 on wm.h alone (dylink.c), then as a loader of the kernel
 * (os_dylink.c): a program built with clang -pie and its library built
 * with -shared, calls both ways, data and function pointers through the
 * shared memory and table, fuel across modules, a process per task, and a
 * linked task saved in the middle of its work and read into another.
 */
#include "dylink.h"
#include "os_dylink.h"
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Binary
{
    uint8_t* bytes;
    uint32_t size;
} Binary;

static int g_failures;
static Binary g_library;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

/* The interpreter's memory in use (wm_set_allocator), to see a library's module parsed or not (decision 2). */
static size_t g_live;

static void* counted(void* pointer, size_t size)
{
    size_t* header = pointer ? (size_t*)pointer - 2 : NULL;
    size_t* moved;
    if (header)
        g_live -= header[0];
    if (size == 0)
    {
        free(header);
        return NULL;
    }
    if (!(moved = (size_t*)realloc(header, size + 2 * sizeof(size_t))))
    {
        if (header)
            g_live += header[0];
        return NULL;
    }
    moved[0] = size;
    g_live += size;
    return moved + 2;
}

/* What the library's parsed module takes. */
static size_t module_bytes(void)
{
    size_t before = g_live, size;
    WmModule* module = NULL;
    if (wm_module_load(&module, g_library.bytes, g_library.size))
        return 0;
    size = g_live - before;
    wm_module_free(module);
    return size;
}

static int load(const char* path, Binary* out)
{
    FILE* file = fopen(path, "rb");
    long size;
    out->bytes = NULL;
    if (!file || fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET))
    {
        if (file)
            fclose(file);
        return 0;
    }
    out->bytes = (uint8_t*)malloc((size_t)size);
    out->size = (uint32_t)size;
    if (!out->bytes || fread(out->bytes, 1, (size_t)size, file) != (size_t)size)
        out->size = 0;
    fclose(file);
    return out->size != 0;
}

/* A state snapshot in RAM. */
static struct
{
    uint8_t bytes[1 << 16];
    uint32_t length, at;
} g_state;

static WmResult state_put(void* context, const void* data, uint32_t size)
{
    (void)context;
    if (g_state.length + size > sizeof(g_state.bytes))
        return "exhaustion: state too large";
    memcpy(g_state.bytes + g_state.length, data, size);
    g_state.length += size;
    return NULL;
}

static WmResult state_get(void* context, void* data, uint32_t size)
{
    (void)context;
    if (g_state.at + size > g_state.length)
        return "malformed: state too short";
    memcpy(data, g_state.bytes + g_state.at, size);
    g_state.at += size;
    return NULL;
}

/* A page store in RAM: slots of one frame each. */
#define SLOTS 128
static uint8_t g_slots[SLOTS][4096];

static OsStatus slot_write(void* context, uint32_t slot, const void* frame)
{
    (void)context;
    memcpy(g_slots[slot], frame, sizeof(g_slots[slot]));
    return OS_STATUS_OK;
}

static OsStatus slot_read(void* context, uint32_t slot, void* frame)
{
    (void)context;
    memcpy(frame, g_slots[slot], sizeof(g_slots[slot]));
    return OS_STATUS_OK;
}

static WmResult find(void* context, const char* name, WmModule** out)
{
    (void)context;
    if (strcmp(name, "library.so"))
        return "unlinkable: no such library";
    return wm_module_load(out, g_library.bytes, g_library.size);
}

static WmFunc* function(WmInstance* instance, const char* name)
{
    WmExtern found;
    return wm_instance_export(instance, name, (uint32_t)strlen(name), &found) ? NULL : found.as.func;
}

static uint32_t call(WmRuntime* runtime, WmInstance* instance, const char* name, uint32_t argument, int* ok)
{
    uint32_t value = UINT32_MAX;
    WmFunc* f = function(instance, name);
    *ok = f && wm_call(runtime, f, &argument, &value) == NULL;
    return value;
}

/* dylink.c alone, on the interpreter. */
static void alone(const Binary* app)
{
    WmRuntime* runtime = wm_runtime_new(64u * 1024u, 1024u);
    WmModule* module = NULL;
    Dylink* process = NULL;
    WmInstance* instance;
    DylinkOptions options = {NULL, find, NULL, NULL, 16u * 1024u, NULL};
    uint32_t value = 0, argument = 5, attempts;
    WmResult result;
    int ok;

    check(!wm_module_load(&module, app->bytes, app->size) && dylink_is(module), "the program has dylink.0");
    check(dylink_build(&process, module, &options) == NULL && (instance = dylink_instance(process)) != NULL,
          "built with its library, none of their code run");
    if (!process || !dylink_instance(process))
        return;
    instance = dylink_instance(process);
    check(dylink_start(process, runtime) == NULL && dylink_start(process, runtime) == NULL,
          "started once; again does nothing");
    wm_set_fuel(runtime, 1);
    result = wm_call(runtime, function(instance, "app_main"), &argument, &value);
    for (attempts = 0; attempts < 2000 && result == wm_fuel_exhausted; ++attempts)
    {
        wm_set_fuel(runtime, 1);
        result = wm_resume(runtime, &value);
    }
    check(result == NULL && value == 42, "fuel of one instruction at a time, across a call into the library");
    wm_disable_fuel(runtime);
    check(call(runtime, instance, "app_name_char", 1, &ok) == 'V' && ok,
          "the library returns a const char pointer through the shared memory");
    check(call(runtime, instance, "app_struct", 0, &ok) == 41 && ok, "the program passes a struct pointer");
    check(call(runtime, instance, "app_callback", 0, &ok) == 17 && ok,
          "the library calls the program's function pointer through the shared table");
    check(call(runtime, instance, "app_counter", 0, &ok) == 2 && ok, "the library keeps its data between calls");
    dylink_free(process);

    /* As the kernel swaps: stopped inside the library, its state kept, the memory kept apart, all else freed. */
    {
        WmRuntime* again = wm_runtime_new(64u * 1024u, 1024u);
        WmMemory* kept;
        value = 0;
        g_state.length = g_state.at = 0;
        process = NULL;
        check(dylink_build(&process, module, &options) == NULL && dylink_start(process, runtime) == NULL,
              "a process built again, for a swap");
        instance = dylink_instance(process);
        wm_set_fuel(runtime, 20000);
        result = wm_call(runtime, function(instance, "app_saved"), NULL, &value);
        check(result == wm_fuel_exhausted && wm_snapshot_write_state(runtime, instance, state_put, NULL) == NULL &&
                  (kept = wm_instance_detach_memory(instance)) != NULL,
              "stopped in the library's work, its state written, the program's memory kept apart");
        dylink_free(process);
        wm_runtime_free(runtime);
        runtime = again;
        options.memory = kept;
        process = NULL;
        check(dylink_build(&process, module, &options) == NULL && wm_instance_memory(dylink_instance(process)) == kept &&
                  wm_snapshot_read(runtime, dylink_instance(process), state_get, NULL) == NULL,
              "built on the kept memory, nothing run, nothing written into it; the state read over it");
        options.memory = NULL;
        wm_disable_fuel(runtime);
        check(wm_resume(runtime, &value) == NULL && value == 2,
              "it goes on and ends with the library's count it had: the library's data was not written again");
        dylink_free(process);
    }
    wm_module_free(module);
    wm_runtime_free(runtime);
}

/* A stream in RAM, for the whole system. */
typedef struct Stream
{
    uint8_t* bytes;
    uint32_t size;
    uint32_t at;
} Stream;

static OsStatus stream_write(void* context, const void* data, uint32_t size)
{
    Stream* s = (Stream*)context;
    uint8_t* more = (uint8_t*)realloc(s->bytes, s->size + size);
    if (!more)
        return OS_STATUS_OUT_OF_MEMORY;
    memcpy(more + s->size, data, size);
    s->bytes = more;
    s->size += size;
    return OS_STATUS_OK;
}

static OsStatus stream_read(void* context, void* data, uint32_t size)
{
    Stream* s = (Stream*)context;
    if (size > s->size - s->at)
        return OS_STATUS_BUFFER_TOO_SMALL;
    memcpy(data, s->bytes + s->at, size);
    s->at += size;
    return OS_STATUS_OK;
}

static const Binary* g_app;

/* The host's module of a task it created, by the task's name. */
static OsStatus find_module(void* context, const char* name, const uint8_t** out_bytes, uint32_t* out_size)
{
    (void)context, (void)name;
    *out_bytes = g_app->bytes;
    *out_size = g_app->size;
    return OS_STATUS_OK;
}

static uint32_t run_to_end(OsTaskHandle task)
{
    OsValue value = {0};
    OsStatus status = OS_STATUS_OK;
    for (uint32_t i = 0; status == OS_STATUS_OK && i < 4000U && task && os_task_get_state(task) != OS_TASK_DEAD; ++i)
        status = os_schedule();
    if (!task || os_task_get_state(task) != OS_TASK_DEAD || os_task_get_return_value(task, &value) != OS_STATUS_OK ||
        value.type != OS_VALUE_TYPE_I32)
        return 0;
    return value.value.i32;
}

static OsTaskHandle task_of(const Binary* app, const char* entry, const char* name)
{
    OsTaskHandle task = NULL;
    if (os_task_create(&task, app->bytes, app->size, entry, name, 64U * 1024U, OS_TASK_PRIORITY_NORMAL) != OS_STATUS_OK)
        return NULL;
    return task;
}

/* What making a task of the program adds to the interpreter's memory. */
static size_t task_cost(const Binary* app, const char* name, OsTaskHandle* out)
{
    size_t before = g_live;
    *out = task_of(app, "app_counter", name);
    return *out ? g_live - before : 0U;
}

/* Decision 2: a library's module is parsed once for every process linked with it, and goes with the last. What the second costs. */
static size_t shared_module(const Binary* app, const char* what)
{
    OsTaskHandle warm, a, b, c;
    size_t library = module_bytes(), base, first, second, again;
    char line[160];
    /* The kernel's own lasting things (host functions) made once, before anything is measured. */
    if ((warm = task_of(app, "app_counter", "warm")) != NULL)
        os_task_delete(warm);
    wm_code_cache_trim(0U);
    base = g_live;
    first = task_cost(app, "first", &a);
    second = task_cost(app, "second", &b);
    snprintf(line, sizeof(line), "%s: the second task takes the first's module (%zu and %zu bytes; the module %zu, more once predecoded)",
             what, first, second, library);
    check(a && b && library > 0U && first >= second + library, line);
    if (a)
        os_task_delete(a);
    check(b && os_task_get_state(b) != OS_TASK_DEAD && run_to_end(b) == 2U,
          "the first freed, the module stays with the second, which runs to its end");
    if (b)
        os_task_delete(b);
    wm_code_cache_trim(0U);
    check(g_live == base, "the last freed, the module goes");
    again = task_cost(app, "again", &c);
    check(c && again == first, "the next task parses it again");
    if (c)
        os_task_delete(c);
    return second;
}

/* What a task linked with a library already parsed costs, by bytes and from files: the same, none parses it. */
static size_t g_second_by_bytes, g_second_mapped, g_second_read;

/* A file system in RAM for decision 1: it maps its files or not, and counts what is opened and read whole. */
typedef struct TestFile
{
    const char* name;
    const uint8_t* bytes;
    uint32_t size;
    uint32_t opens;
} TestFile;

static struct
{
    TestFile files[2];
    int maps;
    uint32_t whole_reads;
} g_fs;

static TestFile* fs_file(const char* path)
{
    for (uint32_t i = 0; i < 2U; ++i)
        if (g_fs.files[i].name && strcmp(g_fs.files[i].name, path) == 0)
            return &g_fs.files[i];
    return NULL;
}

static OsStatus fs_stat(void* context, const char* path, OsFsStat* out)
{
    TestFile* file = fs_file(path);
    (void)context;
    out->type = file ? OS_FS_TYPE_FILE : OS_FS_TYPE_DIRECTORY;
    out->size = file ? file->size : 0U;
    return file || path[0] == '\0' || strcmp(path, "lib") == 0 ? OS_STATUS_OK : OS_STATUS_FILE_NOT_FOUND;
}

static OsStatus fs_open(void* context, const char* path, void** out)
{
    TestFile* file = fs_file(path);
    (void)context;
    if (!file)
        return OS_STATUS_FILE_NOT_FOUND;
    ++file->opens;
    *out = file;
    return OS_STATUS_OK;
}

static OsStatus fs_read(void* context, void* handle, uint64_t offset, void* buffer, uint32_t length, uint32_t* out)
{
    TestFile* file = (TestFile*)handle;
    (void)context;
    *out = offset >= file->size ? 0U : (uint32_t)(file->size - offset < length ? file->size - offset : length);
    memcpy(buffer, file->bytes + offset, *out);
    g_fs.whole_reads += offset == 0U && *out == file->size;
    return OS_STATUS_OK;
}

static void fs_close(void* context, void* handle) { (void)context, (void)handle; }

static OsStatus fs_read_dir(void* context, const char* path, uint32_t index, OsFsDirEntry* out)
{
    (void)context, (void)path, (void)index, (void)out;
    return OS_STATUS_END_OF_DIRECTORY;
}

static const uint8_t* fs_map(void* context, void* handle, uint64_t* out_size)
{
    TestFile* file = (TestFile*)handle;
    (void)context;
    *out_size = file->size;
    return g_fs.maps ? file->bytes : NULL;
}

static const OsFsDriver g_fs_driver = {fs_stat, fs_open, fs_read, fs_close, fs_read_dir, fs_map};

static void with_card(void)
{
    os_init();
    os_dylink_add();
    os_fs_mount("/sd", &g_fs_driver, NULL);
}

/* Decision 1: libraries from a directory of the file system. */
static void directory(const Binary* app)
{
    static const OsPageStore store = {NULL, SLOTS, 8U, slot_write, slot_read, NULL, NULL, NULL, NULL, NULL};
    OsTaskHandle a, b, swapped, patched = NULL;
    uint32_t before;
    uint8_t* changed = (uint8_t*)malloc(g_library.size + 5U);
    uint8_t* renamed = (uint8_t*)malloc(app->size);
    static const uint8_t custom[5] = {0x00, 0x03, 0x01, 'x', 0x00};

    memset(&g_fs, 0, sizeof(g_fs));
    g_fs.files[0] = (TestFile){"lib/library.so", g_library.bytes, g_library.size, 0U};
    g_fs.files[1] = (TestFile){"rary.so", g_library.bytes, g_library.size, 0U};
    with_card();
    check(os_dylink_set_directory("") == OS_STATUS_INVALID_ARGUMENT &&
              os_dylink_set_directory("sd/lib") == OS_STATUS_INVALID_ARGUMENT &&
              os_dylink_set_directory("/sd/lib") == OS_STATUS_OK && g_fs.files[0].opens == 0U,
          "a directory of libraries, absolute; nothing opened when it is set");

    g_fs.maps = 1;
    check(run_to_end(task_of(app, "app_counter", "mapped")) == 2U && g_fs.files[0].opens > 0U && g_fs.whole_reads == 0U,
          "a library from the directory where the file system maps it: run in place, nothing read");

    g_fs.maps = 0;
    a = task_of(app, "app_counter", "read-a");
    b = task_of(app, "app_counter", "read-b");
    check(a && b && g_fs.whole_reads == 1U, "where it cannot map, read into RAM once: two processes share the copy");
    check(run_to_end(a) == 2U && run_to_end(b) == 2U, "both run, each with the library's data of its own");
    os_task_delete(a);
    os_task_delete(b);
    check(run_to_end(task_of(app, "app_counter", "read-c")) == 2U && g_fs.whole_reads == 2U,
          "their processes freed, the copy went: the next one reads the file again");
    g_fs.maps = 1;
    g_second_mapped = shared_module(app, "a file of the directory, mapped");
    g_fs.maps = 0;
    g_second_read = shared_module(app, "a file of the directory, read into RAM");

    check(os_page_store_set(&store) == OS_STATUS_OK, "a page store");
    swapped = task_of(app, "app_saved", "swapped");
    before = g_fs.whole_reads;
    check(swapped && os_schedule() == OS_STATUS_OK && os_task_swap_out(swapped) == OS_STATUS_OK,
          "a task linked with the card's library stopped in its work and swapped out");
    if (changed)
    {
        memcpy(changed, g_library.bytes, g_library.size);
        memcpy(changed + g_library.size, custom, sizeof(custom));
        g_fs.files[0].bytes = changed;
        g_fs.files[0].size = g_library.size + (uint32_t)sizeof(custom);
    }
    check(swapped && os_task_swap_in(swapped) != OS_STATUS_OK && os_task_is_swapped(swapped) &&
              g_fs.whole_reads == before + 1U,
          "the file changed on the card while it was out: refused, the task stays swapped out with its state");
    g_fs.files[0].bytes = g_library.bytes;
    g_fs.files[0].size = g_library.size;
    check(swapped && os_task_swap_in(swapped) == OS_STATUS_OK && g_fs.whole_reads == before + 2U &&
              run_to_end(swapped) == 2U,
          "the same file again: it comes back, its library read again (the copies went with the processes), and ends");

    /* "library.so" made "../rary.so" in the program's dylink.0: /sd/lib/../rary.so is a file, and is not opened. */
    for (uint32_t i = 0; renamed && i + 10U <= app->size; ++i)
        if (memcmp(app->bytes + i, "library.so", 10U) == 0)
        {
            memcpy(renamed, app->bytes, app->size);
            memcpy(renamed + i, "../rary.so", 10U);
            os_task_create(&patched, renamed, app->size, "app_counter", "patched", 64U * 1024U,
                           OS_TASK_PRIORITY_NORMAL);
            break;
        }
    check(renamed && patched == NULL && g_fs.files[1].opens == 0U,
          "a needed name that is a path does not leave the directory");


    /* Changed on the card while a process runs its library: the next one gets a module of the new contents. */
    {
        OsTaskHandle old = task_of(app, "app_counter", "old"), fresh = NULL;
        uint32_t reads = g_fs.whole_reads;
        if (changed)
        {
            g_fs.files[0].bytes = changed;
            g_fs.files[0].size = g_library.size + (uint32_t)sizeof(custom);
            fresh = task_of(app, "app_counter", "new");
        }
        check(old && fresh && g_fs.whole_reads == reads + 1U, "the file changed: the next process reads it, a new module");
        check(old && fresh && run_to_end(old) == 2U && run_to_end(fresh) == 2U,
              "the one before runs on with the module it had, the new one with its own");
        g_fs.files[0].bytes = g_library.bytes;
        g_fs.files[0].size = g_library.size;
        if (old)
            os_task_delete(old);
        if (fresh)
            os_task_delete(fresh);
    }

    check(os_dylink_set_directory("/sd/none") == OS_STATUS_OK && task_of(app, "app_counter", "missing") == NULL,
          "no such file in the directory: the program does not load");
    os_dylink_set_directory(NULL);
    os_shutdown();
    free(changed);
    free(renamed);
}

/* After kernel(): library.so is added by bytes, and taken before the directory's file. */
static void bytes_first(const Binary* app)
{
    with_card();
    g_fs.files[0].opens = 0U;
    os_dylink_set_directory("/sd/lib");
    check(run_to_end(task_of(app, "app_counter", "by-bytes")) == 2U && g_fs.files[0].opens == 0U,
          "a library added by bytes is taken before the directory's file of that name");
    os_dylink_set_directory(NULL);
    os_shutdown();
}

/* As the kernel's loader. */
static void kernel(const Binary* app)
{
    OsTaskHandle first, second, saved, again, missing = NULL;
    uint8_t* image = NULL;
    uint32_t size = 0, written = 0;
    OsStatus status = OS_STATUS_OK;

    check(os_init() == OS_STATUS_OK && os_dylink_add() == OS_STATUS_OK, "the kernel, and the loader joins it");
    check(os_dylink_add_library("library.so", g_library.bytes, g_library.size) == OS_STATUS_OK &&
              os_dylink_add_library("library.so", g_library.bytes, g_library.size) == OS_STATUS_INVALID_ARGUMENT,
          "a library by its name; the same name again is refused");
    g_second_by_bytes = shared_module(app, "added by bytes");

    first = task_of(app, "app_counter", "linked-a");
    second = task_of(app, "app_counter", "linked-b");
    check(first && second, "two tasks of the program");
    for (uint32_t i = 0; status == OS_STATUS_OK && i < 4000U && first && second &&
                         (os_task_get_state(first) != OS_TASK_DEAD || os_task_get_state(second) != OS_TASK_DEAD);
         ++i)
        status = os_schedule();
    check(first && second && os_task_get_run_count(first) > 1U && os_task_get_run_count(second) > 1U,
          "fuel switches between the two linked tasks");
    {
        OsValue a = {0}, b = {0};
        check(first && second && os_task_get_return_value(first, &a) == OS_STATUS_OK &&
                  os_task_get_return_value(second, &b) == OS_STATUS_OK && a.value.i32 == 2U && b.value.i32 == 2U,
              "each has the library's data of its own");
    }
    check(run_to_end(task_of(app, "app_counter", "linked-c")) == 2U, "a later one starts with fresh library data");

    /* Saved in the middle of its work, with the library's counter at 1, and read into another task. */
    saved = task_of(app, "app_saved", "saved");
    check(saved && os_schedule() == OS_STATUS_OK && os_task_get_state(saved) == OS_TASK_READY,
          "a linked task stopped by fuel in its work");
    check(saved && os_task_swap_out(saved) == OS_STATUS_UNSUPPORTED, "it does not swap out yet");
    check(saved && os_task_get_snapshot_size(saved, &size) == OS_STATUS_OK && size > 0U &&
              (image = (uint8_t*)malloc(size)) != NULL &&
              os_task_save_snapshot(saved, image, size, &written) == OS_STATUS_OK && written == size,
          "saved: the program's instance and the library's, one memory, one table");
    if (saved)
        os_task_delete(saved);
    again = task_of(app, "app_saved", "again");
    check(again && image && os_task_load_snapshot(again, image, written) == OS_STATUS_OK,
          "read into a new task its loader built the same way");
    {
        OsTaskHandle fresh = task_of(app, "app_saved", "fresh");
        uint32_t whole = run_to_end(fresh);
        uint32_t result = again ? run_to_end(again) : 0U;
        /* From the middle: fewer slices than a whole run. With the library's memory not read back, 1. */
        check(fresh && again && whole == 2U && result == 2U &&
                  os_task_get_run_count(again) < os_task_get_run_count(fresh),
              "it goes on from the middle, with the library's counter it was saved with");
    }
    free(image);

    {
        /* Processes of one program share its parsed module (kernel decision 9), linked or not. */
        OsTaskHandle a = NULL, b = NULL;
        check(os_task_spawn(&a, app->bytes, app->size, NULL, 0, 0, 0) == OS_STATUS_OK &&
                  os_task_spawn(&b, app->bytes, app->size, NULL, 0, 0, 0) == OS_STATUS_OK && os_get_program_count() == 1U,
              "two processes of the linked program, one program loaded");
        for (uint32_t i = 0; a && b && i < 4000U &&
                             (os_task_get_state(a) != OS_TASK_DEAD || os_task_get_state(b) != OS_TASK_DEAD);
             ++i)
            os_schedule();
        check(a && b && os_task_get_exit_reason(a) == OS_TASK_EXIT_RETURNED &&
                  os_task_get_exit_reason(b) == OS_TASK_EXIT_RETURNED,
              "both run to their end, each with its own library");
    }
    {
        OsTaskHandle cancelled = task_of(app, "app_work_only", "cancelled");
        check(cancelled && os_schedule() == OS_STATUS_OK && os_task_delete(cancelled) == OS_STATUS_OK,
              "a linked task deleted in the middle of a slice");
    }
    os_shutdown();

    /* Deep sleep: the whole system saved with a linked task in the middle of its work, RAM lost, restored. */
    {
        Stream stream = {NULL, 0, 0};
        OsTaskHandle sleeper;
        uint32_t id = 0;
        g_app = app;
        os_init();
        os_dylink_add();
        sleeper = task_of(app, "app_saved", "sleeper");
        if (sleeper)
            id = os_task_get_id(sleeper);
        check(sleeper && os_schedule() == OS_STATUS_OK && os_system_save(stream_write, &stream) == OS_STATUS_OK,
              "the whole system saved with a linked task in its work");
        os_shutdown();
        os_init();
        check(os_dylink_add() == OS_STATUS_OK &&
                  os_system_restore(stream_read, &stream, find_module, NULL, 0U) == OS_STATUS_OK,
              "after the sleep: the kernel, its loader, and the system restored");
        check(run_to_end(os_task_find_by_id(id)) == 2U, "the linked task goes on to its end");
        os_shutdown();
        free(stream.bytes);
    }

    /* Swap: a linked task in the middle of the library's work leaves RAM whole, its frames to the store, and comes back. */
    {
        static const OsPageStore store = {NULL, SLOTS, 8U, slot_write, slot_read, NULL, NULL, NULL, NULL, NULL};
        OsPageStats stats;
        OsTaskHandle fresh, swapped;
        uint32_t whole, result;
        size_t parsed;
        os_init();
        os_dylink_add();
        check(os_page_store_set(&store) == OS_STATUS_OK, "a page store of 128 frames, 8 of them in RAM");
        fresh = task_of(app, "app_saved", "fresh");
        whole = run_to_end(fresh);
        {
            /* What a task costs while no process holds the library's module. */
            OsTaskHandle probe = NULL;
            parsed = task_cost(app, "before", &probe);
            if (probe)
                os_task_delete(probe);
        }
        swapped = task_of(app, "app_saved", "swapped");
        check(swapped && os_schedule() == OS_STATUS_OK && os_task_get_state(swapped) == OS_TASK_READY &&
                  os_task_swap_out(swapped) == OS_STATUS_OK && os_task_is_swapped(swapped),
              "a linked task stopped inside the library swapped out: its state and frames to the store");
        (void)os_page_get_stats(&stats);
        check(stats.stores > 0U, "its frames were written to the store");
        {
            /* Swapped out, its process is freed: it holds no module of the library (decision 2). */
            OsTaskHandle probe = NULL;
            size_t after = task_cost(app, "after", &probe);
            check(probe && after == parsed, "the swapped-out task holds no module: the next task parses it as the first did");
            if (probe)
                os_task_delete(probe);
        }
        result = swapped ? run_to_end(swapped) : 0U;
        check(whole == 2U && result == 2U && !os_task_is_swapped(swapped) &&
                  os_task_get_run_count(swapped) == os_task_get_run_count(fresh),
              "back on its kept memory, it ends as a run that never left: the same count, the same slices");

        /* Swapped out, then the whole system saved for a deep sleep and restored. */
        {
            Stream stream = {NULL, 0, 0};
            OsTaskHandle sleeper = task_of(app, "app_saved", "sleeper");
            uint32_t id = sleeper ? os_task_get_id(sleeper) : 0U;
            g_app = app;
            check(sleeper && os_schedule() == OS_STATUS_OK && os_task_swap_out(sleeper) == OS_STATUS_OK &&
                      os_system_save(stream_write, &stream) == OS_STATUS_OK,
                  "a swapped-out linked task, and the whole system saved");
            os_shutdown();
            os_init();
            os_dylink_add();
            os_page_store_set(&store);
            check(os_system_restore(stream_read, &stream, find_module, NULL, 0U) == OS_STATUS_OK &&
                      run_to_end(os_task_find_by_id(id)) == 2U,
                  "restored after the sleep, it goes on to its end");
            free(stream.bytes);
        }
        os_shutdown();
    }

    /* Without the loader the program is an ordinary one, and its imports are nothing the kernel has. */
    check(os_init() == OS_STATUS_OK &&
              os_task_create(&missing, app->bytes, app->size, "app_counter", "plain", 64U * 1024U,
                             OS_TASK_PRIORITY_NORMAL) != OS_STATUS_OK &&
              missing == NULL,
          "without os_dylink_add the program does not load");
    os_shutdown();
}

int main(void)
{
    Binary app;
    wm_set_allocator(counted);
    if (!load("test/program.wasm", &app) || !load("test/library.so", &g_library))
    {
        printf("FAIL the test's program and library (test/build.sh)\n");
        return 1;
    }
    alone(&app);
    directory(&app);
    kernel(&app);
    bytes_first(&app);
    check(g_second_by_bytes > 0U && g_second_mapped == g_second_by_bytes && g_second_read == g_second_by_bytes,
          "a second task costs the same with its library by bytes or from a file: neither parses it again");
    free(app.bytes);
    free(g_library.bytes);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}
