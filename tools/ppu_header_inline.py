#!/usr/bin/env python3
"""Give an already generated PPU source directory the inline guest-access fast paths, header only.

Lifts made before ps3recomp 12158c45 call the out-of-line vm_read*/vm_write* for every guest load and
store. 12158c45's lifter emits inline load fast paths in the ppu_recomp.h preamble; the lifted bodies
call vm_read32(...) either way, so adding the same preamble (and #define vm_readN to its inline form)
to an older header gives the fast path without a relift: function bodies, hooks and splices stay
byte-identical, only ppu_recomp.h changes.

--stores adds the store half the lifter does not emit: vm_write*_inl store directly only when the
runtime allows inline stores (vm_inline_store_ok: no store diagnostic in the environment), no dynamic
store watch is armed (barrier/sync watch, null-page sweep, critical-write watch), the address is in
[0x10000, 0xE0000000) and inside the mapping, is not the GCM put wake word, and neither its first nor
its last byte is on a line the coherence bitmap marks. Everything else -- null page, raw SPU problem
state, the put kick, coherent lines -- takes the out-of-line store unchanged. The value and its byte
order are the out-of-line store's. Needs a runtime that exports vm_inline_store_ok.

A provenance.json in the directory keeps its history and gains a transform record with the
before/after header digests (its outputs entry for ppu_recomp.h is updated).

usage: ppu_header_inline.py SOURCE_DIR OUTPUT_DIR [--stores]   (OUTPUT_DIR must not exist)"""
import argparse, hashlib, json, shutil, sys
from pathlib import Path

LIFTER_LOADS_END = '#define vm_read64(a) vm_read64_inl(a)\n#endif\n'
ANCHOR = 'int ppu_store_conditional64(ppu_context* ctx, uint64_t addr, uint64_t val);\n#ifdef __cplusplus\n}\n#endif\n'
LOADS = r'''
/* Inline guest-load fast paths (ps3recomp 12158c45 lifter preamble, applied to an older header). The
 * runtime sets vm_inline_ok once it knows no memory diagnostic is armed; anything the out-of-line path
 * treats specially -- the null page, the raw-SPU register windows, the gcm ref-poll word, the end of
 * the mapping -- still goes there, so the result is the same, just without the call. */
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t* vm_base;
extern int      vm_inline_ok;
extern uint32_t ppu_vm_size;
extern uint32_t ppu_hle_inject_base;
#ifdef __cplusplus
}
#endif
static inline int vm_inl_ok(uint32_t ea, uint32_t n)
{
    return __builtin_expect(vm_inline_ok, 1) && ea >= 0x10000u && ea < 0xE0000000u &&
           ea != ppu_hle_inject_base + 0x2008u &&
           (ppu_vm_size == 0 || (uint64_t)ea + n <= ppu_vm_size);
}
static inline uint8_t vm_read8_inl(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_inl_ok(ea, 1)) return __atomic_load_n(vm_base + ea, __ATOMIC_RELAXED);
    return vm_read8(a);
}
static inline uint16_t vm_read16_inl(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_inl_ok(ea, 2)) {   /* aligned: single-copy atomic, as on Cell */
        uint16_t v;
        if (!(ea & 1u)) v = __atomic_load_n((const uint16_t*)(vm_base + ea), __ATOMIC_RELAXED);
        else memcpy(&v, vm_base + ea, 2);
        return __builtin_bswap16(v); }
    return vm_read16(a);
}
static inline uint32_t vm_read32_inl(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_inl_ok(ea, 4)) {   /* aligned: single-copy atomic, as on Cell */
        uint32_t v;
        if (!(ea & 3u)) v = __atomic_load_n((const uint32_t*)(vm_base + ea), __ATOMIC_RELAXED);
        else memcpy(&v, vm_base + ea, 4);
        return __builtin_bswap32(v); }
    return vm_read32(a);
}
static inline uint64_t vm_read64_inl(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_inl_ok(ea, 8)) {   /* aligned: single-copy atomic, as on Cell */
        uint64_t v;
        if (!(ea & 7u)) v = __atomic_load_n((const uint64_t*)(vm_base + ea), __ATOMIC_RELAXED);
        else memcpy(&v, vm_base + ea, 8);
        return __builtin_bswap64(v); }
    return vm_read64(a);
}
#ifndef PPU_NO_INLINE_LOADS
#define vm_read8(a)  vm_read8_inl(a)
#define vm_read16(a) vm_read16_inl(a)
#define vm_read32(a) vm_read32_inl(a)
#define vm_read64(a) vm_read64_inl(a)
#endif
'''
STORES = r'''
/* Inline guest-store fast paths (tools/ppu_header_inline.py --stores). A store goes straight to guest
 * memory only when the out-of-line store would do nothing else: no store diagnostic or dynamic store
 * watch, an ordinary address (not the null page, the raw-SPU windows, the mapping end or the GCM put
 * wake word), and no line the coherence bitmap marks. Otherwise the out-of-line store runs. */
#ifdef __cplusplus
extern "C" {
#endif
extern int      vm_inline_store_ok;
extern uint32_t g_barrier_sync_watch;
extern uint32_t g_null_sweep_hi;
extern uint32_t g_gcm_consumer_put_wake_ea;
extern const unsigned char* const g_spu_coh_bitmap;
extern const int* const g_spu_coh_armed;
extern const uint64_t* const g_vm_critical_range;
#ifdef __cplusplus
}
#endif
static inline int vm_inl_store_ok(uint32_t ea, uint32_t n)
{
    if (!__builtin_expect(vm_inline_store_ok, 1) || g_barrier_sync_watch || g_null_sweep_hi ||
        *g_vm_critical_range) return 0;
    if (ea < 0x10000u || ea >= 0xE0000000u || (ppu_vm_size != 0 && (uint64_t)ea + n > ppu_vm_size) ||
        ea == g_gcm_consumer_put_wake_ea) return 0;
    if (*g_spu_coh_armed) {
        const uint32_t l0 = ea >> 7, l1 = (ea + n - 1) >> 7;
        if (((g_spu_coh_bitmap[l0 >> 3] >> (l0 & 7)) | (g_spu_coh_bitmap[l1 >> 3] >> (l1 & 7))) & 1u) return 0;
    }
    return 1;
}
static inline void vm_write8_inl(uint64_t a, uint8_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_inl_store_ok(ea, 1)) { __atomic_store_n(vm_base + ea, v, __ATOMIC_RELAXED); return; }
    vm_write8(a, v);
}
static inline void vm_write16_inl(uint64_t a, uint16_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_inl_store_ok(ea, 2)) { const uint16_t w = __builtin_bswap16(v);
        if (!(ea & 1u)) __atomic_store_n((uint16_t*)(vm_base + ea), w, __ATOMIC_RELAXED); else memcpy(vm_base + ea, &w, 2);
        return; }
    vm_write16(a, v);
}
static inline void vm_write32_inl(uint64_t a, uint32_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_inl_store_ok(ea, 4)) { const uint32_t w = __builtin_bswap32(v);
        if (!(ea & 3u)) __atomic_store_n((uint32_t*)(vm_base + ea), w, __ATOMIC_RELAXED); else memcpy(vm_base + ea, &w, 4);
        return; }
    vm_write32(a, v);
}
static inline void vm_write64_inl(uint64_t a, uint64_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (vm_inl_store_ok(ea, 8)) { const uint64_t w = __builtin_bswap64(v);
        if (!(ea & 7u)) __atomic_store_n((uint64_t*)(vm_base + ea), w, __ATOMIC_RELAXED); else memcpy(vm_base + ea, &w, 8);
        return; }
    vm_write64(a, v);
}
#ifndef PPU_NO_INLINE_STORES
#define vm_write8(a, v)  vm_write8_inl(a, v)
#define vm_write16(a, v) vm_write16_inl(a, v)
#define vm_write32(a, v) vm_write32_inl(a, v)
#define vm_write64(a, v) vm_write64_inl(a, v)
#endif
'''


def digest(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source', type=Path)
    ap.add_argument('output', type=Path)
    ap.add_argument('--stores', action='store_true', help='also inline guest stores (see above)')
    a = ap.parse_args()
    if a.output.exists():
        sys.exit(f'{a.output} exists; use a fresh output directory')
    record = None
    if (a.source / 'provenance.json').is_file():
        record = json.loads((a.source / 'provenance.json').read_text())
        for name, d in record.get('outputs', {}).items():
            if digest(a.source / name) != d:
                sys.exit(f'{name}: source directory does not match its provenance')
    text = (a.source / 'ppu_recomp.h').read_text()
    if 'vm_write32_inl' in text:
        sys.exit('ppu_recomp.h already has inline guest stores')
    if 'vm_read32_inl' in text:
        # A lifter from 12158c45 on emitted the loads already: add only the stores, after them.
        if not a.stores:
            sys.exit('ppu_recomp.h already has inline guest loads; nothing to do without --stores')
        at, add = LIFTER_LOADS_END, STORES
    else:
        at, add = ANCHOR, LOADS + (STORES if a.stores else '')
    if text.count(at) != 1:
        sys.exit('ppu_recomp.h: unexpected shape')
    shutil.copytree(a.source, a.output)
    header = a.output / 'ppu_recomp.h'
    before = digest(a.source / 'ppu_recomp.h')
    header.write_text(text.replace(at, at + add, 1))
    after = digest(header)
    if record is not None:
        record.setdefault('outputs', {})['ppu_recomp.h'] = after
        record['inline_access_header_transform'] = {
            'meaning': 'Inline guest-access fast paths added to ppu_recomp.h after lifting; lifted bodies unchanged',
            'loads_from_lifter_revision': '12158c45650cc5d00d228a62f98c3a204aacc300', 'stores': a.stores,
            'tool_sha256': digest(Path(__file__)), 'header_before_sha256': before, 'header_after_sha256': after}
        (a.output / 'provenance.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'{a.output}: ppu_recomp.h {before[:12]} -> {after[:12]} (loads{" + stores" if a.stores else ""})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
