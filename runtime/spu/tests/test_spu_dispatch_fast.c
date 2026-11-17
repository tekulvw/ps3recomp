/* Synthetic indirect-branch scenarios for PS3RECOMP_SPU_DISPATCH_FAST.
 *
 * Each scenario sets up a context, branches once and prints the resulting
 * state on stdout. tools/test_spu_dispatch_fast.py runs this program with the
 * switch off and on, under each diagnostic switch, and requires identical
 * output; it also builds one mutant per fast-path exclusion and requires the
 * output to change. The scenarios cover every case the fast path must leave
 * to the full resolver: saved interrupt registers, the two policy-module
 * service addresses, the taskset syscall address, the resolver-only image,
 * the task resume that restarts at depth zero, a miss, the job-return address
 * and the diagnostics. The last part checks the overlay residency counters. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
#include "../spu_channels.c"
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
unsigned long long ps3_ms_now(void) { return 1000; }
void spurs_ef_set_from_spu(uint32_t a, uint16_t b) { (void)a; (void)b; }
uint64_t spurs_pm_build_context(uint8_t* a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; return 0;
}
void ydkj_wake_all_event_flags(void) {}

static int landed;
static void land_low(spu_context* c)   { (void)c; landed = 1; }
static void land_task(spu_context* c)  { (void)c; landed = 2; }
static void land_svc(spu_context* c)   { (void)c; landed = 3; }
static void land_cri(spu_context* c)   { (void)c; landed = 4; }
static void land_ret(spu_context* c)   { (void)c; landed = 5; }
static void land_job(spu_context* c)   { (void)c; landed = 6; }
static void land_resume(spu_context* c){ (void)c; landed = 7; }

static uint32_t ls_hash(const spu_context* c)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < SPU_LS_SIZE; ++i) h = (h ^ c->ls[i]) * 16777619u;
    return h;
}
static void put32(spu_context* c, uint32_t a, uint32_t v)
{
    c->ls[a] = (uint8_t)(v >> 24); c->ls[a + 1] = (uint8_t)(v >> 16);
    c->ls[a + 2] = (uint8_t)(v >> 8); c->ls[a + 3] = (uint8_t)v;
}
static void fresh(spu_context* c, int image, uint32_t pc)
{
    c->image_id = image; c->status = SPU_STATUS_RUNNING; c->pc = pc;
    c->policy_mode = 0; c->irq_saved = 0; c->int_enable = 0; c->host_depth = 0;
    c->resident_task = 0; c->resident_ovl = 0; c->stop_code = 0;
    memset(c->gpr, 0, sizeof c->gpr);
}
/* One branch inside a halt scope, as spu_run_with_halt provides one. */
static int branch(spu_context* c)
{
    volatile int code;
    s_spu_halt_armed = 1;
    code = SPU_SETJMP(s_spu_halt_env);
    if (code == 0) spu_indirect_branch(c);
    s_spu_halt_armed = 0;
    return code;
}
static void report(const char* name, spu_context* c, int code)
{
    /* The fast path hands its lookup over and the full resolver takes it. */
    assert(t_ib_pre.ctx == NULL);
    printf("%-14s land=%d jmp=%d pc=%05X task=%d status=%u stop=%X r3=%08X:%08X r7=%08X irq=%d "
           "ls=%08X rdv=%d\n",
           name, landed, code, (unsigned)c->pc, c->resident_task, (unsigned)c->status,
           (unsigned)c->stop_code, c->gpr[3]._u32[0], c->gpr[3]._u32[1], c->gpr[7]._u32[0],
           c->irq_saved, ls_hash(c), c->region_dispatch_valid);
    landed = 0;
}

int main(void)
{
    /* Big enough for the job-done diagnostic's completion word; untouched
     * pages cost nothing. */
    vm_base = calloc(1, 0x48000000u); assert(vm_base);
    spu_context* c = calloc(1, sizeof(*c)); assert(c);
    spu_context_init(c, 0);

    spu_begin_image(3);
    spu_register_function(0xA00, land_low);
    spu_register_function(0x9C0, land_svc);
    spu_register_function(0x9D0, land_svc);
    spu_register_function(0x3FF00, land_ret);
    spu_register_function(0x4A40, land_job);
    spu_begin_image(22);
    spu_register_function(0xA70, land_svc);
    spu_begin_image(23);
    spu_register_function(0xA00, land_cri);
    spu_begin_image(40);
    spu_register_function(0x3100, land_task);
    spu_begin_image(41);
    spu_register_function(0x3200, land_resume);
    spu_taskset_register_task_entry(0x3100, 40);
    spu_taskset_register_task_elf(0x00100000u, 41, 0);

    int code;
    /* An ordinary hit: the pc is masked and the resident task cleared. */
    fresh(c, 3, 0x80000A00u); c->resident_task = 40;
    code = branch(c); report("hit-low", c, code);
    /* A task entry above the kernel area: the task image becomes resident. */
    fresh(c, 3, 0x3100);
    code = branch(c); report("task-entry", c, code);
    /* Saved interrupt registers, eligible to restore at this pc. */
    fresh(c, 3, 0xA00); c->irq_saved = 1; c->int_enable = 1; c->irq_resume_pc = 0xA00;
    c->gpr[7]._u32[0] = 11; c->irq_gpr[7]._u32[0] = 42;
    code = branch(c); report("irq-restore", c, code);
    /* Policy-module services: select-workload returns to the link register,
     * exit-to-kernel stops the context. Both have lifted code at the address. */
    fresh(c, 3, 0x9D0); c->policy_mode = 1; c->gpr[0]._u32[0] = 0xA00;
    put32(c, 0x1C0, 1); put32(c, 0x1C4, 0x2000); put32(c, 0x1DC, 5);
    code = branch(c); report("pm-select", c, code);
    put32(c, 0x1C0, 0); put32(c, 0x1C4, 0); put32(c, 0x1DC, 0);
    fresh(c, 3, 0x9C0); c->policy_mode = 1;
    code = branch(c); report("pm-exit", c, code);
    /* Policy mode alone at an ordinary address is a plain hit. */
    fresh(c, 3, 0xA00); c->policy_mode = 1;
    code = branch(c); report("pm-plain", c, code);
    /* The taskset syscall address, with lifted code there for the image. */
    fresh(c, 22, 0xA70); c->gpr[0]._u32[0] = 0xA00;
    code = branch(c); report("ts-syscall", c, code);
    /* The resolver-only image (its entry handling runs under YDKJ_CRI_CHAIN). */
    fresh(c, 23, 0xA00);
    code = branch(c); report("resolver-only", c, code);
    /* A task resume with no task resident at depth > 0 restarts at depth 0;
     * once the task is resident the same branch is a plain hit. */
    put32(c, 0x2794, 0x00100000u);
    fresh(c, 3, 0x3200); c->host_depth = 1;
    code = branch(c); report("task-resume", c, code);
    fresh(c, 3, 0x3200); c->host_depth = 1; c->resident_task = 41;
    code = branch(c); report("task-resident", c, code);
    fresh(c, 3, 0x3200); c->host_depth = 0;
    code = branch(c); report("task-depth0", c, code);
    put32(c, 0x2794, 0);
    /* A miss on a stop word: the full resolver synthesises the stop. */
    fresh(c, 3, 0x5000); put32(c, 0x5000, 0x00000123u);
    code = branch(c); report("miss-stop", c, code);
    put32(c, 0x5000, 0);
    /* The job-return address halts outside policy mode, even when lifted. */
    fresh(c, 3, SPU_JOB_RETURN_LS);
    code = branch(c); report("job-return", c, code);
    fresh(c, 3, SPU_JOB_RETURN_LS); c->policy_mode = 1;
    code = branch(c); report("job-ret-pm", c, code);
    /* A job module resident (the job diagnostics' case), with a completion
     * word for the job-done diagnostic. */
    fresh(c, 3, 0x4A40); c->resident_ovl = 200; put32(c, 0xC00, 0x470A0010u);
    code = branch(c); report("job-entry", c, code);
    printf("job-done word=%u\n", vm_base[0x470A0013u]);
    put32(c, 0xC00, 0);
    /* The poll diagnostic reports every two million resolver calls. */
    if (getenv("SPU_POLLTRACE"))
        for (unsigned i = 0; i < 2000000; i++) { fresh(c, 3, 0xA00); branch(c); }
    landed = 0;
#ifndef _WIN32
    printf("ibhist total=%llu\n", (unsigned long long)s_ibhist_total);
#endif

    /* The gate line's copies read the same as their sources, before and
     * after the first read, and the stack-reset count follows registration. */
    for (int i = 0; i < 2; ++i)
        printf("gates dcache=%d scope=%d\n", spu_dcache_mode(), spu_region_scope_on());
    fresh(c, 3, 0xA00); c->host_depth = 1;
    printf("stack reset needed before=%d", spu_stack_reset_needed(c));
    spu_register_stack_reset_entry(0x7000, 3);
    printf(" after=%d\n", spu_stack_reset_needed(c));

    /* Overlay residency counters: one dispatch through a verified slot. */
    c->resident_code[0].verify_state = SPU_OVERLAY_VERIFIED;
    c->resident_code[0].source_index = 0;
    for (int i = 0; i < 5; ++i) spu_overlay_resident_verified(c, 0);
    c->resident_code[0].verify_state = SPU_OVERLAY_PENDING;
    spu_overlay_resident_verified(c, 0);
    fprintf(stderr, "residency counters: checks=%llu dispatches=%llu refused=%llu\n",
            (unsigned long long)s_ovl_res_checks, (unsigned long long)s_ovl_res_dispatches,
            (unsigned long long)s_ovl_res_refused);
    printf("residency checks=%llu dispatches=%llu\n",
           (unsigned long long)s_ovl_res_checks, (unsigned long long)s_ovl_res_dispatches);
    free(c);
    return 0;
}
