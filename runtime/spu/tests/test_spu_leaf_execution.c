/* Generated pure ALU/LS/control blocks versus the actual interpreter. */
#define main leaf_exec_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
#include "leaf_program.h"
void legacy_test_spu_recomp_register(void);
int raw_test_spu_leaf_register(void);
static unsigned admissions;
int observed_leaf_enter(const spu_context* c,int image,uint32_t start,uint32_t end) {
    int ok=spu_verified_leaf_enter(c,image,start,end);admissions+=(unsigned)ok;return ok;
}
static uint32_t rng=1;
static uint32_t next(void) {return rng=rng*1664525u+1013904223u;}
int main(int argc,char** argv) {
    const int enabled=argc>1&&atoi(argv[1]);
    spu_verified_code_segment seg={0x10000,sizeof(leaf_program),leaf_program};
    CHECK(spu_verified_overlay_register(501,701,&seg,1));
    spu_begin_image(701);legacy_test_spu_recomp_register();spu_begin_image(0);
    CHECK(raw_test_spu_leaf_register());
    for(unsigned trial=1;trial<=16;++trial) {
        spu_context* oracle=calloc(1,sizeof(*oracle));spu_context* native=calloc(1,sizeof(*native));
        CHECK(oracle!=NULL&&native!=NULL);spu_context_init(oracle,0);spu_context_init(native,0);
        for(unsigned r=0;r<128;++r)for(unsigned lane=0;lane<4;++lane)
            oracle->gpr[r]._u32[lane]=next();
        oracle->gpr[6]=spu_link(0x20000);oracle->gpr[8]=spu_splat_u32(trial);
        oracle->gpr[10]=spu_splat_u32(0x11000);
        for(unsigned lane=0;lane<4;++lane) {
            oracle->gpr[30]._f32[lane]=(float)(trial+lane)*.25f;
            oracle->gpr[31]._f32[lane]=-1.5f;
            oracle->gpr[32]._f32[lane]=2.25f;
        }
        memcpy(native->gpr,oracle->gpr,sizeof(oracle->gpr));
        for(unsigned k=0;k<64;++k)oracle->ls[0x11000+k]=(uint8_t)next();
        memcpy(native->ls+0x11000,oracle->ls+0x11000,64);
        memcpy(oracle->ls+0x10000,leaf_program,sizeof(leaf_program));
        memcpy(native->ls+0x10000,leaf_program,sizeof(leaf_program));
        oracle->image_id=native->image_id=501;
        /* This disables compiled rejoin only, not any instruction semantics. */
        oracle->policy_mode=0;native->policy_mode=1;native->status=SPU_STATUS_RUNNING;
        if(enabled && (trial&1))native->ls[0x10020]^=1; /* outside both independent units */
        CHECK(spu_interp_run_until(oracle,0x10000,0x20000)==0x20000);
        ++native->host_depth;native->pc=0x10000;
        if(trial&1)spu_verified_leaf_indirect_call(native,0x20000);
        else g_spu_trampoline_fn=spu_indirect_branch;
        spu_drain_call(native,0x20000);--native->host_depth;
        CHECK(native->pc==oracle->pc && native->host_depth==0);
        CHECK(native->status==oracle->status && native->int_enable==oracle->int_enable);
        CHECK(!memcmp(native->gpr,oracle->gpr,sizeof(native->gpr)));
        CHECK(!memcmp(native->ls+0x11000,oracle->ls+0x11000,64));
        spu_mfc_release(oracle);spu_coh_unregister(oracle);free(oracle);
        spu_mfc_release(native);spu_coh_unregister(native);free(native);
    }
    CHECK(admissions==(enabled?32u:0u));
    printf("LEAF EXEC mode=%d trials=16 passed=%d failed=%d\n",enabled,g_pass,g_fail);
    return g_fail?1:0;
}
