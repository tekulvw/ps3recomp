/* Opt-in numeric interpretation of a watched PUT source, immediately before
 * copying it. This is diagnostics only: arbitrary DMA bytes need not be PCM. */
#ifndef SPU_DMA_SOURCE_TRACE_H
#define SPU_DMA_SOURCE_TRACE_H
#include "spu_context.h"
#include "../platform/win32_compat.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef struct mfc_source_trace_config {
    uint64_t lo, hi;
    int enabled;
    volatile LONG ready;
    volatile LONG64 next_ms;
    INIT_ONCE once;
} mfc_source_trace_config;
static mfc_source_trace_config mfc_source_trace = {0,0,0,0,0,INIT_ONCE_STATIC_INIT};

static BOOL WINAPI mfc_source_trace_init(PINIT_ONCE once, PVOID arg, PVOID* out)
{
    const char* flag = getenv("SPU_DMA_SOURCE_F32BE");
    const char* range = getenv("SPU_DMA_RANGE");
    (void)once; (void)arg; (void)out;
    if (flag && !strcmp(flag, "1") && range && *range) {
        char *end, *last;
        errno = 0;
        uint64_t lo = strtoull(range, &end, 16);
        if (!errno && end != range && *end == '-') {
            uint64_t hi = strtoull(end+1, &last, 16);
            if (!errno && last != end+1 && !*last && lo < hi && hi <= 0x100000000ull) {
                mfc_source_trace.lo = lo;
                mfc_source_trace.hi = hi;
                mfc_source_trace.enabled = 1;
            }
        }
    }
    InterlockedExchange(&mfc_source_trace.ready, 1);
    return TRUE;
}

/* Called only for validated PUTs. The core already masks the LSA and rejects
 * LS overruns, sizes above 16 KiB and 32-bit EA wrap; repeat the read bounds here
 * so this observer cannot acquire an unchecked source if reused elsewhere. */
static inline void mfc_trace_source_f32be(spu_context* spu, uint32_t lsa,
                                         uint64_t ea, uint32_t size, uint32_t cmd)
{
    if (!InterlockedCompareExchange(&mfc_source_trace.ready, 0, 0))
        InitOnceExecuteOnce(&mfc_source_trace.once, mfc_source_trace_init, NULL, NULL);
    if (!mfc_source_trace.enabled) return;
    uint64_t start = (uint32_t)ea, end = start + size;
    if (!spu || !spu->ls || !size || size > 16384 ||
        (uint64_t)lsa + size > SPU_LS_SIZE || end > 0x100000000ull ||
        start >= mfc_source_trace.hi || end <= mfc_source_trace.lo) return;
    /* Interpret only whole, transfer-relative four-byte words in the watch.
     * A watch that cuts through a float excludes that partial word. */
    uint32_t first = start < mfc_source_trace.lo ?
        (uint32_t)((mfc_source_trace.lo - start + 3) / 4) : 0;
    uint32_t last = (uint32_t)(((end < mfc_source_trace.hi ? end : mfc_source_trace.hi) - start) / 4);
    if (last <= first) return;
    extern unsigned long long ps3_ms_now(void);
    unsigned long long now = ps3_ms_now();
    LONG64 prior = InterlockedCompareExchange64(&mfc_source_trace.next_ms, 0, 0);
    /* The atomic claim bounds all emitting threads to one scan per five seconds
     * per translation unit. Disabled/outside-range PUTs never read the clock. */
    if (now < (unsigned long long)prior || now > 0x7FFFFFFFFFFFFFFFull - 5000) return;
    if (InterlockedCompareExchange64(&mfc_source_trace.next_ms, (LONG64)(now+5000), prior) != prior) return;
    uint32_t finite = 0, nonzero = 0, nonfinite = 0;
    float peak = 0;
    for (uint32_t i = first; i < last; ++i) {
        const uint8_t* p = spu->ls + lsa + i*4;
        uint32_t bits = ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3];
        uint32_t magnitude = bits & 0x7FFFFFFFu;
        if ((magnitude & 0x7F800000u) == 0x7F800000u) { ++nonfinite; continue; }
        ++finite;
        if (magnitude) ++nonzero; /* Both signed zeros compare as zero. */
        float value;
        memcpy(&value, &magnitude, sizeof value);
        if (value > peak) peak = value;
    }
    fprintf(stderr, "[dma-source-f32be] img=%d pc=0x%05X cmd=0x%X lsa=0x%05X ea=0x%08X size=%u offset=%u words=%u finite=%u nonzero=%u nonfinite=%u peak=%.9g\n",
        spu->image_id, spu->pc & SPU_LS_MASK, cmd, lsa, (uint32_t)ea, size,
        first*4, last-first, finite, nonzero, nonfinite, (double)peak);
}
#endif
