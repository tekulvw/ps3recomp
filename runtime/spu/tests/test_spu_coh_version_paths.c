/*
 * Per-line version counters (PS3RECOMP_COH_VERSIONS) through the runtime's real
 * paths: spu_mfc_atomic and mfc_do_transfer (driven through spu_wrch), the PPU
 * stores and store-conditionals of ppu_memory.h, a host line section, and the
 * blocking RdEventStat wait. Synthetic.
 *
 * test_spu_coh_version.c proves the counter protocol itself. This proves that
 * each writer in the runtime takes it, and that each place an SPU can observe
 * a lost reservation delivers it -- the guest-visible contract of the default
 * scheme, with the event read through the channel (rchcnt) as guest code reads
 * it rather than from event_status the instant after a write.
 *
 * Build:
 *   clang -std=gnu17 -I include -I runtime/spu -I runtime/platform \
 *         -o /tmp/test_spu_coh_version_paths \
 *         runtime/spu/tests/test_spu_coh_version_paths.c \
 *         runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c \
 *         runtime/spu/spu_coherency.c runtime/spu/spu_workload.c \
 *         runtime/spu/spurs_policy.c runtime/spu/spurs_job.c \
 *         runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c \
 *         runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c \
 *         runtime/platform/win32_compat.c -lpthread -lm
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints a summary.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;
struct spu_context;
void* ps3_jobio_active;
void ps3_jobio_event(struct spu_context* c, uint32_t k, uint32_t cmd, uint32_t lsa, uint64_t ea, uint32_t n, const uint8_t* b, uint32_t x)
{ (void)c; (void)k; (void)cmd; (void)lsa; (void)ea; (void)n; (void)b; (void)x; }

/* The units under test. */
#include "../spu_channels.c"
#include "../../ppu/ppu_memory.h"
#include "../spu_coh_section.h"
#include "../spu_coh_version.h"

static int g_pass, g_fail;
static const char* g_current = "(none)";
#define TEST(name) g_current = (name)
#define CHECK(cond) do {                                                     \
    if (cond) __atomic_fetch_add(&g_pass, 1, __ATOMIC_RELAXED);              \
    else { __atomic_fetch_add(&g_fail, 1, __ATOMIC_RELAXED);                 \
           fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n", g_current,           \
                   __FILE__, __LINE__, #cond); }                             \
} while (0)

#define LINE_A 0x00020000u
#define LINE_B 0x00020080u   /* LINE_A's neighbour */
#define LINE_T 0x00030000u   /* torture line */
#define LSA_A  0x00001000u
#define LSA_B  0x00002000u

static spu_context g_a, g_b;

static void wrch(spu_context* ctx, uint32_t channel, uint32_t v) { spu_wrch(ctx, channel, spu_make_preferred_u32(v)); }
static void spu_atomic(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, lsa); wrch(ctx, MFC_EAH, 0); wrch(ctx, MFC_EAL, ea); wrch(ctx, MFC_Cmd, cmd);
}
static void spu_dma(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, lsa); wrch(ctx, MFC_EAH, 0); wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Size, size); wrch(ctx, MFC_TagID, 0); wrch(ctx, MFC_Cmd, cmd);
}
/* What the guest does to learn about events: rchcnt SPU_RdEventStat. */
static int lr(spu_context* ctx) { (void)spu_rchcnt(ctx, SPU_RdEventStat); return (ctx->event_status & SPU_EVENT_LR) != 0; }

static void reset(void)
{
    spu_context_init(&g_a, 0); g_a.spu_id = 0;
    spu_context_init(&g_b, 0); g_b.spu_id = 1;
    memset(g_guest_mem + LINE_A, 0, 0x400);
}

/* ---------------------------------------------------------------------------
 * One thread, every writer
 * -------------------------------------------------------------------------*/
static void test_writers(void)
{
    reset();
    TEST("a peer's PUTLLC: the holder sees LR when it looks, and its PUTLLC fails");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    g_b.ls[LSA_B] = 0xB0;
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLC_CMD);
    CHECK(g_b.atomic_stat == 0 && g_guest_mem[LINE_A] == 0xB0);
    CHECK(!(g_b.event_status & SPU_EVENT_LR));          /* a commit is not a loss */
    CHECK(lr(&g_a) && !g_a.resv_valid);
    g_a.ls[LSA_A] = 0xA0;
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK(g_a.atomic_stat == 1 && g_guest_mem[LINE_A] == 0xB0);

    reset();
    TEST("a failed PUTLLC is quiet, and a re-reservation sees the new data");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLC_CMD);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);    /* fails */
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    CHECK(!lr(&g_b) && g_b.resv_valid);

    reset();
    TEST("PUTLLUC ends the peer's reservation, and the issuer's own on the line");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLUC_CMD);
    CHECK(g_b.atomic_stat == 2);
    CHECK(lr(&g_a) && !g_a.resv_valid);
    CHECK(lr(&g_b) && !g_b.resv_valid);

    reset();
    TEST("a plain PUT ends a peer's reservation but not the issuer's own");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    memset(g_b.ls + 0x3000, 0x5A, 0x100);
    spu_dma(&g_b, 0x3000, LINE_A + 0x40, 0x10, MFC_PUT_CMD);
    CHECK(g_guest_mem[LINE_A + 0x40] == 0x5A);
    CHECK(lr(&g_a) && !g_a.resv_valid);
    CHECK(!lr(&g_b) && g_b.resv_valid);
    TEST("...whose PUTLLC then still compares the bytes, as before");
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLC_CMD);
    CHECK(g_b.atomic_stat == 1);

    reset();
    TEST("a PUT spanning lines ends only the reservation on the line it covers");
    spu_atomic(&g_a, LSA_A, LINE_B, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_A + 0x200, MFC_GETLLAR_CMD);
    spu_dma(&g_b, 0x3000, LINE_A, 0x100, MFC_PUT_CMD);   /* LINE_A and LINE_B */
    CHECK(lr(&g_a));
    CHECK(!lr(&g_b) && g_b.resv_valid);

    reset();
    TEST("SDCRZ ends reservations on the lines it zeroes");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_dma(&g_b, 0, LINE_A, 0x80, MFC_SDCRZ_CMD);
    CHECK(lr(&g_a));

    reset();
    TEST("PPU stores: the reserved line ends the reservation, the neighbour does not");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    vm_write32(LINE_B + 4, 7);
    CHECK(!lr(&g_a) && g_a.resv_valid);
    vm_write8(LINE_A + 127, 1);
    CHECK(lr(&g_a));
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    g_a.event_status = 0;
    vm_write64(LINE_A + 8, 0x0102030405060708ull);
    CHECK(lr(&g_a));
    CHECK(vm_read64(LINE_A + 8) == 0x0102030405060708ull);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    g_a.event_status = 0;
    vm_write32(LINE_A + 4, vm_read32(LINE_A + 4));       /* same bytes */
    CHECK(lr(&g_a));

    reset();
    TEST("a PPU block store over two reserved lines ends both");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_B, MFC_GETLLAR_CMD);
    vm_memset(LINE_A + 0x70, 0x33, 0x20);
    CHECK(lr(&g_a) && lr(&g_b));
    CHECK(g_guest_mem[LINE_A + 0x70] == 0x33 && g_guest_mem[LINE_B + 0x0F] == 0x33);

    reset();
    TEST("a PPU store-conditional ends the reservation; a failed one does not");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    { ppu_context c; memset(&c, 0, sizeof c);
      uint32_t v = ppu_lwarx(&c, LINE_A + 0x10);
      vm_write32(LINE_A + 0x10, v + 9);                  /* the value moves */
      spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
      g_a.event_status = 0;
      CHECK(ppu_stwcx(&c, LINE_A + 0x10, v + 1) == 0);
      CHECK(!lr(&g_a) && g_a.resv_valid);
      v = ppu_lwarx(&c, LINE_A + 0x10);
      CHECK(ppu_stwcx(&c, LINE_A + 0x10, v + 1) == 1);
      CHECK(lr(&g_a)); }

    reset();
    TEST("a host line section that notifies ends the reservation; one that does not, does not");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_coh_write_begin(LINE_A, 0x80);
    g_guest_mem[LINE_A + 3] ^= 0;                      /* read, no change */
    spu_coh_write_end();
    CHECK(!lr(&g_a) && g_a.resv_valid);
    spu_coh_write_begin(LINE_A + 4, 4);
    g_guest_mem[LINE_A + 4] = 0x44;
    spu_coh_notify_write(LINE_A);
    spu_coh_write_end();
    CHECK(lr(&g_a));

    reset();
    TEST("an SPU with LR in its event mask settles at a hop (interrupts, bisled)");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    g_a.event_mask = SPU_EVENT_LR;
    vm_write32(LINE_A, 1);
    CHECK(!spu_coh_versions_on() || !(g_a.event_status & SPU_EVENT_LR));   /* nobody looked yet */
    SPU_LR_POLL(&g_a);
    CHECK((g_a.event_status & SPU_EVENT_LR) && !g_a.resv_valid);
}

/* ---------------------------------------------------------------------------
 * A parked SPU is woken by the write, not by its re-poll
 * -------------------------------------------------------------------------*/
static atomic_int g_parked, g_woke;
static void* parked_spu(void* arg)
{
    (void)arg;
    for (int r = 0; r < 50; r++) {
        spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
        g_a.event_status = 0;
        g_a.event_mask = SPU_EVENT_LR;
        atomic_store(&g_parked, r + 1);
        uint32_t v = spu_rdch(&g_a, SPU_RdEventStat)._u32[0];
        if (!(v & SPU_EVENT_LR)) { TEST("wait: woke without LR"); CHECK(0); }
        wrch(&g_a, SPU_WrEventAck, SPU_EVENT_LR);
        atomic_store(&g_woke, r + 1);
    }
    return NULL;
}

static void test_parked_wake(void)
{
    reset();
    g_spu_force_ch_block = 1;
    const unsigned long long wakes0 = g_spu_cohv_wakes;
    pthread_t t;
    pthread_create(&t, NULL, parked_spu, NULL);
    for (int r = 1; r <= 50; r++) {
        while (atomic_load(&g_parked) < r) sched_yield();
        struct timespec d = { 0, 200000 };   /* let it park (it may not have; both orders are tested) */
        if (r & 1) nanosleep(&d, NULL);
        vm_write32(LINE_A + 0x20, (uint32_t)r);
        while (atomic_load(&g_woke) < r) sched_yield();
    }
    pthread_join(t, NULL);
    g_spu_force_ch_block = 0;
    TEST("wait: every round woke");
    CHECK(atomic_load(&g_woke) == 50);
    TEST("wait: writers found parked waiters and woke them");
    CHECK(!spu_coh_versions_on() || g_spu_cohv_wakes > wakes0);
}

/* ---------------------------------------------------------------------------
 * Torture through the real paths: SPU PUTLLC, PPU stwcx. and PPU stores on
 * one line, counters exact
 * -------------------------------------------------------------------------*/
enum { T_SPUS = 3 };
static int T_ITERS = 2000;   /* COHV_PATHS_ITERS overrides */
static spu_context g_t[T_SPUS];
static atomic_int g_stop;

static void* t_spu(void* arg)
{
    spu_context* c = (spu_context*)arg;
    const uint32_t lsa = 0x4000;
    for (int done = 0; done < T_ITERS;) {
        spu_atomic(c, lsa, LINE_T, MFC_GETLLAR_CMD);
        uint32_t v; memcpy(&v, c->ls + lsa, 4); v++; memcpy(c->ls + lsa, &v, 4);
        spu_atomic(c, lsa, LINE_T, MFC_PUTLLC_CMD);
        if (c->atomic_stat == 0) done++;
    }
    return NULL;
}
static void* t_ppu_sc(void* arg)
{
    (void)arg;
    ppu_context c; memset(&c, 0, sizeof c);
    for (int done = 0; done < T_ITERS;) {
        uint32_t v = ppu_lwarx(&c, LINE_T + 8);
        done += ppu_stwcx(&c, LINE_T + 8, v + 1);
    }
    return NULL;
}
static void* t_ppu_store(void* arg)
{
    (void)arg;
    uint32_t v = 0;
    /* A store every microsecond or so: a PPU posting into the line, not a
     * store loop that leaves no window for a PUTLLC to commit in. */
    while (!atomic_load(&g_stop)) {
        vm_write32(LINE_T + 0x40, ++v);
        for (int i = 0; i < 4000; i++) __asm__ __volatile__("" ::: "memory");
    }
    return NULL;
}

static void test_torture(void)
{
    memset(g_guest_mem + LINE_T, 0, 128);
    pthread_t s[T_SPUS], p, q;
    for (int i = 0; i < T_SPUS; i++) { spu_context_init(&g_t[i], 0); g_t[i].spu_id = 2 + (uint32_t)i; }
    /* Mark the line before the threads start: the first reservation's window
     * (a store that tested the mark just before it was set) is outside what
     * this checks. */
    spu_atomic(&g_t[0], 0x4000, LINE_T, MFC_GETLLAR_CMD);
    pthread_create(&q, NULL, t_ppu_store, NULL);
    for (int i = 0; i < T_SPUS; i++) pthread_create(&s[i], NULL, t_spu, &g_t[i]);
    pthread_create(&p, NULL, t_ppu_sc, NULL);
    for (int i = 0; i < T_SPUS; i++) pthread_join(s[i], NULL);
    pthread_join(p, NULL);
    atomic_store(&g_stop, 1);
    pthread_join(q, NULL);
    uint32_t spu_ctr, ppu_ctr;
    memcpy(&spu_ctr, g_guest_mem + LINE_T, 4);
    ppu_ctr = vm_read32(LINE_T + 8);
    TEST("torture: every SPU PUTLLC increment survived");
    CHECK(spu_ctr == (uint32_t)T_SPUS * T_ITERS);
    TEST("torture: every PPU store-conditional increment survived");
    CHECK(ppu_ctr == (uint32_t)T_ITERS);
    if (spu_ctr != (uint32_t)T_SPUS * T_ITERS || ppu_ctr != (uint32_t)T_ITERS)
        fprintf(stderr, "  spu=%u ppu=%u\n", spu_ctr, ppu_ctr);
}

int main(void)
{
    /* COHV_PATHS_SCHEME=legacy runs the same checks on the default scheme,
     * for comparison: they are written to hold on both. */
    const char* scheme = getenv("COHV_PATHS_SCHEME");
    spu_coh_versions_install(!(scheme && !strcmp(scheme, "legacy")));
    const char* it = getenv("COHV_PATHS_ITERS");
    if (it && atoi(it) > 0) T_ITERS = atoi(it);
    test_writers();
    test_parked_wake();
    test_torture();
    printf("\nversion counter path tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the channel layer, stubbed (as test_spu_lockline_peer.c).
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) { (void)c; (void)a; (void)b; (void)d; }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
