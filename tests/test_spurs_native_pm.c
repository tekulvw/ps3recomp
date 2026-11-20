/* Native policy-module seam (PS3RECOMP_SPURS_NATIVE_PM) through the real
 * SPURS initializer, coordinator, worker, policy runner, resolver and drain.
 * Manufactured policy images, policy modules and job code only; no title data.
 *
 * argv[1] scenario:
 *   sched [reg|badsha|none] -- one lane runs workload WID RUNS times. The
 *       lifted module and the native one do the same guest work: GETLLAR,
 *       increment the counter in the workload's line, PUTLLC, one kernel poll,
 *       exit. "reg" registers a native module for the image with the right
 *       SHA-256, "badsha" with a wrong one, "none" registers nothing.
 *   direct -- calls spu_run_policy_module_native directly: services through
 *       the window (result in r3, return to r0, r80-r127 kept), job returns by
 *       host return and by a computed jump, the payload hook, a run that does
 *       not exit, and the inert payload hook default.
 *   badslot -- job code branches to an empty window slot (must stop the process).
 * Prints one "result" line of key=value fields. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
#include "cellSpurs.c"
#include "spurs_native_pm.h"
#include "spu_helpers.h"
void spu_indirect_branch(spu_context*);
void spu_drain_call(spu_context*, uint32_t);
void spu_wrch(spu_context*, uint32_t, u128);
u128 spu_rdch(spu_context*, uint32_t);
void spu_begin_image(int);
void spu_register_function(uint32_t, void (*)(spu_context*));
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
int g_resv_store_active;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return 1000ull + (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
}

enum { GUEST = 16u << 20, SPURS_EA = 0x100000, PM_EA = 0x200000, PM_SIZE = 0x180, LIST_EA = 0x300000,
       WID = 3, RUNS = 6, IMAGE = 61,
       LS_LINE = 0x1000, LS_CTX = 0x1400, JOB_A = 0x4000, JOB_B = 0x4100, JOB_C = 0x4200, JOB_BAD = 0x4300,
       JOB_SITE = 0x4800, STACK = 0x3FF00, MARK = 0x7E57AB1Eu };
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static volatile uint32_t g_lifted_runs, g_native_runs, g_services, g_job_ok, g_job_bad, g_hook_calls,
    g_job_c_ran, g_observed_services, g_modes, g_lifted_saw_services, g_poll_answer;

static void mfc(spu_context* c, uint32_t cmd, uint32_t lsa, uint32_t ea)
{
    spu_wrch(c, MFC_LSA, spu_make_preferred_u32(lsa));
    spu_wrch(c, MFC_EAH, spu_make_preferred_u32(0));
    spu_wrch(c, MFC_EAL, spu_make_preferred_u32(ea));
    spu_wrch(c, MFC_Size, spu_make_preferred_u32(128));
    spu_wrch(c, MFC_TagID, spu_make_preferred_u32(0));
    spu_wrch(c, MFC_Cmd, spu_make_preferred_u32(cmd));
}
/* The guest work both module forms do: count this run in the workload's line. */
static void count_run(spu_context* c, uint32_t list_ea)
{
    for (;;) {
        mfc(c, MFC_GETLLAR_CMD, LS_LINE, list_ea); (void)spu_rdch(c, MFC_RdAtomicStat);
        spu_ls_write32(c, LS_LINE, spu_ls_read32(c, LS_LINE) + 1);
        mfc(c, MFC_PUTLLC_CMD, LS_LINE, list_ea);
        if (spu_rdch(c, MFC_RdAtomicStat)._u32[0] == 0) return;
    }
}

/* ---- the lifted module (a C stand-in, registered under the image fingerprint) */
static void lifted_pm(spu_context* c)
{
    __atomic_add_fetch(&g_lifted_runs, 1, __ATOMIC_ACQ_REL);
    if (c->native_services) __atomic_add_fetch(&g_lifted_saw_services, 1, __ATOMIC_ACQ_REL);
    count_run(c, c->gpr[4]._u32[1]);
    c->gpr[0] = spu_link(0x0A40); c->pc = SPURS_PM_SELECT_WORKLOAD_LS;    /* bisl selectWorkload */
    spu_indirect_branch(c);
    __atomic_store_n(&g_poll_answer, c->gpr[3]._u32[0], __ATOMIC_RELEASE);
    c->pc = SPURS_PM_EXIT_TO_KERNEL_LS;
    g_spu_trampoline_fn = spu_indirect_branch;
}

/* ---- job code (lifted-style C), entered by the native module ------------- */
static void set_mark(spu_context* c) { for (int r = 80; r < 128; r++) c->gpr[r] = spu_make_preferred_u32(MARK + (uint32_t)r); }
static int mark_ok(spu_context* c)
{
    for (int r = 80; r < 128; r++) if (c->gpr[r]._u32[0] != MARK + (uint32_t)r) return 0;
    return 1;
}
/* bisl $0 through the service address at context +0x04*slot (the lifter's bracket) */
static uint32_t call_service(spu_context* c, uint32_t ctx_ls, unsigned slot, uint32_t arg)
{
    uint32_t target = spu_ls_read32(c, ctx_ls + 4 * slot);
    c->gpr[0] = spu_link(JOB_SITE + 4); int32_t si = (int32_t)c->image_id;
    c->gpr[3] = spu_make_preferred_u32(ctx_ls); c->gpr[4] = spu_make_preferred_u32(arg);
    c->pc = target; c->host_depth++;
    spu_indirect_branch(c); spu_drain_call(c, JOB_SITE + 4);
    c->host_depth--; spu_img_restore(c, si);
    if ((c->pc & SPU_LS_MASK) != JOB_SITE + 4) __atomic_add_fetch(&g_job_bad, 1, __ATOMIC_ACQ_REL);
    return c->gpr[3]._u32[0];
}
static void job_a(spu_context* c)   /* returns by host return (`bi $r0`) */
{
    const uint32_t ctx_ls = c->gpr[3]._u32[0], arg = c->gpr[4]._u32[0];
    const u128 link = c->gpr[0];
    set_mark(c);
    uint32_t r = call_service(c, ctx_ls, 0, arg);
    uint32_t q = call_service(c, ctx_ls, 1, r);
    if (r == arg + 0x100 && q == (r ^ 0xFFFF) && mark_ok(c)) __atomic_add_fetch(&g_job_ok, 1, __ATOMIC_ACQ_REL);
    else __atomic_add_fetch(&g_job_bad, 1, __ATOMIC_ACQ_REL);
    c->gpr[0] = link;
    SPU_RET(c);
}
static void job_b(spu_context* c)   /* returns by a computed jump to r0 */
{
    __atomic_add_fetch(&g_job_ok, 1, __ATOMIC_ACQ_REL);
    c->pc = c->gpr[0]._u32[0];
    g_spu_trampoline_fn = spu_indirect_branch;
}
static void job_c(spu_context* c) { __atomic_add_fetch(&g_job_c_ran, 1, __ATOMIC_ACQ_REL); SPU_RET(c); }
static void job_bad(spu_context* c)   /* calls an empty window slot */
{
    c->gpr[0] = spu_link(JOB_SITE + 4); c->pc = SPURS_NATIVE_PM_SLOT_LS(5); c->host_depth++;
    spu_indirect_branch(c); spu_drain_call(c, JOB_SITE + 4);
    c->host_depth--;
    SPU_RET(c);
}

/* ---- the native module ---------------------------------------------------- */
static int svc_add(spu_context* c)
{
    __atomic_add_fetch(&g_services, 1, __ATOMIC_ACQ_REL);
    c->gpr[3] = spu_make_preferred_u32(c->gpr[4]._u32[0] + 0x100);
    return SPURS_NATIVE_SERVICE_RETURN;
}
static int svc_flip(spu_context* c)
{
    __atomic_add_fetch(&g_services, 1, __ATOMIC_ACQ_REL);
    c->gpr[3] = spu_make_preferred_u32(c->gpr[4]._u32[0] ^ 0xFFFF);
    return SPURS_NATIVE_SERVICE_RETURN;
}
static const struct spurs_native_services s_services = { { svc_add, svc_flip } };
static int g_direct, g_direct_rc_mode;
static int native_run(spu_context* c, const struct spurs_native_pm_run* a)
{
    __atomic_add_fetch(&g_native_runs, 1, __ATOMIC_ACQ_REL);
    __atomic_or_fetch(&g_modes, 1u << a->mode, __ATOMIC_ACQ_REL);
    if (c->native_services == &s_services) __atomic_add_fetch(&g_observed_services, 1, __ATOMIC_ACQ_REL);
    assert(a->wid == WID && a->pm_size == PM_SIZE && (uint32_t)a->wkl_data == LIST_EA && a->image_id == IMAGE);
    count_run(c, (uint32_t)a->wkl_data);
    spurs_kernel_select_workload(c);
    __atomic_store_n(&g_poll_answer, c->gpr[3]._u32[0], __ATOMIC_RELEASE);
    if (g_direct) {
        memset(c->ls + LS_CTX, 0, 0x40);
        spu_ls_write32(c, LS_CTX + 0, SPURS_NATIVE_PM_SLOT_LS(0));
        spu_ls_write32(c, LS_CTX + 4, SPURS_NATIVE_PM_SLOT_LS(1));
        if (spurs_native_pm_enter_job(c, JOB_A, STACK, 0x1000, LS_CTX, 7) != 0) g_job_bad++;
        if (spurs_native_pm_enter_job(c, JOB_B, STACK, 0x1000, LS_CTX, 0) != 0) g_job_bad++;
        if (spurs_native_pm_enter_job(c, JOB_C, STACK, 0x1000, LS_CTX, 0) != 0) g_job_bad++;   /* hook claims it */
        if (g_direct == 2) spurs_native_pm_enter_job(c, JOB_BAD, STACK, 0x1000, LS_CTX, 0);
        if (g_direct_rc_mode) return 1;
    }
    return 0;
}
static int hook(spu_context* c, uint32_t entry)
{
    (void)c;
    __atomic_add_fetch(&g_hook_calls, 1, __ATOMIC_ACQ_REL);
    return entry == JOB_C;
}
static struct spurs_native_pm s_native = { "fixture-native", 0, {0}, native_run, &s_services };

static void setup_image(void)
{
    for (uint32_t i = 0; i < PM_SIZE; i++) vm_base[PM_EA + i] = (uint8_t)(i * 11u + 5u);
    const uint64_t fp = spu_workload_fingerprint(vm_base + PM_EA, PM_SIZE);
    spu_workload_register_img(fp, lifted_pm, IMAGE, "fixture");
    s_native.fingerprint = fp;
    spurs_native_pm_sha256(vm_base + PM_EA, PM_SIZE, s_native.sha256);
    spu_begin_image(IMAGE);
    spu_register_function(JOB_A, job_a);
    spu_register_function(JOB_B, job_b);
    spu_register_function(JOB_C, job_c);
    spu_register_function(JOB_BAD, job_bad);
}

static int sched(const char* reg)
{
    if (!strcmp(reg, "reg")) assert(spurs_native_pm_register(&s_native) == 0);
    else if (!strcmp(reg, "badsha")) { s_native.sha256[7] ^= 1; assert(spurs_native_pm_register(&s_native) == 0); }
    assert(spurs_initialize_common(SPURS_EA, 1, "fixture") == CELL_OK);
    s_workloads[WID].in_use = 1; s_workloads[WID].is_taskset = 0;
    s_workloads[WID].pm = (const void*)(uintptr_t)PM_EA; s_workloads[WID].sizePm = PM_SIZE;
    s_workloads[WID].data = LIST_EA; s_workloads[WID].spurs_ea = SPURS_EA;
    vm_write64(SPURS_EA + SPURS_WKL_INFO1 + WID * SPURS_WKL_INFO_SZ + 8, LIST_EA);
    vm_base[SPURS_EA + SPURS_WKL_STATE1 + WID] = 2;
    vm_base[SPURS_EA + SPURS_WKL_MAXCONT + WID] = 1;
    vm_base[SPURS_EA + SPURS_WKL_INFO1 + WID * SPURS_WKL_INFO_SZ + 0x18] = 1;
    vm_write32(SPURS_EA + SPURS_WKL_ENABLED, 0x80000000u >> WID);
    for (int r = 0; r < RUNS; r++) {
        /* one request at a time: each run is admitted, runs, and returns */
        const uint32_t before = vm_read32(LIST_EA);
        vm_write8(SPURS_EA + SPURS_WKL_READY1 + WID, 1);
        const uint64_t t0 = now_ns();
        while (vm_read32(LIST_EA) == before || vm_read8(SPURS_EA + SPURS_WKL_CURCONT + WID) != 0) {
            if (now_ns() - t0 > 5000000000ull) { fprintf(stderr, "FAIL: run %d never completed\n", r); return 1; }
            const struct timespec d = { 0, 100000 }; nanosleep(&d, NULL);
        }
    }
    const struct timespec settle = { 0, 20000000 }; nanosleep(&settle, NULL);
    struct SpursInst* si = &s_inst[0];
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < 0x1000; i++) { h ^= vm_base[SPURS_EA + i]; h *= 1099511628211ull; }
    for (uint32_t i = 0; i < 0x80; i++) { h ^= vm_base[LIST_EA + i]; h *= 1099511628211ull; }
    printf("result count=%u lifted=%u native=%u consults=%lu services_installed=%u lifted_saw_services=%u "
           "modes=%u poll=%u curcont=%u active=%u idle=%u ready=%u gate=%d mem=%016llX\n",
           vm_read32(LIST_EA), g_lifted_runs, g_native_runs, spurs_native_pm_consults(), g_observed_services,
           g_lifted_saw_services, g_modes, g_poll_answer, vm_read8(SPURS_EA + SPURS_WKL_CURCONT + WID),
           si->active[WID], si->idle[WID], vm_read8(SPURS_EA + SPURS_WKL_READY1 + WID), g_spu_dispatch_gates.pm_extras,
           (unsigned long long)h);
    fflush(stdout);
    _exit(0);
}

static int direct(int bad)
{
    g_direct = bad ? 2 : 1;
    const uint8_t* pm = vm_base + PM_EA;
    int dflt = spurs_native_pm_payload_hook() == NULL;
    spurs_native_pm_set_payload_hook(hook);
    int rc0 = spu_run_policy_module_native(&s_native, lifted_pm, IMAGE, pm, PM_SIZE, LIST_EA, WID, SPURS_EA, 0);
    g_direct_rc_mode = 1;
    int rc1 = spu_run_policy_module_native(&s_native, lifted_pm, IMAGE, pm, PM_SIZE, LIST_EA, WID, SPURS_EA, 0);
    g_direct_rc_mode = 0;
    /* the same resident context afterwards: a native run must leave no service table behind */
    int rc2 = spu_run_policy_module(lifted_pm, IMAGE, pm, PM_SIZE, LIST_EA, WID, SPURS_EA, 0);
    printf("result rc0=%d rc1=%d rc2=%d native=%u lifted=%u services=%u job_ok=%u job_bad=%u hook=%u job_c=%u "
           "services_installed=%u lifted_saw_services=%u hook_default_null=%d count=%u\n",
           rc0, rc1, rc2, g_native_runs, g_lifted_runs, g_services, g_job_ok, g_job_bad, g_hook_calls, g_job_c_ran,
           g_observed_services, g_lifted_saw_services, dflt, vm_read32(LIST_EA));
    fflush(stdout);
    return 0;
}

int main(int argc, char** argv)
{
    vm_base = (uint8_t*)calloc(1, GUEST); ppu_vm_size = GUEST;
    { extern uint8_t g_vm_page_bitmap[65536 / 8]; memset(g_vm_page_bitmap, 255, sizeof g_vm_page_bitmap); }
    setup_image();
    const char* mode = argc > 1 ? argv[1] : "sched";
    if (!strcmp(mode, "sched")) return sched(argc > 2 ? argv[2] : "none");
    if (!strcmp(mode, "direct")) return direct(0);
    if (!strcmp(mode, "badslot")) return direct(1);
    return 2;
}
