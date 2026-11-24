/* The NP social provider (docs/np-social-provider.md): inert without a
 * provider; with one, events reach the title's context-sensitive handler and
 * sceNpBasicGetEvent, presence, friend requests and messages reach the
 * provider, and NP ID lookups complete asynchronously. Synthetic guest memory
 * only; no game files.
 *
 * Run by tools/test_np_social.py in two modes: "full" (every callback) and
 * "minimal" (a provider with no callbacks). */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ps3emu/np_social.h"
#include "ps3emu/guest_call.h"
#include "ps3emu/nid.h"
#include "../runtime/ppu/ppu_context.h"
#include "../runtime/ppu/ppu_memory.h"

/* Runtime symbols the linked libraries expect. */
uint8_t* vm_base;
uint32_t ppu_vm_size = 1u << 20;
int g_resv_store_active; uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) {(void)a; (void)v; (void)w;}
int spu_coh_is_reserved(uint32_t a) {(void)a; return 0;}
void spu_coh_notify_write(uint32_t a) {(void)a;}
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void edat_add_klicensee(const uint8_t klic[16]) {(void)klic;}
void cellOskDialog_tick(void) {}
void cellMsgDialog_tick(void) {}
void cellGameUpdate_tick(void) {}
void vm_note_host_write(const void* host, size_t len) {(void)host; (void)len;}

/* The imports under test (sceNp.h and sceNpBasic.h cannot share a unit). */
int32_t sceNpInit(uint32_t poolSize, void* poolPtr);
int32_t sceNpTerm(void);
int32_t sceNpBasicGetEvent(uint32_t* event, void* from, void* data, uint32_t* size);
int32_t sceNpBasicRegisterContextSensitiveHandler(const void* context, void* handler, void* arg);
int32_t sceNpBasicUnregisterHandler(void);
int32_t sceNpBasicSetPresenceDetails(const void* presence, uint32_t options);
int32_t cellSysutilCheckCallback(void);
int32_t sceNpLookupInit(void);
int32_t sceNpLookupCreateTitleCtx(uint32_t commId, uint32_t selfNpId);
int32_t sceNpLookupCreateTransactionCtx(int32_t titleCtxId);
int32_t sceNpLookupDestroyTransactionCtx(int32_t transId);
int32_t sceNpLookupAbortTransaction(int32_t transId);
int32_t sceNpLookupPollAsync(int32_t transId, uint32_t result);
int32_t sceNpLookupWaitAsync(int32_t transId, uint32_t result);

#define NO_EVENT          0x8002A66Au
#define INVALID_ARGUMENT  0x8002A665u
#define NOT_SUPPORTED     0x8002A663u
#define EXCEEDS_MAX       0x8002A66Bu
#define C_INVALID_ARG     0x8002A104u
#define C_ABORTED         0x8002A109u
#define C_BUSY            0x8002A1ABu
#define NID_ADD_FRIEND    0x27C69EBAu
#define NID_SEND_GUI      0x01FBBC9Bu
#define NID_NPID_ASYNC    0xD12E40AEu
#define NID_NPID          0x166DCC11u

#define P(ea) ((void*)(uintptr_t)(ea))

/* ---- HLE context-handler registry (stands in for ppu_hle.cpp) ---- */
typedef void (*ctx_fn)(ppu_context*);
static struct { uint32_t nid; ctx_fn fn; } s_ctx[16];
static int s_ctx_n;
void ps3_hle_register_ctx(uint32_t nid, const char* name, ctx_fn fn)
{
    (void)name;
    assert(s_ctx_n < 16);
    s_ctx[s_ctx_n].nid = nid; s_ctx[s_ctx_n].fn = fn; s_ctx_n++;
}
static ctx_fn ctx_handler(uint32_t nid)
{
    for (int i = 0; i < s_ctx_n; i++) if (s_ctx[i].nid == nid) return s_ctx[i].fn;
    return NULL;
}
static int64_t call_ctx(uint32_t nid, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    ctx_fn fn = ctx_handler(nid);
    assert(fn);
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d; ctx.gpr[7] = e;
    fn(&ctx);
    return (int64_t)ctx.gpr[3];
}

/* ---- The title's handler (a guest OPD the fake caller recognises) ---- */
#define HANDLER_OPD 0x3000u
#define HANDLER_ARG 0x3100u
static struct { int32_t event, ret; uint32_t req, arg; } s_calls[512];
static int s_ncalls;
static int s_fetch_inside;   /* the handler fetches its event itself */
static uint32_t s_inside_event[64]; static int s_ninside;

static void guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)a4; (void)a5; (void)a6; (void)a7;
    assert(opd == HANDLER_OPD);
    assert(s_ncalls < 512);
    s_calls[s_ncalls].event = (int32_t)a0; s_calls[s_ncalls].ret = (int32_t)a1;
    s_calls[s_ncalls].req = (uint32_t)a2; s_calls[s_ncalls].arg = (uint32_t)a3;
    s_ncalls++;
    if (s_fetch_inside) {
        vm_write32(0x7100, 64);
        assert(sceNpBasicGetEvent(P(0x7000), P(0x7200), P(0x7400), P(0x7100)) == 0);
        s_inside_event[s_ninside++] = vm_read32(0x7000);
    }
}

/* ---- The provider ---- */
static struct {
    int presence_calls, has_data, has_status; uint32_t size; uint8_t data[128]; char status[64];
    int friend_calls; char friend_id[17]; char friend_msg[600]; int32_t friend_answer;
    int message_calls; ps3_np_social_message msg; char msg_ids[12][17]; char subject[32];
    char body[600]; uint8_t msg_data[64];
    int lookup_calls; char lookup_id[17]; uint64_t lookup_req;
    int handler_calls, handler_state;
} P_;

static void on_presence(void* u, int has_data, const uint8_t* data, uint32_t size, int has_status, const char* status)
{
    assert(u == &P_);
    P_.presence_calls++; P_.has_data = has_data; P_.has_status = has_status; P_.size = size;
    memset(P_.data, 0, sizeof P_.data); memset(P_.status, 0, sizeof P_.status);
    if (has_data && size) memcpy(P_.data, data, size);
    if (has_status) { assert(status && strlen(status) < 64); strcpy(P_.status, status); }
    else assert(!status);
}
static int32_t on_friend(void* u, const char* id, const char* msg)
{
    (void)u; P_.friend_calls++;
    strcpy(P_.friend_id, id); strcpy(P_.friend_msg, msg);
    return P_.friend_answer;
}
static int32_t on_message(void* u, const ps3_np_social_message* m)
{
    (void)u; P_.message_calls++; P_.msg = *m;
    for (uint32_t i = 0; i < m->recipient_count; i++) strcpy(P_.msg_ids[i], m->recipients[i]);
    strcpy(P_.subject, m->subject); strcpy(P_.body, m->body);
    if (m->data_size) { assert(m->data_size <= sizeof P_.msg_data); memcpy(P_.msg_data, m->data, m->data_size); }
    return 0;
}
static void on_lookup(void* u, const char* id, uint64_t req)
{
    (void)u; P_.lookup_calls++; strcpy(P_.lookup_id, id); P_.lookup_req = req;
}
static void on_handler(void* u, int registered)
{
    (void)u; P_.handler_calls++; P_.handler_state = registered;
}

/* ---- helpers ---- */
static void put_str(uint32_t ea, const char* s) { memcpy(vm_ptr8(ea), s, strlen(s) + 1); }

static uint32_t get_event(uint32_t room, uint32_t* size_out)
{
    memset(vm_ptr8(0x5000), 0xAA, 0x200);     /* from, 216 bytes + guard */
    memset(vm_ptr8(0x5400), 0xAA, 0x200);     /* data */
    vm_write32(0x5300, room);
    vm_write32(0x5304, 0xEEEEEEEEu);
    int32_t rc = sceNpBasicGetEvent(P(0x5304), P(0x5000), P(0x5400), P(0x5300));
    if (rc != 0) return (uint32_t)rc;
    if (size_out) *size_out = vm_read32(0x5300);
    return 0;
}
static uint32_t ev(void) { return vm_read32(0x5304); }

struct completion { uint64_t req; int32_t result; const char* id; int delay_ms; };
static void* complete_later(void* arg)
{
    struct completion* c = arg;
    struct timespec ts = {0, (long)c->delay_ms * 1000000L};
    nanosleep(&ts, NULL);
    ps3_np_social_complete_lookup(c->req, c->result, c->id);
    return NULL;
}

static void presence_details(uint32_t ea, const char* status, const uint8_t* data, uint32_t size)
{
    memset(vm_ptr8(ea), 0, 392);
    if (status) memcpy(vm_ptr8(ea + 0x80), status, strlen(status));
    if (data) memcpy(vm_ptr8(ea + 0x100), data, size < 128 ? size : 128);
    vm_write32(ea + 0x180, size);
}

/* ---- inert default ---- */
static void inert(void)
{
    /* GetEvent: NO_EVENT, every out-parameter untouched, even NULL ones. */
    memset(vm_ptr8(0x5000), 0xAA, 0x800);
    assert((uint32_t)sceNpBasicGetEvent(P(0x5304), P(0x5000), P(0x5400), P(0x5300)) == NO_EVENT);
    assert((uint32_t)sceNpBasicGetEvent(NULL, NULL, NULL, NULL) == NO_EVENT);
    for (int i = 0; i < 0x800; i++) assert(vm_base[0x5000 + i] == 0xAA);
    /* SetPresenceDetails: not supported. */
    presence_details(0x6000, "x", (const uint8_t*)"abc", 3);
    assert((uint32_t)sceNpBasicSetPresenceDetails(P(0x6000), 3) == NOT_SUPPORTED);
    /* The handler is accepted and never called. */
    assert(sceNpBasicRegisterContextSensitiveHandler(P(0x100), P(HANDLER_OPD), P(HANDLER_ARG)) == 0);
    cellSysutilCheckCallback();
    assert(s_ncalls == 0);
    /* The provider-only imports stay unregistered; pushes are refused. */
    assert(s_ctx_n == 0);
    assert(ps3_np_social_push_event(1, 0, "alice", NULL, 0) != 0);
    ps3_np_social_complete_lookup(1, 0, "alice");
    assert(P_.handler_calls == 0);
    puts("inert without a provider: PASS");
}

static void install(int full)
{
    ps3_np_social_provider p;
    memset(&p, 0, sizeof p);
    p.abi = PS3_NP_SOCIAL_ABI + 1;
    p.user = &P_;
    assert(ps3_np_social_set_provider(NULL) != 0);
    assert(ps3_np_social_set_provider(&p) != 0);
    assert(s_ctx_n == 0);
    p.abi = PS3_NP_SOCIAL_ABI;
    if (full) {
        p.set_presence = on_presence; p.add_friend = on_friend; p.send_message = on_message;
        p.lookup_online_id = on_lookup; p.handler_changed = on_handler;
    }
    assert(ps3_np_social_set_provider(&p) == 0);
    assert(ps3_np_social_set_provider(&p) != 0);
    assert(ctx_handler(NID_ADD_FRIEND) && ctx_handler(NID_SEND_GUI) &&
           ctx_handler(NID_NPID_ASYNC) && ctx_handler(NID_NPID));
    assert(ps3_compute_nid("sceNpBasicAddFriend") == NID_ADD_FRIEND);
    assert(ps3_compute_nid("sceNpLookupNpIdAsync") == NID_NPID_ASYNC);
    puts("provider registration: PASS");
}

static void events(void)
{
    const uint8_t pres[5] = {1, 2, 0, 4, 5};
    /* Bad pushes. */
    assert(ps3_np_social_push_event(1, 0, "", NULL, 0) != 0);
    assert(ps3_np_social_push_event(1, 0, "seventeen-chars-x", NULL, 0) != 0);
    assert(ps3_np_social_push_event(1, 0, NULL, NULL, 0) != 0);
    assert(ps3_np_social_push_event(1, 0, "alice", pres, 129) != 0);
    assert(ps3_np_social_push_event(1, 0, "alice", NULL, 3) != 0);

    assert(ps3_np_social_push_event(1, 0, "alice", pres, 5) == 0);
    assert(ps3_np_social_push_event(0, 0, "bob", NULL, 0) == 0);
    assert(ps3_np_social_push_event(9, 0, "carol", NULL, 0) == 0);
    assert(ps3_np_social_push_event(10, 0, "sixteen_chars_ok", NULL, 0) == 0);

    /* No handler yet: nothing announced, nothing to fetch. */
    cellSysutilCheckCallback();
    assert(s_ncalls == 0);
    assert(get_event(512, NULL) == NO_EVENT);
    /* A handler without NP initialised is dropped at the poll. */
    assert(sceNpBasicRegisterContextSensitiveHandler(P(0x100), P(HANDLER_OPD), P(HANDLER_ARG)) == 0);
    assert(P_.handler_state == 1 || !P_.handler_calls);
    cellSysutilCheckCallback();
    assert(s_ncalls == 0);

    assert(sceNpInit(131072, P(0x8000)) == 0);
    assert(sceNpBasicRegisterContextSensitiveHandler(P(0x100), P(HANDLER_OPD), P(HANDLER_ARG)) == 0);
    int handler_calls = P_.handler_calls;
    cellSysutilCheckCallback();
    assert(s_ncalls == 4);
    int32_t want[4] = {1, 0, 9, 10};
    for (int i = 0; i < 4; i++) {
        assert(s_calls[i].event == want[i] && s_calls[i].ret == 0 &&
               s_calls[i].req == 0 && s_calls[i].arg == HANDLER_ARG);
    }
    cellSysutilCheckCallback();          /* announced once only */
    assert(s_ncalls == 4);

    /* Argument checks take nothing. */
    assert((uint32_t)sceNpBasicGetEvent(NULL, P(0x5000), P(0x5400), P(0x5300)) == INVALID_ARGUMENT);
    assert((uint32_t)sceNpBasicGetEvent(P(0x5304), NULL, P(0x5400), P(0x5300)) == INVALID_ARGUMENT);
    assert((uint32_t)sceNpBasicGetEvent(P(0x5304), P(0x5000), P(0x5400), NULL) == INVALID_ARGUMENT);

    /* PRESENCE: from is an SceNpUserInfo, data is the bytes, cut to the room. */
    uint32_t size = 0;
    assert(get_event(3, &size) == 0);
    assert(ev() == 1 && size == 3);
    assert(!memcmp(vm_ptr8(0x5000), "alice", 6));
    assert(!memcmp(vm_ptr8(0x5000 + 36), "alice", 6));
    for (int i = 0; i < 216; i++)
        if (i >= 5 && !(i >= 36 && i < 41)) assert(vm_base[0x5000 + i] == 0);
    assert(vm_base[0x5000 + 216] == 0xAA);
    assert(!memcmp(vm_ptr8(0x5400), pres, 3) && vm_base[0x5403] == 0xAA);

    /* OFFLINE: no data. */
    assert(get_event(512, &size) == 0);
    assert(ev() == 0 && size == 0 && !memcmp(vm_ptr8(0x5000), "bob", 4) && vm_base[0x5400] == 0xAA);

    /* Unregistering returns announced events to the next handler. */
    assert(sceNpBasicUnregisterHandler() == 0);
    assert(P_.handler_state == 0 && P_.handler_calls == handler_calls + 1);
    assert(get_event(512, NULL) == NO_EVENT);
    cellSysutilCheckCallback();
    assert(s_ncalls == 4);
    assert(sceNpBasicRegisterContextSensitiveHandler(P(0x100), P(HANDLER_OPD), P(HANDLER_ARG)) == 0);
    assert(P_.handler_state == 1);
    cellSysutilCheckCallback();
    assert(s_ncalls == 6 && s_calls[4].event == 9 && s_calls[5].event == 10);
    assert(get_event(512, NULL) == 0 && ev() == 9 && !memcmp(vm_ptr8(0x5000), "carol", 6));
    assert(get_event(512, NULL) == 0 && ev() == 10 && !memcmp(vm_ptr8(0x5000), "sixteen_chars_ok", 16) &&
           vm_base[0x5000 + 16] == 0);
    assert(get_event(512, NULL) == NO_EVENT);

    /* A NULL data pointer takes the event and receives nothing. */
    assert(ps3_np_social_push_event(1, 0, "dave", pres, 5) == 0);
    cellSysutilCheckCallback();
    vm_write32(0x5300, 64);
    assert(sceNpBasicGetEvent(P(0x5304), P(0x5000), NULL, P(0x5300)) == 0);
    assert(vm_read32(0x5304) == 1 && vm_read32(0x5300) == 0);

    /* The handler may fetch inside the call; order is kept. Result codes pass through. */
    s_fetch_inside = 1;
    assert(ps3_np_social_push_event(3, (int32_t)0x8002A671u, "erin", NULL, 0) == 0);
    assert(ps3_np_social_push_event(1, 0, "erin", pres, 2) == 0);
    cellSysutilCheckCallback();
    s_fetch_inside = 0;
    assert(s_ninside == 2 && s_inside_event[0] == 3 && s_inside_event[1] == 1);
    assert((uint32_t)s_calls[s_ncalls - 2].ret == 0x8002A671u);
    assert(get_event(512, NULL) == NO_EVENT);

    /* The queue holds 256 events; the 257th is refused and never delivered. */
    assert(sceNpBasicUnregisterHandler() == 0);
    for (int i = 0; i < 256; i++) assert(ps3_np_social_push_event(1, 0, "flood", pres, 1) == 0);
    assert(ps3_np_social_push_event(1, 0, "lost", pres, 1) != 0);
    assert(sceNpBasicRegisterContextSensitiveHandler(P(0x100), P(HANDLER_OPD), P(HANDLER_ARG)) == 0);
    int before = s_ncalls;
    cellSysutilCheckCallback();
    assert(s_ncalls == before + 256);
    for (int i = 0; i < 256; i++) assert(get_event(512, NULL) == 0 && !memcmp(vm_ptr8(0x5000), "flood", 6));
    assert(get_event(512, NULL) == NO_EVENT);

    /* sceNpTerm: the next poll drops the handler; events wait for a new one. */
    assert(ps3_np_social_push_event(0, 0, "gus", NULL, 0) == 0);
    assert(sceNpTerm() == 0);
    before = s_ncalls;
    cellSysutilCheckCallback();
    assert(s_ncalls == before && P_.handler_state == 0);
    assert(sceNpInit(131072, P(0x8000)) == 0);
    cellSysutilCheckCallback();
    assert(s_ncalls == before);
    assert(sceNpBasicRegisterContextSensitiveHandler(P(0x100), P(HANDLER_OPD), P(HANDLER_ARG)) == 0);
    cellSysutilCheckCallback();
    assert(s_ncalls == before + 1 && get_event(512, NULL) == 0 && ev() == 0);
    puts("events: queue, handler announcements, GetEvent layout, re-registration, overflow, term: PASS");
}

static void presence(int full)
{
    const uint8_t data[128] = {9, 8, 7, 0, 6};
    presence_details(0x6000, "In a match", data, 5);
    assert((uint32_t)sceNpBasicSetPresenceDetails(NULL, 3) == INVALID_ARGUMENT);
    assert((uint32_t)sceNpBasicSetPresenceDetails(P(0x6000), 0) == INVALID_ARGUMENT);
    assert((uint32_t)sceNpBasicSetPresenceDetails(P(0x6000), 4) == INVALID_ARGUMENT);
    if (!full) {
        assert((uint32_t)sceNpBasicSetPresenceDetails(P(0x6000), 3) == NOT_SUPPORTED);
        return;
    }
    assert(sceNpBasicSetPresenceDetails(P(0x6000), 3) == 0);
    assert(P_.presence_calls == 1 && P_.has_data && P_.has_status && P_.size == 5 &&
           !memcmp(P_.data, data, 5) && !strcmp(P_.status, "In a match"));
    assert(sceNpBasicSetPresenceDetails(P(0x6000), 1) == 0);
    assert(P_.presence_calls == 2 && P_.has_data && !P_.has_status && P_.size == 5);
    /* Data size checked only when data is set; status cut at 63 bytes. */
    memset(vm_ptr8(0x6000 + 0x80), 'S', 64);
    vm_write32(0x6000 + 0x180, 129);
    assert((uint32_t)sceNpBasicSetPresenceDetails(P(0x6000), 1) == EXCEEDS_MAX);
    assert((uint32_t)sceNpBasicSetPresenceDetails(P(0x6000), 3) == EXCEEDS_MAX);
    assert(P_.presence_calls == 2);
    assert(sceNpBasicSetPresenceDetails(P(0x6000), 2) == 0);
    assert(P_.presence_calls == 3 && !P_.has_data && P_.has_status && strlen(P_.status) == 63);
    vm_write32(0x6000 + 0x180, 128);
    assert(sceNpBasicSetPresenceDetails(P(0x6000), 1) == 0 && P_.size == 128);
    vm_write32(0x6000 + 0x180, 0);
    assert(sceNpBasicSetPresenceDetails(P(0x6000), 1) == 0 && P_.size == 0 && P_.has_data);
    puts("presence details: PASS");
}

static void npid_at(uint32_t ea, const char* handle)
{
    memset(vm_ptr8(ea), 0, 36);
    memcpy(vm_ptr8(ea), handle, strlen(handle) < 17 ? strlen(handle) : 17);
}

static void friends_and_messages(int full)
{
    npid_at(0x9000, "frank");
    put_str(0x9100, "hi, it's me");
    if (!full) {
        assert((uint32_t)call_ctx(NID_ADD_FRIEND, 0x9000, 0x9100, 0xFFFFFFFF, 0, 0) == NOT_SUPPORTED);
    } else {
        assert(call_ctx(NID_ADD_FRIEND, 0x9000, 0x9100, 0xFFFFFFFF, 0, 0) == 0);
        assert(P_.friend_calls == 1 && !strcmp(P_.friend_id, "frank") && !strcmp(P_.friend_msg, "hi, it's me"));
        assert(call_ctx(NID_ADD_FRIEND, 0x9000, 0, 0xFFFFFFFF, 0, 0) == 0);
        assert(P_.friend_calls == 2 && !strcmp(P_.friend_msg, ""));
        P_.friend_answer = (int32_t)0x8002A66Fu;
        assert((uint32_t)call_ctx(NID_ADD_FRIEND, 0x9000, 0, 0, 0, 0) == 0x8002A66Fu);
        P_.friend_answer = 0;
    }
    int calls = P_.friend_calls;
    assert((uint32_t)call_ctx(NID_ADD_FRIEND, 0, 0x9100, 0, 0, 0) == INVALID_ARGUMENT);
    npid_at(0x9000, "");
    assert((uint32_t)call_ctx(NID_ADD_FRIEND, 0x9000, 0x9100, 0, 0, 0) == INVALID_ARGUMENT);
    memset(vm_ptr8(0x9000), 'z', 17);   /* unterminated handle */
    assert((uint32_t)call_ctx(NID_ADD_FRIEND, 0x9000, 0x9100, 0, 0, 0) == INVALID_ARGUMENT);
    npid_at(0x9000, "frank");
    memset(vm_ptr8(0x9200), 'b', 513); vm_base[0x9200 + 513] = 0;
    assert((uint32_t)call_ctx(NID_ADD_FRIEND, 0x9000, 0x9200, 0, 0, 0) == EXCEEDS_MAX);
    vm_base[0x9200 + 512] = 0;           /* exactly 512 is fine */
    if (full) assert(call_ctx(NID_ADD_FRIEND, 0x9000, 0x9200, 0, 0, 0) == 0 && strlen(P_.friend_msg) == 512);
    else assert(P_.friend_calls == calls);

    /* SendMessageGui: SceNpBasicMessageDetails at 0xA000, recipients at 0xA100. */
    npid_at(0xA100, "gina"); npid_at(0xA100 + 36, "hank");
    put_str(0xA200, "Subject"); put_str(0xA300, "Body text");
    memcpy(vm_ptr8(0xA400), "\x01\x02\x03", 3);
    memset(vm_ptr8(0xA000), 0, 40);
    vm_write64(0xA000, 0x1122334455667788ull);
    vm_write16(0xA008, 1); vm_write16(0xA00A, 7); vm_write32(0xA00C, 0x10);
    vm_write32(0xA010, 0xA100); vm_write32(0xA014, 2);
    vm_write32(0xA018, 0xA200); vm_write32(0xA01C, 0xA300);
    vm_write32(0xA020, 0xA400); vm_write32(0xA024, 3);
    if (!full) {
        assert((uint32_t)call_ctx(NID_SEND_GUI, 0xA000, 0xFFFFFFFF, 0, 0, 0) == NOT_SUPPORTED);
    } else {
        assert(call_ctx(NID_SEND_GUI, 0xA000, 0xFFFFFFFF, 0, 0, 0) == 0);
        assert(P_.message_calls == 1 && P_.msg.msg_id == 0x1122334455667788ull && P_.msg.main_type == 1 &&
               P_.msg.sub_type == 7 && P_.msg.features == 0x10 && P_.msg.recipient_count == 2 &&
               !strcmp(P_.msg_ids[0], "gina") && !strcmp(P_.msg_ids[1], "hank") &&
               !strcmp(P_.subject, "Subject") && !strcmp(P_.body, "Body text") &&
               P_.msg.data_size == 3 && !memcmp(P_.msg_data, "\x01\x02\x03", 3));
    }
    int msgs = P_.message_calls;
    assert((uint32_t)call_ctx(NID_SEND_GUI, 0, 0, 0, 0, 0) == INVALID_ARGUMENT);
    vm_write32(0xA014, 0);
    assert((uint32_t)call_ctx(NID_SEND_GUI, 0xA000, 0, 0, 0, 0) == INVALID_ARGUMENT);
    vm_write32(0xA014, 13);
    assert((uint32_t)call_ctx(NID_SEND_GUI, 0xA000, 0, 0, 0, 0) == INVALID_ARGUMENT);
    vm_write32(0xA014, 2);
    put_str(0xA200, "nineteen-bytes-long");
    assert((uint32_t)call_ctx(NID_SEND_GUI, 0xA000, 0, 0, 0, 0) == EXCEEDS_MAX);
    put_str(0xA200, "Subject");
    vm_write32(0xA020, 0);
    assert((uint32_t)call_ctx(NID_SEND_GUI, 0xA000, 0, 0, 0, 0) == INVALID_ARGUMENT);
    assert(P_.message_calls == msgs);
    puts("friend requests and messages: PASS");
}

static void lookups(int full)
{
    assert(sceNpLookupInit() == 0);
    int32_t title = sceNpLookupCreateTitleCtx(0, 0);
    int32_t t = sceNpLookupCreateTransactionCtx(title);
    assert(title > 0 && t > 0);
    put_str(0xB000, "alice");
    memset(vm_ptr8(0xB100), 0xAA, 64);

    /* Argument checks. */
    assert((uint32_t)call_ctx(NID_NPID_ASYNC, 99, 0xB000, 0xB100, 0, 0) == 0x8002A106u);
    assert((uint32_t)call_ctx(NID_NPID_ASYNC, t, 0, 0xB100, 0, 0) == C_INVALID_ARG);
    assert((uint32_t)call_ctx(NID_NPID_ASYNC, t, 0xB000, 0, 0, 0) == C_INVALID_ARG);
    put_str(0xB200, "");
    assert((uint32_t)call_ctx(NID_NPID_ASYNC, t, 0xB200, 0xB100, 0, 0) == C_INVALID_ARG);
    memset(vm_ptr8(0xB300), 'q', 17);
    assert((uint32_t)call_ctx(NID_NPID_ASYNC, t, 0xB300, 0xB100, 0, 0) == C_INVALID_ARG);

    if (!full) {
        /* No callback: completes at once as not found. */
        assert(call_ctx(NID_NPID_ASYNC, t, 0xB000, 0xB100, 0, 0) == 0);
        vm_write32(0xB400, 0x12345678);
        assert(sceNpLookupPollAsync(t, 0xB400) == 0 && vm_read32(0xB400) == 0x8002A41Bu);
        assert(vm_base[0xB100] == 0xAA);
        puts("lookups (no callback): PASS");
        return;
    }

    /* Found: running until the provider answers, then the NP ID. */
    assert(call_ctx(NID_NPID_ASYNC, t, 0xB000, 0xB100, 0, 0) == 0);
    assert(P_.lookup_calls == 1 && !strcmp(P_.lookup_id, "alice") && P_.lookup_req);
    assert((uint32_t)call_ctx(NID_NPID_ASYNC, t, 0xB000, 0xB100, 0, 0) == C_BUSY);
    vm_write32(0xB400, 0x12345678);
    assert(sceNpLookupPollAsync(t, 0xB400) == 1 && vm_read32(0xB400) == 0x12345678);
    ps3_np_social_complete_lookup(P_.lookup_req, 0, "Alice");
    ps3_np_social_complete_lookup(P_.lookup_req, PS3_NP_SOCIAL_LOOKUP_NOT_FOUND, NULL);   /* ignored */
    assert(vm_base[0xB100] == 0xAA);                       /* written at the poll */
    assert(sceNpLookupPollAsync(t, 0xB400) == 0 && vm_read32(0xB400) == 0);
    assert(!memcmp(vm_ptr8(0xB100), "Alice", 6));
    for (int i = 5; i < 36; i++) assert(vm_base[0xB100 + i] == 0);
    assert(vm_base[0xB100 + 36] == 0xAA);
    assert(sceNpLookupPollAsync(t, 0xB400) == 0 && vm_read32(0xB400) == 0);   /* stays finished */

    /* Not found: the result, and the NP ID untouched. */
    memset(vm_ptr8(0xB100), 0xAA, 64);
    assert(call_ctx(NID_NPID_ASYNC, t, 0xB000, 0xB100, 0, 0) == 0);
    ps3_np_social_complete_lookup(P_.lookup_req, PS3_NP_SOCIAL_LOOKUP_NOT_FOUND, NULL);
    assert(sceNpLookupPollAsync(t, 0xB400) == 0 && vm_read32(0xB400) == 0x8002A41Bu);
    assert(vm_base[0xB100] == 0xAA);

    /* A "found" completion without a valid ID is a failure. */
    assert(call_ctx(NID_NPID_ASYNC, t, 0xB000, 0xB100, 0, 0) == 0);
    ps3_np_social_complete_lookup(P_.lookup_req, 0, "");
    assert(sceNpLookupPollAsync(t, 0xB400) == 0 && vm_read32(0xB400) == (uint32_t)PS3_NP_SOCIAL_LOOKUP_FAILED);

    /* Abort: ABORTED, and a late completion is ignored. */
    assert(call_ctx(NID_NPID_ASYNC, t, 0xB000, 0xB100, 0, 0) == 0);
    uint64_t req = P_.lookup_req;
    assert(sceNpLookupAbortTransaction(t) == 0);
    ps3_np_social_complete_lookup(req, 0, "alice");
    assert(sceNpLookupPollAsync(t, 0xB400) == 0 && vm_read32(0xB400) == C_ABORTED);
    assert(vm_base[0xB100] == 0xAA);

    /* Wait: completes when another thread answers. */
    assert(call_ctx(NID_NPID_ASYNC, t, 0xB000, 0xB100, 0, 0) == 0);
    struct completion c = {P_.lookup_req, 0, "ALICE", 50};
    pthread_t th;
    assert(pthread_create(&th, NULL, complete_later, &c) == 0);
    assert(sceNpLookupWaitAsync(t, 0xB400) == 0 && vm_read32(0xB400) == 0);
    pthread_join(th, NULL);
    assert(!memcmp(vm_ptr8(0xB100), "ALICE", 6));

    /* The blocking form answers the result. */
    int calls = P_.lookup_calls;
    struct completion c2 = {0, PS3_NP_SOCIAL_LOOKUP_NOT_FOUND, NULL, 50};
    /* The request number is the next one the provider will see. */
    c2.req = P_.lookup_req + 1;
    assert(pthread_create(&th, NULL, complete_later, &c2) == 0);
    assert((uint32_t)call_ctx(NID_NPID, t, 0xB000, 0xB100, 0, 0) == 0x8002A41Bu);
    pthread_join(th, NULL);
    assert(P_.lookup_calls == calls + 1 && P_.lookup_req == c2.req);

    /* Destroying a transaction abandons its lookup. */
    assert(call_ctx(NID_NPID_ASYNC, t, 0xB000, 0xB100, 0, 0) == 0);
    req = P_.lookup_req;
    assert(sceNpLookupDestroyTransactionCtx(t) == 0);
    ps3_np_social_complete_lookup(req, 0, "alice");
    int32_t t2 = sceNpLookupCreateTransactionCtx(title);
    assert(t2 > 0);
    assert(sceNpLookupPollAsync(t2, 0xB400) == 0 && vm_read32(0xB400) == 0);   /* a plain transaction */

    puts("lookups: PASS");
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int full = argc > 1 && !strcmp(argv[1], "full");
    vm_base = calloc(1, ppu_vm_size);
    assert(vm_base);
    unsetenv("PS3RECOMP_NP_LOCAL_TICKET"); unsetenv("PS3RECOMP_NP_TICKET_FILE");
    g_ps3_guest_caller = guest_caller;

    inert();
    install(full);
    if (full) events();
    else {
        assert(sceNpInit(131072, P(0x8000)) == 0);
        /* No handler_changed callback: registration still works. */
        assert(sceNpBasicRegisterContextSensitiveHandler(P(0x100), P(HANDLER_OPD), P(HANDLER_ARG)) == 0);
        assert(ps3_np_social_push_event(1, 0, "zed", (const uint8_t*)"x", 1) == 0);
        cellSysutilCheckCallback();
        assert(s_ncalls == 1 && get_event(512, NULL) == 0 && ev() == 1);
    }
    presence(full);
    friends_and_messages(full);
    lookups(full);
    printf("NP social provider (%s): PASS\n", full ? "full" : "minimal");
    return 0;
}
