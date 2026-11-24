/*
 * ps3recomp - NP social provider, runtime side (libs/network/np_social.c).
 * Shared by sceNpBasic.c and sceNpLookup.c; see docs/np-social-provider.md.
 */
#ifndef PS3RECOMP_NP_SOCIAL_INTERNAL_H
#define PS3RECOMP_NP_SOCIAL_INTERNAL_H

#include <stdint.h>
#include "ps3emu/np_social.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NP_SOCIAL_QUEUE_MAX 256

typedef struct np_social_event {
    int32_t  event;
    int32_t  ret_code;
    char     online_id[PS3_NP_SOCIAL_ONLINE_ID_MAX + 1];
    uint32_t size;
    uint8_t  data[PS3_NP_SOCIAL_PRESENCE_MAX];
} np_social_event;

/* The installed provider, or NULL (the inert default). */
const ps3_np_social_provider* np_social_get(void);

/* Mark the oldest unannounced event announced; 1 and its code when there was one. */
int np_social_announce(int32_t* event, int32_t* ret_code);
/* Remove and return the oldest announced event; 1 when there was one. */
int np_social_take(np_social_event* out);
/* The title gained (1) or lost (0) its handler. Losing it returns announced,
 * untaken events to the unannounced state. Tells the provider. */
void np_social_handler(int registered);

/* Lookups: start one (a non-zero request number), poll it (1 done with result
 * and canonical id, 0 running, -1 unknown), or drop it (a later completion is
 * ignored). */
uint64_t np_social_lookup_start(const char* online_id);
int np_social_lookup_poll(uint64_t request, int32_t* result, char online_id[PS3_NP_SOCIAL_ONLINE_ID_MAX + 1]);
void np_social_lookup_drop(uint64_t request);

/* An online ID as the provider interface carries it: 1..16 bytes, no NUL inside. */
int np_social_valid_id(const char* online_id);

/* Registration of the provider-only imports (sceNpBasic.c, sceNpLookup.c). */
void np_basic_social_register(void);
void np_lookup_social_register(void);

/* Delivery at each cellSysutilCheckCallback (sceNpBasic.c). */
void np_basic_social_tick(void);

#ifdef __cplusplus
}
#endif

#endif
