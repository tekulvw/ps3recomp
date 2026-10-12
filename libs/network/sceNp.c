/*
 * ps3recomp - sceNp HLE implementation
 *
 * Provides fake PSN identity so games can proceed through NP checks.
 * The username defaults to "PS3Player" but is configurable.
 */

#include "sceNp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "ps3emu/guest_call.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "np_psnr.h"
#include "../system/cellSysutil.h"


/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_np_initialized = 0;
static int  s_score_initialized = 0;
static char s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH + 1] = "PS3Player";

#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_ticket_lock = SRWLOCK_INIT;
#define TICKET_LOCK() AcquireSRWLockExclusive(&s_ticket_lock)
#define TICKET_UNLOCK() ReleaseSRWLockExclusive(&s_ticket_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_ticket_lock = PTHREAD_MUTEX_INITIALIZER;
#define TICKET_LOCK() pthread_mutex_lock(&s_ticket_lock)
#define TICKET_UNLOCK() pthread_mutex_unlock(&s_ticket_lock)
#endif
static SceNpManagerCallback s_npmgr_cb;
static void* s_npmgr_cb_arg;
static char s_local_ticket[16];
static u32 s_local_ticket_size;
static int s_ticket_pending, s_ticket_delivering;
static int s_status_pending, s_manager_delivering;
static u64 s_np_generation;

static int np_is_initialized(void)
{
    TICKET_LOCK(); int initialized = s_np_initialized; TICKET_UNLOCK();
    return initialized;
}

static int np_local_ticket_enabled(void)
{
    const char* flag = getenv("PS3RECOMP_NP_LOCAL_TICKET");
    return flag && strcmp(flag, "1") == 0;
}

/* Strict ASCII identity; reject rather than silently repairing a wrong name. */
static u32 np_local_account(const char* name, char out[16])
{
    u32 n = 0;
    while (n < 17 && name[n]) {
        unsigned char c = (unsigned char)name[n];
        if (n == 16) return 0;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
        out[n++] = (char)c;
    }
    return n >= 3 ? n : 0;
}

static void np_clear_ticket(void)
{
    s_ticket_pending = 0; s_local_ticket_size = 0;
    memset(s_local_ticket, 0, sizeof(s_local_ticket));
}

static void np_cancel_manager(void)
{
    np_clear_ticket(); s_status_pending = 0;
    s_ticket_delivering = s_manager_delivering = 0;
    ++s_np_generation;
}

static int np_local_ticket_available(void)
{
    if (np_local_ticket_enabled()) return 1;
    TICKET_LOCK(); np_clear_ticket(); TICKET_UNLOCK();
    return 0;
}

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void sceNpSetFakeUsername(const char* username)
{
    if (username) {
        strncpy(s_fake_username, GUEST_PTR(username, const char*),
            SCE_NP_ONLINEID_MAX_LENGTH);
        s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH] = '\0';
    }
}

const char* np_fake_username(void)
{
    return s_fake_username;
}

/* Build a fake NP ID from the current username (PS3_NP_ONLINE_ID wins, so
 * two instances on one machine can be two players). */
static void np_build_fake_id(SceNpId* npId)
{
    memset(npId, 0, sizeof(SceNpId));
    strncpy(npId->handle.data, np_psnr_online_id(), SCE_NP_ONLINEID_MAX_LENGTH);
    npId->handle.term = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr)
{
    (void)poolSize;
    (void)poolPtr;

    printf("[sceNp] Init(poolSize=%u, username=\"%s\")\n",
           poolSize, np_psnr_online_id());

    TICKET_LOCK();
    if (s_np_initialized) { TICKET_UNLOCK(); return SCE_NP_ERROR_ALREADY_INITIALIZED; }
    s_np_initialized = 1;
    ++s_np_generation;
    TICKET_UNLOCK();
    return CELL_OK;
}

s32 sceNpTerm(void)
{
    printf("[sceNp] Term()\n");

    TICKET_LOCK();
    if (!s_np_initialized) { TICKET_UNLOCK(); return SCE_NP_ERROR_NOT_INITIALIZED; }
    s_np_initialized = 0;
    s_npmgr_cb = NULL; s_npmgr_cb_arg = NULL;
    np_cancel_manager();
    TICKET_UNLOCK();
    return CELL_OK;
}

s32 sceNpGetNpId(s32 userId, SceNpId* npId)
{
    (void)userId;

    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* A guest address, like every sibling getter's: writing through it
     * untranslated hit a host address and crashed Simpsons Arcade the first
     * time it asked for its NP ID online. */
    npId = GUEST_PTR(npId, SceNpId*);
    np_build_fake_id(npId);
    printf("[sceNp] GetNpId(user=%d) -> \"%s\"\n", userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetOnlineId(s32 userId, SceNpOnlineId* onlineId)
{
    onlineId = GUEST_PTR(onlineId, SceNpOnlineId*);
    (void)userId;

    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineId, 0, sizeof(SceNpOnlineId));
    strncpy(onlineId->data, np_psnr_online_id(), SCE_NP_ONLINEID_MAX_LENGTH);
    onlineId->term = '\0';

    printf("[sceNp] GetOnlineId(user=%d) -> \"%s\"\n", userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetOnlineName(s32 userId, SceNpOnlineName* onlineName)
{
    onlineName = GUEST_PTR(onlineName, SceNpOnlineName*);
    (void)userId;

    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineName)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineName, 0, sizeof(SceNpOnlineName));
    strncpy(onlineName->data, np_psnr_online_id(),
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);

    printf("[sceNp] GetOnlineName(user=%d) -> \"%s\"\n",
           userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetUserProfile(s32 userId, SceNpUserInfo* userInfo)
{
    userInfo = GUEST_PTR(userInfo, SceNpUserInfo*);
    (void)userId;

    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!userInfo)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(userInfo, 0, sizeof(SceNpUserInfo));
    np_build_fake_id(&userInfo->npId);
    strncpy(userInfo->onlineName.data, np_psnr_online_id(),
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);
    /* Leave avatar URL empty */

    printf("[sceNp] GetUserProfile(user=%d) -> \"%s\"\n",
           userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetAccountRegion(s32 userId, u32* region)
{
    (void)userId;

    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!region)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Region: US (SCEA) = 0x5553 ('US') */
    vm_write32((u32)(uintptr_t)region, (u32)0x5553);

    printf("[sceNp] GetAccountRegion(user=%d) -> US\n", userId);
    return CELL_OK;
}

s32 sceNpGetAccountAge(s32 userId, s32* age)
{
    (void)userId;

    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!age)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)age, 25); /* default adult age */
    printf("[sceNp] GetAccountAge(user=%d) -> 25\n", userId);
    return CELL_OK;
}

s32 sceNpGetMyLanguages(SceNpMyLanguages* langs)
{
    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!langs)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    u32 langs_ea = (u32)(uintptr_t)langs;
    vm_write32(langs_ea + 0, SCE_NP_LANG_ENGLISH);
    vm_write32(langs_ea + 4, 0);
    vm_write32(langs_ea + 8, 0);

    printf("[sceNp] GetMyLanguages() -> English\n");
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP Manager (sign-in state)
 *
 * The toolkit runs with a fake local profile but no live PSN connection. So the
 * manager reports OFFLINE — games gate their online flows on GetStatus and skip
 * them cleanly — while the identity getters still hand back the fake account,
 * matching how NP behaves on a real signed-in-but-disconnected console.
 * Imported by most online-capable titles.
 * -----------------------------------------------------------------------*/

/* No purchased content is provisioned by the local NP implementation.
 * The API returns the number of IDs, so an empty list requires no guest write.
 * In particular, (NULL, 0) is a valid count query. */
s32 sceNpManagerGetEntitlementIdList(void* entIdList, u32 entIdListNum)
{
    (void)entIdList;
    (void)entIdListNum;
    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;
    printf("[sceNp] GetEntitlementIdList -> 0 (no purchased content)\n");
    return 0;
}

s32 sceNpManagerGetStatus(s32* status)
{
    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!status)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    /* `status` is a GUEST address -- the HLE ABI adapter passes pointer
     * parameters straight through as guest values, so dereferencing one
     * writes to whatever host address shares that number. Same trap as
     * cellGcmSys had. Tokyo Jungle calls this during its online init. */
    /* Signed in when there is a psnr server to be signed in to. */
    int local = np_local_ticket_available();
    s32 st = (np_psnr_enabled() || local) ? SCE_NP_MANAGER_STATUS_ONLINE : SCE_NP_MANAGER_STATUS_OFFLINE;
    vm_write32((uint32_t)(uintptr_t)status, (uint32_t)st);
    { static s32 last = -2;
      if (st != last) printf("[sceNp] ManagerGetStatus() -> %s\n", st < 0 ? "OFFLINE" : "ONLINE");
      last = st; }
    return CELL_OK;
}

s32 sceNpManagerRegisterCallback(SceNpManagerCallback callback, void* arg)
{
    TICKET_LOCK();
    if (!s_np_initialized) { TICKET_UNLOCK(); return SCE_NP_ERROR_NOT_INITIALIZED; }
    np_cancel_manager();
    s_npmgr_cb     = callback;
    s_npmgr_cb_arg = arg;
    s_status_pending = callback && (np_psnr_enabled() || np_local_ticket_enabled());
    TICKET_UNLOCK();
    printf("[sceNp] ManagerRegisterCallback()\n");
    /* Sign-in and ticket notifications share a cancellable registration. */
    return CELL_OK;
}

s32 sceNpManagerUnregisterCallback(void)
{
    TICKET_LOCK();
    s_npmgr_cb     = NULL;
    s_npmgr_cb_arg = NULL;
    np_cancel_manager();
    TICKET_UNLOCK();
    return CELL_OK;
}

s32 sceNpManagerRequestTicket2(const SceNpId* npId, const SceNpTicketVersion* version,
    const char* serviceId, const void* cookie, u32 cookieSize,
    const char* entitlementId, u32 consumedCount)
{
    TICKET_LOCK();
    if (!s_np_initialized) { TICKET_UNLOCK(); return SCE_NP_ERROR_NOT_INITIALIZED; }
    u64 generation = s_np_generation;
    TICKET_UNLOCK();
    if (!npId || !serviceId || cookieSize > 1024 || (cookieSize && !cookie))
        return SCE_NP_AUTH_EINVALID_ARGUMENT;
    if (!np_local_ticket_available()) return SCE_NP_ERROR_OFFLINE;
    /* No cookie/entitlement authority exists in this local provider. */
    if (cookieSize || entitlementId || consumedCount) return (s32)CELL_ENOSYS;
    if (version && (vm_read16((u32)(uintptr_t)version) != 2 ||
                   vm_read16((u32)(uintptr_t)version + 2) != 1))
        return SCE_NP_AUTH_EINVALID_ARGUMENT;
    if (!vm_read8((u32)(uintptr_t)serviceId)) return SCE_NP_AUTH_EINVALID_ARGUMENT;
    const SceNpId* id = GUEST_PTR(npId, const SceNpId*);
    char requested[16], configured[16];
    if (id->handle.term) return SCE_NP_AUTH_EINVALID_ARGUMENT;
    u32 size = np_local_account(id->handle.data, requested);
    u32 configured_size = np_local_account(np_psnr_online_id(), configured);
    if (!size || !configured_size) return SCE_NP_AUTH_EINVALID_ARGUMENT;
    if (size != configured_size || memcmp(requested, configured, size))
        return SCE_NP_ERROR_DIFFERENT_USER;
    TICKET_LOCK();
    if (!s_np_initialized || generation != s_np_generation) {
        TICKET_UNLOCK(); return SCE_NP_ERROR_NOT_INITIALIZED;
    }
    if (!s_npmgr_cb) { TICKET_UNLOCK(); return SCE_NP_ERROR_INVALID_STATE; }
    if (s_ticket_pending || s_ticket_delivering) { TICKET_UNLOCK(); return SCE_NP_AUTH_EBUSY; }
    memcpy(s_local_ticket, requested, size); s_local_ticket_size = size;
    s_ticket_pending = 1;
    TICKET_UNLOCK();
    printf("[sceNp] RequestTicket2: local account payload pending (%u bytes, not PSN)\n", size);
    return CELL_OK;
}

/* Delivered only on the game's normal sysutil polling thread. Keep the request
 * busy during guest execution and retain it if no guest dispatcher exists. */
void ps3_np_ticket_tick(void)
{
    TICKET_LOCK();
    ps3_guest_caller_fn caller = g_ps3_guest_caller;
    if (!np_local_ticket_enabled()) {
        np_clear_ticket();
        if (!np_psnr_enabled()) s_status_pending = 0;
    }
    if (!s_np_initialized || (!s_status_pending && !s_ticket_pending) || s_manager_delivering || !s_npmgr_cb || !caller) {
        TICKET_UNLOCK(); return;
    }
    u32 opd = (u32)(uintptr_t)s_npmgr_cb, arg = (u32)(uintptr_t)s_npmgr_cb_arg;
    u64 generation = s_np_generation;
    u32 event = s_status_pending ? SCE_NP_MANAGER_STATUS_ONLINE : SCE_NP_MANAGER_EVENT_GOT_TICKET;
    u32 result = s_status_pending ? 0 : s_local_ticket_size;
    if (s_status_pending) s_status_pending = 0;
    else { s_ticket_pending = 0; s_ticket_delivering = 1; }
    s_manager_delivering = 1;
    TICKET_UNLOCK();
    caller(opd, event, result, arg, 0, 0, 0, 0, 0);
    TICKET_LOCK();
    if (generation == s_np_generation) s_ticket_delivering = s_manager_delivering = 0;
    TICKET_UNLOCK();
}

s32 sceNpManagerGetTicket(void* buffer, u32* bufferSize)
{
    if (!np_is_initialized()) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!bufferSize) return SCE_NP_ERROR_INVALID_ARGUMENT;
    if (!np_local_ticket_available()) return SCE_NP_ERROR_OFFLINE;
    TICKET_LOCK();
    if (!s_np_initialized) { TICKET_UNLOCK(); return SCE_NP_ERROR_NOT_INITIALIZED; }
    if (!s_local_ticket_size || s_ticket_pending) { TICKET_UNLOCK(); return SCE_NP_ERROR_INVALID_STATE; }
    u32 size_ea = (u32)(uintptr_t)bufferSize, size = s_local_ticket_size;
    if (buffer && vm_read32(size_ea) < size) {
        vm_write32(size_ea, size); TICKET_UNLOCK(); return SCE_NP_ERROR_INSUFFICIENT_BUFFER;
    }
    if (buffer) vm_memcpy_to((u32)(uintptr_t)buffer, s_local_ticket, size);
    vm_write32(size_ea, size);
    TICKET_UNLOCK();
    printf("[sceNp] GetTicket: local account payload (%u bytes, not PSN)\n", size);
    return CELL_OK;
}

/* Identity getters: reuse the fake-profile implementations (offline-with-account). */
s32 sceNpManagerGetNpId(SceNpId* npId)               { return sceNpGetNpId(0, npId); }
s32 sceNpManagerGetOnlineId(SceNpOnlineId* onlineId) { return sceNpGetOnlineId(0, onlineId); }
s32 sceNpManagerGetOnlineName(SceNpOnlineName* name) { return sceNpGetOnlineName(0, name); }
s32 sceNpManagerGetAccountAge(s32* age)              { return sceNpGetAccountAge(0, age); }

/* Parental controls: an unrestricted adult account. Unimplemented, these
 * returned OK without writing, and the title read whatever was in its stack. */
s32 sceNpManagerGetContentRatingFlag(s32* isRestricted, s32* age)
{
    if (!np_is_initialized()) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (isRestricted) vm_write32((u32)(uintptr_t)isRestricted, 0);
    if (age) vm_write32((u32)(uintptr_t)age, 25);
    return CELL_OK;
}

s32 sceNpManagerGetChatRestrictionFlag(s32* isRestricted)
{
    if (!np_is_initialized()) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (isRestricted) vm_write32((u32)(uintptr_t)isRestricted, 0);
    return CELL_OK;
}

/* Score setup is local even while the NP manager is offline. Games may
 * initialize it before starting the worker that decides whether to use PSN. */
s32 sceNpScoreInit(void)
{
    if (s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED;
    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_score_initialized = 1;
    return CELL_OK;
}

s32 sceNpScoreTerm(void)
{
    if (!s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    if (!np_is_initialized())
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_score_initialized = 0;
    return CELL_OK;
}
