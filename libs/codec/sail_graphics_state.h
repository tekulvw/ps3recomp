/* Internal host snapshot; no guest layout is inferred from this record. */
#ifndef PS3RECOMP_SAIL_GRAPHICS_STATE_H
#define PS3RECOMP_SAIL_GRAPHICS_STATE_H
#include "ps3emu/ps3types.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SailGraphicsConfig {
    u32 self_ea, player_ea, arg_ea;
    u32 callbacks[5]; /* makeup, cleanup, formatChanged, allocFrame, freeFrame */
    u8 preferred[32];
    u32 has_preferred;
} SailGraphicsConfig;
/* A snapshot is not an ownership pin. Consumers must prevent Finalize/Destroy
 * while dispatching, and revalidate guest-owned OPDs before invocation. */
s32 sail_graphics_config_for_player(u32 player, SailGraphicsConfig* out);
#ifdef __cplusplus
}
#endif
#endif
