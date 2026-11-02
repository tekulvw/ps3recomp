#ifndef PS3RECOMP_RSX_NATIVE_OWNER_H
#define PS3RECOMP_RSX_NATIVE_OWNER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Temporary host lifetime/ordering metadata, never a renderer command API.
 * Opt in with PS3RECOMP_RSX_NATIVE_OWNER=1 before backend initialization.
 * Getter is unretained; retain(lifetime) must succeed before native dereference.
 * Callbacks and userdata are process-lived. Native try_begin never waits or
 * acquires a consumer lease; success holds original recording/queue ownership
 * through native encode+submit+signal. End on that same thread.
 * Negative poll/status never authorizes GPU-resource release. Native close must
 * genuinely drain before release. Host teardown/reinit refuses retained owners.
 * Explicit shutdown retry belongs to the external host lifecycle thread after
 * successful native close, outside callbacks, and only for a pending request. */
#define RSX_NATIVE_OWNER_ABI 1u
enum rsx_native_owner_backend { RSX_NATIVE_OWNER_METAL=1, RSX_NATIVE_OWNER_VULKAN=2 };
typedef void (*rsx_native_proc)(void);
typedef struct rsx_native_owner {
    uint32_t abi_version,struct_size,backend,reserved;
    uint64_t lifetime;
    void *device,*queue,*userdata;
    uint32_t queue_family,reserved1;
    void *instance,*physical_device;
    rsx_native_proc (*get_instance_proc)(void *,const char *);
    rsx_native_proc (*get_device_proc)(void *,const char *);
    int (*retain)(void *,uint64_t);
    void (*release)(void *,uint64_t);
    int (*try_begin)(void *,uint64_t);
    void (*end)(void *,uint64_t);
} rsx_native_owner;
#define RSX_NATIVE_OWNER_FEATURE_BC UINT64_C(1)
#define RSX_NATIVE_OWNER_FEATURE_ANISOTROPY UINT64_C(2)
#define RSX_NATIVE_OWNER_FEATURE_DEPTH_BOUNDS UINT64_C(4)
typedef struct rsx_native_owner_v2 {
    uint32_t abi_version,struct_size;
    rsx_native_owner base;
    uint64_t known_features,enabled_features;
} rsx_native_owner_v2;
#ifdef __cplusplus
}
#endif
#endif
