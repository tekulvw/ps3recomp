/* Neutral generated legacy/region differential over the real dispatcher. */
#define main region_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
#include "region_program.h"
extern void legacy_test_spu_recomp_register(void);
extern void region_test_spu_recomp_register(void);
int main(int argc, char** argv)
{
    int regions = argc > 1 && atoi(argv[1]);
    int enabled = argc > 2 && atoi(argv[2]);
    uint32_t entry = argc > 3 ? (uint32_t)strtoul(argv[3],NULL,0) : 0x10000;
    unsigned depth = argc > 4 ? (unsigned)atoi(argv[4]) : 1;
    spu_context* ctx = calloc(1,sizeof(*ctx)); CHECK(ctx != NULL);
    spu_context_init(ctx,0); ctx->policy_mode=1;ctx->image_id=501;ctx->host_depth=depth;
    ctx->pc=entry;ctx->gpr[0]._u32[0]=0x20000;ctx->int_enable=enabled;
    if (entry!=0x10000) {
        ctx->gpr[7]=spu_splat_u32(41);
        ctx->gpr[8]=spu_splat_u32(entry<0x10010?1:0);
    }
    memcpy(ctx->ls+0x10000,region_program,sizeof(region_program));
    spu_verified_code_segment segment={0x10000,sizeof(region_program),region_program};
    CHECK(spu_verified_overlay_register(501,701,&segment,1));
    spu_begin_image(701);
    if(regions)region_test_spu_recomp_register();else legacy_test_spu_recomp_register();
    spu_begin_image(0);
    spu_lifted_fn fn=spu_lifted_lookup(ctx,entry);
    if(fn)fn(ctx);
    else {
        /* Legacy lacks the mid-body entry. The original interpreter is the
         * architecture oracle for that exact PC and starting register file. */
        CHECK(!regions && (entry==0x10004 || entry==0x1000C));
        spu_interp_run_until(ctx,entry,0x20000);
        if (ctx->pc != 0x20000 && spu_lifted_lookup(ctx,ctx->pc))
            g_spu_trampoline_fn=spu_indirect_branch;
    }
    spu_drain_call(ctx,0x20000);
    CHECK(ctx->gpr[7]._u32[0] == (entry==0x10000?3u:entry==0x10004?44u:entry==0x10008?42u:41u));
    CHECK(ctx->gpr[8]._u32[0] == 0);
    CHECK(ctx->pc==0x20000 && ctx->host_depth==depth);
    CHECK(!memcmp(ctx->ls+0x10000,region_program,sizeof(region_program)));
    unsigned long long digest=1469598103934665603ull;
    const unsigned char* bytes=(const unsigned char*)ctx->gpr;
    for(size_t i=0;i<sizeof(ctx->gpr);++i) digest=(digest^bytes[i])*1099511628211ull;
    printf("RESULT pc=%x depth=%u irq=%u regs=%016llx steps=%llu failed=%d\n",ctx->pc,ctx->host_depth,ctx->int_enable,digest,(unsigned long long)ctx->steps,g_fail);
    spu_mfc_release(ctx);spu_coh_unregister(ctx);free(ctx);
    return g_fail?1:0;
}
