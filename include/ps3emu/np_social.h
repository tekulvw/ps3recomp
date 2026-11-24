/*
 * ps3recomp - NP social provider (friends, presence, friend requests, lookups)
 *
 * An embedding application may supply PSN-style friends and presence through
 * this provider: host callbacks for what the title sends, and a queue the
 * provider feeds with NP Basic events. Generic: NP IDs are online ID handles,
 * presence is opaque bytes. Behaviour: docs/np-social-provider.md.
 *
 * Inert until ps3_np_social_set_provider() is called: the NP Basic and Lookup
 * imports then behave exactly as without this header.
 */
#ifndef PS3EMU_NP_SOCIAL_H
#define PS3EMU_NP_SOCIAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_NP_SOCIAL_ABI 1

/* SDK event codes the provider pushes most (SCE_NP_BASIC_EVENT_*). */
#define PS3_NP_SOCIAL_EVENT_OFFLINE            0
#define PS3_NP_SOCIAL_EVENT_PRESENCE           1
#define PS3_NP_SOCIAL_EVENT_ADD_FRIEND_RESULT  3
#define PS3_NP_SOCIAL_EVENT_OUT_OF_CONTEXT     9
#define PS3_NP_SOCIAL_EVENT_FRIEND_REMOVED     10

#define PS3_NP_SOCIAL_ONLINE_ID_MAX     16   /* bytes, without the NUL */
#define PS3_NP_SOCIAL_PRESENCE_MAX      128  /* opaque presence bytes */
#define PS3_NP_SOCIAL_STATUS_MAX        63   /* status text bytes, without the NUL */
#define PS3_NP_SOCIAL_MESSAGE_RECIPIENTS 12

/* Lookup results for ps3_np_social_complete_lookup (SDK codes). */
#define PS3_NP_SOCIAL_LOOKUP_FOUND      0
#define PS3_NP_SOCIAL_LOOKUP_NOT_FOUND  ((int32_t)0x8002A41Bu) /* COMMUNITY_SERVER_ERROR_NO_SUCH_USER_NPID */
#define PS3_NP_SOCIAL_LOOKUP_FAILED     ((int32_t)0x8002A10Bu) /* COMMUNITY_ERROR_BAD_RESPONSE */

/* A message from sceNpBasicSendMessageGui, copied out of guest memory. */
typedef struct ps3_np_social_message {
    uint64_t msg_id;
    uint16_t main_type;   /* SCE_NP_BASIC_MESSAGE_MAIN_TYPE_* */
    uint16_t sub_type;
    uint32_t features;
    uint32_t recipient_count;
    const char* recipients[PS3_NP_SOCIAL_MESSAGE_RECIPIENTS];  /* online IDs */
    const char* subject;  /* "" when none */
    const char* body;     /* "" when none */
    const uint8_t* data;  /* NULL when data_size is 0 */
    uint32_t data_size;
} ps3_np_social_message;

/* Every callback may be NULL. Callbacks run on a guest thread inside the
 * import that triggered them and must return promptly (no network waits).
 * Pointers they receive are valid only during the call. */
typedef struct ps3_np_social_provider {
    uint32_t abi;   /* PS3_NP_SOCIAL_ABI */
    void* user;     /* passed back to every callback */

    /* sceNpBasicSetPresenceDetails: the player's own presence. `data`/`size`
     * are meaningful when has_data; `status` (NUL-terminated) when has_status. */
    void (*set_presence)(void* user, int has_data, const uint8_t* data, uint32_t size,
                         int has_status, const char* status);
    /* sceNpBasicAddFriend: a friend request. Returns the import's answer
     * (0 when the request was taken, else a negative SDK error). */
    int32_t (*add_friend)(void* user, const char* online_id, const char* message);
    /* sceNpBasicSendMessageGui. Returns the import's answer. */
    int32_t (*send_message)(void* user, const ps3_np_social_message* message);
    /* sceNpLookupNpIdAsync: resolve an online ID. Finish it later, from any
     * thread, with ps3_np_social_complete_lookup(request, ...). */
    void (*lookup_online_id)(void* user, const char* online_id, uint64_t request);
    /* The title registered (1) or dropped (0) its NP Basic event handler. */
    void (*handler_changed)(void* user, int registered);
} ps3_np_social_provider;

/* Install the provider (copied). Once, from the host, before the guest runs.
 * Returns 0, or non-zero (and changes nothing) for NULL, a wrong abi, or a
 * second call. */
int ps3_np_social_set_provider(const ps3_np_social_provider* provider);

/* Queue one NP Basic event for the title (any thread). online_id: 1..16 bytes;
 * data: 0..128 bytes (NULL when size is 0). Returns 0 when queued, non-zero
 * when refused (no provider, bad arguments, queue full). */
int ps3_np_social_push_event(int32_t event, int32_t ret_code, const char* online_id,
                             const uint8_t* data, uint32_t size);

/* Finish a lookup started by lookup_online_id (any thread). result is
 * PS3_NP_SOCIAL_LOOKUP_FOUND with the canonical online_id, or a negative SDK
 * error. Unknown or finished requests are ignored. */
void ps3_np_social_complete_lookup(uint64_t request, int32_t result, const char* online_id);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_NP_SOCIAL_H */
