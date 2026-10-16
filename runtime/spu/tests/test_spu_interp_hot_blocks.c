/* Diagnostic-only counters must leave the real interpreter result unchanged. */
#define main hot_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
extern uint64_t g_spu_interp_steps;
int main(void) {
    spu_context* c=calloc(1,sizeof(*c));CHECK(c!=0);spu_context_init(c,0);
    c->image_id=-1;c->gpr[8]._u32[0]=600000;
    const uint32_t words[]={0x1cffc408,0x217fff88,0x00002000};
    for(unsigned i=0;i<3;++i)for(unsigned b=0;b<4;++b)
        c->ls[0x10000+4*i+b]=(uint8_t)(words[i]>>(24-8*b));
    uint32_t rc=spu_interp_run_until(c,0x10000,0);
    CHECK(rc==0x2000 && c->pc==0x1000c && c->gpr[8]._u32[0]==0);
    CHECK(g_spu_interp_steps==1200001 && c->status==SPU_STATUS_STOPPED_BY_STOP);
    printf("HOT diagnostic architecture checks passed=%d failed=%d\n",g_pass,g_fail);
    spu_mfc_release(c);spu_coh_unregister(c);free(c);return g_fail?1:0;
}
