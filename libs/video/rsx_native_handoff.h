#ifndef PS3RECOMP_RSX_NATIVE_HANDOFF_H
#define PS3RECOMP_RSX_NATIVE_HANDOFF_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RSX_NATIVE_HANDOFF_ABI 1u
/* Decoded actual dispatch state, captured before the qualified packet. The
 * native surface is an existing exact-key allocation, never a created target.
 * A zero native_surface or ready=0 cannot qualify a native submission. */
typedef struct rsx_native_source_target {
    uint32_t ready, color_a_offset, color_a_dma, color_a_pitch;
    uint32_t surface_format, clip_x, clip_y, clip_width, clip_height;
    uint32_t color_target, native_surface, native_width, native_height;
    uint32_t viewport_z_scale_bits, viewport_z_offset_bits;
} rsx_native_source_target;
typedef struct rsx_native_handoff_event {
    uint32_t abi_version, struct_size;
    uint64_t cookie, vm_identity, vm_base_identity, source_observation;
    uint32_t context_ea, header_ea, header, argument, fifo_get, fifo_put;
    rsx_native_source_target target;
} rsx_native_handoff_event;
typedef void (*rsx_native_handoff_fn)(void*, const rsx_native_handoff_event*);
typedef struct rsx_native_handoff_arm {
    uint32_t abi_version, struct_size;
    uint64_t vm_identity, source_observation;
    uint32_t context_ea, header_ea, expected_header, expected_argument;
    void* userdata;
    rsx_native_handoff_fn consume;
} rsx_native_handoff_arm;
/* Explicit opt-in API for a source-qualified original writer. Arm immediately
 * before the actual post-refill write; publish immediately after its header
 * store. Publish checks the original context and exact two words. The writer
 * owns that command span until consumption/cancellation; EA alone is no token.
 * Up to 64 one-shot entries; cookies are never reused. Status: 0 success,
 * -1 invalid, -3 stale/mismatched publication, -4 capacity/active callback.
 * vm_identity is the producer's genuine opaque VM lifecycle serial; the SDK
 * separately captures actual vm_base and echoes it as vm_base_identity. The
 * producer must cancel at VM retirement and revalidate its lifecycle serial in
 * consume: equal base addresses alone cannot prove a new mapping generation.
 * No Main/frame identity is inferred from opaque source_observation.
 * Consume runs once on the serialized FIFO consumer BEFORE header decode,
 * outside the registry mutex. It may be a different host thread on each call;
 * no thread-affine frontend session may be cached on this thread implicitly.
 * userdata/callback must outlive an active callback. Cancel returns -4 while
 * it runs; after successful cancel no future callback can begin. Callback
 * must not throw, recursively drain FIFO, or wait for GPU completion. */
int cellGcm_native_handoff_arm(const rsx_native_handoff_arm*, uint64_t* cookie);
int cellGcm_native_handoff_publish(uint64_t cookie);
int cellGcm_native_handoff_cancel(uint64_t cookie);
/* Read-only actual dispatch decoder snapshot; caller holds consumer lease.
 * Returns 1 for a qualified existing color-A allocation; otherwise 0. */
int rsx_draw_engine_native_source_target(rsx_native_source_target* out);
#ifdef __cplusplus
}
#endif
#endif
