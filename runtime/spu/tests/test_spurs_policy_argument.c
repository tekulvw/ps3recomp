/* Exercise the actual policy dispatcher and MFC path, without title assets. */
#include "spu_helpers.h"
#include "spu_workload.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char memory[0x40000];
uint8_t* vm_base = memory;
uint32_t ppu_vm_size = sizeof(memory);
extern unsigned char g_vm_page_bitmap[];
extern void spu_halt(spu_context*);
extern void spu_wrch(spu_context*, unsigned, u128);
static u128 observed;
static int transfer, calls, require_same_context;
static spu_context* previous;
static void entry(spu_context* c) {
    observed = c->gpr[4]; ++calls;
    if (require_same_context) {
        assert(previous == c);
        assert(spu_ls_read32(c, 0x3000) == 0x12345678);
    }
    previous = c;
    /* An optional pointer in the high half must remain zero when disabled.
     * Duplicating the low half makes this legitimate conditional PUT overwrite
     * the queue instead. A nonzero high half must still enable the PUT. */
    if (transfer && c->gpr[4]._u32[0]) {
        memset(c->ls + 0x2000, 0xff, 16);
        spu_wrch(c, MFC_LSA, spu_splat_u32(0x2000));
        spu_wrch(c, MFC_EAH, spu_splat_u32(0));
        spu_wrch(c, MFC_EAL, spu_splat_u32(c->gpr[4]._u32[0] + 0x30));
        spu_wrch(c, MFC_Size, spu_splat_u32(16));
        spu_wrch(c, MFC_TagID, spu_splat_u32(1));
        spu_wrch(c, MFC_Cmd, spu_splat_u32(0x20));
    }
    spu_ls_write32(c, 0x3000, 0x12345678);
    c->gpr[4] = spu_splat_u32(0xdeadbeef); /* stale entry register on redispatch */
    spu_halt(c);
}
static int failures;
static void run(uint64_t data, int repeat) {
    static const uint8_t image[4] = {0};
    require_same_context = repeat;
    if (!repeat) previous = NULL;
    memset(memory, 0xa5, sizeof(memory));
    memset(g_vm_page_bitmap, 0, 8192);
    g_vm_page_bitmap[0] = 0x0f;
    int before = calls;
    spu_run_policy_module(entry, 77, image, sizeof(image), data, 3, 0, 0);
    assert(calls == before + 1);
    uint32_t hi = data >> 32, lo = data;
    int good = observed._u32[0] == hi && observed._u32[1] == lo;
    /* Spare words have no argument payload. Do not leak duplicated pointers. */
    good &= observed._u32[2] == 0 && observed._u32[3] == 0;
    if (transfer) {
        for (unsigned i = 0; i < 16; ++i) {
            good &= memory[0x10030 + i] == 0xa5;
            good &= memory[0x20030 + i] == (hi ? 0xff : 0xa5);
        }
    }
    printf("data=%016llX repeat=%d r4=%08X:%08X:%08X:%08X %s\n",
        (unsigned long long)data, repeat, observed._u32[0], observed._u32[1],
        observed._u32[2], observed._u32[3], good ? "PASS" : "FAIL");
    failures += !good;
}
int main(void) {
    transfer = 1;
    run(0x0000000000010000ull, 0);
    run(0x0000000000010000ull, 1);
    run(0x0002000000010000ull, 0);
    transfer = 0;
    run(0x0123456789abcdefull, 0);
    run(0x0123456789abcdefull, 1);
    run(0, 0);
    return failures ? 1 : 0;
}

/* External title/PPU boundaries; no emulated policy or DMA behavior is mocked. */
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) {
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;

/* No lv2 SPU thread is published by this standalone workload fixture. */
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
