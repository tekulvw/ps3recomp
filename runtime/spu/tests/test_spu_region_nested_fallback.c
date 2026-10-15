/* Neutral generated-code differential. Observe the real fallback stop PC;
 * the wrapper delegates every operation to the real SPU interpreter. */
#define spu_interp_run_until nested_observed_run_until
#define main nested_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
#undef spu_interp_run_until
#include "nested_program.h"
extern uint32_t spu_interp_run_until(spu_context*, uint32_t, uint32_t);
extern void nested_legacy_spu_recomp_register(void);
extern void nested_region_spu_recomp_register(void);
static unsigned fallback_calls, finishes;
static uint32_t first_stop, last_stop;
uint32_t nested_observed_run_until(spu_context* c, uint32_t pc, uint32_t stop)
{
    if (!fallback_calls++) first_stop=stop;
    last_stop=stop;
    return spu_interp_run_until(c,pc,stop);
}
static void finish(spu_context* c)
{
    ++finishes;
    c->status=SPU_STATUS_STOPPED_BY_STOP;
    spu_halt(c);
}
static void driver(spu_context* c)
{
    c->host_depth=1;
    spu_lifted_fn fn=spu_lifted_lookup(c,0x10000);
    CHECK(fn != NULL);
    if(fn)fn(c);
    spu_drain_call(c,0x20000);
    finish(c);
}
int main(int argc, char** argv)
{
    CHECK(argc==3);
    const int regions=atoi(argv[1]), irq=atoi(argv[2]);
    spu_context* c=calloc(1,sizeof(*c));CHECK(c != NULL);
    spu_context_init(c,0);c->policy_mode=1;c->image_id=501;
    c->pc=0x10000;c->gpr[0]=spu_link(0x20000);
    c->gpr[11]=spu_link(0x30000);c->int_enable=irq;
    memcpy(c->ls+0x10000,nested_program,sizeof(nested_program));
    /* Deliberately unlifted code: ORI forces the full fallback interpreter,
     * then the indirect callee returns through the non-r0 link register r7. */
    const uint32_t external[]={0x04000000u|(23u<<7)|23u,
        0x1c000000u|(1u<<14)|(23u<<7)|23u,0x35000000u|(7u<<7)};
    for(unsigned i=0;i<3;++i)spu_ls_write32(c,0x30000+4*i,external[i]);
    spu_verified_code_segment segment={0x10000,sizeof(nested_program),nested_program};
    CHECK(spu_verified_overlay_register(501,701,&segment,1));
    spu_begin_image(701);
    if(regions)nested_region_spu_recomp_register();else nested_legacy_spu_recomp_register();
    spu_begin_image(501);spu_register_function(0x20000,finish);spu_begin_image(0);
    const int halted=spu_run_with_halt(driver,c);
    unsigned long long digest=1469598103934665603ull;
    const unsigned char* bytes=(const unsigned char*)c->gpr;
    for(size_t i=0;i<sizeof(c->gpr);++i)digest=(digest^bytes[i])*1099511628211ull;
    printf("RESULT mode=%d irq=%d halted=%d finish=%u pc=%x depth=%u counters=%u,%u,%u,%u regs=%016llx fallback=%u first_stop=%x last_stop=%x failed=%d\n",
        regions,irq,halted,finishes,c->pc,c->host_depth,c->gpr[20]._u32[0],
        c->gpr[21]._u32[0],c->gpr[22]._u32[0],c->gpr[23]._u32[0],digest,
        fallback_calls,first_stop,last_stop,g_fail);
    fflush(stdout);
    CHECK(halted==1 && finishes==1 && c->pc==0x20000);
    CHECK(c->gpr[20]._u32[0]==1 && c->gpr[21]._u32[0]==1);
    CHECK(c->gpr[22]._u32[0]==1 && c->gpr[23]._u32[0]==1);
    CHECK(fallback_calls>0);
    CHECK(!memcmp(c->ls+0x10000,nested_program,sizeof(nested_program)));
    spu_mfc_release(c);spu_coh_unregister(c);free(c);
    return g_fail?1:0;
}
