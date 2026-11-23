/* The short dispatch entry (PS3RECOMP_SPU_IB_FAST=1, spu_channels.c), run by
 * tools/test_spu_ib_fast.py. Synthetic images and contexts only.
 *
 * Every dispatch below goes through spu_indirect_branch. With the switch on,
 * a repeated target under an unchanged key must be answered by the short
 * entry (the runner counts the full entry's runs through a patched
 * counter), and every change of an input the full entry reads -- image,
 * overlay, task, policy mode, local-store pointer, registry, verified-image
 * bytes epoch, resident-slot residency, context lifetime, a saved interrupt
 * register file, a taskset registration -- must send the next dispatch back
 * through the full entry. The function reached must always be the one the
 * full resolver names. Reserved service addresses are never recorded. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

unsigned ibf_slow;   /* the runner's patch increments this in the full entry */
static int g_pass, g_fail;
#define CHECK(x) do { if (x) g_pass++; else { g_fail++; printf("FAIL: line %d: %s\n", __LINE__, #x); } } while (0)

static int g_reached;
static void primary(spu_context* c) { (void)c; g_reached = 1; }
static void overlay(spu_context* c) { (void)c; g_reached = 2; }
static void task(spu_context* c) { (void)c; g_reached = 3; }
static void verified_a(spu_context* c) { (void)c; g_reached = 4; }
static void verified_b(spu_context* c) { (void)c; g_reached = 5; }
static void reserved(spu_context* c) { (void)c; g_reached = 6; }
static void late(spu_context* c) { (void)c; g_reached = 7; }
static const uint8_t bytes_a[8] = {0x11, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t bytes_b[8] = {0x22, 3, 4, 5, 6, 7, 8, 9};

/* One dispatch of pc on c. Returns which function ran; *slow says whether
 * the full entry ran for it. */
static int dispatch(spu_context* c, uint32_t pc, int* slow)
{
    const unsigned before = ibf_slow;
    g_reached = 0;
    c->pc = pc;
    spu_indirect_branch(c);
    *slow = ibf_slow != before;
    return g_reached;
}
/* With the switch on, the first dispatch of a new key runs the full entry
 * and the second does not; with it off, both do. The function reached must
 * be the resolver's answer both times. */
static void expect(spu_context* c, uint32_t pc, int fn_id, int first_slow)
{
    const int on = spu_ib_fast_on();
    int slow;
    CHECK(dispatch(c, pc, &slow) == fn_id);
    CHECK(slow == (on ? first_slow : 1));
    CHECK(dispatch(c, pc, &slow) == fn_id);
    CHECK(slow == (on ? 0 : 1));
}
static spu_context* fresh(int image)
{
    spu_context* c = calloc(1, sizeof(*c)); assert(c);
    spu_context_init(c, 0);
    c->image_id = image; c->status = SPU_STATUS_RUNNING;
    return c;
}

int main(void)
{
    vm_base = calloc(1, 1 << 20); assert(vm_base);
    const uint32_t A = 0xB000, V = 0x10000;
    spu_begin_image(311); spu_register_function(A, primary); spu_register_function(A + 4, primary);
    spu_begin_image(312); spu_register_function(A, overlay);
    spu_begin_image(313); spu_register_function(A, task);
    spu_begin_image(321); spu_register_function(V, primary);
    spu_begin_image(331); spu_register_function(V, verified_a);
    spu_begin_image(332); spu_register_function(V, verified_b);
    spu_begin_image(341); spu_register_function(SPURS_PM_EXIT_TO_KERNEL_LS, reserved);
    spu_register_function(SPURS_PM_SELECT_WORKLOAD_LS, reserved);
    spu_register_function(SPURS_TASKSET_PM_SYSCALL_LS, reserved);
    spu_register_function(SPU_JOB_RETURN_LS - 0x100, reserved);
    spu_begin_image(0);
    spu_context* c = fresh(311);
    spu_fast_path_resolve();
    const int on = spu_ib_fast_on();
    printf("switch %s\n", on ? "on" : "off");
    int slow;

    /* Plain resolution, then the same key again. */
    expect(c, A, 1, 1);
    expect(c, A + 4, 1, 1);
    expect(c, A, 1, 0);   /* the entry at A survived the dispatch of A+4 */
    /* Ownership inputs: overlay, task, back again. */
    c->resident_ovl = 312; expect(c, A, 2, 1);
    c->resident_task = 313; expect(c, A, 3, 1);
    c->resident_task = 0; expect(c, A, 2, 1);
    c->resident_ovl = 0; expect(c, A, 1, 1);
    /* Policy mode and a repointed local store change the key. */
    c->policy_mode = 1; expect(c, A, 1, 1);
    c->policy_mode = 0; expect(c, A, 1, 1);
    uint8_t* raw = calloc(1, SPU_LS_SIZE); assert(raw);
    c->ls = raw; expect(c, A, 1, 1);
    c->ls = c->ls_store; expect(c, A, 1, 1);
    /* A registration after the entry was recorded. */
    spu_begin_image(351); spu_register_function(A + 0x100, late); spu_begin_image(0);
    expect(c, A, 1, 1);
    /* A saved interrupt register file keeps the full entry (it may restore). */
    c->irq_saved = 1; c->irq_resume_pc = A + 0x200;
    CHECK(dispatch(c, A, &slow) == 1); CHECK(slow == 1);
    CHECK(dispatch(c, A, &slow) == 1); CHECK(slow == 1);
    c->irq_saved = 0; expect(c, A, 1, 0);
    /* The image itself, with everything else equal. */
    c->image_id = 312; expect(c, A, 2, 1);
    c->image_id = 311; expect(c, A, 1, 1);
    /* A new context lifetime at the same address, with the same image, never
     * reads the old entry. */
    const uint32_t old_id = c->dcache_id;
    spu_context_init(c, 0); c->image_id = 311; c->status = SPU_STATUS_RUNNING;
    CHECK(c->dcache_id == 0);
    expect(c, A, 1, 1);
    if (spu_dcache_mode() == 1) CHECK(c->dcache_id && c->dcache_id != old_id);
    c->image_id = 312; expect(c, A, 2, 1);

    /* Verified images: bytes in local store decide; a GET into any verified
     * image's code drops the entry, and the resolver's answer is kept. */
    spu_context* p = fresh(321); p->policy_mode = 1;
    memcpy(p->ls + V, bytes_a, 8);
    expect(p, V, 1, 1);
    spu_verified_code_segment seg_a = {V, 8, bytes_a};
    CHECK(spu_verified_overlay_register(321, 331, &seg_a, 1));
    expect(p, V, 4, 1);
    spu_verified_code_segment seg_b = {V + 0x100, 8, bytes_b};
    CHECK(spu_verified_overlay_register(321, 332, &seg_b, 1));
    expect(p, V, 4, 1);   /* a registration: full entry once, then short */
    /* A GET overlapping image B's segment only: the dispatch cache keeps
     * A's entry by coverage, the short entry drops every entry; both must
     * answer verified_a through the full resolver's rules. */
    spu_overlay_note_get(p, 0x700000, p->ls + V + 0x100, 4);
    expect(p, V, 4, 1);
    /* A GET elsewhere (no verified code): nothing dropped. */
    spu_overlay_note_get(p, 0x710000, p->ls + 0x30000, 16);
    expect(p, V, 4, 0);
    /* A GET into A's own bytes: dropped, and the bytes decide again. */
    memcpy(p->ls + V, bytes_a, 8);
    spu_overlay_note_get(p, 0x720000, p->ls + V, 8);
    expect(p, V, 4, 1);
    /* Resident code slots: a residency change is a key change. */
    spu_overlay_register_region(0x800000, 0x100, 312);
    spu_overlay_note_get(c, 0x800000, c->ls + A, 16);
    expect(c, A, 2, 1);
    spu_overlay_note_get(c, 0x810000, c->ls + A, 16);   /* another source: slot cleared */
    expect(c, A, 2, 1);

    /* Reserved service addresses are never recorded, even where a function
     * is registered there for a non-policy context. */
    spu_context* r = fresh(341);
    const uint32_t reserved_pcs[] = {SPURS_PM_EXIT_TO_KERNEL_LS, SPURS_PM_SELECT_WORKLOAD_LS,
                                     SPURS_TASKSET_PM_SYSCALL_LS};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(dispatch(r, reserved_pcs[i], &slow) == 6); CHECK(slow == 1);
        CHECK(dispatch(r, reserved_pcs[i], &slow) == 6); CHECK(slow == 1);
    }
    expect(r, SPU_JOB_RETURN_LS - 0x100, 6, 1);   /* an ordinary neighbour is recorded */

    /* A taskset registration closes the short entry for every context. */
    spu_taskset_register_task_entry(0x3000, 313);
    CHECK(dispatch(c, A, &slow) == 2); CHECK(slow == 1);
    CHECK(dispatch(c, A, &slow) == 2); CHECK(slow == 1);

    free(raw); free(c); free(p); free(r);
    printf("IB_FAST switch=%d passed=%d failed=%d\n", on, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
