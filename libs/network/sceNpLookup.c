/*
 * ps3recomp - sceNpLookup (player profiles and avatars), answered locally
 *
 * A player's profile is their online ID: that is all psnr knows about anyone,
 * so the lookup needs no server. The name comes back as the NP ID's handle,
 * with an empty "about me", English, country "us" and no avatar (an image
 * of size 0). Transactions complete immediately; PollAsync reports done.
 *
 * Simpsons Arcade retries Lookup title-context creation every frame until it
 * succeeds and holds its online session until it does, so a context that
 * merely "fakes OK" (id 0) kept every match from starting.
 *
 * Layouts (SDK ABI):
 *   SceNpUserInfo { SceNpId userId (36); SceNpOnlineName name (48); SceNpAvatarUrl icon (128) }
 *   SceNpAvatarImage { u8 data[200 KB]; u32 size; u8 reserved[12] }
 *   SceNpMyLanguages { s32 language1..3; pad } / SceNpCountryCode { char data[2]; term; pad }
 */

#include "sceNp.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../runtime/ppu/ppu_context.h"
#include "ps3emu/nid.h"
#include "np_social_internal.h"   /* the opt-in social provider's lookups */

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define LOOKUP_ERROR_INVALID_ID   0x8002A106u
#define AVATAR_IMAGE_DATA_SIZE    (200u * 1024u)

#define TITLE_MAX 8
#define TRANS_MAX 32

static int s_lookup_init;
static int s_titles[TITLE_MAX + 1];
static int s_trans[TRANS_MAX + 1];   /* 0 free, else owning title */

/* A social-provider NP ID lookup on a transaction (docs/np-social-provider.md).
 * Only sceNpLookupNpIdAsync, registered with a provider, sets one up; every
 * other transaction keeps lookup = 0 and its old answers. */
typedef struct {
    int      lookup;     /* 0 none, 1 running, 2 finished */
    uint64_t request;    /* the provider's request while running */
    u32      npid;       /* guest SceNpId out-parameter */
    s32      result;     /* once finished */
} TransLookup;
static TransLookup s_lookup[TRANS_MAX + 1];

#define COMMUNITY_INVALID_ARGUMENT  0x8002A104u
#define COMMUNITY_ABORTED           0x8002A109u
#define COMMUNITY_TIMEOUT           0x8002A10Fu
#define COMMUNITY_BUSY              0x8002A1ABu   /* BUSY_BY_ANOTHER_TRANSACTION */
#define LOOKUP_WAIT_MS              30000
#define LOOKUP_WAIT_STEP_MS         10

static void lookup_finish(int t, s32 result)
{
    if (s_lookup[t].lookup == 1) np_social_lookup_drop(s_lookup[t].request);
    s_lookup[t].lookup = 2;
    s_lookup[t].request = 0;
    s_lookup[t].result = result;
}

/* Collect a provider completion: 1 when the lookup is finished (now or before). */
static int lookup_settle(int t)
{
    TransLookup* l = &s_lookup[t];
    if (l->lookup == 2) return 1;
    int32_t result = 0;
    char id[PS3_NP_SOCIAL_ONLINE_ID_MAX + 1];
    int state = np_social_lookup_poll(l->request, &result, id);
    if (state == 0) return 0;
    if (state < 0) result = (s32)COMMUNITY_ABORTED;
    else if (result == PS3_NP_SOCIAL_LOOKUP_FOUND && l->npid) {
        vm_memset(l->npid, 0, 36);
        vm_memcpy_to(l->npid, id, strnlen(id, PS3_NP_SOCIAL_ONLINE_ID_MAX));
    }
    lookup_finish(t, result);
    return 1;
}

s32 sceNpLookupInit(void)  { s_lookup_init = 1; return CELL_OK; }
s32 sceNpLookupTerm(void)  { s_lookup_init = 0; return CELL_OK; }

s32 sceNpLookupCreateTitleCtx(u32 commId, u32 selfNpId)
{
    (void)commId; (void)selfNpId;
    for (int i = 1; i <= TITLE_MAX; i++)
        if (!s_titles[i]) { s_titles[i] = 1; return i; }
    return (s32)SCE_NP_ERROR_OUT_OF_MEMORY;
}

s32 sceNpLookupDestroyTitleCtx(s32 titleCtxId)
{
    if (titleCtxId < 1 || titleCtxId > TITLE_MAX || !s_titles[titleCtxId])
        return (s32)LOOKUP_ERROR_INVALID_ID;
    s_titles[titleCtxId] = 0;
    return CELL_OK;
}

s32 sceNpLookupCreateTransactionCtx(s32 titleCtxId)
{
    if (titleCtxId < 1 || titleCtxId > TITLE_MAX || !s_titles[titleCtxId])
        return (s32)LOOKUP_ERROR_INVALID_ID;
    for (int i = 1; i <= TRANS_MAX; i++)
        if (!s_trans[i]) { s_trans[i] = titleCtxId; return i; }
    return (s32)SCE_NP_ERROR_OUT_OF_MEMORY;
}

s32 sceNpLookupDestroyTransactionCtx(s32 transId)
{
    if (transId < 1 || transId > TRANS_MAX || !s_trans[transId])
        return (s32)LOOKUP_ERROR_INVALID_ID;
    if (s_lookup[transId].lookup == 1) np_social_lookup_drop(s_lookup[transId].request);
    memset(&s_lookup[transId], 0, sizeof(s_lookup[transId]));
    s_trans[transId] = 0;
    return CELL_OK;
}

static int trans_ok(s32 id) { return id >= 1 && id <= TRANS_MAX && s_trans[id]; }

s32 sceNpLookupAbortTransaction(s32 transId)
{
    if (!trans_ok(transId)) return (s32)LOOKUP_ERROR_INVALID_ID;
    if (s_lookup[transId].lookup == 1 && !lookup_settle(transId))
        lookup_finish(transId, (s32)COMMUNITY_ABORTED);
    return CELL_OK;
}

/* Everything completes at the call, except a provider lookup still running. */
s32 sceNpLookupPollAsync(s32 transId, u32 result)
{
    if (!trans_ok(transId)) return (s32)LOOKUP_ERROR_INVALID_ID;
    if (s_lookup[transId].lookup) {
        if (!lookup_settle(transId)) return 1;
        if (result) vm_write32(result, (u32)s_lookup[transId].result);
        return 0;
    }
    if (result) vm_write32(result, 0);
    return 0;
}

static void lookup_sleep_step(void)
{
#ifdef _WIN32
    Sleep(LOOKUP_WAIT_STEP_MS);
#else
    struct timespec ts = {0, LOOKUP_WAIT_STEP_MS * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

s32 sceNpLookupWaitAsync(s32 transId, u32 result)
{
    if (trans_ok(transId) && s_lookup[transId].lookup == 1) {
        for (int waited = 0; !lookup_settle(transId); waited += LOOKUP_WAIT_STEP_MS) {
            if (waited >= LOOKUP_WAIT_MS) {
                fprintf(stderr, "[sceNpLookup] NP ID lookup: no answer in %d s; giving up\n",
                        LOOKUP_WAIT_MS / 1000);
                lookup_finish(transId, (s32)COMMUNITY_TIMEOUT);
                break;
            }
            lookup_sleep_step();
        }
    }
    return sceNpLookupPollAsync(transId, result);
}

static void empty_avatar(u32 image)
{
    if (image) vm_write32(image + AVATAR_IMAGE_DATA_SIZE, 0);   /* size = 0 */
}

/* (transId, npId*, userInfo*, aboutMe*, languages*, countryCode*, avatarImage*, prio, option) */
s32 sceNpLookupUserProfileAsync(s32 transId, u32 npId, u32 userInfo, u32 aboutMe,
                                u32 languages, u32 countryCode, u32 avatarImage, s32 prio)
{
    (void)prio;
    if (!trans_ok(transId)) return (s32)LOOKUP_ERROR_INVALID_ID;
    if (!npId) return (s32)SCE_NP_ERROR_INVALID_ARGUMENT;
    if (userInfo) {
        memset(vm_ptr8(userInfo), 0, 36 + 48 + 128);
        memcpy(vm_ptr8(userInfo), vm_ptr8(npId), 36);               /* userId */
        memcpy(vm_ptr8(userInfo + 36), vm_ptr8(npId), 16);          /* name = handle */
    }
    if (aboutMe) memset(vm_ptr8(aboutMe), 0, 64);
    if (languages) {
        vm_write32(languages, SCE_NP_LANG_ENGLISH);
        vm_write32(languages + 4, 0);
        vm_write32(languages + 8, 0);
    }
    if (countryCode) memcpy(vm_ptr8(countryCode), "us\0", 4);
    empty_avatar(avatarImage);
    return CELL_OK;
}

s32 sceNpLookupUserProfile(s32 transId, u32 npId, u32 userInfo, u32 aboutMe,
                           u32 languages, u32 countryCode, u32 avatarImage)
{
    return sceNpLookupUserProfileAsync(transId, npId, userInfo, aboutMe,
                                       languages, countryCode, avatarImage, 0);
}

/* (transId, avatarUrl*, avatarImage*, prio, option) */
s32 sceNpLookupAvatarImageAsync(s32 transId, u32 avatarUrl, u32 avatarImage, s32 prio, u32 option)
{
    (void)avatarUrl; (void)prio; (void)option;
    if (!trans_ok(transId)) return (s32)LOOKUP_ERROR_INVALID_ID;
    empty_avatar(avatarImage);
    return CELL_OK;
}

s32 sceNpLookupAvatarImage(s32 transId, u32 avatarUrl, u32 avatarImage, u32 option)
{
    return sceNpLookupAvatarImageAsync(transId, avatarUrl, avatarImage, 0, option);
}

/* ---------------------------------------------------------------------------
 * NP ID lookup through the social provider (docs/np-social-provider.md).
 * Registered only by ps3_np_social_set_provider(); without a provider these
 * imports stay unresolved, as before.
 * -----------------------------------------------------------------------*/

/* (transId, onlineId*, npId*, prio, option*) */
static s32 lookup_npid_start(s32 transId, u32 online_id, u32 npid)
{
    if (!trans_ok(transId)) return (s32)LOOKUP_ERROR_INVALID_ID;
    if (!online_id || !npid) return (s32)COMMUNITY_INVALID_ARGUMENT;
    char id[PS3_NP_SOCIAL_ONLINE_ID_MAX + 1];
    size_t n = 0;
    for (; n <= PS3_NP_SOCIAL_ONLINE_ID_MAX; n++) {
        id[n] = (char)vm_read8(online_id + (u32)n);
        if (!id[n]) break;
    }
    if (n == 0 || n > PS3_NP_SOCIAL_ONLINE_ID_MAX) return (s32)COMMUNITY_INVALID_ARGUMENT;
    if (s_lookup[transId].lookup == 1) return (s32)COMMUNITY_BUSY;
    const ps3_np_social_provider* social = np_social_get();
    TransLookup* l = &s_lookup[transId];
    memset(l, 0, sizeof(*l));
    l->npid = npid;
    if (!social || !social->lookup_online_id) {
        l->lookup = 2;
        l->result = PS3_NP_SOCIAL_LOOKUP_NOT_FOUND;
        return CELL_OK;
    }
    l->request = np_social_lookup_start(id);
    if (!l->request) {   /* every provider slot busy */
        l->lookup = 0;
        return (s32)COMMUNITY_BUSY;
    }
    l->lookup = 1;
    social->lookup_online_id(social->user, id, l->request);
    return CELL_OK;
}

static void hle_lookup_npid_async(ppu_context* ctx)
{
    s32 rc = lookup_npid_start((s32)ctx->gpr[3], (u32)ctx->gpr[4], (u32)ctx->gpr[5]);
    printf("[sceNpLookup] NpIdAsync(trans=%d) -> 0x%08x\n", (int)(s32)ctx->gpr[3], (u32)rc);
    ctx->gpr[3] = (u64)(s64)rc;
}

/* The blocking form: the asynchronous call, then the wait; answers the result. */
static void hle_lookup_npid(ppu_context* ctx)
{
    s32 trans = (s32)ctx->gpr[3];
    s32 rc = lookup_npid_start(trans, (u32)ctx->gpr[4], (u32)ctx->gpr[5]);
    if (rc == CELL_OK) {
        sceNpLookupWaitAsync(trans, 0);
        rc = s_lookup[trans].result;
    }
    printf("[sceNpLookup] NpId(trans=%d) -> 0x%08x\n", (int)trans, (u32)rc);
    ctx->gpr[3] = (u64)(s64)rc;
}

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

void np_lookup_social_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("sceNpLookupNpIdAsync"), "sceNpLookupNpIdAsync", hle_lookup_npid_async);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpLookupNpId"), "sceNpLookupNpId", hle_lookup_npid);
}
