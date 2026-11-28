/*
 * prx_loader.h — runtime loader for statically-recompiled PS3 PRX modules.
 *
 * A relocatable PRX (libsre, libresc, ...) is brought into a static recompile
 * in three artifacts produced offline:
 *
 *   1. tools/prx_relocate.py  -> a LINKED image: the PRX segments flattened to
 *      a base-0 blob with every PT_SCE_PPURELA relocation pre-applied at a
 *      fixed load base B (value = B + addend). The image bytes are final.
 *   2. tools/ppu_lifter.py --base B --symbol-prefix P_  -> host C functions for
 *      the module's code, with B-based guest addresses and P_-namespaced
 *      symbols (P_func_*, P_function_table[]) so they link beside the title.
 *   3. The namespaced P_function_table[] (a func_entry array) maps each guest
 *      address (B + offset) to its lifted host function.
 *
 * At runtime this loader does the two things that turn those artifacts into a
 * live module:
 *
 *   A. Copies the relocated image into guest RAM at B, so the module's data,
 *      function descriptors (OPDs) and pointer tables are visible to both the
 *      lifted code (vm_read*) and the host indirect-call dispatcher (OPD walk).
 *   B. Registers every lifted function at its guest address in the host
 *      dispatch table, so guest bctrl/branch through a B-region address lands
 *      in the right host function.
 *
 * The loader is intentionally decoupled from any one title: the host supplies
 * the dispatch registrar (a function pointer) and the guest-memory base. This
 * mirrors the existing cross-repo seam (the runtime owns mechanism; the game
 * owns its dispatch table).
 */
#ifndef PS3RECOMP_PRX_LOADER_H
#define PS3RECOMP_PRX_LOADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Binary-compatible with the func_entry the PPU lifter emits, so a caller can
 * pass (const prx_func_entry*)libsre_function_table directly. */
typedef struct {
    uint64_t    addr;            /* guest address = load base + func offset */
    void      (*func)(void* ctx);/* lifted host function (takes ppu_context*) */
    const char* name;            /* symbol name (for diagnostics) */
} prx_func_entry;

/* Legacy function export: NID and base-0 OPD vaddr. The loader rebases the
 * vaddr; publish variable storage through the typed loaded-library API below.
 * This two-word structure is kept ABI-compatible with existing integrations. */
typedef struct {
    uint32_t nid;          /* import/export hash */
    uint32_t vaddr;        /* base-0 function OPD vaddr */
} prx_export;

typedef struct {
    const char*           name;        /* "libsre", "libresc", ... */
    uint32_t              base;        /* guest load base B (must match lift) */
    const uint8_t*        image;       /* relocated linked image bytes */
    uint32_t              image_size;  /* image length = PRX memsz */
    const prx_func_entry* funcs;       /* namespaced function_table[] */
    uint64_t              func_count;  /* function_table_count */
    const prx_export*     exports;     /* NID -> base-0 vaddr (from elf_parser) */
    uint32_t              export_count;
} prx_module;

/* Host-provided dispatch registrar: bind a guest address to a host function.
 * For flOw this is dispatch_register_external. */
typedef void (*prx_register_fn)(uint32_t guest_addr, void (*host)(void*));

/* Result of a load. */
typedef struct {
    int      ok;                 /* 1 on success, 0 on error */
    uint32_t funcs_registered;   /* how many entries were registered */
    uint32_t base;               /* echo of the load base */
    uint32_t image_size;         /* echo of the image size */
} prx_load_result;

/*
 * Load one relocated PRX module into the live process.
 *
 *   m   — the module description (image bytes + namespaced function table).
 *   reg — the host dispatch registrar (NULL skips registration, e.g. for a
 *         data-only image or a dry run).
 *
 * Copies m->image into guest RAM at m->base (via the runtime's vm_base) and
 * registers every function. Returns a result struct; on failure ok==0 and the
 * reason is logged to stderr. Safe to call once per module at startup.
 */
prx_load_result prx_load_module(const prx_module* m, prx_register_fn reg);

/*
 * Convenience: read a linked image (.bin from prx_relocate.py) off disk into a
 * malloc'd buffer. Returns the buffer (caller frees) and writes the size to
 * *out_size, or NULL on error. Useful for tests and file-backed integration;
 * production builds may instead embed the image as a byte array.
 */
uint8_t* prx_image_load_file(const char* path, uint32_t* out_size);

/*
 * Export registry — the seam that lets a title's NID-import resolver call into
 * a loaded PRX. prx_load_module records every export as NID -> (base + vaddr).
 * A title's import dispatcher calls prx_resolve_export(nid); on a non-zero
 * result it points CTR at that guest address and invokes the normal indirect
 * dispatcher (OPD walk -> registered host function), instead of falling back to
 * its HLE stub. This is what turns flOw's cellSpurs imports into real libsre.
 *
 * Returns the guest address (load base + export vaddr) for nid, or 0 if no
 * loaded module exports it.
 */
uint32_t prx_resolve_export(uint32_t nid);

#define PRX_LOADED_EXPORT_LIMIT 65536u
#define PRX_LOADED_LIBRARY_LIMIT 4096u
#define PRX_LOADED_LIBRARY_NAME_MAX 128u

/* An already-loaded guest library: functions name OPDs; variables name data.
 * The batch owns no image and never copies or executes guest code. */
typedef enum {
    PRX_EXPORT_FUNCTION = 0,
    PRX_EXPORT_VARIABLE = 1
} prx_export_kind;
typedef struct {
    uint32_t nid;
    uint32_t guest_addr;
    uint32_t kind;
} prx_loaded_export;
/* Registration is idempotent for the identical owner/name/symbol batch.
 * A conflicting owner/name, duplicate (kind,NID), or invalid address fails
 * without changing the registry. guest_size is explicit, in (0, 2^32].
 * All public operations synchronize with registration/unregistration.
 * Legacy prx_resolve_export is function-only; typed lookup includes globals. */
typedef enum {
    PRX_REG_OK = 0,
    PRX_REG_INVALID,
    PRX_REG_CAPACITY,
    PRX_REG_NO_MEMORY,
    PRX_REG_CONFLICT,
    PRX_REG_NOT_FOUND
} prx_registry_status;

/* owner is a stable nonzero identity (a guest descriptor address is suitable).
 * guest_size is the actual address-space bound, in 1..2^32. All names/entries
 * are copied. Invalid/conflicting/allocation-failed batches publish nothing.
 * Repeating the identical owner/name/batch is idempotent; changing an owner or
 * registering a second owner of the same library name is an explicit conflict.
 * Publication, removal, lookup and counts are synchronized. */
prx_registry_status prx_register_loaded_library(uint64_t owner, const char* name,
    const prx_loaded_export* exports, uint32_t count, uint64_t guest_size);
prx_registry_status prx_unregister_loaded_library(uint64_t owner);
uint32_t prx_resolve_library_export(const char* library, uint32_t nid,
                                  prx_export_kind kind);

/* Number of exports currently registered (diagnostics/tests). */
uint32_t prx_export_registry_count(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_PRX_LOADER_H */
