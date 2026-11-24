/*
 * ps3recomp - sceNpBasic HLE implementation
 *
 * Offline stub: reports empty friends/block lists, accepts presence
 * updates silently, returns NOT_CONNECTED for messaging. Sufficient
 * for games that check NP Basic but don't require live PSN.
 */

#include "sceNpBasic.h"
#include <stdio.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../../runtime/ppu/ppu_context.h"
#include "ps3emu/guest_call.h"
#include "ps3emu/nid.h"
#include "np_social_internal.h"                /* the opt-in social provider */

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_initialized = 0;
static SceNpBasicEventHandler s_event_handler = NULL;
static SceNpBasicPresenceHandler s_presence_handler = NULL;
static void* s_handler_arg = NULL;
static SceNpBasicPresence s_own_presence;

/* sceNpBasic is usable once sceNpInit has run: in the SDK a title calls
 * sceNpInit and then sceNpBasic*, and many never call sceNpBasicInit. Checking
 * only Basic's own flag answered NOT_INITIALIZED to every presence, message
 * and handler call of such a title. sceNpBasicInit still works for callers
 * that use it. sceNp.h and this file's header cannot share a translation
 * unit, so the accessor is declared here (defined in sceNp.c; the name
 * stays outside the cell/sys/sce prefixes gen_hle_nids.py exports). */
int np_is_initialized(void);
static int basic_ready(void) { return s_initialized || np_is_initialized(); }
extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
/* The opt-in social provider's parts (end of this file). */
static s32 np_basic_social_get_event(u32 event, u32 from, u32 data, u32 size);
static void np_basic_social_drop_handler(void);

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpBasicInit(void* poolMem, u32 poolSize)
{
    (void)poolMem;
    (void)poolSize;
    printf("[sceNpBasic] Init(poolSize=%u)\n", poolSize);

    if (s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_ALREADY_INITIALIZED;

    memset(&s_own_presence, 0, sizeof(s_own_presence));
    s_event_handler = NULL;
    s_presence_handler = NULL;
    s_handler_arg = NULL;
    s_initialized = 1;
    return CELL_OK;
}

s32 sceNpBasicTerm(void)
{
    printf("[sceNpBasic] Term()\n");

    if (!s_initialized)
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    s_initialized = 0;
    s_event_handler = NULL;
    s_presence_handler = NULL;
    np_basic_social_drop_handler();
    return CELL_OK;
}

/* The title's main loop drains PSN events with a loop that exits ONLY when
 * sceNpBasicGetEvent returns the specific code 0x8002A66A (SCE_NP_BASIC_ERROR_NO_EVENT)
 * -- verified in the recompiled drain loop func_00063E34 (cmpwi r3, 0x8002A66A ->
 * exit). Returning any OTHER error (e.g. 0x8002AA09) is NOT recognized: the game
 * then reads a STALE/garbage event out-param and loops forever, hanging the boot
 * before NP-init/trophy/GThread. So the exact value matters. We report no event
 * and don't touch the out-params. */
#ifndef SCE_NP_BASIC_ERROR_NO_EVENT
#define SCE_NP_BASIC_ERROR_NO_EVENT 0x8002A66A
#endif
s32 sceNpBasicGetEvent(u32* event, void* from, void* data, u32* size)
{
    /* out-params are GUEST EAs; the game checks the return (0x8002A66A) and
     * exits the drain BEFORE reading them, so we leave them untouched. */
    if (!np_social_get())
        return (s32)SCE_NP_BASIC_ERROR_NO_EVENT;
    return np_basic_social_get_event((u32)(uintptr_t)event, (u32)(uintptr_t)from,
                                     (u32)(uintptr_t)data, (u32)(uintptr_t)size);
}

s32 sceNpBasicRegisterHandler(SceNpBasicEventHandler handler,
                               SceNpBasicPresenceHandler presenceHandler,
                               void* arg)
{
    printf("[sceNpBasic] RegisterHandler()\n");

    if (!basic_ready())
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    s_event_handler = handler;
    s_presence_handler = presenceHandler;
    s_handler_arg = arg;
    return CELL_OK;
}

s32 sceNpBasicUnregisterHandler(void)
{
    printf("[sceNpBasic] UnregisterHandler()\n");

    if (!basic_ready())
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    s_event_handler = NULL;
    s_presence_handler = NULL;
    s_handler_arg = NULL;
    np_basic_social_drop_handler();
    return CELL_OK;
}

/* The list counts answer without sceNpBasicInit: titles built on libsceNp2
 * never call it (NP init brings Basic up), and Simpsons Arcade asks whether a
 * joining player is on its block list -- an error there reads as "unknown". */
s32 sceNpBasicGetFriendListEntryCount(u32* count)
{
    if (!count)
        return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)count, (u32)0); /* offline -- no friends */
    return CELL_OK;
}

s32 sceNpBasicGetFriendListEntry(u32 index, SceNpBasicFriendListEntry* entry)
{
    (void)index;
    (void)entry;

    if (!basic_ready())
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    return (s32)SCE_NP_BASIC_ERROR_DATA_NOT_FOUND;
}

s32 sceNpBasicGetFriendPresence(const SceNpOnlineId* onlineId,
                                 SceNpBasicPresence* presence)
{
    (void)onlineId;
    (void)presence;

    if (!basic_ready())
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    return (s32)SCE_NP_BASIC_ERROR_DATA_NOT_FOUND;
}

s32 sceNpBasicSetPresence(const SceNpBasicPresence* presence)
{
    presence = GUEST_PTR(presence, const SceNpBasicPresence*);
    printf("[sceNpBasic] SetPresence()\n");

    if (!basic_ready())
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    if (!presence)
        return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;

    s_own_presence = *presence;
    return CELL_OK;
}

static s32 np_basic_social_set_presence(const ps3_np_social_provider* social, u32 details, u32 options);
s32 sceNpBasicSetPresenceDetails(const void* presence, u32 options)
{
    /* Retail imports this independently of the older local presence cache.
     * The exact Basic errors differ from this header's legacy placeholders.
     * Do not fabricate a successful publication or dereference unused payloads. */
    if (!presence || options < 1 || options > 3)
        return (s32)0x8002a665u; /* SCE_NP_BASIC_ERROR_INVALID_ARGUMENT */
    {
        const ps3_np_social_provider* social = np_social_get();
        if (social && social->set_presence)
            return np_basic_social_set_presence(social, (u32)(uintptr_t)presence, options);
    }
    printf("[sceNpBasic] SetPresenceDetails: publishing unavailable (options=%u)\n", options);
    return (s32)0x8002a663u; /* SCE_NP_BASIC_ERROR_NOT_SUPPORTED */
}

s32 sceNpBasicSendMessage(const SceNpOnlineId* to,
                           const void* body, u32 bodySize)
{
    to = GUEST_PTR(to, const SceNpOnlineId*);
    (void)body;
    (void)bodySize;

    printf("[sceNpBasic] SendMessage(to=%.16s)\n",
           to ? to->data : "(null)");

    if (!basic_ready())
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    return (s32)SCE_NP_BASIC_ERROR_NOT_CONNECTED;
}

s32 sceNpBasicSendMessageAttachment(const SceNpOnlineId* to,
                                      const char* subject,
                                      const void* data, u32 dataSize)
{
    to = GUEST_PTR(to, const SceNpOnlineId*);
    (void)data;
    (void)dataSize;

    printf("[sceNpBasic] SendMessageAttachment(to=%.16s, subj=%s)\n",
           to ? to->data : "(null)",
           subject ? subject : "(null)");

    if (!basic_ready())
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    return (s32)SCE_NP_BASIC_ERROR_NOT_CONNECTED;
}

s32 sceNpBasicSendInGameInvitation(const SceNpOnlineId* to,
                                     const void* data, u32 dataSize)
{
    to = GUEST_PTR(to, const SceNpOnlineId*);
    (void)data;
    (void)dataSize;

    printf("[sceNpBasic] SendInGameInvitation(to=%.16s)\n",
           to ? to->data : "(null)");

    if (!basic_ready())
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    return (s32)SCE_NP_BASIC_ERROR_NOT_CONNECTED;
}

s32 sceNpBasicRecvInGameInvitation(void* data, u32 dataMaxSize,
                                     u32* dataSize)
{
    (void)data;
    (void)dataMaxSize;

    if (!basic_ready())
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    if (dataSize)
        vm_write32((u32)(uintptr_t)dataSize, (u32)0);

    return (s32)SCE_NP_BASIC_ERROR_DATA_NOT_FOUND;
}

s32 sceNpBasicGetBlockListEntryCount(u32* count)
{
    if (!count)
        return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)count, (u32)0);
    return CELL_OK;
}

/* The block list is empty (count 0 above), so there is no entry to read. */
s32 sceNpBasicGetBlockListEntry(u32 index, void* npid)
{
    printf("[sceNpBasic] GetBlockListEntry(%u) -> DATA_NOT_FOUND (empty list)\n", index);
    if (!npid)
        return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;
    return (s32)SCE_NP_BASIC_ERROR_DATA_NOT_FOUND;
}

s32 sceNpBasicAddBlockListEntry(const SceNpOnlineId* onlineId)
{
    onlineId = GUEST_PTR(onlineId, const SceNpOnlineId*);
    printf("[sceNpBasic] AddBlockListEntry(%.16s)\n",
           onlineId ? onlineId->data : "(null)");

    if (!basic_ready())
        return (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;

    /* No-op in offline mode */
    return CELL_OK;
}

/* Players met online, and the context-sensitive (XMB) handler: nothing is
 * recorded, the history is empty, the handler is accepted and never fires. */
s32 sceNpBasicGetPlayersHistoryEntryCount(u32 options, u32* count)
{
    (void)options;
    if (!count) return (s32)SCE_NP_BASIC_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)count, 0);
    return CELL_OK;
}

s32 sceNpBasicAddPlayersHistoryAsync(const void* npids, u32 count, const void* description, u32* reqId)
{
    (void)npids; (void)count; (void)description;
    if (reqId) vm_write32((u32)(uintptr_t)reqId, 1);
    return CELL_OK;
}

/* The handler is recorded for the social provider (docs/np-social-provider.md).
 * Without a provider nothing ever reads it, so the title sees no change. */
static u32 s_cs_handler;      /* guest OPD */
static u32 s_cs_arg;          /* guest pointer, passed back */
static int s_cs_registered;

s32 sceNpBasicRegisterContextSensitiveHandler(const void* context, void* handler, void* arg)
{
    (void)context;
    s_cs_handler = (u32)(uintptr_t)handler;
    s_cs_arg = (u32)(uintptr_t)arg;
    s_cs_registered = handler != NULL;
    np_social_handler(s_cs_registered);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Social provider (docs/np-social-provider.md). Everything below is reached
 * only once ps3_np_social_set_provider() has installed a provider.
 * -----------------------------------------------------------------------*/

#define BASIC_INVALID_ARGUMENT ((s32)0x8002a665u)
#define BASIC_NOT_SUPPORTED    ((s32)0x8002a663u)
#define BASIC_EXCEEDS_MAX      ((s32)0x8002a66bu)

/* SDK guest layouts (big-endian). */
#define NPID_SIZE            36   /* SceNpId: handle (16 + NUL + 3 pad), opt 8, reserved 8 */
#define USER_INFO_SIZE       216  /* SceNpUserInfo: SceNpId, online name (48 + NUL + 3), avatar URL 128 */
#define USER_INFO_NAME       36
#define PRESENCE_STATUS      0x80   /* SceNpBasicPresenceDetails: title 128, status 64, comment 64, */
#define PRESENCE_STATUS_SIZE 64     /* data 128, size u32, state s32 */
#define PRESENCE_DATA        0x100
#define PRESENCE_SIZE        0x180
#define BODY_MAX             512
#define SUBJECT_MAX          18
#define ATTACHMENT_MAX       (1024u * 1024u)

static void np_basic_social_drop_handler(void)
{
    if (s_cs_registered) {
        s_cs_registered = 0;
        np_social_handler(0);
    }
}

/* At each cellSysutilCheckCallback: announce queued events to the title's
 * handler, one call per event, outside every lock (the handler may fetch). */
static int s_ticking;
void np_basic_social_tick(void)
{
    if (!np_social_get() || s_ticking) return;
    if (!basic_ready()) { np_basic_social_drop_handler(); return; }
    ps3_guest_caller_fn caller = g_ps3_guest_caller;
    if (!s_cs_registered || !caller) return;
    s_ticking = 1;
    int32_t event, ret;
    static int said;
    while (s_cs_registered && np_social_announce(&event, &ret)) {
        if (!said) { said = 1; fprintf(stderr, "[np-social] first event (%d) announced to the title's handler\n", (int)event); }
        caller(s_cs_handler, (u64)(s64)event, (u64)(s64)ret, 0, s_cs_arg, 0, 0, 0, 0);
    }
    s_ticking = 0;
}

static s32 np_basic_social_get_event(u32 event, u32 from, u32 data, u32 size)
{
    if (!event || !from || !size) return BASIC_INVALID_ARGUMENT;
    np_social_event e;
    if (!np_social_take(&e)) return (s32)SCE_NP_BASIC_ERROR_NO_EVENT;
    static int said;
    if (!said) { said = 1; fprintf(stderr, "[np-social] the title fetched its first event (%d, %u bytes of data)\n", (int)e.event, (unsigned)e.size); }
    u32 room = vm_read32(size);
    u32 n = e.size < room ? e.size : room;
    if (n && !data) n = 0;
    vm_write32(event, (u32)e.event);
    vm_memset(from, 0, USER_INFO_SIZE);
    size_t id_len = strnlen(e.online_id, PS3_NP_SOCIAL_ONLINE_ID_MAX);
    vm_memcpy_to(from, e.online_id, id_len);                   /* NP ID handle */
    vm_memcpy_to(from + USER_INFO_NAME, e.online_id, id_len);  /* online name */
    if (n) vm_memcpy_to(data, e.data, n);
    vm_write32(size, n);
    return CELL_OK;
}

static s32 np_basic_social_set_presence(const ps3_np_social_provider* social, u32 details, u32 options)
{
    uint8_t bytes[PS3_NP_SOCIAL_PRESENCE_MAX];
    char status[PRESENCE_STATUS_SIZE];
    u32 n = 0;
    int has_data = (options & 1) != 0, has_status = (options & 2) != 0;
    if (has_data) {
        n = vm_read32(details + PRESENCE_SIZE);
        if (n > PS3_NP_SOCIAL_PRESENCE_MAX) return BASIC_EXCEEDS_MAX;
        if (n) vm_memcpy_from(bytes, details + PRESENCE_DATA, n);
    }
    status[0] = 0;
    if (has_status) {
        vm_memcpy_from(status, details + PRESENCE_STATUS, PRESENCE_STATUS_SIZE);
        status[PRESENCE_STATUS_SIZE - 1] = 0;
    }
    social->set_presence(social->user, has_data, has_data ? bytes : NULL, n, has_status,
                         has_status ? status : NULL);
    return CELL_OK;
}

/* A guest NUL-terminated string of at most `max` bytes into `out` (max + 1).
 * 1 on success, 0 when it is longer. A zero address is the empty string. */
static int guest_string(u32 ea, char* out, size_t max)
{
    out[0] = 0;
    if (!ea) return 1;
    for (size_t i = 0; i <= max; i++) {
        char c = (char)vm_read8(ea + (u32)i);
        if (i == max) return c == 0;
        out[i] = c;
        if (!c) return 1;
    }
    return 0;
}

/* An SceNpId's handle: 1..16 bytes terminated within its 17. */
static int guest_npid_handle(u32 npid, char out[PS3_NP_SOCIAL_ONLINE_ID_MAX + 1])
{
    if (!npid) return 0;
    if (!guest_string(npid, out, PS3_NP_SOCIAL_ONLINE_ID_MAX)) return 0;
    return out[0] != 0;
}

/* sceNpBasicAddFriend(const SceNpId* contact, const char* body, u32 container) */
static void hle_add_friend(ppu_context* ctx)
{
    const ps3_np_social_provider* social = np_social_get();
    char id[PS3_NP_SOCIAL_ONLINE_ID_MAX + 1];
    static char body[BODY_MAX + 1];
    s32 rc;
    if (!basic_ready()) rc = (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;
    else if (!guest_npid_handle((u32)ctx->gpr[3], id)) rc = BASIC_INVALID_ARGUMENT;
    else if (!guest_string((u32)ctx->gpr[4], body, BODY_MAX)) rc = BASIC_EXCEEDS_MAX;
    else if (!social || !social->add_friend) rc = BASIC_NOT_SUPPORTED;
    else rc = social->add_friend(social->user, id, body);
    printf("[sceNpBasic] AddFriend(%s) -> 0x%08x\n", rc == BASIC_INVALID_ARGUMENT ? "(invalid)" : id, (u32)rc);
    ctx->gpr[3] = (u64)(s64)rc;
}

/* sceNpBasicSendMessageGui(const SceNpBasicMessageDetails* msg, u32 container)
 * SceNpBasicMessageDetails: msgId u64 @0, mainType u16 @8, subType u16 @10,
 * msgFeatures u32 @12, npids @16, count @20, subject @24, body @28, data @32,
 * size @36. */
static void hle_send_message_gui(ppu_context* ctx)
{
    const ps3_np_social_provider* social = np_social_get();
    u32 msg = (u32)ctx->gpr[3];
    static char ids[PS3_NP_SOCIAL_MESSAGE_RECIPIENTS][PS3_NP_SOCIAL_ONLINE_ID_MAX + 1];
    static char subject[SUBJECT_MAX + 1], body[BODY_MAX + 1];
    ps3_np_social_message m;
    memset(&m, 0, sizeof(m));
    s32 rc = CELL_OK;
    if (!basic_ready()) rc = (s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED;
    else if (!msg) rc = BASIC_INVALID_ARGUMENT;
    if (rc == CELL_OK) {
        m.msg_id = vm_read64(msg);
        m.main_type = vm_read16(msg + 8);
        m.sub_type = vm_read16(msg + 10);
        m.features = vm_read32(msg + 12);
        u32 npids = vm_read32(msg + 16);
        m.recipient_count = vm_read32(msg + 20);
        m.data_size = vm_read32(msg + 36);
        if (!npids || m.recipient_count < 1 || m.recipient_count > PS3_NP_SOCIAL_MESSAGE_RECIPIENTS)
            rc = BASIC_INVALID_ARGUMENT;
        for (u32 i = 0; rc == CELL_OK && i < m.recipient_count; i++) {
            if (!guest_npid_handle(npids + i * NPID_SIZE, ids[i])) rc = BASIC_INVALID_ARGUMENT;
            else m.recipients[i] = ids[i];
        }
        if (rc == CELL_OK && (!guest_string(vm_read32(msg + 24), subject, SUBJECT_MAX) ||
                              !guest_string(vm_read32(msg + 28), body, BODY_MAX) ||
                              m.data_size > ATTACHMENT_MAX))
            rc = BASIC_EXCEEDS_MAX;
        u32 data = vm_read32(msg + 32);
        if (rc == CELL_OK && m.data_size && !data) rc = BASIC_INVALID_ARGUMENT;
        if (rc == CELL_OK) {
            m.subject = subject;
            m.body = body;
            m.data = m.data_size ? vm_ptr8(data) : NULL;
            rc = social && social->send_message ? social->send_message(social->user, &m) : BASIC_NOT_SUPPORTED;
        }
    }
    printf("[sceNpBasic] SendMessageGui(type=%u, recipients=%u) -> 0x%08x\n",
           (unsigned)m.main_type, (unsigned)m.recipient_count, (u32)rc);
    ctx->gpr[3] = (u64)(s64)rc;
}

void np_basic_social_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("sceNpBasicAddFriend"), "sceNpBasicAddFriend", hle_add_friend);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpBasicSendMessageGui"), "sceNpBasicSendMessageGui",
                         hle_send_message_gui);
}
