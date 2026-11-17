/* Synthetic checks for the dispatch gate line (spu_context.h) and the overlay
 * residency counters, run by tools/test_spu_dispatch_gates.py: the counters
 * count only while their statistics or shadow check is on, the statistics
 * still report them, the gate copies read the same as their sources and the
 * stack-reset count follows registration. */
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

int main(void)
{
    vm_base = calloc(1, 1 << 20); assert(vm_base);
    spu_context* c = calloc(1, sizeof(*c)); assert(c);
    spu_context_init(c, 0);
    assert(((uintptr_t)&g_spu_dispatch_gates & 63) == 0);
    assert(sizeof g_spu_dispatch_gates == 64);

    for (int i = 0; i < 2; ++i)
        printf("gates dcache=%d scope=%d\n", spu_dcache_mode(), spu_region_scope_on());
    printf("sources dcache=%s scope=%d\n", getenv("SPU_DISPATCH_CACHE") ? getenv("SPU_DISPATCH_CACHE") : "-",
           __atomic_load_n(&g_spu_region_scope_mode, __ATOMIC_RELAXED));
    c->image_id = 3; c->host_depth = 1; c->policy_mode = 0;
    printf("stack reset needed before=%d", spu_stack_reset_needed(c));
    spu_register_stack_reset_entry(0x7000, 3);
    printf(" after=%d count=%u\n", spu_stack_reset_needed(c), g_spu_stack_reset_count);

    /* Five dispatches through a verified slot, then one first check. */
    c->resident_code[0].verify_state = SPU_OVERLAY_VERIFIED;
    c->resident_code[0].source_index = 0;
    for (int i = 0; i < 5; ++i) assert(spu_overlay_resident_verified(c, 0));
    c->resident_code[0].verify_state = SPU_OVERLAY_PENDING;
    assert(spu_overlay_resident_verified(c, 0));
    printf("residency checks=%llu dispatches=%llu refused=%llu\n",
           (unsigned long long)s_ovl_res_checks, (unsigned long long)s_ovl_res_dispatches,
           (unsigned long long)s_ovl_res_refused);
    free(c);
    return 0;
}
