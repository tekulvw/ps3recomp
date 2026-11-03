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
/* A distinct producer-owned Main-style ring writer. The caller owns the real
 * state record and ring span; this API does not turn a guessed address into a
 * writer lease. Arm before writing/releasing its first packet. Publish checks
 * the unchanged four-word writer state and exact packet bytes BEFORE PUT can
 * expose them. It does not use or advance a PPE GCM context. The source owner
 * must retain the actual VM/allocation/publication identity through consume or
 * cancel. All ordinary handoff cancellation/lifetime rules apply. */
typedef struct rsx_native_handoff_ring_event {
    uint32_t abi_version, struct_size;
    rsx_native_handoff_event source;
    uint32_t writer_state_ea, ring_begin_ea, ring_bytes, reserved;
} rsx_native_handoff_ring_event;
typedef void (*rsx_native_handoff_ring_fn)(void*, const rsx_native_handoff_ring_event*);
typedef struct rsx_native_handoff_ring_arm {
    uint32_t abi_version, struct_size;
    uint64_t vm_identity, source_observation;
    uint32_t writer_state_ea, ring_begin_ea, ring_bytes, header_ea;
    uint32_t expected_header, expected_argument;
    void* userdata;
    rsx_native_handoff_ring_fn consume;
} rsx_native_handoff_ring_arm;
int cellGcm_native_handoff_ring_arm(const rsx_native_handoff_ring_arm*, uint64_t* cookie);
int cellGcm_native_handoff_ring_publish(uint64_t cookie);
/* Read-only actual dispatch decoder snapshot; caller holds consumer lease.
 * Returns 1 for a qualified existing color-A allocation; otherwise 0. */
int rsx_draw_engine_native_source_target(rsx_native_source_target* out);
/* Bounded original register words at a caller-qualified FIFO boundary. Caller
 * holds serialized consumer ownership; this reports inherited earlier work,
 * never frontend source ownership or original-work removal. No mutation,
 * allocation or submission. Methods must be aligned in0..1FFC, count1..256.
 * 0 success; -1 invalid (output unchanged), -3 inactive, -4 busy, -6 failed. */
int rsx_draw_engine_native_registers(const uint32_t* methods,uint32_t* values,uint32_t count);
/* Actual engine canvas used to size depth allocations; never current target
 * or viewport dimensions. Same owner/consumer/status contract as registers.
 * On failure output remains unchanged. No allocation or GPU operation. */
int rsx_draw_engine_native_canvas(uint32_t dimensions[2]);
/* Explicit source-key lookup, independent of current dispatch registers.
 * Width/height are exact physical image dimensions, including the source AA
 * sample grid. format is the source color-format code or depth-format code;
 * aa_grid is x|(y<<8) for color, and zero for depth. No allocation, snapshot,
 * copy, current-target fallback or content-readiness inference is performed.
 * The caller holds the serialized consumer lease through lookup and borrowing
 * the returned backend handle. The host image's lifetime/generation callbacks
 * then govern retention and validation. This does not certify GPU completion.
 * Returns 1 for one exact existing allocation, 0 for absent/ambiguous, negative
 * for invalid/busy/stale/failed. The result is unretained metadata only. */
/* queued_clear_planes bits: color1, depth2, stencil4. These record actual
 * full-allocation clear commands since allocation, not arbitrary draw writes,
 * GPU completion or scene/content identity. They become usable initialization
 * evidence only with a successful original native boundary and the exact
 * source association under the same serialized consumer/owner lease. */
enum rsx_native_image_kind { RSX_NATIVE_COLOR_IMAGE=0, RSX_NATIVE_DEPTH_IMAGE=1 };
typedef struct rsx_native_image_key {
    uint32_t abi_version, struct_size, kind, location;
    uint32_t offset, pitch, format, width, height, aa_grid, reserved[2];
} rsx_native_image_key;
typedef struct rsx_native_image_match {
    uint32_t abi_version, struct_size, kind, native_handle;
    uint32_t width, height, queued_clear_planes, reserved;
} rsx_native_image_match;
int rsx_draw_engine_native_image(const rsx_native_image_key*, rsx_native_image_match*);
/* Serialized consumer-scoped output preparation. Current cache/input backing is
 * unchanged until accepted external write publication. Scope IDs never reuse.
 * sampled_planes require existing contents. Missing attachment-only color outputs may
 * be allocated and actually cleared only with flags=1 (explicit POC policy).
 * Returned backing key names the real physical allocation; same-grid/pitch
 * larger reuse follows the ordinary target allocator. Clear commands must pass
 * a successful original native boundary before native use. End releases all
 * unpromoted allocations; borrowed HostImages require their own real pins. */
int rsx_draw_engine_native_images_begin(uint64_t source_observation, uint64_t* scope);
int rsx_draw_engine_native_prepare_image(uint64_t scope, const rsx_native_image_key*,
    uint32_t sampled_planes, uint32_t attachment_planes, uint32_t flags,
    rsx_native_image_key* backing, rsx_native_image_match*);
int rsx_draw_engine_native_images_end(uint64_t scope);

/* Protected read for a native image provider's validate-planes callback.
 * Caller MUST first validate the retained image identity under its native owner
 * lease, which excludes original decoder mutation. This never acquires an
 * original-owner gate recursively. required_planes must be nonzero/subset7.
 * 0 = known clears cover requested planes; -3 = absent/ambiguous allocation;
 * -5 = missing initialization; -1 invalid; -6 decoder failed. No GPU wait. */
int rsx_draw_engine_native_image_clear_planes(uint32_t handle, uint32_t required_planes);
/* Explicit external native-pass continuation. Caller holds the serialized FIFO
 * consumer lease and genuine retained HostImage identities through this call.
 * The native generation/ticket name an accepted queued submission, never GPU
 * completion. All rows are validated before any decoder mutation. Ordered
 * repeated image keys retain the last producer's AA-expanded logical clip;
 * physical allocation/canvas dimensions cannot substitute for that clip.
 * Potential writes dirty caches but do not initialize any uncleared plane.
 * Passive words exclude programs/array pointers/execution methods. Constants
 * and current attributes contain only source-known actual writes; other rows
 * remain unchanged. Subsequent original draws must bind fresh VP instructions,
 * FP address/control and any consumed array addresses/formats/base state.
 * Status0 success; -1 invalid, -3 stale identity, -4 busy, -5 budget, -6 failed.
 */
typedef struct rsx_native_external_image_write {
    rsx_native_image_key key;
    uint32_t native_handle, planes, source_clip_width, source_clip_height;
} rsx_native_external_image_write;
typedef struct rsx_native_register_write { uint32_t method, value; } rsx_native_register_write;
typedef struct rsx_native_vector_write { uint32_t index, words[4]; } rsx_native_vector_write;
typedef struct rsx_native_external_pass {
    uint32_t abi_version, struct_size;
    uint64_t source_observation, native_generation, native_ticket;
    const rsx_native_external_image_write* writes;
    uint32_t write_count, reserved;
    const rsx_native_register_write* registers;
    uint32_t register_count, reserved1;
    const rsx_native_vector_write* constants;
    uint32_t constant_count, reserved2;
    const rsx_native_vector_write* current_attributes;
    uint32_t current_attribute_count, reserved3;
} rsx_native_external_pass;
int rsx_draw_engine_native_publish_external(const rsx_native_external_pass*);
/* A candidate-only guard failure names the native publication and missing
 * original bind. failure:1 VP start,2 VP words,3 FP address,4 FP control,
 * 5 array format,6 array offset,7 array base,8 missing resident program,
 * 9 indexed address/type,10 candidate late failure. detail is the missing method/word/array index. */
typedef struct rsx_native_external_status {
    uint32_t abi_version, struct_size, active, failure;
    uint64_t source_observation, native_generation, native_ticket;
    uint32_t vp_start, detail;
} rsx_native_external_status;
int rsx_draw_engine_native_external_status(rsx_native_external_status*);
/* Candidate failure only: prevent later original draws until owned lifecycle
 * stop. Does not publish guest completion, mutate passive state, or wait for
 * GPU work. Zero native identities explicitly mean no queued witness known. */
int rsx_draw_engine_native_abort_external(uint64_t source_observation,
    uint64_t native_generation,uint64_t native_ticket,int32_t reason);
#ifdef __cplusplus
}
#endif
#endif
