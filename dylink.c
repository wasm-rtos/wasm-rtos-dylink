/*
 * dylink.0 on wm.h. The program and its libraries are instances in one
 * runtime: the program's memory and table (or ones made here, when it
 * imports them) are every library's, and the globals the convention names
 * (__memory_base, __stack_pointer, GOT.mem, GOT.func...) are made here and
 * given through the imports. A function import of another module's code is
 * bound to it with wm_instance_link_function, so calls go straight in.
 */
#include "dylink.h"

#include <string.h>

#define E_MALFORMED "malformed: dylink.0 section"
#define E_MISSING "unlinkable: dylink.0 dependency is missing"
#define E_DUPLICATE "unlinkable: duplicate dylink.0 symbol"
#define E_UNRESOLVED "unlinkable: unresolved dylink.0 symbol"
#define E_TYPE "unlinkable: dylink.0 symbol type mismatch"
#define E_UNSUPPORTED "unsupported: dylink.0 feature"
#define E_MEMORY "exhaustion: out of memory"
#define E_MEMORY_SIZE "exhaustion: dylink.0 memory too large"

#define NONE 0xFFFFFFFFu
#define DEFAULT_STACK_SIZE (64u * 1024u)

enum
{
    MEM_INFO = 1,
    NEEDED = 2,
    EXPORT_INFO = 3,
    IMPORT_INFO = 4
};

enum
{
    BINDING_WEAK = 1,
    BINDING_LOCAL = 2,
    VISIBILITY_HIDDEN = 4,
    TLS = 0x100
};

/* How a function import is bound: not yet, to another module's function, to the host's. */
enum
{
    UNBOUND = 0,
    BOUND = 1,
    HOST = 2
};

typedef struct Info
{
    char* module;
    char* name;
    uint32_t flags;
} Info;

typedef struct Metadata
{
    uint32_t memory_size;
    uint32_t memory_align;
    uint32_t table_size;
    uint32_t table_align;
    char** needed;
    uint32_t needed_count;
    Info* exports;
    uint32_t export_count;
    Info* imports;
    uint32_t import_count;
} Metadata;

/* A module of the program: where its data and table entries go, and how its function imports are bound. */
typedef struct Object
{
    char* name;
    WmModule* module;
    WmInstance* instance;
    Metadata meta;
    uint32_t memory_base;
    uint32_t table_base;
    uint8_t* bound;
    uint32_t func_imports;
    uint8_t is_main;
    uint8_t visit;
} Object;

/* A GOT entry, filled once every module is in: a function's table index or a data address. */
typedef struct Got
{
    Object* object;
    WmGlobal* global;
    const char* name;
    uint32_t name_length;
    uint8_t func;
} Got;

struct Dylink
{
    const DylinkOptions* options;
    Object* objects;
    uint32_t count;
    uint32_t capacity;
    /* Dependencies before the modules that need them, for starting. */
    uint32_t* order;
    uint32_t order_count;
    WmMemory* memory;
    uint8_t owns_memory;
    WmTable* table;
    uint8_t owns_table;
    /* Every global made here, freed with the process. */
    WmGlobal** globals;
    uint32_t global_count;
    uint32_t global_capacity;
    Got* got;
    uint32_t got_count;
    uint32_t got_capacity;
    WmGlobal* stack_pointer;
    WmGlobal* stack_low;
    WmGlobal* stack_high;
    uint8_t stack_low_mutable;
    uint8_t stack_high_mutable;
    uint32_t stack_low_value;
    uint32_t stack_high_value;
    uint32_t heap_base;
    uint32_t heap_end;
    uint8_t started;
    /* The module being instantiated, for the resolver. */
    Object* current;
};

typedef Dylink State;

static void* allocate(size_t size)
{
    void* p = wm_allocate(NULL, size ? size : 1u);
    if (p)
        memset(p, 0, size);
    return p;
}

static void release(void* p) { wm_allocate(p, 0); }

static int is(const char* name, uint32_t length, const char* text)
{
    return name && length == strlen(text) && !memcmp(name, text, length);
}

/* ---- The dylink.0 section ---- */

typedef struct Reader
{
    const uint8_t* p;
    const uint8_t* end;
} Reader;

static int read_u32(Reader* r, uint32_t* out)
{
    uint32_t value = 0;
    uint32_t shift = 0;
    uint8_t byte;
    do
    {
        if (r->p >= r->end || shift > 28u)
            return 0;
        byte = *r->p++;
        value |= (uint32_t)(byte & 0x7Fu) << shift;
        shift += 7u;
    } while (byte & 0x80u);
    *out = value;
    return 1;
}

static char* copy_name(const char* name, uint32_t length)
{
    char* copy = (char*)allocate((size_t)length + 1u);
    if (copy)
        memcpy(copy, name, length);
    return copy;
}

static int read_string(Reader* r, char** out)
{
    uint32_t length;
    if (!read_u32(r, &length) || length > (uint32_t)(r->end - r->p))
        return 0;
    *out = copy_name((const char*)r->p, length);
    r->p += length;
    return *out != NULL;
}

static void free_infos(Info* infos, uint32_t count)
{
    for (uint32_t i = 0; infos && i < count; ++i)
    {
        release(infos[i].module);
        release(infos[i].name);
    }
    release(infos);
}

static void free_metadata(Metadata* m)
{
    for (uint32_t i = 0; m->needed && i < m->needed_count; ++i)
        release(m->needed[i]);
    release(m->needed);
    free_infos(m->exports, m->export_count);
    free_infos(m->imports, m->import_count);
    memset(m, 0, sizeof(*m));
}

static int read_infos(Reader* r, int has_module, Info** out, uint32_t* count)
{
    if (!read_u32(r, count) || *count > (uint32_t)(r->end - r->p))
        return 0;
    *out = (Info*)allocate(((size_t)*count + 1u) * sizeof(Info));
    if (!*out)
        return 0;
    for (uint32_t i = 0; i < *count; ++i)
        if ((has_module && !read_string(r, &(*out)[i].module)) || !read_string(r, &(*out)[i].name) ||
            !read_u32(r, &(*out)[i].flags))
            return 0;
    return 1;
}

static WmResult parse_section(Metadata* m, Reader* r)
{
    uint32_t seen = 0;
    while (r->p < r->end)
    {
        uint32_t type, size;
        Reader sub;
        int ok = 1;
        if (!read_u32(r, &type) || !read_u32(r, &size) || size > (uint32_t)(r->end - r->p) ||
            (type < 32u && (seen & (1u << type))))
            return E_MALFORMED;
        if (type < 32u)
            seen |= 1u << type;
        sub.p = r->p;
        sub.end = r->p + size;
        r->p = sub.end;
        switch (type)
        {
            case MEM_INFO:
                ok = read_u32(&sub, &m->memory_size) && read_u32(&sub, &m->memory_align) &&
                     read_u32(&sub, &m->table_size) && read_u32(&sub, &m->table_align);
                break;
            case NEEDED:
                ok = read_u32(&sub, &m->needed_count) && m->needed_count <= (uint32_t)(sub.end - sub.p) &&
                     (m->needed = (char**)allocate(((size_t)m->needed_count + 1u) * sizeof(char*))) != NULL;
                for (uint32_t i = 0; ok && i < m->needed_count; ++i)
                    ok = read_string(&sub, &m->needed[i]);
                break;
            case EXPORT_INFO: ok = read_infos(&sub, 0, &m->exports, &m->export_count); break;
            case IMPORT_INFO: ok = read_infos(&sub, 1, &m->imports, &m->import_count); break;
            /* Runtime paths and the rest mean nothing here. */
            default: sub.p = sub.end;
        }
        if (!ok || sub.p != sub.end)
            return E_MALFORMED;
    }
    return seen & (1u << MEM_INFO) ? NULL : E_MALFORMED;
}

/* The dylink.0 custom section's contents (it comes first, but any place is accepted). */
static int find_section(const WmModule* module, Reader* out)
{
    const uint8_t* bytes;
    uint32_t size;
    if (!wm_module_custom_sections(module, "dylink.0", 8u, 0, &bytes, &size))
        return 0;
    out->p = bytes;
    out->end = bytes + size;
    return 1;
}

int dylink_is(const WmModule* module)
{
    Reader r;
    return module && find_section(module, &r);
}

/* ---- Symbols ---- */

static uint32_t info_flags(const Info* infos, uint32_t count, const char* module, uint32_t module_length,
                           const char* name, uint32_t name_length)
{
    for (uint32_t i = 0; i < count; ++i)
        if ((!module || (infos[i].module && is(module, module_length, infos[i].module))) &&
            is(name, name_length, infos[i].name))
            return infos[i].flags;
    return 0;
}

static uint32_t import_flags(const Object* o, const WmExternType* imp)
{
    return info_flags(o->meta.imports, o->meta.import_count, imp->module, imp->module_length, imp->name,
                      imp->name_length);
}

/* An export that is a function import of its module nothing has bound: no definition of its own. */
static int unbound_export(const Object* o, const WmExternType* x)
{
    return x->kind == WM_EXTERN_FUNC && x->index < o->func_imports && o->bound[x->index] == UNBOUND;
}

/*
 * The definition of a symbol among the modules in the process: a strong one
 * wins over weak ones, two strong ones are an error. Local and hidden
 * exports are not seen from other modules. type is the export's (its
 * global's type and mutability).
 */
static WmResult find_symbol(State* s, uint8_t kind, const char* name, uint32_t length, WmExtern* found,
                            Object** owner, WmExternType* type)
{
    int found_weak = 0;
    found->kind = kind;
    found->as.func = NULL;
    *owner = NULL;
    for (uint32_t oi = 0; oi < s->count; ++oi)
    {
        Object* o = &s->objects[oi];
        WmExternType x;
        uint32_t flags;
        if (!o->instance)
            continue;
        flags = info_flags(o->meta.exports, o->meta.export_count, NULL, 0, name, length);
        if (flags & (BINDING_LOCAL | VISIBILITY_HIDDEN))
            continue;
        for (uint32_t e = 0; e < wm_module_exports(o->module, e, &x); ++e)
        {
            WmExtern candidate;
            int weak = (flags & BINDING_WEAK) != 0;
            if (x.kind != kind || x.name_length != length || memcmp(x.name, name, length) || unbound_export(o, &x) ||
                wm_instance_export(o->instance, x.name, x.name_length, &candidate))
                continue;
            if (found->as.func && !found_weak && !weak &&
                (kind == WM_EXTERN_FUNC ? !wm_func_equal(found->as.func, candidate.as.func)
                                        : found->as.global != candidate.as.global))
                return E_DUPLICATE;
            if (!found->as.func || (found_weak && !weak))
            {
                *found = candidate;
                *owner = o;
                *type = x;
                found_weak = weak;
            }
        }
    }
    return NULL;
}

static WmGlobal* new_global(State* s, uint8_t type, uint8_t mutable_, uint64_t value)
{
    WmGlobal* g;
    if (s->global_count == s->global_capacity)
    {
        uint32_t capacity = s->global_capacity ? s->global_capacity * 2u : 16u;
        WmGlobal** globals = (WmGlobal**)wm_allocate(s->globals, capacity * sizeof(WmGlobal*));
        if (!globals)
            return NULL;
        s->globals = globals;
        s->global_capacity = capacity;
    }
    g = wm_global_new(type, mutable_, value);
    if (g)
        s->globals[s->global_count++] = g;
    return g;
}

/* The import of this kind and names, of the module being instantiated. */
static int find_import(const WmModule* m, uint8_t kind, const char* module, uint32_t module_length, const char* name,
                       uint32_t name_length, WmExternType* out)
{
    for (uint32_t i = 0; i < wm_module_imports(m, i, out); ++i)
        if (out->kind == kind && out->module_length == module_length && out->name_length == name_length &&
            !memcmp(out->module, module, module_length) && !memcmp(out->name, name, name_length))
            return 1;
    return 0;
}

/* A global shared by the modules that import it, made like the first import. */
static WmResult shared_global(State* s, WmGlobal** shared, uint8_t* shared_mutable, const WmExternType* t,
                              uint32_t value, WmGlobal** out)
{
    if (!*shared)
    {
        *shared = new_global(s, t->type, t->mutable_, value);
        *shared_mutable = t->mutable_;
    }
    if (!*shared)
        return E_MEMORY;
    if (wm_global_type(*shared) != t->type || *shared_mutable != t->mutable_)
        return E_TYPE;
    *out = *shared;
    return NULL;
}

static WmResult global_import(State* s, const WmExternType* t, WmGlobal** out)
{
    Object* o = s->current;
    const char* name = t->name;
    uint32_t n = t->name_length;
    int env = is(t->module, t->module_length, "env");
    int got = is(t->module, t->module_length, "GOT.func") || is(t->module, t->module_length, "GOT.mem");
    static const char* const special[] = {"__memory_base", "__table_base", "__stack_pointer", "__stack_low",
                                          "__stack_high",  "__heap_base",  "__heap_end"};
    int is_special = got;
    for (size_t i = 0; env && i < sizeof(special) / sizeof(special[0]); ++i)
        is_special |= is(name, n, special[i]);
    if (is_special && t->type != WM_I32)
        return E_TYPE;
    *out = NULL;
    if (env && is(name, n, "__memory_base"))
        *out = new_global(s, t->type, t->mutable_, o->memory_base);
    else if (env && is(name, n, "__table_base"))
        *out = new_global(s, t->type, t->mutable_, o->table_base);
    else if (env && is(name, n, "__heap_base"))
        *out = new_global(s, t->type, t->mutable_, s->heap_base);
    else if (env && is(name, n, "__heap_end"))
        *out = new_global(s, t->type, t->mutable_, s->heap_end);
    else if (env && is(name, n, "__stack_pointer"))
    {
        if (!t->mutable_)
            return E_TYPE;
        *out = s->stack_pointer;
    }
    else if (env && is(name, n, "__stack_low"))
        return shared_global(s, &s->stack_low, &s->stack_low_mutable, t, s->stack_low_value, out);
    else if (env && is(name, n, "__stack_high"))
        return shared_global(s, &s->stack_high, &s->stack_high_mutable, t, s->stack_high_value, out);
    else if (got)
    {
        if (s->got_count == s->got_capacity)
        {
            uint32_t capacity = s->got_capacity ? s->got_capacity * 2u : 16u;
            Got* entries = (Got*)wm_allocate(s->got, capacity * sizeof(Got));
            if (!entries)
                return E_MEMORY;
            s->got = entries;
            s->got_capacity = capacity;
        }
        *out = new_global(s, t->type, t->mutable_, 0);
        if (*out)
        {
            Got* g = &s->got[s->got_count++];
            g->object = o;
            g->global = *out;
            g->name = name;
            g->name_length = n;
            g->func = is(t->module, t->module_length, "GOT.func");
        }
    }
    else
    {
        /* A global another module exports: that module must be in already. */
        WmExtern found;
        Object* owner;
        WmExternType x;
        WmResult result = find_symbol(s, WM_EXTERN_GLOBAL, name, n, &found, &owner, &x);
        if (result)
            return result;
        if (!found.as.global)
        {
            if (!(import_flags(o, t) & BINDING_WEAK))
                return E_UNRESOLVED;
            *out = new_global(s, t->type, t->mutable_, 0);
        }
        else if (x.type != t->type || x.mutable_ != t->mutable_)
            return E_TYPE;
        else
            *out = found.as.global;
    }
    return *out ? NULL : E_MEMORY;
}

/* Imports while a module is instantiated: host functions now, other modules' functions after, the rest now. */
static WmResult resolve(void* context, const char* module, uint32_t module_length, const char* name,
                        uint32_t name_length, uint8_t kind, WmExtern* out)
{
    State* s = (State*)context;
    WmExternType t;
    out->kind = kind;
    switch (kind)
    {
        case WM_EXTERN_FUNC:
        {
            WmResult result = NULL;
            out->as.func = NULL;
            if (s->options && s->options->host)
                result = s->options->host(s->options->host_context, module, module_length, name, name_length, kind,
                                          out);
            if (!result && out->as.func &&
                find_import(s->current->module, WM_EXTERN_FUNC, module, module_length, name, name_length, &t))
                s->current->bound[t.index] = HOST;
            return result;
        }
        case WM_EXTERN_MEMORY:
            if (!is(module, module_length, "env") || !is(name, name_length, "memory") || !s->memory)
                return E_UNSUPPORTED;
            out->as.memory = s->memory;
            return NULL;
        case WM_EXTERN_TABLE:
            if (!is(module, module_length, "env") || !is(name, name_length, "__indirect_function_table") ||
                !s->table)
                return E_UNSUPPORTED;
            out->as.table = s->table;
            return NULL;
        default:
            if (!find_import(s->current->module, WM_EXTERN_GLOBAL, module, module_length, name, name_length, &t))
                return E_UNRESOLVED;
            return global_import(s, &t, &out->as.global);
    }
}

/* ---- Loading ---- */

static int find_dependency(State* s, const char* name)
{
    for (uint32_t i = 0; i < s->count; ++i)
        if (!s->objects[i].is_main && strcmp(s->objects[i].name, name) == 0)
            return (int)i;
    return -1;
}

static WmResult add_object(State* s, const char* name, WmModule* module, int is_main, uint32_t* index)
{
    Object* o;
    Reader section;
    WmExternType t;
    if (!name || !module)
        return E_MISSING;
    if (!find_section(module, &section))
        return "unlinkable: module has no dylink.0 section";
    for (uint32_t i = 0; i < s->count; ++i)
        if (s->objects[i].module == module)
            return E_DUPLICATE;
    if (s->count == s->capacity)
    {
        uint32_t capacity = s->capacity ? s->capacity * 2u : 4u;
        Object* objects = (Object*)wm_allocate(s->objects, capacity * sizeof(Object));
        if (!objects)
            return E_MEMORY;
        s->objects = objects;
        s->capacity = capacity;
    }
    o = &s->objects[s->count];
    memset(o, 0, sizeof(*o));
    o->module = module;
    o->is_main = (uint8_t)is_main;
    /* From here on the object, with a library's module, is the process's to free. */
    *index = s->count++;
    for (uint32_t i = 0; i < wm_module_imports(module, i, &t); ++i)
        o->func_imports += t.kind == WM_EXTERN_FUNC;
    if (!(o->name = copy_name(name, (uint32_t)strlen(name))) ||
        !(o->bound = (uint8_t*)allocate((size_t)o->func_imports + 1u)))
        return E_MEMORY;
    return parse_section(&o->meta, &section);
}

/* Every library the program needs, through the embedder's finder. A cycle is fine: symbols come later. */
static WmResult collect(State* s, uint32_t index)
{
    if (s->objects[index].visit)
        return NULL;
    s->objects[index].visit = 1;
    for (uint32_t i = 0; i < s->objects[index].meta.needed_count; ++i)
    {
        const char* needed = s->objects[index].meta.needed[i];
        int found = find_dependency(s, needed);
        uint32_t dependency;
        WmResult result;
        if (found < 0)
        {
            WmModule* module = NULL;
            if (!s->options || !s->options->find)
                return E_MISSING;
            if ((result = s->options->find(s->options->context, needed, &module)))
                return result;
            if (!module)
                return E_MISSING;
            if ((result = add_object(s, needed, module, 0, &dependency)))
            {
                /* Not the process's yet unless it was added. */
                if (s->count == 0 || s->objects[s->count - 1u].module != module)
                    wm_module_free(module);
                return result;
            }
        }
        else
            dependency = (uint32_t)found;
        if ((result = collect(s, dependency)))
            return result;
    }
    return NULL;
}

/* Dependencies before the modules that need them. */
static void order_visit(State* s, uint32_t index)
{
    if (s->objects[index].visit)
        return;
    s->objects[index].visit = 1;
    for (uint32_t i = 0; i < s->objects[index].meta.needed_count; ++i)
    {
        int dependency = find_dependency(s, s->objects[index].meta.needed[i]);
        if (dependency >= 0)
            order_visit(s, (uint32_t)dependency);
    }
    s->order[s->order_count++] = index;
}

static int place(uint32_t* cursor, uint32_t align_log2, uint32_t size, uint32_t* base)
{
    uint32_t mask;
    if (align_log2 >= 32u)
        return 0;
    mask = align_log2 ? (1u << align_log2) - 1u : 0;
    if (*cursor > UINT32_MAX - mask)
        return 0;
    *base = (*cursor + mask) & ~mask;
    if (size > UINT32_MAX - *base)
        return 0;
    *cursor = *base + size;
    return 1;
}

/* The program may define the memory and the table; the libraries import them from env. */
static WmResult check_object(const Object* o, const WmExternType* memory, const WmExternType* table,
                             uint32_t table_count)
{
    const Metadata* d = &o->meta;
    if (memory && (memory->module ? !is(memory->module, memory->module_length, "env") ||
                                        !is(memory->name, memory->name_length, "memory")
                                  : !o->is_main))
        return E_UNSUPPORTED;
    if (table && (table->module ? !is(table->module, table->module_length, "env") ||
                                      !is(table->name, table->name_length, "__indirect_function_table")
                                : !o->is_main))
        return E_UNSUPPORTED;
    if (table_count > 1u || d->memory_align >= 32u || d->table_align >= 32u)
        return E_UNSUPPORTED;
    for (uint32_t i = 0; i < d->export_count; ++i)
        if (d->exports[i].flags & TLS)
            return E_UNSUPPORTED;
    for (uint32_t i = 0; i < d->import_count; ++i)
        if (d->imports[i].flags & TLS)
            return E_UNSUPPORTED;
    return NULL;
}

static int table_index(const State* s, const WmFunc* f)
{
    for (uint32_t i = 1; i < wm_table_size(s->table); ++i)
        if (wm_func_equal(wm_table_get(s->table, i), f))
            return (int)i;
    return -1;
}

/* Function imports of other modules' code: deps first, so that a re-exported import is bound when it is found. */
static WmResult link_functions(State* s)
{
    for (uint32_t k = 0; k < s->order_count; ++k)
    {
        Object* o = &s->objects[s->order[k]];
        WmExternType imp;
        for (uint32_t i = 0; i < wm_module_imports(o->module, i, &imp); ++i)
        {
            WmExtern found;
            Object* owner;
            WmExternType x;
            WmResult result;
            if (imp.kind != WM_EXTERN_FUNC || o->bound[imp.index] != UNBOUND)
                continue;
            if ((result = find_symbol(s, WM_EXTERN_FUNC, imp.name, imp.name_length, &found, &owner, &x)))
                return result;
            if (!found.as.func)
                continue;
            if (wm_instance_link_function(o->instance, imp.index, found.as.func))
                return E_TYPE;
            o->bound[imp.index] = BOUND;
        }
    }
    return NULL;
}

static WmResult check_imports(State* s)
{
    for (uint32_t oi = 0; oi < s->count; ++oi)
    {
        Object* o = &s->objects[oi];
        WmExternType imp;
        for (uint32_t i = 0; i < wm_module_imports(o->module, i, &imp); ++i)
            if (imp.kind == WM_EXTERN_FUNC && o->bound[imp.index] == UNBOUND && !(import_flags(o, &imp) & BINDING_WEAK))
                return E_UNRESOLVED;
    }
    return NULL;
}

/* Every exported function of a module's own gets a table entry, unless it has one, so that GOT.func can name it. */
static WmResult fill_table(State* s, uint32_t cursor)
{
    for (uint32_t oi = 0; oi < s->count; ++oi)
    {
        Object* o = &s->objects[oi];
        WmExternType x;
        for (uint32_t e = 0; e < wm_module_exports(o->module, e, &x); ++e)
        {
            WmExtern f;
            if (x.kind != WM_EXTERN_FUNC || (x.index < o->func_imports && o->bound[x.index] != BOUND) ||
                wm_instance_export(o->instance, x.name, x.name_length, &f) || table_index(s, f.as.func) >= 0)
                continue;
            if (cursor >= wm_table_size(s->table) || wm_table_set(s->table, cursor++, f.as.func))
                return E_UNSUPPORTED;
        }
    }
    return NULL;
}

static WmResult fill_got(State* s)
{
    for (uint32_t i = 0; i < s->got_count; ++i)
    {
        Got* g = &s->got[i];
        WmExtern found;
        Object* owner;
        WmExternType x;
        WmResult result = find_symbol(s, g->func ? WM_EXTERN_FUNC : WM_EXTERN_GLOBAL, g->name, g->name_length,
                                      &found, &owner, &x);
        uint64_t value = 0;
        if (result)
            return result;
        if (g->func && found.as.func && table_index(s, found.as.func) >= 0)
            value = (uint32_t)table_index(s, found.as.func);
        else if (!g->func && found.as.global && owner)
        {
            uint32_t relative;
            if (wm_global_type(found.as.global) != WM_I32)
                return E_TYPE;
            relative = (uint32_t)wm_global_get(found.as.global);
            if (relative > UINT32_MAX - owner->memory_base)
                return E_UNSUPPORTED;
            value = relative + owner->memory_base;
        }
        else
        {
            const Metadata* d = &g->object->meta;
            uint32_t flags = info_flags(d->imports, d->import_count, g->func ? "GOT.func" : "GOT.mem",
                                        g->func ? 8u : 7u, g->name, g->name_length);
            if (!(flags & BINDING_WEAK))
                return E_UNRESOLVED;
        }
        wm_global_set(g->global, value);
    }
    return NULL;
}

/* An initializer the module may export: no parameters, no results. */
static WmResult call_optional(WmRuntime* runtime, WmInstance* inst, const char* name, int* found)
{
    WmExtern f;
    const uint8_t *params, *results;
    uint32_t param_count, result_count;
    *found = wm_instance_export(inst, name, (uint32_t)strlen(name), &f) == NULL && f.kind == WM_EXTERN_FUNC;
    if (!*found)
        return NULL;
    wm_func_type(f.as.func, &params, &param_count, &results, &result_count);
    if (param_count || result_count)
        return E_TYPE;
    return wm_call(runtime, f.as.func, NULL, NULL);
}

WmResult dylink_start(Dylink* s, WmRuntime* runtime)
{
    WmResult result;
    int found;
    if (s->started)
        return NULL;
    s->started = 1;
    for (uint32_t i = 0; i < s->order_count; ++i)
        if ((result = wm_instance_start(s->objects[s->order[i]].instance, runtime)))
            return result;
    for (uint32_t i = 0; i < s->order_count; ++i)
        if ((result = call_optional(runtime, s->objects[s->order[i]].instance, "__wasm_apply_data_relocs", &found)))
            return result;
    for (uint32_t i = 0; i < s->order_count; ++i)
    {
        WmInstance* inst = s->objects[s->order[i]].instance;
        if ((result = call_optional(runtime, inst, "_initialize", &found)))
            return result;
        if (!found && (result = call_optional(runtime, inst, "__wasm_call_ctors", &found)))
            return result;
    }
    return NULL;
}

/* The program's own table: it must export it, for the libraries to be given it. */
static WmTable* exported_table(const Object* o)
{
    WmExternType x;
    WmExtern t;
    for (uint32_t e = 0; e < wm_module_exports(o->module, e, &x); ++e)
        if (x.kind == WM_EXTERN_TABLE && x.index == 0 && !wm_instance_export(o->instance, x.name, x.name_length, &t))
            return t.as.table;
    return NULL;
}

/* Whether the module imports its memory. */
static int imports_memory(const WmModule* module)
{
    WmExternType t;
    return wm_module_memories(module, 0, &t) && t.module;
}

static WmResult build(State* s, WmModule* program)
{
    WmMemory* kept = s->options ? s->options->memory : NULL;
    uint32_t main_index;
    uint32_t memory_cursor = 0, table_cursor = 1;
    uint32_t min_pages = 0, max_pages = NONE, min_table = 1, max_table = NONE;
    uint32_t stack_size = s->options && s->options->stack_size ? s->options->stack_size : DEFAULT_STACK_SIZE;
    uint32_t export_count = 0, first_export, table_size, pages;
    uint64_t required;
    int page_log2 = -1;
    WmExternType program_memory, program_table;
    int program_has_memory, program_has_table;
    WmResult result;
    Object* main;

    if ((result = add_object(s, "main", program, 1, &main_index)) || (result = collect(s, main_index)))
        return result;
    if (!(s->order = (uint32_t*)allocate(s->count * sizeof(uint32_t))))
        return E_MEMORY;
    for (uint32_t i = 0; i < s->count; ++i)
        s->objects[i].visit = 0;
    order_visit(s, main_index);

    /* Data, then table entries, one module after another; the linear stack after all the data, then the heap. */
    for (uint32_t i = 0; i < s->count; ++i)
    {
        Object* o = &s->objects[i];
        WmExternType memory, table, x;
        uint32_t memories = wm_module_memories(o->module, 0, &memory);
        uint32_t tables = wm_module_tables(o->module, 0, &table);
        if ((result = check_object(o, memories ? &memory : NULL, tables ? &table : NULL, tables)))
            return result;
        if (!place(&memory_cursor, o->meta.memory_align, o->meta.memory_size, &o->memory_base) ||
            !place(&table_cursor, o->meta.table_align, o->meta.table_size, &o->table_base))
            return E_UNSUPPORTED;
        if (memories)
        {
            if (page_log2 >= 0 && page_log2 != memory.page_log2)
                return E_UNSUPPORTED;
            page_log2 = memory.page_log2;
            if (memory.min > min_pages)
                min_pages = memory.min;
            if (memory.has_max && memory.max < max_pages)
                max_pages = memory.max;
        }
        if (tables)
        {
            if (table.min > min_table)
                min_table = table.min;
            if (table.has_max && table.max < max_table)
                max_table = table.max;
        }
        for (uint32_t e = 0; e < wm_module_exports(o->module, e, &x); ++e)
            export_count += x.kind == WM_EXTERN_FUNC;
    }
    if (page_log2 < 0)
        page_log2 = 16;
    if (!place(&memory_cursor, 4, stack_size, &s->stack_low_value) || !place(&memory_cursor, 4, 0, &s->stack_high_value))
        return E_UNSUPPORTED;
    s->heap_base = memory_cursor;
    required = memory_cursor;
    if ((uint64_t)min_pages << page_log2 > required)
        required = (uint64_t)min_pages << page_log2;
    if (((required + (1u << page_log2) - 1u) >> page_log2) > (max_pages == NONE ? UINT32_MAX : max_pages) ||
        ((required + (1u << page_log2) - 1u) >> page_log2) << page_log2 > UINT32_MAX)
        return E_MEMORY_SIZE;
    pages = (uint32_t)((required + (1u << page_log2) - 1u) >> page_log2);
    s->heap_end = (uint32_t)((uint64_t)pages << page_log2);
    if (export_count > UINT32_MAX - table_cursor)
        return E_UNSUPPORTED;
    first_export = table_cursor;
    table_size = table_cursor + export_count > min_table ? table_cursor + export_count : min_table;
    if (table_size > max_table)
        return E_UNSUPPORTED;
    if (!(s->stack_pointer = new_global(s, WM_I32, 1, s->stack_high_value)))
        return E_MEMORY;

    /* The program first: its memory and table, grown to fit everything, are the libraries'. */
    program_has_memory = wm_module_memories(program, 0, &program_memory) != 0;
    program_has_table = wm_module_tables(program, 0, &program_table) != 0;
    /* Back from swap: the memory kept apart is the program's own, with its bytes. */
    if (kept && (!program_has_memory || program_memory.module))
        return E_UNSUPPORTED;
    if (program_has_memory && program_memory.module)
    {
        if (!(s->memory = wm_memory_new_paged(pages, max_pages, (uint8_t)page_log2)))
            return E_MEMORY;
        s->owns_memory = 1;
    }
    if (program_has_table && program_table.module)
    {
        if (!(s->table = wm_table_new(WM_FUNCREF, table_size, max_table)))
            return E_MEMORY;
        s->owns_table = 1;
    }
    main = &s->objects[main_index];
    s->current = main;
    if ((result = kept ? wm_instantiate_with_memory(&main->instance, program, kept, resolve, s)
                       : wm_instantiate(&main->instance, program, NULL, resolve, s)))
        return result;
    if (kept)
    {
        /* Its start functions, relocations and constructors ran before it was kept: never again. */
        s->memory = kept;
        s->started = 1;
    }
    else if (!s->memory && (s->memory = wm_instance_memory(main->instance)) != NULL)
    {
        uint32_t size;
        (void)wm_memory_data(s->memory, &size);
        if ((size >> page_log2) < pages && wm_memory_grow(s->memory, pages - (size >> page_log2)) == NONE)
            return E_MEMORY_SIZE;
    }
    if (!s->table && program_has_table)
    {
        if (!(s->table = exported_table(main)))
            return E_UNSUPPORTED;
        if (wm_table_size(s->table) < table_size &&
            wm_table_grow(s->table, table_size - wm_table_size(s->table)) == NONE)
            return E_UNSUPPORTED;
    }
    if (!s->table)
    {
        if (!(s->table = wm_table_new(WM_FUNCREF, table_size, max_table)))
            return E_MEMORY;
        s->owns_table = 1;
    }
    for (uint32_t i = 0; i < s->order_count; ++i)
    {
        Object* o = &s->objects[s->order[i]];
        if (o->is_main)
            continue;
        s->current = o;
        /* On a kept memory a library that imports it is bound to it, its data not written again. */
        if ((result = kept && imports_memory(o->module) ? wm_instantiate_with_memory(&o->instance, o->module, kept, resolve, s)
                                                       : wm_instantiate(&o->instance, o->module, NULL, resolve, s)))
            return result;
    }

    if ((result = link_functions(s)) || (result = check_imports(s)) || (result = fill_table(s, first_export)) ||
        (result = fill_got(s)))
        return result;
    return NULL;
}

WmResult dylink_build(Dylink** out, WmModule* program, const DylinkOptions* options)
{
    State* s;
    WmResult result;
    *out = NULL;
    if (!program)
        return E_MISSING;
    if (!(s = (State*)allocate(sizeof(State))))
        return E_MEMORY;
    s->options = options;
    *out = s;
    result = build(s, program);
    /* The options need not outlive the call. */
    s->options = NULL;
    return result;
}

WmInstance* dylink_instance(const Dylink* s)
{
    for (uint32_t i = 0; s && i < s->count; ++i)
        if (s->objects[i].is_main)
            return s->objects[i].instance;
    return NULL;
}

void dylink_free(Dylink* s)
{
    if (!s)
        return;
    /* Instances first: they may hold the shared memory and table. */
    for (uint32_t i = 0; i < s->count; ++i)
        wm_instance_free(s->objects[i].instance);
    for (uint32_t i = 0; i < s->global_count; ++i)
        wm_global_free(s->globals[i]);
    if (s->owns_memory)
        wm_memory_free(s->memory);
    if (s->owns_table)
        wm_table_free(s->table);
    for (uint32_t i = 0; i < s->count; ++i)
    {
        Object* o = &s->objects[i];
        free_metadata(&o->meta);
        release(o->name);
        release(o->bound);
        if (!o->is_main)
            wm_module_free(o->module);
    }
    release(s->objects);
    release(s->order);
    release(s->globals);
    release(s->got);
    release(s);
}
