#ifndef PS3RECOMP_RSX_LIVE_DRAW_OWNER_H
#define PS3RECOMP_RSX_LIVE_DRAW_OWNER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Temporary, default-off ownership seam only; no frontend renderer API.
 * Set PS3RECOMP_RSX_NATIVE_OWNER=1 before initialization to enable.
 * Metadata is UNRETAINED: retain(userdata,lifetime) must succeed before any
 * native handle dereference. userdata/callbacks are process-lived. Retention
 * prevents original shutdown/reinit; close/drain frontend then retry shutdown.
 * A successful explicit original checked flush establishes the clean boundary.
 * try_begin never waits: dirty/original encoding => -4; stale/closing => -3;
 * failed original owner => -6. None of these imply GPU terminal completion.
 * Hold the successful lease through encoding and queue Execute/Signal; end on
 * the same thread. No recursive native lease or original reentry is allowed.
 * Original nesting lock order: consumer lease -> owner gate -> legacy locks.
 * Native lease acquires only owner gate, so it cannot invert consumer locks. */
#define RSX_LIVE_DRAW_OWNER_ABI 1u
typedef struct rsx_live_draw_owner {
    uint32_t struct_size,abi_version;
    uint64_t lifetime;
    void *device,*queue,*userdata;
    int (*retain)(void *,uint64_t);
    void (*release)(void *,uint64_t);
    int (*try_begin)(void *,uint64_t);
    void (*end)(void *,uint64_t);
} rsx_live_draw_owner;
/* Caller initializes struct_size/abi_version. No device/queue is created. */
int rsx_live_draw_borrow_owner(rsx_live_draw_owner *);
/* External compatibility lifecycle code checks this after successful native
 * close, then retries only on the host lifecycle thread, outside callbacks.
 * requested: 1 pending request, 0 none, negative stale/busy/unsupported.
 * retry: 1 teardown performed, 0 no request, -4 retained, -3 stale, -6 failed.
 * A normal frontend drop never requests or performs host teardown. */
int rsx_live_draw_owner_shutdown_requested(uint64_t lifetime);
int rsx_live_draw_owner_retry_shutdown(uint64_t lifetime);
/* Existing report callback registration belongs to owner configuration before
 * concurrent ingress. The enabled owner gate snapshots a coherent begin/end
 * pair; default-off operation retains the existing registration contract. */
#ifdef __cplusplus
}
#endif
#endif
