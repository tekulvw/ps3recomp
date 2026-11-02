#ifndef PS3RECOMP_RSX_FRONTEND_H
#define PS3RECOMP_RSX_FRONTEND_H

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in semantic compute interface. Borrowed device lifetime ends at backend
 * shutdown. Serialize calls; descriptors/host bytes are consumed before return.
 * No guest packets, window/present work, or implicit original-work fallback. */
#define RSX_FRONTEND_ABI_VERSION 1u
#define RSX_FRONTEND_MAX_BINDINGS 16u
#define RSX_FRONTEND_NO_SIZE_INDEX UINT32_MAX
typedef struct rsx_frontend_device rsx_frontend_device;
enum rsx_frontend_status {
    RSX_FRONTEND_OK = 0,
    RSX_FRONTEND_INDETERMINATE = 1, /* ticket retained; poll for real terminal */
    RSX_FRONTEND_INVALID = -1,
    RSX_FRONTEND_UNSUPPORTED = -2,
    RSX_FRONTEND_STALE = -3,
    RSX_FRONTEND_BUSY = -4,
    RSX_FRONTEND_ALLOCATION_FAILED = -5,
    RSX_FRONTEND_DEVICE_FAILED = -6
};
enum rsx_frontend_access { RSX_FRONTEND_READ = 1, RSX_FRONTEND_READ_WRITE = 2 };
enum rsx_frontend_shader_language {
    RSX_FRONTEND_MSL = 1, RSX_FRONTEND_HLSL = 2, RSX_FRONTEND_SPIRV = 3
};
typedef struct rsx_frontend_buffer { uint64_t device_generation, id, version; } rsx_frontend_buffer;
typedef struct rsx_frontend_pipeline { uint64_t device_generation, id; } rsx_frontend_pipeline;
typedef struct rsx_frontend_ticket { uint64_t device_generation, submission_id; } rsx_frontend_ticket;

typedef struct rsx_frontend_caps_desc {
    uint32_t struct_size, abi_version;
    uint64_t device_generation;
    uint32_t compute_supported, draws_supported, max_pending, max_bindings;
    uint32_t runtime_sizes_slot, reserved;
} rsx_frontend_caps_desc;

typedef struct rsx_frontend_buffer_desc {
    uint32_t struct_size, abi_version;
    uint64_t byte_length;
    const void *initial_bytes; /* NULL means zero initialize */
} rsx_frontend_buffer_desc;

/* One complete immutable resource version. update publishes complete CPU bytes
 * into a new MTLBuffer; it never reads/mutates the previous in-flight version.
 * Each version remains addressable until explicitly released. Submission retains
 * its own references through GPU terminal completion even after public release. */
int rsx_frontend_buffer_create(rsx_frontend_device *, const rsx_frontend_buffer_desc *, rsx_frontend_buffer *);
int rsx_frontend_buffer_update(rsx_frontend_device *, rsx_frontend_buffer previous,
    const rsx_frontend_buffer_desc *, rsx_frontend_buffer *);
int rsx_frontend_buffer_release(rsx_frontend_device *, rsx_frontend_buffer);
/* Diagnostic/readback only; returns BUSY until all uses are terminal and polled. */
int rsx_frontend_buffer_read(rsx_frontend_device *, rsx_frontend_buffer, uint64_t offset,
    void *destination, uint64_t length);

typedef struct rsx_frontend_binding_layout {
    uint32_t slot, access;
    uint64_t minimum_bytes;
    uint32_t runtime_array_stride; /* zero for fixed layouts */
    uint32_t runtime_size_index; /* dense metadata indices or NO_SIZE_INDEX */
    uint32_t alignment_bytes, reserved; /* manifest alignment, power of two >=4 */
} rsx_frontend_binding_layout;
/* Initial native compute accepts runtime arrays starting at byte zero only:
 * minimum_bytes must equal runtime_array_stride. Struct headers before runtime
 * arrays return UNSUPPORTED; their translation metadata is not native support. */

typedef struct rsx_frontend_pipeline_desc {
    uint32_t struct_size, abi_version;
    const void *source_bytes;
    uint64_t source_length;
    const char *entry_point;
    uint32_t entry_point_length, binding_count;
    const rsx_frontend_binding_layout *bindings;
    uint32_t threads_per_group[3], shader_language;
} rsx_frontend_pipeline_desc;
int rsx_frontend_pipeline_create(rsx_frontend_device *, const rsx_frontend_pipeline_desc *, rsx_frontend_pipeline *);
int rsx_frontend_pipeline_release(rsx_frontend_device *, rsx_frontend_pipeline);

typedef struct rsx_frontend_binding {
    uint32_t slot, reserved;
    rsx_frontend_buffer buffer;
    uint64_t offset, byte_length;
} rsx_frontend_binding;
typedef struct rsx_frontend_frame {
    uint32_t struct_size, abi_version;
    rsx_frontend_pipeline pipeline;
    const rsx_frontend_binding *bindings;
    uint32_t binding_count, groups[3];
} rsx_frontend_frame;

/* Negative returns guarantee no commit. OK/INDETERMINATE provide a qualified
 * ticket; only a genuine terminal event authorizes resource reclamation.
 * Existing-backend uncommitted work makes submit BUSY: callers must explicitly
 * submit that work first. Shared queue commit order then preserves dependencies;
 * this API never flushes/replays original work or creates a second queue. */
int rsx_frontend_submit(rsx_frontend_device *, const rsx_frontend_frame *, rsx_frontend_ticket *);
typedef struct rsx_frontend_event {
    uint32_t struct_size, abi_version;
    rsx_frontend_ticket ticket;
    uint32_t succeeded, no_further_gpu_access;
    uint32_t native_status, reserved;
} rsx_frontend_event;
/* count=0 means no event, never completion. Each terminal event is emitted once.
 * API errors do not certify terminal state and do not release pending leases. */
int rsx_frontend_poll(rsx_frontend_device *, rsx_frontend_event *, uint32_t capacity, uint32_t *count);
int rsx_frontend_caps(rsx_frontend_device *, rsx_frontend_caps_desc *);

#ifdef __cplusplus
}
#endif
#endif
