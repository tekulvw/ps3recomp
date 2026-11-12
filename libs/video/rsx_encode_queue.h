#ifndef PS3RECOMP_RSX_ENCODE_QUEUE_H
#define PS3RECOMP_RSX_ENCODE_QUEUE_H
/* PS3RECOMP_RSX_ENCODE_THREAD=1 for a backend that records commands as the
 * engine calls it (the Vulkan backend; Metal hands its own record list over
 * instead): the engine's calls go through a queue to a thread of their own,
 * which makes the real backend calls in the same order, so the FIFO walker
 * goes on decoding while the backend records, writes descriptors, copies into
 * its arenas and submits.
 *
 * What the walker still decides, it decides alone: labels, reports and the
 * deferred-report queue never see the backend except through the engine.
 *
 * The calls fall into four kinds:
 *  - queued: everything that records in stream order (binds, draws, clears,
 *    copies into existing targets, releases, presents). Pointer arguments are
 *    copied into the queue when the call is made: the engine reuses its
 *    buffers for the next draw.
 *  - immediate: calls that make a new object and touch nothing a queued call
 *    can see (texture, target and pipeline creation, uploads into a texture
 *    just made, crossbar views). They run on the caller, under the backend
 *    lock, while the queue may still hold earlier calls: an object nothing
 *    earlier names can be made at any point before its first use.
 *  - ordered with a result: the walker waits for the queue to empty, then
 *    makes the call itself (readbacks, depth snapshots and stores, waits).
 *    A colour snapshot is split when the backend offers it: the copy object is
 *    made at once and the copy itself is queued.
 *  - tickets: submit_async returns a ticket of the queue's own, which the
 *    encode thread maps to the backend's when it submits; completion_status
 *    answers from what the encode thread has seen complete, waiting when told
 *    to. rebind_vs_constants is answered at once, and the encode thread binds
 *    the bytes it kept when the backend cannot reuse its upload.
 *
 * Off (the default), the backend is used directly and none of this runs. */
#include "rsx_draw_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_encq_hooks {
    /* Optional split colour snapshot: alloc runs on the caller under the
     * backend lock and returns the copy's handle (0 on failure) and a token;
     * copy is queued and records the copy itself. */
    u32  (*color_snapshot_alloc)(void* user, u32 surface, u32* token);
    void (*color_snapshot_copy)(void* user, u32 snapshot, u32 surface, u32 token);
} rsx_encq_hooks;

/* The vtable to register in place of `real` when the switch is on and the
 * thread starts at engine init; NULL when the switch is off. The queue holds
 * `real` and `hooks` until rsx_encq_unwrap. One queue per process. */
const rsx_draw_backend* rsx_encq_wrap(const rsx_draw_backend* real, const rsx_encq_hooks* hooks);
void rsx_encq_unwrap(void);
/* Is the encode thread running? */
int  rsx_encq_active(void);
/* Wait until every queued call has been made. */
void rsx_encq_drain(void);
/* Queue fn(arg) in order with the engine's calls (a host present's second
 * half, for instance); runs at once when the thread is not running. Returns
 * its place in the queue, for rsx_encq_wait_done (0 when it already ran). */
u64  rsx_encq_call(void (*fn)(void*), void* arg);
/* Wait until the call at `place` (from rsx_encq_call) has been made. */
void rsx_encq_wait_done(u64 place);
/* Exclusive use of the backend against the encode thread, for a call from
 * outside the engine (combine with rsx_encq_drain when it must follow what is
 * queued). No-ops when the thread is not running. */
void rsx_encq_lock(void);
void rsx_encq_unlock(void);

typedef struct rsx_encq_stats {
    unsigned long long ops, payload_bytes, full_waits, drains, immediates, lock_waits, tickets,
                       snapshots_split, ordered_calls;
    double full_wait_ms, drain_ms, lock_wait_ms, encode_ms;
} rsx_encq_stats;
void rsx_encq_get_stats(rsx_encq_stats* out);

#ifdef __cplusplus
}
#endif
#endif
