#ifndef PS3RECOMP_SPURS_POLICY_SERVICE_H
#define PS3RECOMP_SPURS_POLICY_SERVICE_H
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
/* A service identity resolves one resident policy context, never a fabricated
 * local store. Hold a reference until the PPU read/write operation finishes. */
uint32_t spurs_policy_service_identity(const spu_context* context);
spu_context* spurs_policy_service_acquire(uint32_t id);
void spurs_policy_service_release(spu_context* context);
static inline int spurs_policy_service_is_id(uint32_t id) { return id >= 0x80000000u; }
#ifdef __cplusplus
}
#endif
#endif
