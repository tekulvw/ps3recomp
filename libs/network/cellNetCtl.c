/*
 * ps3recomp - cellNetCtl HLE implementation
 *
 * Provides network control state and info queries.  Returns sensible
 * defaults so that games see an active wired network connection.
 */

#include "cellNetCtl.h"
#include "cellSysutil.h"   /* cellSysutilQueueEvent, CELL_SYSUTIL_MAX_CALLBACKS */
#include "ps3emu/guest_call.h" /* g_ps3_guest_caller -- cellGameUpdate answers */
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_write32 (guest mem) */
#include "ps3emu/endian.h" /* ps3_bswap32 -- CellNetCtlInfo/NatInfo integer fields are guest big-endian */
#include "np_psnr.h"      /* np_psnr_identity -- the player name the MAC derives from */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* The generic HLE adapter passes GUEST addresses for pointer args; translate to
 * a host pointer (struct out-params) or write scalars big-endian via vm_write32. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <ifaddrs.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_netctl_initialized = 0;

/* Offline by default: titles gate their whole online boot path on this state
 * and hang waiting on PSN jobs that can never complete if we claim a live
 * connection. A real console with no link reports Disconnected and games take
 * their offline path (verified against LBP's working RPCS3 boot, where
 * GetState never reported a connection). PS3_NET_ONLINE=1 restores the old
 * fake-connected behaviour for online experiments. */
static int netctl_online(void)
{
    static int v = -1;
    if (v < 0) v = getenv("PS3_NET_ONLINE") ? 1 : 0;
    return v;
}

typedef struct {
    int              in_use;
    cellNetCtlHandler handler;
    void*            arg;
} NetCtlHandlerSlot;

static NetCtlHandlerSlot s_handlers[CELL_NET_CTL_HANDLER_MAX];

/* Try to get the host machine's IP address */
static int netctl_get_host_ip(char* buf, size_t buflen)
{
#ifdef _WIN32
    WSADATA wsa;
    char hostname[256];
    struct addrinfo hints, *res;
    int ret;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        goto fallback;

    if (gethostname(hostname, sizeof(hostname)) != 0) {
        WSACleanup();
        goto fallback;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(hostname, NULL, &hints, &res);
    if (ret != 0 || !res) {
        WSACleanup();
        goto fallback;
    }

    {
        struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
        u8* ip = (u8*)&addr->sin_addr;
        snprintf(buf, buflen, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    freeaddrinfo(res);
    /* Don't WSACleanup here -- we may need it later */
    return 1;
#else
    char hostname[256];
    struct addrinfo hints, *res;
    int ret;

    if (gethostname(hostname, sizeof(hostname)) != 0)
        goto fallback;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(hostname, NULL, &hints, &res);
    if (ret != 0 || !res)
        goto fallback;

    {
        struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
        u8* ip = (u8*)&addr->sin_addr;
        snprintf(buf, buflen, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    freeaddrinfo(res);
    return 1;
#endif

fallback:
    snprintf(buf, buflen, "192.168.1.100");
    return 0;
}

/* Explicit per-process test readiness only. A hostname lookup can block the
 * guest's caller indefinitely on local mDNS. Unset retains the original route;
 * a present but malformed override is refused, never sent to the resolver. */
static int netctl_parse_ipv4_override(const char* text, char output[16])
{
    size_t length = 0;
    while (length < 16 && text[length]) ++length;
    if (!length || length == 16) return 0;
    const char* p = text;
    for (unsigned part = 0; part < 4; ++part) {
        const char* first = p;
        unsigned value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 3) return 0;
            value = value * 10 + (unsigned)(*p++ - '0');
        }
        if (!digits || value > 255 || (digits > 1 && *first == '0')) return 0;
        if (part < 3) { if (*p++ != '.') return 0; }
        else if (*p) return 0;
    }
    memcpy(output, text, length + 1);
    return 1;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellNetCtlInit(void)
{
    printf("[cellNetCtl] Init()\n");

    if (s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_TERMINATED;

    memset(s_handlers, 0, sizeof(s_handlers));
    s_netctl_initialized = 1;
    return CELL_OK;
}

void cellNetCtlTerm(void)
{
    printf("[cellNetCtl] Term()\n");

    memset(s_handlers, 0, sizeof(s_handlers));
    s_netctl_initialized = 0;
}

s32 cellNetCtlGetState(s32* state)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!state)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    if (!netctl_online()) {
        /* Offline: FAIL the query rather than report state=Disconnected.
         * LBP's connect job (sub_F433C(0)) polls GetState forever until
         * IPObtained and only exits on ret<0 -- and the working RPCS3 boot
         * shows exactly one GetState call failing with NOT_INITIALIZED,
         * after which the game proceeds offline. Still write a sane state
         * for callers that ignore the return value. */
        vm_write32((uint32_t)(uintptr_t)state, CELL_NET_CTL_STATE_Disconnected);
        printf("[cellNetCtl] GetState() -> error NOT_INITIALIZED (offline mode)\n");
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;
    }

    vm_write32((uint32_t)(uintptr_t)state, CELL_NET_CTL_STATE_IPObtained);
    printf("[cellNetCtl] GetState() -> IPObtained\n");
    return CELL_OK;
}

s32 cellNetCtlGetInfo(s32 code, CellNetCtlInfo* info)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!info)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    info = GUEST_PTR(info, CellNetCtlInfo*);
    memset(info, 0, sizeof(CellNetCtlInfo));

    switch (code)
    {
    case CELL_NET_CTL_INFO_DEVICE:
        info->device = ps3_bswap32((u32)CELL_NET_CTL_DEVICE_WIRED);
        break;

    case CELL_NET_CTL_INFO_ETHER_ADDR:
        /* Fake PS3-like MAC: 00:04:1F:xx:xx:xx (Sony OUI). With a player name
         * (np_psnr_identity) the low three bytes come from it, so two players
         * are two consoles; otherwise the historical AB:CD:EF. */
        {
            const char* id = np_psnr_identity();
            uint32_t h = 0xABCDEFu;
            if (id && *id) {
                h = 2166136261u;   /* FNV-1a */
                for (const char* p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
            }
            info->ether_addr.data[0] = 0x00;
            info->ether_addr.data[1] = 0x04;
            info->ether_addr.data[2] = 0x1F;
            info->ether_addr.data[3] = (uint8_t)(h >> 16);
            info->ether_addr.data[4] = (uint8_t)(h >> 8);
            info->ether_addr.data[5] = (uint8_t)h;
        }
        break;

    case CELL_NET_CTL_INFO_MTU:
        info->mtu = ps3_bswap32(1500u);
        break;

    case CELL_NET_CTL_INFO_LINK:
        info->link = ps3_bswap32((u32)(netctl_online() ? CELL_NET_CTL_LINK_CONNECTED
                                                       : CELL_NET_CTL_LINK_DISCONNECTED));
        break;

    case CELL_NET_CTL_INFO_LINK_TYPE:
        info->link_type = ps3_bswap32((u32)CELL_NET_CTL_LINK_TYPE_1000BASE_FULL);
        break;

    case CELL_NET_CTL_INFO_IP_ADDRESS:
        {
            const char* override = getenv("PS3_NETCTL_IPV4_OVERRIDE");
            if (override) {
                if (!netctl_parse_ipv4_override(override, info->ip_address)) {
                    fprintf(stderr, "[cellNetCtl] GetInfo(IP_ADDRESS) refused invalid PS3_NETCTL_IPV4_OVERRIDE\n");
                    return CELL_NET_CTL_ERROR_NOT_AVAIL;
                }
                printf("[cellNetCtl] GetInfo(IP_ADDRESS) using explicit per-process IPv4 override\n");
            } else {
                netctl_get_host_ip(info->ip_address, sizeof(info->ip_address));
            }
        }
        break;

    case CELL_NET_CTL_INFO_NETMASK:
        strncpy(info->netmask, "255.255.255.0", sizeof(info->netmask) - 1);
        break;

    case CELL_NET_CTL_INFO_DEFAULT_ROUTE:
        strncpy(info->default_route, "192.168.1.1",
                sizeof(info->default_route) - 1);
        break;

    case CELL_NET_CTL_INFO_PRIMARY_DNS:
        strncpy(info->primary_dns, "8.8.8.8", sizeof(info->primary_dns) - 1);
        break;

    case CELL_NET_CTL_INFO_SECONDARY_DNS:
        strncpy(info->secondary_dns, "8.8.4.4",
                sizeof(info->secondary_dns) - 1);
        break;

    case CELL_NET_CTL_INFO_IP_CONFIG:
        info->ip_config = 0; /* DHCP */
        break;

    case CELL_NET_CTL_INFO_HTTP_PROXY_CONFIG:
        info->http_proxy_config = 0; /* disabled */
        break;

    case CELL_NET_CTL_INFO_UPNP_CONFIG:
        info->upnp_config = ps3_bswap32(1u); /* enabled */
        break;

    default:
        printf("[cellNetCtl] GetInfo(code=%d) - unknown code\n", code);
        return CELL_NET_CTL_ERROR_INVALID_CODE;
    }

    printf("[cellNetCtl] GetInfo(code=%d) -> OK\n", code);
    return CELL_OK;
}

s32 cellNetCtlGetNatInfo(CellNetCtlNatInfo* natInfo)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!natInfo)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    natInfo = GUEST_PTR(natInfo, CellNetCtlNatInfo*);
    natInfo->size        = ps3_bswap32((u32)sizeof(CellNetCtlNatInfo));
    natInfo->upnp_status = 0;
    natInfo->stun_status = 0;
    natInfo->nat_type    = ps3_bswap32((u32)CELL_NET_CTL_NATINFO_NAT_TYPE_2); /* moderate */
    natInfo->mapped_addr = 0;

    printf("[cellNetCtl] GetNatInfo() -> NAT Type 2\n");
    return CELL_OK;
}

s32 cellNetCtlAddHandler(cellNetCtlHandler handler, void* arg, s32* hid)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (!handler || !hid)
        return CELL_NET_CTL_ERROR_INVALID_ADDR;

    for (s32 i = 0; i < CELL_NET_CTL_HANDLER_MAX; i++) {
        if (!s_handlers[i].in_use) {
            s_handlers[i].in_use  = 1;
            s_handlers[i].handler = handler;
            s_handlers[i].arg     = arg;
            vm_write32((uint32_t)(uintptr_t)hid, (uint32_t)i);   /* guest out-param */
            printf("[cellNetCtl] AddHandler(hid=%d)\n", i);
            return CELL_OK;
        }
    }

    return CELL_NET_CTL_ERROR_HANDLER_MAX;
}

s32 cellNetCtlDelHandler(s32 hid)
{
    if (!s_netctl_initialized)
        return CELL_NET_CTL_ERROR_NOT_INITIALIZED;

    if (hid < 0 || hid >= CELL_NET_CTL_HANDLER_MAX)
        return CELL_NET_CTL_ERROR_INVALID_ID;

    if (!s_handlers[hid].in_use)
        return CELL_NET_CTL_ERROR_ID_NOT_FOUND;

    s_handlers[hid].in_use  = 0;
    s_handlers[hid].handler = NULL;
    s_handlers[hid].arg     = NULL;

    printf("[cellNetCtl] DelHandler(hid=%d)\n", hid);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Net-start ("connect to PSN") dialog
 *
 * Real hardware shows a system dialog and signals progress asynchronously
 * through the cellSysutil callback queue (LOADED -> FINISHED), after which the
 * game calls UnloadAsync to retrieve the result. The headless dialog posts
 * LOADED + FINISHED immediately. Unload must match the configured network
 * state: reporting success while disconnected strands titles waiting for a
 * connection that never happened. An offline dialog completes as canceled;
 * the explicit online mode retains its existing successful result.
 * -----------------------------------------------------------------------*/

static void netstart_broadcast(u32 status)
{
    /* Sysutil events target a specific slot; the game may have registered its
     * callback on any of them, so notify all and let it filter by status. */
    for (int slot = 0; slot < CELL_SYSUTIL_MAX_CALLBACKS; ++slot)
        cellSysutilQueueEvent(slot, status, 0);
}

s32 cellNetCtlNetStartDialogLoadAsync(const CellNetCtlNetStartDialogParam* param)
{
    (void)param;
    printf("[cellNetCtl] NetStartDialogLoadAsync() -> headless completion\n");
    netstart_broadcast(CELL_SYSUTIL_NET_CTL_NETSTART_LOADED);
    netstart_broadcast(CELL_SYSUTIL_NET_CTL_NETSTART_FINISHED);
    return CELL_OK;
}

s32 cellNetCtlNetStartDialogAbortAsync(void)
{
    printf("[cellNetCtl] NetStartDialogAbortAsync()\n");
    return CELL_OK;
}

s32 cellNetCtlNetStartDialogUnloadAsync(CellNetCtlNetStartDialogResult* result)
{
    u32 dialog_result = netctl_online() ? CELL_OK : CELL_NET_CTL_ERROR_DIALOG_CANCELED;
    if (result)
        vm_write32((u32)(uintptr_t)result + 4, dialog_result);
    printf("[cellNetCtl] NetStartDialogUnloadAsync() -> result=0x%08x\n", dialog_result);
    netstart_broadcast(CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Game update check (cellGameUpdate*)
 *
 * Behaviour is specified in docs/cell-game-update.md. In short: there is no
 * patch server, so a check always answers "no update" (and Finish answers
 * "finished"). Answers are recorded here and handed to the title's callback
 * from its own cellSysutilCheckCallback (cellGameUpdate_tick), never from
 * inside the call that asked, so a title that arms its wait state after the
 * call still sees the answer. Term drops answers not yet delivered.
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_gu_lock = SRWLOCK_INIT;
#define GU_LOCK()   AcquireSRWLockExclusive(&s_gu_lock)
#define GU_UNLOCK() ReleaseSRWLockExclusive(&s_gu_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_gu_lock = PTHREAD_MUTEX_INITIALIZER;
#define GU_LOCK()   pthread_mutex_lock(&s_gu_lock)
#define GU_UNLOCK() pthread_mutex_unlock(&s_gu_lock)
#endif

typedef struct {
    int pending;
    u32 cb_opd;     /* guest OPD of the title's callback */
    u32 userdata;   /* guest pointer, passed back untouched */
    s32 status;     /* CELL_GAMEUPDATE_RESULT_STATUS_* */
} GameUpdateAnswer;

static int              s_gu_initialized;
static int              s_gu_delivering;
static u64              s_gu_generation;   /* bumped by Term */
static GameUpdateAnswer s_gu_check;        /* answer to a Start */
static GameUpdateAnswer s_gu_finish;       /* answer to a Finish */

static const char* gu_status_name(s32 status)
{
    switch (status) {
    case CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE: return "NO_UPDATE";
    case CELL_GAMEUPDATE_RESULT_STATUS_FINISHED:  return "FINISHED";
    case CELL_GAMEUPDATE_RESULT_STATUS_ABORTED:   return "ABORTED";
    default:                                      return "?";
    }
}

s32 cellGameUpdateInit(void)
{
    GU_LOCK();
    s32 rc = CELL_OK;
    if (s_gu_initialized) {
        rc = (s32)CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED;
    } else {
        s_gu_initialized = 1;
        memset(&s_gu_check, 0, sizeof(s_gu_check));
        memset(&s_gu_finish, 0, sizeof(s_gu_finish));
    }
    GU_UNLOCK();
    printf("[cellGameUpdate] Init() -> 0x%08x\n", (u32)rc);
    return rc;
}

s32 cellGameUpdateTerm(void)
{
    GU_LOCK();
    s32 rc = CELL_OK;
    int dropped = 0;
    if (!s_gu_initialized) {
        rc = (s32)CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED;
    } else {
        dropped = s_gu_check.pending + s_gu_finish.pending;
        memset(&s_gu_check, 0, sizeof(s_gu_check));
        memset(&s_gu_finish, 0, sizeof(s_gu_finish));
        s_gu_initialized = 0;
        s_gu_delivering = 0;
        s_gu_generation++;
    }
    GU_UNLOCK();
    printf("[cellGameUpdate] Term() -> 0x%08x (%d undelivered answer(s) dropped)\n",
           (u32)rc, dropped);
    return rc;
}

/* Record one answer for the next sysutil poll. */
static s32 gu_record(GameUpdateAnswer* slot, CellGameUpdateCallback cb_func,
                     void* userdata, s32 status)
{
    if (!s_gu_initialized) return (s32)CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED;
    if (!cb_func)          return (s32)CELL_GAMEUPDATE_ERROR_INVALID_ADDR;
    if (slot->pending)     return (s32)CELL_GAMEUPDATE_ERROR_BUSY;
    slot->pending  = 1;
    slot->cb_opd   = (u32)(uintptr_t)cb_func;
    slot->userdata = (u32)(uintptr_t)userdata;
    slot->status   = status;
    return CELL_OK;
}

static s32 gu_start(const char* what, CellGameUpdateCallback cb_func, void* userdata)
{
    GU_LOCK();
    s32 rc = gu_record(&s_gu_check, cb_func, userdata,
                       CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE);
    GU_UNLOCK();
    printf("[cellGameUpdate] %s(cb=0x%08x, userdata=0x%08x) -> 0x%08x%s\n", what,
           (u32)(uintptr_t)cb_func, (u32)(uintptr_t)userdata, (u32)rc,
           rc == CELL_OK ? ", no update (answered at the next sysutil poll)" : "");
    return rc;
}

s32 cellGameUpdateCheckStartAsync(const CellGameUpdateParam* param,
                                  CellGameUpdateCallback cb_func, void* userdata)
{
    if (!param) {
        printf("[cellGameUpdate] CheckStartAsync(param=NULL) -> 0x%08x\n",
               (u32)CELL_GAMEUPDATE_ERROR_INVALID_ADDR);
        return (s32)CELL_GAMEUPDATE_ERROR_INVALID_ADDR;
    }
    return gu_start("CheckStartAsync", cb_func, userdata);
}

s32 cellGameUpdateCheckStartWithoutDialogAsync(CellGameUpdateCallback cb_func, void* userdata)
{
    return gu_start("CheckStartWithoutDialogAsync", cb_func, userdata);
}

s32 cellGameUpdateCheckFinishAsync(CellGameUpdateCallback cb_func, void* userdata)
{
    GU_LOCK();
    s32 rc = gu_record(&s_gu_finish, cb_func, userdata,
                       CELL_GAMEUPDATE_RESULT_STATUS_FINISHED);
    GU_UNLOCK();
    printf("[cellGameUpdate] CheckFinishAsync(cb=0x%08x, userdata=0x%08x) -> 0x%08x\n",
           (u32)(uintptr_t)cb_func, (u32)(uintptr_t)userdata, (u32)rc);
    return rc;
}

s32 cellGameUpdateCheckAbort(void)
{
    GU_LOCK();
    s32 rc = CELL_OK;
    int aborted = 0;
    if (!s_gu_initialized) {
        rc = (s32)CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED;
    } else if (s_gu_check.pending) {
        s_gu_check.status = CELL_GAMEUPDATE_RESULT_STATUS_ABORTED;
        aborted = 1;
    }
    GU_UNLOCK();
    printf("[cellGameUpdate] CheckAbort() -> 0x%08x%s\n", (u32)rc,
           aborted ? ", pending check now answers ABORTED" : "");
    return rc;
}

void cellGameUpdate_tick(void)
{
    GU_LOCK();
    ps3_guest_caller_fn caller = g_ps3_guest_caller;
    if (!caller || s_gu_delivering || (!s_gu_check.pending && !s_gu_finish.pending)) {
        GU_UNLOCK();
        return;
    }
    /* Take this poll's answers off the record before any guest code runs:
     * a nested poll cannot repeat them, and a step the callback starts goes
     * to a later poll. */
    GameUpdateAnswer batch[2];
    int n = 0;
    if (s_gu_check.pending)  { batch[n++] = s_gu_check;  s_gu_check.pending = 0; }
    if (s_gu_finish.pending) { batch[n++] = s_gu_finish; s_gu_finish.pending = 0; }
    const u64 generation = s_gu_generation;
    s_gu_delivering = 1;
    GU_UNLOCK();

    for (int i = 0; i < n; i++) {
        GU_LOCK();
        const int live = generation == s_gu_generation;   /* no Term meanwhile */
        GU_UNLOCK();
        if (!live) break;
        printf("[cellGameUpdate] callback(status=%s(%d), error=0x0) -> cb=0x%08x\n",
               gu_status_name(batch[i].status), batch[i].status, batch[i].cb_opd);
        caller(batch[i].cb_opd, (u64)(u32)batch[i].status, 0, batch[i].userdata,
               0, 0, 0, 0, 0);
    }

    GU_LOCK();
    if (generation == s_gu_generation) s_gu_delivering = 0;
    GU_UNLOCK();
}
