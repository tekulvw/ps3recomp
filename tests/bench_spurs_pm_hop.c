/* Synthetic SPURS policy-module hop chain through the real scheduler.
 *
 * Two workloads, A and B, each with a hand-written policy module (no lifted
 * code, no game data). Each PM run counts a hop and raises the OTHER
 * workload's ready count, then exits to the kernel, so the chain is serial:
 * one PM run at a time, each started by the previous one's request.
 *
 * spu kick: the PM raises the ready count with GETLLAR/PUTLLC on the SPURS
 *           instance's first line, as an SPU does (the commit wakes the
 *           coordinator through the line-commit hook).
 * ppu kick: the PM only records that it ran; a host thread standing in for
 *           the PPU raises the next ready count with a plain store once it
 *           sees the hop, through the coherent store path a PPU store takes
 *           (vm_write8).
 *           ppu-idle=N waits N us first, so the workers are asleep, as they
 *           are when a PPU tick releases the next work.
 *
 * Reported per hop: the wall time, and the latency from one PM run's request
 * to the next PM run's start (its exit, the worker's bookkeeping, the
 * coordinator's pass and the wake of the worker that runs it).
 *
 * argv: hops (default 2000), lanes (nSpus, default 1), spu | ppu | ppu-idle=N.
 * The fixture fails if the chain stalls; every hop must run. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
#include "../libs/spurs/cellSpurs.c"
u128 spu_rdch(spu_context*, uint32_t);
void spu_wrch(spu_context*, uint32_t, u128);
void spu_indirect_branch(spu_context*);
/* Inert stand-ins for the PPU side this fixture does not link. */
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
int g_resv_store_active;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return 1000ull + (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
}

enum { GUEST = 16u << 20, SPURS_EA = 0x100000, PM_A = 0x200000, PM_B = 0x201000, PM_SIZE = 0x100 };
static volatile uint32_t g_hops, g_target, g_ppu_kick, g_ppu_idle_us;
static uint64_t g_start_ns[1 << 16], g_kick_ns[1 << 16];
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static u128 u32v(uint32_t v) { u128 r; memset(&r, 0, sizeof r); r._u32[0] = v; return r; }
static void ch_w(spu_context* c, uint32_t ch, uint32_t v) { spu_wrch(c, ch, u32v(v)); }
static uint32_t ch_r(spu_context* c, uint32_t ch) { return spu_rdch(c, ch)._u32[0]; }

/* Raise wid's ready count to 1 with an SPU atomic on the instance line. */
static void spu_kick(spu_context* c, uint32_t wid)
{
    for (;;) {
        ch_w(c, MFC_LSA, 0x3F000); ch_w(c, MFC_EAH, 0); ch_w(c, MFC_EAL, SPURS_EA); ch_w(c, MFC_Cmd, MFC_GETLLAR_CMD);
        (void)ch_r(c, MFC_RdAtomicStat);
        c->ls[0x3F000 + SPURS_WKL_READY1 + wid] = 1;
        ch_w(c, MFC_LSA, 0x3F000); ch_w(c, MFC_EAH, 0); ch_w(c, MFC_EAL, SPURS_EA); ch_w(c, MFC_Cmd, MFC_PUTLLC_CMD);
        if (ch_r(c, MFC_RdAtomicStat) == 0) return;
    }
}
static void pm_run(spu_context* c, uint32_t next)
{
    const uint64_t t = now_ns();
    const uint32_t h = __atomic_load_n(&g_hops, __ATOMIC_ACQUIRE);
    if (h < (1u << 16)) g_start_ns[h] = t;
    if (h + 1 < g_target) {
        if (h + 1 < (1u << 16)) g_kick_ns[h + 1] = now_ns();
        if (!g_ppu_kick) spu_kick(c, next);
    }
    __atomic_store_n(&g_hops, h + 1, __ATOMIC_RELEASE);
    c->pc = SPURS_PM_EXIT_TO_KERNEL_LS;          /* return to the kernel */
    g_spu_trampoline_fn = spu_indirect_branch;
}
static void pm_a(spu_context* c) { pm_run(c, 1); }
static void pm_b(spu_context* c) { pm_run(c, 0); }

static void add_workload(uint32_t wid, uint32_t pm, spu_lifted_entry_fn fn, uint32_t nspus)
{
    for (uint32_t i = 0; i < PM_SIZE; i++) vm_base[pm + i] = (uint8_t)(i * 7u + wid * 13u + 1u);
    spu_workload_register_img(spu_workload_fingerprint(vm_base + pm, PM_SIZE), fn, 40 + (int)wid, "bench");
    s_workloads[wid].in_use = 1; s_workloads[wid].is_taskset = 0;
    s_workloads[wid].pm = (const void*)(uintptr_t)pm; s_workloads[wid].sizePm = PM_SIZE;
    s_workloads[wid].data = 0x300000u + wid * 0x100u; s_workloads[wid].spurs_ea = SPURS_EA;
    vm_base[SPURS_EA + SPURS_WKL_STATE1 + wid] = 2;
    vm_base[SPURS_EA + SPURS_WKL_MAXCONT + wid] = 1;
    for (uint32_t sn = 0; sn < nspus; sn++) vm_base[SPURS_EA + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ + 0x18 + sn] = 1;
}

static int cmp64(const void* a, const void* b) { uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b; return x < y ? -1 : x > y; }

int main(int argc, char** argv)
{
    const uint32_t hops = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 0) : 2000u;
    const uint32_t nspus = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 1u;
    g_ppu_kick = argc > 3 && !strncmp(argv[3], "ppu", 3);
    /* ppu-idle=N: the stand-in raises the next request N us after the hop, when
     * the workers have gone back to sleep, as a PPU tick does. */
    if (argc > 3 && !strncmp(argv[3], "ppu-idle=", 9)) g_ppu_idle_us = (uint32_t)strtoul(argv[3] + 9, 0, 0);
    assert(hops >= 2 && hops <= (1u << 16) && nspus >= 1 && nspus <= 6);
    vm_base = (uint8_t*)calloc(1, GUEST); ppu_vm_size = GUEST;
    { extern uint8_t g_vm_page_bitmap[65536 / 8]; memset(g_vm_page_bitmap, 255, sizeof g_vm_page_bitmap); }
    add_workload(0, PM_A, pm_a, nspus);
    add_workload(1, PM_B, pm_b, nspus);
    assert(spurs_initialize_common(SPURS_EA, nspus, "bench") == CELL_OK);
    /* initialize zeroed the instance: publish the workloads' guest fields again. */
    add_workload(0, PM_A, pm_a, nspus);
    add_workload(1, PM_B, pm_b, nspus);
    vm_write32(SPURS_EA + SPURS_WKL_ENABLED, 0xC0000000u);
    g_target = hops;
    const uint64_t t0 = now_ns();
    g_kick_ns[0] = t0;
    vm_write8(SPURS_EA + SPURS_WKL_READY1 + 0, 1);         /* first request, as the PPU makes it */
    uint32_t seen = 0;
    while (__atomic_load_n(&g_hops, __ATOMIC_ACQUIRE) < hops) {
        const uint32_t h = __atomic_load_n(&g_hops, __ATOMIC_ACQUIRE);
        if (g_ppu_kick && h != seen) {                         /* PPU stand-in: plain store */
            seen = h;
            if (g_ppu_idle_us) { struct timespec d = {0, (long)g_ppu_idle_us * 1000}; nanosleep(&d, NULL); }
            if (h < hops) { g_kick_ns[h] = now_ns(); vm_write8(SPURS_EA + SPURS_WKL_READY1 + (h & 1), 1); }
        }
        if (now_ns() - t0 > 60ull * 1000000000ull) { fprintf(stderr, "FAIL: stalled at hop %u\n", h); return 1; }
    }
    const uint64_t t1 = now_ns();
    uint64_t lat[1 << 16]; uint32_t n = 0;
    for (uint32_t h = 1; h < hops; h++) lat[n++] = g_start_ns[h] - g_kick_ns[h];
    qsort(lat, n, sizeof lat[0], cmp64);
    printf("pm_hop kick=%s idle_us=%u nspus=%u hops=%u per_hop_us=%.2f request_to_start_us p50=%.2f p90=%.2f p99=%.2f max=%.2f\n",
           g_ppu_kick ? "ppu" : "spu", g_ppu_idle_us, nspus, hops, (t1 - t0) / 1000.0 / hops,
           lat[n / 2] / 1000.0, lat[n * 9 / 10] / 1000.0, lat[n * 99 / 100] / 1000.0, lat[n - 1] / 1000.0);
    fflush(stdout);
    _exit(0);
}
