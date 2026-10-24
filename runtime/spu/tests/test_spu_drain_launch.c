/* Actual launch checkpoints: shared per-thread history, changing contexts,
 * dynamic IRQ delivery and iret unwinding, stack reset and LS0 completion.
 * A skipped checkpoint, cached context field, or stale trampoline must fail.
 * Neutral functions/ISA only; no guest files or substitute runtime hooks. */
#define main launch_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
extern unsigned spu_recent_pcs(uint32_t*, unsigned);
extern uint32_t g_pm_flow_buf[8192];
extern volatile unsigned g_pm_flow_n;
extern void* volatile g_pm_flow_ctx;
extern void spu_register_stack_reset_entry(uint32_t,int);
extern volatile int g_yz_lockstep_on;

static void history(const uint32_t* want,unsigned n)
{
    uint32_t got[32];
    CHECK(spu_recent_pcs(got,n)==n);
    CHECK(!memcmp(got,want,n*sizeof(*want)));
}
static spu_context* make_context(int image)
{
    spu_context* c=calloc(1,sizeof(*c));CHECK(c!=NULL);
    spu_context_init(c,0);c->image_id=image;c->status=SPU_STATUS_RUNNING;
    return c;
}
static void release_context(spu_context* c)
{
    spu_mfc_release(c);spu_coh_unregister(c);free(c);
}
static void to_return(spu_context* c)
{
    c->pc=c->gpr[6]._u32[0];g_spu_trampoline_fn=spu_indirect_branch;
}
static void nested_leaf(spu_context* c)
{
    CHECK(c->steps==3);c->pc=0x1010;g_spu_trampoline_fn=spu_indirect_branch;
}
static void nested_first(spu_context* c)
{
    CHECK(c->steps==2);c->pc=0x1008;g_spu_trampoline_fn=nested_leaf;
}
static void final_leaf(spu_context* c)
{
    CHECK(c->steps==4 && c->drain_ret_pc==0x101c);
    CHECK(c->lockstep_quantum_ctr==1);g_yz_lockstep_on=0;
    CHECK(g_pm_flow_n==1 && g_pm_flow_buf[0]==0x1014);
    g_pm_flow_ctx=0;c->pc=0x101c;g_spu_trampoline_fn=spu_indirect_branch;
}
static void async_entry(spu_context* c)
{
    c->pc=0x3000;c->gpr[6]=spu_link(0x3010);
    g_spu_trampoline_fn=to_return;spu_drain_call(c,0x3010);
    CHECK(c->steps==1);spu_halt(c);
}
static void outer(spu_context* c)
{
    CHECK(c->steps==1);
    spu_context* raw=make_context(902);
    uint8_t* ls=calloc(1,SPU_LS_SIZE);CHECK(ls!=NULL);raw->ls=ls;
    raw->pc=0x2000;spu_task_launch_check(raw,(void*)to_return);
    CHECK(raw->steps==1);
    c->pc=0x1004;g_spu_trampoline_fn=nested_first;spu_drain_call(c,0x1010);
    CHECK(!c->region_dispatch_valid && c->drain_ret_pc==0);
    raw->pc=0x2004;raw->gpr[6]=spu_link(0x2010);
    g_spu_trampoline_fn=to_return;spu_drain_call(raw,0x2010);CHECK(raw->steps==2);
    free(ls);raw->ls=raw->ls_store;release_context(raw);
    spu_context* async=make_context(903);
    CHECK(spu_run_with_halt(async_entry,async)==1);CHECK(async->steps==1);
    release_context(async);
    g_pm_flow_n=0;g_pm_flow_ctx=c;
    /* A disabled predicate must be read again after the guest enables it.
     * Stay below one quantum so no synthetic ring membership is needed. */
    g_yz_lockstep_on=1;
    c->pc=0x1014;g_spu_trampoline_fn=final_leaf;
}
static void context_history(void)
{
    g_yz_lockstep_on=0;
    spu_context* c=make_context(901);c->pc=0x1000;
    g_spu_trampoline_fn=outer;spu_drain_call(c,0x101c);
    const uint32_t want[]={0x1000,0x2000,0x1004,0x1008,0x2004,0x3000,0x1014};
    history(want,7);CHECK(c->steps==4 && !g_spu_trampoline_fn);
    release_context(c);
}
static void macro_target(spu_context* c)
{
    CHECK(c->steps==2 && c->lockstep_quantum_ctr==1);c->pc=0x9008;
}
static void checkpoint_paths(void)
{
    spu_context* c=make_context(909);c->policy_mode=1;c->pc=0x9000;
    g_yz_lockstep_on=1;
    spu_task_launch_check(c,(void*)macro_target);
    CHECK(c->steps==1 && c->lockstep_quantum_ctr==0); /* launch-only ABI */
    c->pc=0x9004;g_spu_trampoline_fn=macro_target;SPU_DRAIN(c);
    CHECK(spu_region_local_step(c,macro_target)==1);
    CHECK(c->steps==3 && c->lockstep_quantum_ctr==2);
    const uint32_t want[]={0x9000,0x9004,0x9008};history(want,3);
    g_yz_lockstep_on=0;release_context(c);
}
static unsigned irq_count,stale_handler,irq_finished;
static u128 irq_expected[128];
static void irq_resume(spu_context* c)
{
    ++irq_finished;CHECK(irq_count==1 && c->srr0==0x4104);
    CHECK(c->host_depth==2 && c->image_id==904);
    CHECK(c->gpr[13]._u32[0]==0x12345678 && c->gpr[15]._u32[0]==0xabcdef01);
    CHECK(!memcmp(c->gpr,irq_expected,sizeof irq_expected));
    CHECK(c->int_enable && !c->irq_saved && !c->irq_frame && !c->event_status);
    c->pc=0x4110;g_spu_trampoline_fn=spu_indirect_branch;
}
static void deep_iret(spu_context* c)
{
    CHECK(c->irq_frame && c->irq_saved && c->host_depth==4);
    memset(c->gpr,0xa5,sizeof c->gpr);
    c->event_status=0;c->int_enable=1;c->pc=c->srr0;
    g_spu_trampoline_fn=spu_indirect_branch;
}
static void irq_handler(spu_context* c)
{
    ++irq_count;CHECK(!c->int_enable && c->srr0==0x4104 && c->irq_frame);
    c->host_depth+=2;c->pc=0x8004;g_spu_trampoline_fn=deep_iret;
    spu_drain_call(c,0x8008);
    ++stale_handler; /* iret must longjmp to the original drain, not return. */
}
static void irq_start(spu_context* c)
{
    if(c->gpr[9]._u32[0])c->event_status=1;else c->int_enable=1;
    c->pc=0x4104;g_spu_trampoline_fn=spu_indirect_branch;
}
static void dynamic_irq(unsigned raises_event)
{
    spu_context* c=make_context(904);c->policy_mode=1;c->host_depth=2;
    c->pc=0x4100;c->event_mask=1;c->event_status=!raises_event;
    for(unsigned r=0;r<128;++r)for(unsigned lane=0;lane<4;++lane)
        c->gpr[r]._u32[lane]=0xabc00000u+r*4+lane;
    c->int_enable=raises_event;c->gpr[9]=spu_pref_u32(raises_event);
    c->gpr[13]=spu_pref_u32(0x12345678);c->gpr[15]=spu_pref_u32(0xabcdef01);
    memcpy(irq_expected,c->gpr,sizeof irq_expected);
    spu_ls_write32(c,0,0x30100000u); /* BRA 0x8000 */
    irq_count=stale_handler=irq_finished=0;
    g_spu_trampoline_fn=irq_start;spu_drain_call(c,0x4110);
    CHECK(irq_count==1 && !stale_handler && irq_finished==1 && c->steps==5);
    const uint32_t want[]={0x4100,0x4104,0x8004,0x4104,0x4104};history(want,5);
    CHECK(!c->region_dispatch_valid && !c->irq_frame && !c->drain_ret_pc);
    release_context(c);
}
static unsigned reset_count,stale_reset;
static void reset_target(spu_context* c)
{
    ++reset_count;CHECK(c->host_depth==0 && !c->region_dispatch_valid);spu_halt(c);
}
static void reset_bridge(spu_context* c)
{
    /* Registration occurs after the owning and nested drains have begun. */
    spu_register_stack_reset_entry(0x5004,905);
    c->pc=0x5004;g_spu_trampoline_fn=reset_target;
}
static void reset_inner(spu_context* c)
{
    c->host_depth=3;c->pc=0x5000;g_spu_trampoline_fn=reset_bridge;
    spu_drain_call(c,0x5010);++stale_reset;
}
static void reset_outer(spu_context* c)
{
    c->host_depth=1;c->pc=0x4ff0;g_spu_trampoline_fn=reset_inner;
    spu_drain_call(c,0x5014);++stale_reset;
}
static void stack_reset(void)
{
    spu_context* c=make_context(905);reset_count=stale_reset=0;
    CHECK(spu_run_with_halt(reset_outer,c)==1);
    CHECK(reset_count==1 && !stale_reset && c->steps==2 && c->pc==0x5004);
    const uint32_t want[]={0x4ff0,0x5000,0x5004};history(want,3);release_context(c);
}
static unsigned zero_entries,zero_later;
static void zero_again(spu_context* c) {++zero_later;spu_halt(c);}
static void zero_entry(spu_context* c)
{
    ++zero_entries;CHECK(c->steps==1);c->pc=0;g_spu_trampoline_fn=zero_again;
}
static void zero_outer(spu_context* c)
{
    c->pc=0;g_spu_trampoline_fn=zero_entry;spu_drain_call(c,0x6010);
}
static void ls0_completion(void)
{
    spu_context* c=make_context(906);CHECK(spu_run_with_halt(zero_outer,c)==1);
    CHECK(zero_entries==1 && zero_later==0 && c->steps==2);
    const uint32_t want[]={0,0};history(want,2);release_context(c);
}
static void* thread_history(void* arg)
{
    const uint32_t pc=(uint32_t)(uintptr_t)arg;
    uint32_t got[32];CHECK(spu_recent_pcs(got,32)==0);
    spu_context* c=make_context(907);c->pc=pc;c->gpr[6]=spu_link(pc+4);
    g_spu_trampoline_fn=to_return;spu_drain_call(c,pc+4);
    CHECK(spu_recent_pcs(got,32)==1 && got[0]==pc && c->steps==1);
    release_context(c);return NULL;
}
static void depth_probe(spu_context* c)
{
    const unsigned depths[]={3,4,5,7,8};
    for(unsigned i=0;i<5;++i){
        c->host_depth=depths[i];c->pc=0xa000+depths[i]*4;
        SPU_DRAIN(c);
    }
    CHECK(0); /* depth 8 must halt through the real owning driver */
}
int main(int argc,char** argv)
{
    if(argc==2 && !strcmp(argv[1],"depth")){
        spu_context* c=make_context(908);
        CHECK(spu_run_with_halt(depth_probe,c)==1);release_context(c);
        printf("DEPTH passed=%d failed=%d\n",g_pass,g_fail);return g_fail?1:0;
    }
    spu_begin_image(904);spu_register_function(0x8000,irq_handler);
    spu_register_function(0x4104,irq_resume);spu_begin_image(905);
    spu_register_function(0x5004,reset_target);spu_begin_image(0);
    context_history();checkpoint_paths();dynamic_irq(0);dynamic_irq(1);stack_reset();ls0_completion();
    uint32_t before[32],after[32];unsigned n=spu_recent_pcs(before,32);
    /* Separate fresh host threads must not inherit the caller's TLS history.
     * Join each before checking so fixture accounting itself has no data race. */
    for(unsigned i=0;i<2;++i){pthread_t t;CHECK(!pthread_create(&t,NULL,thread_history,(void*)(uintptr_t)(0x7000+4*i)));CHECK(!pthread_join(t,NULL));}
    CHECK(spu_recent_pcs(after,32)==n && !memcmp(before,after,n*sizeof(*before)));
    printf("DRAIN_LAUNCH passed=%d failed=%d\n",g_pass,g_fail);return g_fail?1:0;
}
