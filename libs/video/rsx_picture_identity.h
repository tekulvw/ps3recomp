#ifndef PS3RECOMP_RSX_PICTURE_IDENTITY_H
#define PS3RECOMP_RSX_PICTURE_IDENTITY_H
#include "ps3emu/ps3types.h"
#define RSX_HAS_PICTURE_IDENTITIES 1
#ifdef __cplusplus
extern "C" {
#endif
u64 rsx_picture_create(u32 ea,u32 location,u32 offset,u32 bytes,const u8* pixels);
int rsx_picture_bind_descriptor(u32 descriptor,u64 token);
int rsx_picture_capture_command(u32 data_ea,u32 descriptor,u32 location,u32 offset);
int rsx_picture_take_command(u32 data_ea,u32 location,u32 offset,u64* token);
/* Borrowed bytes require the caller's retained token for the entire read;
 * never release that ownership concurrently. */
const u8* rsx_picture_data(u64 token,u32 location,u32 offset,u32 bytes);
void rsx_picture_release(u64 token);
int rsx_picture_ready(void);
int rsx_picture_live_count(void);
int rsx_picture_reset(void);
#ifdef __cplusplus
}
#endif
#endif
