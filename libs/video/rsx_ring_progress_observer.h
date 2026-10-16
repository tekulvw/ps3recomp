/* Bounded diagnostic hooks for one guest progress word. No guest semantics. */
#ifndef RSX_RING_PROGRESS_OBSERVER_H
#define RSX_RING_PROGRESS_OBSERVER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
 RSX_RING_DECODE=1, RSX_RING_ENQUEUE=2, RSX_RING_TICKET_BIND=3,
 RSX_RING_METAL_COMMIT=4, RSX_RING_METAL_COMPLETE=5,
 RSX_RING_VM_PUBLISH=6, RSX_RING_PENDING_SNAPSHOT=7,
 RSX_RING_TRACK_OVERFLOW=8
};
typedef void (*rsx_ring_progress_observer_fn)(uint32_t kind,uint32_t ea,
 uint32_t value,uint64_t ticket,uint64_t aux);
/* One-shot diagnostic capture. Callback storage must survive in-flight calls.
 * Callback must not block, access guest memory, or perform I/O. */
void rsx_ring_progress_observer_set(uint32_t target_ea,
 rsx_ring_progress_observer_fn observer);
#if defined(__APPLE__) && defined(RSX_RING_PROGRESS_OPTIONAL)
#define RSX_RING_PROGRESS_WEAK __attribute__((weak_import))
#else
#define RSX_RING_PROGRESS_WEAK
#endif
void rsx_ring_progress_metal_committed(uint64_t ticket,uint64_t records)
 RSX_RING_PROGRESS_WEAK;
void rsx_ring_progress_metal_completed(uint64_t ticket,uint64_t status)
 RSX_RING_PROGRESS_WEAK;
#undef RSX_RING_PROGRESS_WEAK
#ifdef __cplusplus
}
#endif
#endif
