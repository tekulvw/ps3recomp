#ifndef PS3RECOMP_RSX_ORIGINAL_OWNER_H
#define PS3RECOMP_RSX_ORIGINAL_OWNER_H
#include <stdint.h>
/* Optional immutable host-owner hooks. Install before concurrent original
 * ingress; callbacks/userdata remain process-lived. A scope captures its exact
 * hook table so nested lifecycle changes cannot mismatch begin/end. No native
 * rendering API or GPU completion is introduced here. */
enum rsx_original_owner_flags {
    RSX_ORIGINAL_READ=0, RSX_ORIGINAL_MUTATE=1,
    RSX_ORIGINAL_BOUNDARY=3, RSX_ORIGINAL_LIFECYCLE_BIT=4,
    RSX_ORIGINAL_LIFECYCLE=5, RSX_ORIGINAL_FAILURE_BIT=8
};
enum { RSX_ORIGINAL_OWNER_REFUSED=-2 };
typedef struct rsx_original_owner_hooks {
    void *user;
    int (*begin)(void *,uint32_t flags);
    void (*end)(void *,uint32_t flags,int primitive_delta,int preserve_boundary);
} rsx_original_owner_hooks;
#endif
