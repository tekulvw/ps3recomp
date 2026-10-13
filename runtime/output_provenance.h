/* Opt-in output-range provenance. No guest state or payload bytes are changed.
 * Header-local budgets cover each emitting translation unit. */
#ifndef PS3_OUTPUT_PROVENANCE_H
#define PS3_OUTPUT_PROVENANCE_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "platform/win32_compat.h"

typedef struct ps3_output_trace_config {
    uint32_t lo, target;
    uint64_t hi;
    int enabled;
    volatile LONG ready, count[3];
    INIT_ONCE once;
} ps3_output_trace_config;
static ps3_output_trace_config ps3_output_trace = {0,0,0,0,0,{0,0,0},INIT_ONCE_STATIC_INIT};

static BOOL WINAPI ps3_output_trace_init(PINIT_ONCE once, PVOID arg, PVOID* out)
{
    char *end, *last;
    const char* env = getenv("PS3_OUTPUT_PROVENANCE");
    (void)once; (void)arg; (void)out;
    if (env && *env) {
        unsigned long long lo = strtoull(env, &end, 16);
        if (end != env && *end == '-') {
            unsigned long long hi = strtoull(end+1, &last, 16);
            if (last != end+1 && !*last && lo < hi && hi <= 0x100000000ull) {
                ps3_output_trace.lo = (uint32_t)lo;
                ps3_output_trace.hi = hi;
                ps3_output_trace.enabled = 1;
            }
        }
    }
    env = getenv("PS3_OUTPUT_PROVENANCE_TARGET");
    if (env && *env) {
        unsigned long long target = strtoull(env, &end, 16);
        if (end != env && !*end && target <= 0xFFFFFFFFull)
            ps3_output_trace.target = (uint32_t)target;
    }
    InterlockedExchange(&ps3_output_trace.ready, 1);
    return TRUE;
}

static inline int ps3_output_trace_enabled(void)
{
    if (!InterlockedCompareExchange(&ps3_output_trace.ready, 0, 0))
        InitOnceExecuteOnce(&ps3_output_trace.once, ps3_output_trace_init, NULL, NULL);
    return ps3_output_trace.enabled;
}

static inline int ps3_output_trace_claim(unsigned kind, uint32_t ea, uint64_t size)
{
    uint64_t end;
    LONG prior;
    if (!ps3_output_trace_enabled() || !size || size > 0x100000000ull - ea) return 0;
    end = (uint64_t)ea + size;
    if (ea >= ps3_output_trace.hi || end <= ps3_output_trace.lo) return 0;
    /* Atomic saturating cap: concurrency cannot exceed64 or wrap the budget. */
    prior = InterlockedCompareExchange(&ps3_output_trace.count[kind], 0, 0);
    while (prior < 64) {
        LONG seen = InterlockedCompareExchange(&ps3_output_trace.count[kind], prior+1, prior);
        if (seen == prior) return 1;
        prior = seen;
    }
    return 0;
}

static inline void ps3_output_trace_mapper(uint32_t syscall, uint32_t requested,
    uint32_t va, uint64_t size, uint64_t requested_size, uint64_t flags, uint32_t align_or_id,
    uint32_t out, uint32_t lr, uint64_t tid)
{
    if (!ps3_output_trace_claim(0, va, size)) return;
    fprintf(stderr, "[output-provenance] mapper=%u request=0x%08X va=0x%08X size=%llu requested_size=%llu flags=0x%llX align_or_id=0x%08X outp=0x%08X lr=0x%08X tid=%llu\n",
        syscall, requested, va, (unsigned long long)size, (unsigned long long)requested_size, (unsigned long long)flags,
        align_or_id, out, lr, (unsigned long long)tid);
}
#endif
