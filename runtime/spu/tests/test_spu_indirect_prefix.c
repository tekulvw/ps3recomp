/* Synthetic full-resolver entry: IRQ fast rejection and diagnostic gates. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
#define spu_irq_regs_maybe_restore counted_irq_restore
#include "../spu_channels.c"
#undef spu_irq_regs_maybe_restore
extern int spu_irq_regs_maybe_restore(spu_context*);
static unsigned restore_calls, landings;
int counted_irq_restore(spu_context* c) {
    ++restore_calls;
    return spu_irq_regs_maybe_restore(c);
}
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
unsigned long long ps3_ms_now(void) { return 1000; }
void spurs_ef_set_from_spu(uint32_t a, uint16_t b) { (void)a; (void)b; }
uint64_t spurs_pm_build_context(uint8_t* a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; return 0;
}
void ydkj_wake_all_event_flags(void) {}
static void landed(spu_context* c) { (void)c; ++landings; }
int main(void) {
    spu_context* c=calloc(1,sizeof(*c)); assert(c);
    vm_base=calloc(1,1<<20); assert(vm_base);
    spu_context_init(c,0); c->image_id=3; c->status=SPU_STATUS_RUNNING;
    spu_begin_image(3); spu_register_function(0xA00,landed);
    c->pc=0x80000A00; spu_indirect_branch(c);
    assert(c->pc==0xA00 && landings==1);
    /* The ordinary path must reject before entering the out-of-line helper. */
    assert(restore_calls==0);
    c->irq_saved=1; c->irq_resume_pc=0xA00; c->int_enable=0;
    c->gpr[7]._u32[0]=11; c->irq_gpr[7]._u32[0]=42;
    spu_indirect_branch(c);
    assert(restore_calls==1 && c->irq_saved && c->gpr[7]._u32[0]==11);
    c->int_enable=1; c->irq_resume_pc=0xB00; spu_indirect_branch(c);
    assert(restore_calls==2 && c->irq_saved && c->gpr[7]._u32[0]==11);
    c->irq_resume_pc=0xA00; spu_indirect_branch(c);
    assert(restore_calls==3 && !c->irq_saved && c->gpr[7]._u32[0]==42);
    assert(landings==4);
    /* The poll diagnostic emits every two million resolver calls. */
    if(getenv("SPU_POLLTRACE"))
        for(unsigned i=4;i<2000000;i++) { c->pc=0xA00; spu_indirect_branch(c); }
    assert(restore_calls==3);
    free(c); free(vm_base);
    puts("PASS indirect entry: target mask, IRQ eligibility and register restore");
    return 0;
}
