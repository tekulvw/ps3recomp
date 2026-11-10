/* cellSpursModulePoll preemption (PS3RECOMP_SPURS_MODULE_POLL_PREEMPT=1)
 * through the real initializer, coordinator, workers and policy runner.
 * Manufactured policy modules only; no game code or data.
 *
 * Workload A ("idler") fills every SPU. Its module polls the kernel every
 * 50 us through the selectWorkload address and exits only when the poll
 * names another workload or after IDLE_MS. Workload B is requested through
 * the instance's workload flag (receiver B, flag word 0) once every SPU
 * holds an idler. B's module records when it starts.
 *
 * argv[1]: lanes. argv[2]: "flag" (B flagged), "lowprio" (B flagged but
 * outranked: A is requested and has the higher priority on every SPU).
 * Prints: B's flag-to-start latency, how many idler polls were answered with
 * a switch, how many times B ran, how many idler runs there were. */
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
void spu_indirect_branch(spu_context*);
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
int g_resv_store_active;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return 1000ull + (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
}

enum { GUEST = 16u << 20, SPURS_EA = 0x100000, PM_A = 0x200000, PM_B = 0x201000, PM_SIZE = 0x100,
       WID_A = 10, WID_B = 4, IDLE_MS = 300 };
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static volatile uint32_t g_idlers_running, g_idler_runs, g_switch_answers, g_b_runs, g_stop;
static volatile uint64_t g_b_start_ns;

static void idler(spu_context* c)
{
    __atomic_add_fetch(&g_idler_runs, 1, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&g_idlers_running, 1, __ATOMIC_ACQ_REL);
    const uint64_t until = now_ns() + (uint64_t)IDLE_MS * 1000000ull;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) && now_ns() < until) {
        /* bisl through the kernel's selectWorkload address, as a module does */
        c->gpr[0]._u32[0] = 0x0A40; c->pc = SPURS_PM_SELECT_WORKLOAD_LS;
        spu_indirect_branch(c);
        if (c->gpr[3]._u32[0] != WID_A) { __atomic_add_fetch(&g_switch_answers, 1, __ATOMIC_ACQ_REL); break; }
        const struct timespec d = { 0, 50000 }; nanosleep(&d, NULL);
    }
    __atomic_sub_fetch(&g_idlers_running, 1, __ATOMIC_ACQ_REL);
    c->pc = SPURS_PM_EXIT_TO_KERNEL_LS;
    g_spu_trampoline_fn = spu_indirect_branch;
}
static void flagged(spu_context* c)
{
    if (!__atomic_load_n(&g_b_start_ns, __ATOMIC_ACQUIRE)) __atomic_store_n(&g_b_start_ns, now_ns(), __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_b_runs, 1, __ATOMIC_ACQ_REL);
    c->pc = SPURS_PM_EXIT_TO_KERNEL_LS;
    g_spu_trampoline_fn = spu_indirect_branch;
}
static void add_workload(uint32_t wid, uint32_t pm, spu_lifted_entry_fn fn, uint32_t lanes, uint32_t prio, uint32_t maxcont)
{
    for (uint32_t i = 0; i < PM_SIZE; i++) vm_base[pm + i] = (uint8_t)(i * 7u + wid * 13u + 1u);
    static int registered[16];
    if (!registered[wid]) { registered[wid] = 1; spu_workload_register_img(spu_workload_fingerprint(vm_base + pm, PM_SIZE), fn, 40 + (int)wid, "fixture"); }
    s_workloads[wid].in_use = 1; s_workloads[wid].is_taskset = 0;
    s_workloads[wid].pm = (const void*)(uintptr_t)pm; s_workloads[wid].sizePm = PM_SIZE;
    s_workloads[wid].data = 0x300000u + wid * 0x100u; s_workloads[wid].spurs_ea = SPURS_EA;
    vm_base[SPURS_EA + SPURS_WKL_STATE1 + wid] = 2;
    vm_base[SPURS_EA + SPURS_WKL_MAXCONT + wid] = (uint8_t)maxcont;
    for (uint32_t sn = 0; sn < lanes; sn++) vm_base[SPURS_EA + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ + 0x18 + sn] = (uint8_t)prio;
}
int main(int argc, char** argv)
{
    const uint32_t lanes = argc > 1 ? (uint32_t)atoi(argv[1]) : 5u;
    const int lowprio = argc > 2 && !strcmp(argv[2], "lowprio");
    vm_base = (uint8_t*)calloc(1, GUEST); ppu_vm_size = GUEST;
    { extern uint8_t g_vm_page_bitmap[65536 / 8]; memset(g_vm_page_bitmap, 255, sizeof g_vm_page_bitmap); }
    assert(spurs_initialize_common(SPURS_EA, lanes, "fixture") == CELL_OK);
    add_workload(WID_A, PM_A, idler, lanes, lowprio ? 8 : 1, lanes);
    add_workload(WID_B, PM_B, flagged, lanes, lowprio ? 1 : 1, 1);
    vm_write32(SPURS_EA + SPURS_WKL_ENABLED, (0x80000000u >> WID_A) | (0x80000000u >> WID_B));
    /* Fill every SPU with an idler; with "lowprio" A stays requested (ready
     * above its contention), so it outranks B on priority. */
    vm_write8(SPURS_EA + SPURS_WKL_READY1 + WID_A, (uint8_t)(lowprio ? 0xFF : lanes));
    const uint64_t t0 = now_ns();
    while (__atomic_load_n(&g_idlers_running, __ATOMIC_ACQUIRE) < lanes) {
        if (now_ns() - t0 > 5000000000ull) { fprintf(stderr, "FAIL: idlers never filled the SPUs\n"); return 1; }
        const struct timespec d = { 0, 100000 }; nanosleep(&d, NULL);
    }
    /* Request B through the workload flag: receiver B, flag word 0. */
    vm_write8(SPURS_EA + SPURS_WKL_FLAG_RECEIVER, WID_B);
    const uint64_t flag_ns = now_ns();
    vm_write32(SPURS_EA + SPURS_WKL_FLAG_VALUE, 0);
    while (!__atomic_load_n(&g_b_start_ns, __ATOMIC_ACQUIRE) && now_ns() - flag_ns < 2000000000ull) {
        const struct timespec d = { 0, 100000 }; nanosleep(&d, NULL);
    }
    const uint64_t b_ns = __atomic_load_n(&g_b_start_ns, __ATOMIC_ACQUIRE);
    /* Let the system settle: ping-pong would show as more switches or B runs. */
    const struct timespec settle = { 0, 50000000 }; nanosleep(&settle, NULL);
    printf("lanes=%u mode=%s b_latency_us=%.1f switch_answers=%u b_runs=%u idler_runs=%u\n", lanes,
           lowprio ? "lowprio" : "flag", b_ns ? (b_ns - flag_ns) / 1000.0 : -1.0,
           g_switch_answers, g_b_runs, g_idler_runs);
    fflush(stdout);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    _exit(0);
}
