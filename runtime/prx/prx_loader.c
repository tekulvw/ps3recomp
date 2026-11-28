/*
 * prx_loader.c — see prx_loader.h for the design.
 *
 * Two steps per module: place the relocated image into guest RAM at its load
 * base, then register every lifted function in the host dispatch table.
 */
#include "prx_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime's guest memory: host_ptr = vm_base + guest_addr. Declared by
 * runtime/memory/vm.h; we only need the symbol, so extern it directly to keep
 * the loader independent of the memory backend's init path. */
extern uint8_t* vm_base;

/* ---- export registry ---------------------------------------------------- *
 * Libraries retain separate ownership and symbol kinds. Dynamic allocation is
 * bounded; a complete validated batch is published under one writer lock. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK s_export_lock = SRWLOCK_INIT;
#define EXPORT_READ_LOCK() AcquireSRWLockShared(&s_export_lock)
#define EXPORT_READ_UNLOCK() ReleaseSRWLockShared(&s_export_lock)
#define EXPORT_WRITE_LOCK() AcquireSRWLockExclusive(&s_export_lock)
#define EXPORT_WRITE_UNLOCK() ReleaseSRWLockExclusive(&s_export_lock)
#else
#include <pthread.h>
static pthread_rwlock_t s_export_lock = PTHREAD_RWLOCK_INITIALIZER;
#define EXPORT_READ_LOCK() pthread_rwlock_rdlock(&s_export_lock)
#define EXPORT_READ_UNLOCK() pthread_rwlock_unlock(&s_export_lock)
#define EXPORT_WRITE_LOCK() pthread_rwlock_wrlock(&s_export_lock)
#define EXPORT_WRITE_UNLOCK() pthread_rwlock_unlock(&s_export_lock)
#endif

typedef struct {
    uint64_t owner;
    char name[PRX_LOADED_LIBRARY_NAME_MAX];
    prx_loaded_export* exports;
    uint32_t count;
} prx_loaded_library;
static prx_loaded_library* s_libraries;
static uint32_t s_library_count, s_library_capacity, s_export_count;

static int export_compare(const void* a, const void* b)
{
    const prx_loaded_export* x = (const prx_loaded_export*)a;
    const prx_loaded_export* y = (const prx_loaded_export*)b;
    if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1;
    return x->nid < y->nid ? -1 : x->nid != y->nid;
}

prx_registry_status prx_register_loaded_library(uint64_t owner, const char* name,
    const prx_loaded_export* exports, uint32_t count, uint64_t guest_size)
{
    if (count > PRX_LOADED_EXPORT_LIMIT) return PRX_REG_CAPACITY;
    if (!owner || !name || !*name || !exports || !count ||
        !guest_size || guest_size > UINT64_C(0x100000000)) return PRX_REG_INVALID;
    size_t name_length = 0;
    while (name_length < PRX_LOADED_LIBRARY_NAME_MAX && name[name_length]) ++name_length;
    if (name_length == PRX_LOADED_LIBRARY_NAME_MAX) return PRX_REG_INVALID;
    prx_loaded_export* copy = (prx_loaded_export*)malloc(count * sizeof(*copy));
    if (!copy) return PRX_REG_NO_MEMORY;
    memcpy(copy, exports, count * sizeof(*copy));
    for (uint32_t i = 0; i < count; ++i) {
        const prx_loaded_export* e = &copy[i];
        uint32_t width = e->kind == PRX_EXPORT_FUNCTION ? 8 : 1;
        if (!e->nid || !e->guest_addr || e->kind > PRX_EXPORT_VARIABLE ||
            (e->kind == PRX_EXPORT_FUNCTION && (e->guest_addr & 3)) ||
            e->guest_addr >= guest_size || width > guest_size - e->guest_addr) {
            free(copy); return PRX_REG_INVALID;
        }
    }
    qsort(copy, count, sizeof(*copy), export_compare);
    for (uint32_t i = 1; i < count; ++i) {
        if (!export_compare(&copy[i-1], &copy[i])) { free(copy); return PRX_REG_INVALID; }
    }
    EXPORT_WRITE_LOCK();
    for (uint32_t i = 0; i < s_library_count; ++i) {
        const prx_loaded_library* lib = &s_libraries[i];
        if (lib->owner == owner || !strcmp(lib->name, name)) {
            int same = lib->owner == owner && !strcmp(lib->name, name) && lib->count == count &&
                       !memcmp(lib->exports, copy, count * sizeof(*copy));
            EXPORT_WRITE_UNLOCK(); free(copy);
            return same ? PRX_REG_OK : PRX_REG_CONFLICT;
        }
    }
    if (count > PRX_LOADED_EXPORT_LIMIT - s_export_count ||
        s_library_count == PRX_LOADED_LIBRARY_LIMIT) {
        EXPORT_WRITE_UNLOCK(); free(copy); return PRX_REG_CAPACITY;
    }
    if (s_library_count == s_library_capacity) {
        uint32_t capacity = s_library_capacity ? s_library_capacity * 2 : 8;
        if (capacity > PRX_LOADED_LIBRARY_LIMIT) capacity = PRX_LOADED_LIBRARY_LIMIT;
        prx_loaded_library* grown = (prx_loaded_library*)realloc(s_libraries, capacity * sizeof(*grown));
        if (!grown) { EXPORT_WRITE_UNLOCK(); free(copy); return PRX_REG_NO_MEMORY; }
        s_libraries = grown; s_library_capacity = capacity;
    }
    prx_loaded_library* lib = &s_libraries[s_library_count++];
    lib->owner = owner; memcpy(lib->name, name, name_length + 1);
    lib->exports = copy; lib->count = count; s_export_count += count;
    EXPORT_WRITE_UNLOCK();
    return PRX_REG_OK;
}

prx_registry_status prx_unregister_loaded_library(uint64_t owner)
{
    EXPORT_WRITE_LOCK();
    for (uint32_t i = 0; i < s_library_count; ++i) {
        if (s_libraries[i].owner != owner) continue;
        free(s_libraries[i].exports); s_export_count -= s_libraries[i].count;
        memmove(s_libraries + i, s_libraries + i + 1,
                (s_library_count - i - 1) * sizeof(*s_libraries));
        --s_library_count;
        EXPORT_WRITE_UNLOCK(); return PRX_REG_OK;
    }
    EXPORT_WRITE_UNLOCK(); return PRX_REG_NOT_FOUND;
}

static uint32_t library_find(const prx_loaded_library* lib, uint32_t nid, uint32_t kind)
{
    const prx_loaded_export wanted = {nid, 0, kind};
    const prx_loaded_export* found = (const prx_loaded_export*)bsearch(&wanted,
        lib->exports, lib->count, sizeof(wanted), export_compare);
    return found ? found->guest_addr : 0;
}

uint32_t prx_resolve_library_export(const char* library, uint32_t nid, prx_export_kind kind)
{
    uint32_t result = 0;
    if (!library || kind > PRX_EXPORT_VARIABLE) return 0;
    EXPORT_READ_LOCK();
    for (uint32_t i = s_library_count; i; --i) {
        if (!strcmp(s_libraries[i-1].name, library)) {
            result = library_find(&s_libraries[i-1], nid, kind); break;
        }
    }
    EXPORT_READ_UNLOCK(); return result;
}

uint32_t prx_resolve_export(uint32_t nid)
{
    uint32_t result = 0;
    EXPORT_READ_LOCK();
    for (uint32_t i = s_library_count; i && !result; --i)
        result = library_find(&s_libraries[i-1], nid, PRX_EXPORT_FUNCTION);
    EXPORT_READ_UNLOCK(); return result;
}

uint32_t prx_export_registry_count(void)
{
    EXPORT_READ_LOCK(); uint32_t result = s_export_count;
    EXPORT_READ_UNLOCK(); return result;
}

prx_load_result prx_load_module(const prx_module* m, prx_register_fn reg)
{
    prx_load_result r;
    memset(&r, 0, sizeof(r));

    if (!m || !m->image || m->image_size == 0) {
        fprintf(stderr, "[prx] load failed: null/empty module\n");
        return r;
    }
    if (!vm_base) {
        fprintf(stderr, "[prx] load failed: vm_base not initialized "
                        "(call vm_init before loading PRX modules)\n");
        return r;
    }

    if ((uint64_t)m->base + m->image_size > UINT64_C(0x100000000) ||
        (m->func_count && !m->funcs)) {
        fprintf(stderr, "[prx] load range/function table invalid\n"); return r;
    }
    r.base       = m->base;
    r.image_size = m->image_size;

    /* Step A — place the relocated image into guest RAM at the load base.
     * The bytes already carry base-applied addresses (prx_relocate.py), so the
     * module's data, OPDs and pointer tables are immediately consistent with
     * the lifted code and the OPD-walking dispatcher. */
    memcpy(vm_base + m->base, m->image, m->image_size);

    /* Step B — register every lifted function at its guest address. The lift
     * baked the load base in (--base B), so e->addr is already B + offset and
     * needs no adjustment here. A null host function (rare: a table hole) is
     * skipped rather than registered. */
    if (reg) {
        for (uint64_t i = 0; i < m->func_count; i++) {
            const prx_func_entry* e = &m->funcs[i];
            if (!e->func)
                continue;
            reg((uint32_t)e->addr, e->func);
            r.funcs_registered++;
        }
    }

    /* Legacy prx_module exports are function OPDs. Use the typed loaded-library
     * API for variable exports; never dispatch a variable as executable code. */
    uint32_t exports_published = 0;
    if (m->export_count > PRX_LOADED_EXPORT_LIMIT || (m->export_count && !m->exports)) {
        fprintf(stderr, "[prx] invalid export table\n"); return r;
    }
    if (m->export_count) {
        prx_loaded_export* entries = (prx_loaded_export*)malloc(m->export_count * sizeof(*entries));
        if (!entries) return r;
        for (uint32_t i = 0; i < m->export_count; ++i) {
            const prx_export* e = &m->exports[i];
            if (!e->nid) continue;
            if ((uint64_t)e->vaddr + 8 > m->image_size ||
                (uint64_t)m->base + e->vaddr > UINT32_MAX) {
                free(entries); fprintf(stderr, "[prx] export outside module image\n"); return r;
            }
            entries[exports_published++] = (prx_loaded_export){e->nid, m->base + e->vaddr, PRX_EXPORT_FUNCTION};
        }
        prx_registry_status status = exports_published ? prx_register_loaded_library(
            UINT64_C(0x8000000000000000) | m->base, m->name ? m->name : "anonymous-prx",
            entries, exports_published, UINT64_C(0x100000000)) : PRX_REG_OK;
        free(entries);
        if (status != PRX_REG_OK) {
            fprintf(stderr, "[prx] export batch rejected: %u\n", (unsigned)status); return r;
        }
    }

    r.ok = 1;
    fprintf(stderr,
            "[prx] loaded %-12s base=0x%08X size=%u (0x%X) funcs=%u/%llu exports=%u\n",
            m->name ? m->name : "?", m->base, m->image_size, m->image_size,
            r.funcs_registered, (unsigned long long)m->func_count,
            exports_published);
    return r;
}

uint8_t* prx_image_load_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[prx] cannot open image '%s'\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "[prx] short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_size)
        *out_size = (uint32_t)n;
    return buf;
}
