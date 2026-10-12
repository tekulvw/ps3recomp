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
s32 sail_graphics_begin_session(u32 player, SailGraphicsConfig* out, u32* session);
/* Begin pins callbacks against teardown. Unpublish hides pixels but retains
 * that pin through free callbacks; withdraw ends the resolved ownership pin.
 * Producer owns and initializes buffer, never overwriting published pixels. */
s32 sail_graphics_publish_frame(u32 player, u32 buffer, const u8 format[32], u64 pts, u64 origin, u32 tag);
#define SAIL_GRAPHICS_HAS_REPLACEMENT 1
/* Same pinned format/session; increasing picture id is distinct from poll tags.
 * After unpublish, only withdraw/begin can reopen publication. */
s32 sail_graphics_replace_frame(u32 player,u32 buffer,const u8 format[32],u64 pts,u64 origin,u32 picture_id);
/* Original descriptor construction brackets frame selection. End transfers
 * one owned immutable-picture reference; caller binds or releases it. */
void sail_graphics_picture_capture_begin(void);
u64 sail_graphics_picture_capture_end(void);
s32 sail_graphics_unpublish_frame(u32 player);
s32 sail_graphics_withdraw_frame(u32 player);
s32 sail_graphics_config_for_player(u32 player, SailGraphicsConfig* out);
#ifdef __cplusplus
}
#endif
#endif
