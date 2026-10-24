/* Observe real checkpoint, guard and interpreter code without replacing their
 * behavior. Only the checkpoint observer writes our neutral guest program. */
#define spu_interp_run_until mutation_observed_run_until
#define spu_verified_code_enter_span mutation_real_enter_span
#define main mutation_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
#undef spu_verified_code_enter_span
#undef spu_interp_run_until
#include "mutation_program.h"
extern uint32_t spu_interp_run_until(spu_context*,uint32_t,uint32_t);
extern void mutation_legacy_spu_recomp_register(void);
extern void mutation_region_spu_recomp_register(void);
static unsigned hook_calls, mutation_count, rejects, fallback_calls, finishes;
static int mutation_armed;
static uint32_t rejected_pc, rejected_entry, fallback_pc;

void mutation_observe_task_launch(spu_context* c,void* target)
{
    (void)target;
    ++hook_calls;
    if(mutation_armed && !mutation_count && c->pc==0x10008) {
        CHECK(c->host_depth==0 && c->int_enable==0 && c->drain_ret_pc==0);
        /* ori r21,r22,99 replaces ori r21,r22,7 after the real checkpoint. */
        spu_ls_write32(c,0x10008,0x04000000u|(99u<<14)|(22u<<7)|21u);
        ++mutation_count;
    }
}
int spu_verified_code_enter_span(const spu_context* c,int image,uint32_t start,uint32_t end)
{
    const int admitted=mutation_real_enter_span(c,image,start,end);
    if(!admitted) {++rejects;rejected_pc=c->pc;rejected_entry=start;}
    return admitted;
}
uint32_t mutation_observed_run_until(spu_context* c,uint32_t pc,uint32_t stop)
{
    ++fallback_calls;fallback_pc=pc;
    return spu_interp_run_until(c,pc,stop);
}
static void sentinel(spu_context* c) { (void)c;abort(); }
static void observed_drain_target(spu_context* c)
{
    c->pc=0x33004;g_spu_trampoline_fn=spu_indirect_branch;
}
static void check_drain_observer(void)
{
    spu_context* c=calloc(1,sizeof(*c));CHECK(c!=NULL);
    spu_context_init(c,0);c->policy_mode=1;c->pc=0x33000;
    const unsigned hooks=hook_calls;
    g_spu_trampoline_fn=observed_drain_target;
    spu_drain_call(c,0x33004);
    CHECK(hook_calls==hooks+1 && c->steps==1 && c->pc==0x33004);
    CHECK(!g_spu_trampoline_fn);free(c);
}
static void finish(spu_context* c)
{
    ++finishes;c->status=SPU_STATUS_STOPPED_BY_STOP;spu_halt(c);
}
static void check_deopts(void)
{
    spu_context* c=calloc(1,sizeof(*c));
    spu_context* snapshot=malloc(sizeof(*snapshot));
    CHECK(c && snapshot);
    unsigned char frame[32],frame_copy[32];
    memset(frame,0xa5,sizeof(frame));memcpy(frame_copy,frame,sizeof(frame));
    for(unsigned which=0;which<3;++which) {
        spu_context_init(c,0);c->policy_mode=1;c->pc=0x10008;
        c->irq_frame=frame;c->irq_saved=1;c->irq_resume_pc=0x12340;c->srr0=0x12340;
        c->event_status=1;c->event_mask=1;
        c->int_enable=which==0;c->host_depth=which==1;
        c->drain_ret_pc=which==2?0x12344:0;
        memcpy(snapshot,c,sizeof(*snapshot));
        const unsigned hooks=hook_calls,history=g_spu_pch_n;
        g_spu_trampoline_fn=sentinel;
        CHECK(spu_region_local_step(c,sentinel)==0);
        CHECK(hook_calls==hooks && g_spu_pch_n==history);
        CHECK(g_spu_trampoline_fn==sentinel);
        CHECK(!memcmp(snapshot,c,sizeof(*snapshot)));
        CHECK(!memcmp(frame,frame_copy,sizeof(frame)));
    }
    /* An admitted unframed, IRQ-disabled transfer leaves an existing saved
     * IRQ frame and pending event untouched; iret remains dispatcher-owned. */
    c->drain_ret_pc=0;c->int_enable=0;c->host_depth=0;
    const unsigned hooks=hook_calls;
    g_spu_trampoline_fn=NULL;
    CHECK(spu_region_local_step(c,sentinel)==1);
    CHECK(hook_calls==hooks+1 && c->irq_frame==frame && c->irq_saved==1);
    CHECK(c->irq_resume_pc==0x12340 && c->srr0==0x12340);
    CHECK(c->event_status==1 && c->event_mask==1 && c->int_enable==0);
    CHECK(!memcmp(frame,frame_copy,sizeof(frame)));
    free(snapshot);free(c);
}
static void driver(spu_context* c)
{
    spu_lifted_fn fn=spu_lifted_lookup(c,0x10000);CHECK(fn != NULL);
    if(fn)fn(c);
}
int main(int argc,char** argv)
{
    CHECK(argc==2);check_deopts();check_drain_observer();
    const int regions=atoi(argv[1]);
    spu_context* c=calloc(1,sizeof(*c));CHECK(c != NULL);
    spu_context_init(c,0);c->policy_mode=1;c->image_id=501;c->pc=0x10000;
    c->gpr[0]=spu_link(0x20000);
    memcpy(c->ls+0x10000,mutation_program,sizeof(mutation_program));
    spu_verified_code_segment segment={0x10000,sizeof(mutation_program),mutation_program};
    CHECK(spu_verified_overlay_register(501,701,&segment,1));
    spu_begin_image(701);
    if(regions)mutation_region_spu_recomp_register();else mutation_legacy_spu_recomp_register();
    spu_begin_image(501);spu_register_function(0x20000,finish);spu_begin_image(0);
    mutation_armed=1;
    const int halted=spu_run_with_halt(driver,c);
    CHECK(halted==1 && finishes==1 && c->pc==0x20000);
    CHECK(c->gpr[20]._u32[0]==1 && c->gpr[21]._u32[0]==99);
    CHECK(mutation_count==1 && rejects==1 && rejected_pc==0x10008 && rejected_entry==0x10008);
    CHECK(fallback_calls==1 && fallback_pc==0x10008);
    CHECK(!spu_lifted_lookup(c,0x10008));
    unsigned long long digest=1469598103934665603ull;
    const unsigned char* bytes=(const unsigned char*)c->gpr;
    for(size_t i=0;i<sizeof(c->gpr);++i)digest=(digest^bytes[i])*1099511628211ull;
    printf("RESULT mode=%d pc=%x counters=%u,%u regs=%016llx mutations=%u rejects=%u rejected_pc=%x fallback=%u fallback_pc=%x deopts=3 failed=%d\n",
        regions,c->pc,c->gpr[20]._u32[0],c->gpr[21]._u32[0],digest,
        mutation_count,rejects,rejected_pc,fallback_calls,fallback_pc,g_fail);
    fflush(stdout);
    spu_mfc_release(c);spu_coh_unregister(c);free(c);
    return g_fail?1:0;
}
