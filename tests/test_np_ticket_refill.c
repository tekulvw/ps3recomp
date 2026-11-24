/* PS3RECOMP_NP_TICKET_REFILL: each ticket file serves one request. A request
 * takes the file (it is gone afterwards), a reconnect waits for the launcher's
 * fresh one, and with none in time it fails instead of reusing an old ticket.
 * Without the switch the file stays and is read again (the earlier behaviour).
 * Real NP handlers, sysutil pump and guest memory; synthetic bytes only. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../libs/network/sceNp.h"
#include "../libs/network/np_psnr.h"
#include "../libs/system/cellSysutil.h"
#include "../runtime/ppu/ppu_memory.h"
#include "ps3emu/guest_call.h"
void edat_add_klicensee(const uint8_t klic[16]) {(void)klic;}
uint8_t* vm_base;
uint32_t ppu_vm_size = 65536;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
int spu_coh_is_reserved(uint32_t a){(void)a;return 0;}
void spu_coh_notify_write(uint32_t a){(void)a;}
void spu_lockline_lock(void){}
void spu_lockline_unlock(void){}
void cellOskDialog_tick(void){}
void cellMsgDialog_tick(void){}
void cellGameUpdate_tick(void){}
void np_basic_social_tick(void){}

static void *request_handler, *get_handler;
void ppu_hle_register_all(void);
void ps3_hle_register(unsigned nid, const char* name, void* fn)
{
    (void)name;
    if (nid == 0x8297F1EC) request_handler = fn;   /* sceNpManagerRequestTicket2 */
    if (nid == 0x0968AA36) get_handler = fn;       /* sceNpManagerGetTicket */
}
typedef s32 (*request_fn)(const SceNpId*, const void*, const char*, const void*, u32, const char*, u32);
typedef s32 (*get_fn)(void*, u32*);
static request_fn request;
static get_fn get;
static unsigned delivered;
static void guest(u32 opd, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h)
{
    (void)opd; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
    if (a == 255) delivered++;
}
static s32 req(void) { return request((void*)0x100, 0, (void*)0x400, 0, 0, 0, 0); }

static char path[1024], claimed[1100];
static void write_ticket(const char* text)
{
    /* As the launcher does: a temporary file renamed over the ticket. */
    char temp[1100]; snprintf(temp, sizeof temp, "%s.new", path);
    FILE* f = fopen(temp, "wb"); assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text)); assert(!fclose(f));
    assert(!rename(temp, path));
}
static int exists(const char* p) { return access(p, F_OK) == 0; }
/* The ticket bytes the game reads back after the completion. */
static void expect_ticket(const char* text)
{
    unsigned before = delivered;
    cellSysutilCheckCallback();
    assert(delivered == before + 1);
    size_t n = strlen(text);
    vm_write32(0x500, 4096); memset(vm_base + 0x600, 0, n + 1);
    assert(get((void*)0x600, (void*)0x500) == 0);
    assert(vm_read32(0x500) == n); assert(!memcmp(vm_base + 0x600, text, n));
}
static double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static void* refill_later(void* text)
{
    usleep(150 * 1000);
    write_ticket((const char*)text);
    return NULL;
}

int main(int argc, char** argv)
{
    assert(argc == 2);   /* a private directory for the synthetic ticket files */
    snprintf(path, sizeof path, "%s/portal-ticket.bin", argv[1]);
    snprintf(claimed, sizeof claimed, "%s.claimed", path);
    ppu_hle_register_all(); assert(request_handler && get_handler);
    request = (request_fn)request_handler; get = (get_fn)get_handler;
    vm_base = calloc(1, ppu_vm_size); assert(vm_base);
    memcpy(vm_base + 0x400, "MAG-service", 12);
    setenv("PS3_NP_ONLINE_ID", "PlayerRetail", 1);
    setenv("PS3RECOMP_NP_LOCAL_TICKET", "1", 1);
    setenv("PS3RECOMP_NP_TICKET_FILE", path, 1);
    assert(sceNpInit(131072, (void*)0x1000) == 0);
    assert(sceNpManagerGetNpId((void*)0x100) == 0);
    g_ps3_guest_caller = guest;
    assert(sceNpManagerRegisterCallback((void*)0x1234, (void*)0x5678) == 0);
    cellSysutilCheckCallback();   /* the ONLINE notification */

    /* Without the switch: the file is read and stays, so a second request sees it again. */
    write_ticket("MAGT-first");
    assert(req() == 0); expect_ticket("MAGT-first");
    assert(exists(path));
    assert(req() == 0); expect_ticket("MAGT-first");

    /* With it: the request takes the file; nothing is left behind. */
    setenv("PS3RECOMP_NP_TICKET_REFILL", "1", 1);
    setenv("PS3RECOMP_NP_TICKET_WAIT_MS", "300", 1);
    assert(req() == 0); expect_ticket("MAGT-first");
    assert(!exists(path)); assert(!exists(claimed));

    /* A reconnect with no fresh ticket yet: it waits, then fails; the old one is never sent. */
    double t0 = now_ms();
    assert((u32)req() == 0x8002aaff);
    double waited = now_ms() - t0;
    assert(waited >= 280 && waited < 5000);
    cellSysutilCheckCallback();
    assert((u32)get(0, (void*)0x500) == 0x8002aa0a);   /* nothing cached to hand out */

    /* A reconnect while the launcher is still fetching: it waits for the new file. */
    setenv("PS3RECOMP_NP_TICKET_WAIT_MS", "5000", 1);
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, refill_later, "MAGT-second"));
    t0 = now_ms();
    assert(req() == 0);
    assert(now_ms() - t0 >= 100);
    pthread_join(writer, NULL);
    expect_ticket("MAGT-second");
    assert(!exists(path));

    /* A ticket refilled ahead of time is taken at once, then gone again. */
    write_ticket("MAGT-third");
    t0 = now_ms();
    assert(req() == 0);
    assert(now_ms() - t0 < 1000);
    expect_ticket("MAGT-third");
    assert(!exists(path));

    /* Each refilled ticket is used once: three sign-ins, three different tickets. */
    const char* tickets[] = {"MAGT-a", "MAGT-b", "MAGT-c"};
    for (int i = 0; i < 3; i++) { write_ticket(tickets[i]); assert(req() == 0); expect_ticket(tickets[i]); }
    assert(!exists(path));

    /* A malformed wait value keeps the default rather than waiting forever or not at all. */
    setenv("PS3RECOMP_NP_TICKET_WAIT_MS", "soon", 1);
    write_ticket("MAGT-d"); assert(req() == 0); expect_ticket("MAGT-d");

    sceNpTerm(); free(vm_base);
    puts("NP ticket refill: single use, wait for a fresh ticket, refusal of an old one, unchanged default PASS");
    return 0;
}
