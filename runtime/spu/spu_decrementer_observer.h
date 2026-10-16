/* Opt-in decrementer-channel observation; never an instruction-loop hook. */
#ifndef SPU_DECREMENTER_OBSERVER_H
#define SPU_DECREMENTER_OBSERVER_H
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SPU_DECREMENTER_OBSERVER_CADENCE 128u
/* Runs synchronously on the owning thread after the real decrementer value is
 * computed, once per 128 enabled reads on that host thread across all contexts.
 * read_ordinal counts reads while a non-NULL observer is installed; it persists
 * across replacement/uninstallation and is not a per-context count. It does not
 * include reads made while disabled. No extra clock read is made by the SDK.
 *
 * reported_pc is ctx->pc, NOT a promise of the current guest instruction for
 * compiled/native callers. Verify the instruction/source identity before
 * attributing a poll loop. The value is exactly what this channel read returns.
 * host_caller is the extracted native return address of the spu_rdch caller,
 * when supported by the compiler, otherwise zero. It identifies a host call
 * site only with the exact binary/load address; it is not a guest instruction
 * address. Inlining/tail calls can affect this witness and must be qualified.
 *
 * The callback may copy bounded scalar metadata. It must not mutate ctx or LS,
 * retain ctx, execute guest code, recursively install a callback, or nonlocally
 * unwind. errno is preserved. Counts are observations, not CPU time shares.
 */
typedef void (*spu_decrementer_observer_fn)(const spu_context* ctx,
                                         uint32_t reported_pc,
                                         uint32_t decrementer_value,
                                         uint64_t read_ordinal,
                                         uintptr_t host_caller);
/* Default NULL. Atomic publication; prefer installation before workers start.
 * Uninstallation does not wait for in-flight callbacks. Callback code/storage
 * must remain alive until workers quiesce. No context pointer is retained. */
void spu_decrementer_observer_set(spu_decrementer_observer_fn observer);
#ifdef __cplusplus
}
#endif
#endif
