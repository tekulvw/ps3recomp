/* The NP, signaling and HTTP imports a linked (ticket) title calls after its
 * update check: each NID is in the generated HLE table, and each answers as a
 * signed-in account with nothing on it (docs/cell-game-update.md). Synthetic
 * guest memory only; no game files. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../libs/network/sceNp.h"
/* sceNpBasic.h redefines sceNp.h types; take only its two codes. */
#define SCE_NP_BASIC_ERROR_INVALID_ARGUMENT 0x8002AA03u
#define SCE_NP_BASIC_ERROR_DATA_NOT_FOUND   0x8002AA08u
#include "../libs/network/sceNpSignaling.h"
#include "../libs/network/cellHttp.h"
#include "../runtime/ppu/ppu_memory.h"

uint8_t* vm_base;
uint32_t ppu_vm_size = 65536;
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

s32 sceNpManagerGetMyLanguages(SceNpMyLanguages* langs);
s32 sceNpManagerGetAccountRegion(void* countryCode, s32* language);
s32 sceNpManagerGetNetworkTime(void* tick);
s32 sceNpBasicGetBlockListEntry(u32 index, void* npid);
s32 sceNpSignalingAddExtendedHandler(SceNpSignalingCtxId ctxId, void* handler, void* arg);
s32 sceNpSignalingTerminateConnection(SceNpSignalingCtxId ctxId, SceNpSignalingConnId connId);
s32 cellHttpTransactionAbortConnection(CellHttpTransId transId);
s32 cellHttpInitCookie(void* pool, u32 poolSize);
s32 cellHttpEndCookie(void);

void ppu_hle_register_all(void);
static const struct { unsigned nid; const char* name; void* fn; } k_expected[] = {
    {0x32200389u, "sceNpManagerGetMyLanguages",         (void*)sceNpManagerGetMyLanguages},
    {0xB1E0718Bu, "sceNpManagerGetAccountRegion",       (void*)sceNpManagerGetAccountRegion},
    {0xBDC07FD5u, "sceNpManagerGetNetworkTime",         (void*)sceNpManagerGetNetworkTime},
    {0xF2B3338Au, "sceNpBasicGetBlockListEntry",        (void*)sceNpBasicGetBlockListEntry},
    {0x50B86D94u, "sceNpSignalingAddExtendedHandler",   (void*)sceNpSignalingAddExtendedHandler},
    {0x95C7BBA3u, "sceNpSignalingTerminateConnection",  (void*)sceNpSignalingTerminateConnection},
    {0x2D52848Bu, "cellHttpTransactionAbortConnection", (void*)cellHttpTransactionAbortConnection},
    {0x9638F766u, "cellHttpInitCookie",                 (void*)cellHttpInitCookie},
    {0x61B2BADEu, "cellHttpEndCookie",                  (void*)cellHttpEndCookie},
};
#define N_EXPECTED (sizeof k_expected / sizeof k_expected[0])
static unsigned registered[N_EXPECTED];
void ps3_hle_register(unsigned nid, const char* name, void* fn)
{
    for (size_t i = 0; i < N_EXPECTED; i++) {
        if (k_expected[i].nid != nid) continue;
        assert(!strcmp(name, k_expected[i].name));
        assert(fn == k_expected[i].fn);
        registered[i]++;
    }
}

#define P(ea) ((void*)(uintptr_t)(ea))

int main(void)
{
    ppu_hle_register_all();
    for (size_t i = 0; i < N_EXPECTED; i++) {
        if (registered[i] != 1) {
            fprintf(stderr, "FAIL: %s (0x%08X) registered %u times\n",
                    k_expected[i].name, k_expected[i].nid, registered[i]);
            return 1;
        }
    }
    vm_base = calloc(1, ppu_vm_size);
    assert(vm_base);

    /* Before NP init the manager getters refuse. */
    assert((u32)sceNpManagerGetMyLanguages(P(0x100)) == SCE_NP_ERROR_NOT_INITIALIZED);
    assert((u32)sceNpManagerGetAccountRegion(P(0x100), P(0x104)) == SCE_NP_ERROR_NOT_INITIALIZED);
    assert((u32)sceNpManagerGetNetworkTime(P(0x100)) == SCE_NP_ERROR_NOT_INITIALIZED);
    unsetenv("PS3RECOMP_NP_LOCAL_TICKET"); unsetenv("PS3RECOMP_NP_TICKET_FILE");
    setenv("PS3_NP_ONLINE_ID", "PlayerLab", 1);
    assert(sceNpInit(131072, P(0x8000)) == 0);

    /* Languages: the sceNpGetMyLanguages answer, English first. */
    memset(vm_base + 0x100, 0xAA, 16);
    assert(sceNpManagerGetMyLanguages(P(0x100)) == 0);
    assert(vm_read32(0x100) == SCE_NP_LANG_ENGLISH);
    assert((u32)sceNpManagerGetMyLanguages(NULL) == SCE_NP_ERROR_INVALID_ARGUMENT);

    /* Region: "us\0\0" and English; either pointer alone is enough. */
    memset(vm_base + 0x200, 0xAA, 16);
    assert(sceNpManagerGetAccountRegion(P(0x200), P(0x208)) == 0);
    assert(!memcmp(vm_base + 0x200, "us\0\0", 4));
    assert(vm_base[0x204] == 0xAA);
    assert(vm_read32(0x208) == SCE_NP_LANG_ENGLISH);
    assert(sceNpManagerGetAccountRegion(NULL, P(0x208)) == 0);
    assert(sceNpManagerGetAccountRegion(P(0x200), NULL) == 0);
    assert((u32)sceNpManagerGetAccountRegion(NULL, NULL) == SCE_NP_ERROR_INVALID_ARGUMENT);

    /* Network time: OFFLINE without a ticket; host UTC in RTC ticks with one. */
    memset(vm_base + 0x300, 0xAA, 8);
    assert((u32)sceNpManagerGetNetworkTime(P(0x300)) == SCE_NP_ERROR_OFFLINE);
    assert(vm_base[0x300] == 0xAA);
    setenv("PS3RECOMP_NP_LOCAL_TICKET", "1", 1);
    assert((u32)sceNpManagerGetNetworkTime(NULL) == SCE_NP_ERROR_INVALID_ARGUMENT);
    const unsigned long long epoch_us = 62135596800000000ull;
    unsigned long long before = epoch_us + (unsigned long long)time(NULL) * 1000000ull;
    assert(sceNpManagerGetNetworkTime(P(0x300)) == 0);
    unsigned long long after = epoch_us + ((unsigned long long)time(NULL) + 1) * 1000000ull;
    unsigned long long tick = vm_read64(0x300);
    assert(tick >= before && tick <= after);

    /* Block list: empty, so no entry at any index. */
    assert((u32)sceNpBasicGetBlockListEntry(0, P(0x400)) == SCE_NP_BASIC_ERROR_DATA_NOT_FOUND);
    assert((u32)sceNpBasicGetBlockListEntry(7, P(0x400)) == SCE_NP_BASIC_ERROR_DATA_NOT_FOUND);
    assert((u32)sceNpBasicGetBlockListEntry(0, NULL) == SCE_NP_BASIC_ERROR_INVALID_ARGUMENT);

    /* Signaling: handler and termination on a real context only. */
    assert((u32)sceNpSignalingAddExtendedHandler(0, P(0x500), P(0x600)) == SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED);
    assert(sceNpSignalingInit() == 0);
    assert(sceNpSignalingCreateCtx(P(0x100), NULL, NULL, P(0x700)) == 0);
    SceNpSignalingCtxId ctx = vm_read32(0x700);
    assert(sceNpSignalingAddExtendedHandler(ctx, P(0x500), P(0x600)) == 0);
    assert(sceNpSignalingTerminateConnection(ctx, 3) == 0);
    assert((u32)sceNpSignalingAddExtendedHandler(ctx + 1, P(0x500), P(0x600)) == SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    assert((u32)sceNpSignalingTerminateConnection(ctx + 1, 3) == SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    assert(sceNpSignalingDestroyCtx(ctx) == 0);
    assert((u32)sceNpSignalingTerminateConnection(ctx, 3) == SCE_NP_SIGNALING_ERROR_CTX_NOT_FOUND);
    assert(sceNpSignalingTerm() == 0);

    /* HTTP: cookies need cellHttp up; aborting a connection is aborting the transaction. */
    assert((u32)cellHttpInitCookie(P(0x1000), 4096) == CELL_HTTP_ERROR_NOT_INITIALIZED);
    assert((u32)cellHttpEndCookie() == CELL_HTTP_ERROR_NOT_INITIALIZED);
    assert((u32)cellHttpTransactionAbortConnection(0) == CELL_HTTP_ERROR_NOT_INITIALIZED);
    assert(cellHttpInit(P(0x2000), 0x4000) == 0);
    assert(cellHttpInitCookie(P(0x1000), 4096) == 0);
    assert((u32)cellHttpTransactionAbortConnection(0) == (u32)cellHttpAbortTransaction(0));
    assert(cellHttpEndCookie() == 0);
    assert(cellHttpEnd() == 0);

    sceNpTerm();
    free(vm_base);
    puts("PASS: linked-account NP/signaling/HTTP imports registered and answering");
    return 0;
}
