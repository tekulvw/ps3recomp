/*
 * ps3recomp - the per-line version counter scheme, as seen by its call sites
 *
 * PS3RECOMP_COH_VERSIONS=1 switches lock-line coherence from the lock-line
 * lock, holder walk and notify (spu_coherency.c) to per-line version counters
 * (spu_coh_version.inc). The call sites -- the SPU atomic and DMA paths, the PPU
 * store and reservation paths, the host line sections -- reach the counter
 * scheme only through g_spu_coh_ops, which spu_coherency.c sets at startup
 * when the switch is on. It stays NULL otherwise, and every site keeps its
 * existing code.
 *
 * One definition per program (weak / selectany), so a target that links the
 * call sites without spu_coherency.c -- the many test fixtures that stub the
 * lock-line lock -- sees NULL and needs no new stubs.
 *
 * Host addresses are passed alongside guest EAs so that the implementation
 * needs no vm_base of its own.
 */

#ifndef SPU_COH_OPS_H
#define SPU_COH_OPS_H

#include <stdint.h>

#ifndef SPU_COH_SHARED
#  if defined(_MSC_VER) || defined(__MINGW32__)
#    define SPU_COH_SHARED __declspec(selectany)
#  else
#    define SPU_COH_SHARED __attribute__((weak))
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spu_coh_ops {
    /* SPU atomics. `spu` is the issuing spu_context. */
    void (*getllar)(void* spu, uint32_t ea, const void* line, void* ls);
    int  (*putllc)(void* spu, uint32_t ea, void* line, const void* ls);      /* 1 = committed */
    void (*putlluc)(void* spu, uint32_t ea, void* line, const void* ls);
    /* Deliver a lost reservation as SPU_EVENT_LR if it has happened. Returns
     * 1 when it raised LR. Called wherever the SPU can observe its events. */
    int  (*settle)(void* spu);
    /* Park/unpark a waiter on the SPU's reserved line around a blocking
     * RdEventStat wait; `storage` is caller-owned spu_cohv_waiter storage
     * (SPU_COH_WAITER_BYTES). wake is called when the line is written. */
    void (*wait_begin)(void* spu, void* storage, void (*wake)(void*));
    void (*wait_end)(void* storage);

    /* A write of n bytes at guest ea (host address dst) from src, or zeros
     * when src is NULL, of any length: each marked line is written under its
     * counter, the rest plainly. `self` is the issuing SPU for an MFC write,
     * whose own reservation it does not cost; NULL otherwise. */
    void (*write)(uint32_t ea, void* dst, const void* src, uint32_t n, const void* self);

    /* PPU reservations. read returns the line's count; *raw receives the
     * naturally aligned 4- or 8-byte value in guest byte order. */
    uint32_t (*load_reserve)(uint32_t ea, const void* p, unsigned size, uint64_t* raw);
    int  (*store_conditional)(uint32_t ea, void* p, unsigned size, uint32_t count,
                              uint64_t expected_raw, uint64_t value_raw);
    /* A plain compare-and-swap on a marked line (store-conditionals of older
     * lifts, which carry no line token). 1 on store. */
    int  (*cas)(uint32_t ea, void* p, unsigned size, uint64_t expected_raw, uint64_t value_raw);

    /* Host read-modify-write sections (spu_coh_section.h). */
    void (*section_begin)(uint32_t ea, uint32_t len);
    void (*section_end)(void);
} spu_coh_ops;

#define SPU_COH_WAITER_BYTES 32

SPU_COH_SHARED const spu_coh_ops* g_spu_coh_ops = 0;

/* Set by ppu_loader.cpp once a store-conditional of an older lift (one that
 * carries no line token) runs under the version scheme: from then on, its
 * writers also break those reservations through ppu_resv_break_line. */
SPU_COH_SHARED int g_spu_cohv_old_ppu_resv = 0;

#ifdef __cplusplus
}
#endif

#endif /* SPU_COH_OPS_H */
