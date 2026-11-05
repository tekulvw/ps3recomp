/* Reservation gate for a native submission whose queue place is fixed at the
 * native boundary but whose encoding finishes later, while the FIFO keeps
 * draining (opt-in: PS3RECOMP_RSX_RESERVATION_GATE=1; otherwise every open
 * refuses and every gate returns at once).
 *
 * The native side opens a reservation with the resources it may read (its
 * pinned spans) and resolves it when it has submitted or abandoned. While it
 * is outstanding, queue work the FIFO records is ordered behind the native
 * submission by the backend's queue order, so the FIFO only has to wait for
 * what queue order cannot cover; it calls rsx_reservation_gate first:
 *
 *   RSX_RESERVATION_PINNED_WRITE  a host-side write, upload, eviction, resize
 *                                 or reuse of a resource overlapping a pinned
 *                                 span (host writes are not queue-ordered);
 *   RSX_RESERVATION_PRESENT       presenting or flipping a surface;
 *   RSX_RESERVATION_READBACK      a CPU readback or a transfer to guest memory;
 *   RSX_RESERVATION_COMPLETION    a host wait for GPU completion, or a fence
 *                                 or report whose host meaning is completion;
 *   RSX_RESERVATION_BOUNDARY      another native boundary;
 *   RSX_RESERVATION_CLOSE         owner close, context change or VM change.
 *
 * A pinned write that overlaps no pinned span, and anything else, proceeds.
 * The gate never holds a lock while it waits on the caller's behalf, and the
 * native side must resolve without anything the waiting FIFO holds (for
 * example its owner lease), or the FIFO could wait forever. */
#ifndef PS3RECOMP_RSX_NATIVE_RESERVATION_H
#define PS3RECOMP_RSX_NATIVE_RESERVATION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum rsx_reservation_cause {
    RSX_RESERVATION_PINNED_WRITE = 0,
    RSX_RESERVATION_PRESENT,
    RSX_RESERVATION_READBACK,
    RSX_RESERVATION_COMPLETION,
    RSX_RESERVATION_BOUNDARY,
    RSX_RESERVATION_CLOSE,
    RSX_RESERVATION_CAUSES
};
/* Address spaces of a span: a backend image id, RSX local memory offsets, or
 * guest/main memory effective addresses. */
enum rsx_reservation_space {
    RSX_RESERVATION_IMAGE = 0,
    RSX_RESERVATION_LOCAL = 1,
    RSX_RESERVATION_MAIN = 2
};
typedef struct rsx_reservation_span {
    uint32_t space, reserved;
    uint64_t begin, end; /* [begin, end) */
} rsx_reservation_span;
#define RSX_RESERVATION_MAX_SPANS 4096u

typedef struct rsx_reservation_stats {
    uint64_t opened, resolved, busy, refused;
    uint64_t waits[RSX_RESERVATION_CAUSES];
    uint64_t wait_ns[RSX_RESERVATION_CAUSES];
} rsx_reservation_stats;

int rsx_reservation_enabled(void);
/* 0 and a token, or: -2 disabled, -4 one is already outstanding, -6 too many
 * spans or an invalid span (the caller keeps its blocking path). */
int rsx_reservation_open(uint64_t lifetime, const rsx_reservation_span *pinned,
                         uint32_t count, uint64_t *token);
/* Submitted or abandoned: releases every waiter. -3 for a token that is not
 * the outstanding one. */
int rsx_reservation_resolve(uint64_t token);
/* Whether `cause` (with `touched` for a pinned write) must wait now. */
int rsx_reservation_affects(int cause, const rsx_reservation_span *touched);
/* Waits while `cause` is affected; returns 1 when it waited, else 0. */
int rsx_reservation_gate(int cause, const rsx_reservation_span *touched);
void rsx_reservation_get_stats(rsx_reservation_stats *out);

#ifdef __cplusplus
}
#endif
#endif
