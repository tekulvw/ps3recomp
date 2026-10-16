/* Neutral actual-drain scope: admission is safe only inside its real caller. */
#define main scope_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
static int expected;
static void caller_continuation(spu_context* ctx) { ++ctx->gpr[22]._u32[0]; }
static void inner(spu_context* ctx) {
    ctx->pc=0x30004;g_spu_trampoline_fn=spu_indirect_branch;
}
static void renewed(spu_context* ctx) {
    ctx->pc=0x10028;
    CHECK(spu_region_local_step(ctx,renewed)==expected);
    ++ctx->gpr[21]._u32[0];
    ctx->pc=0x20000;
    CHECK(!spu_region_local_step(ctx,caller_continuation));
    g_spu_trampoline_fn=spu_indirect_branch;
}
static void start(spu_context* ctx) {
    ctx->pc=0x10008;
    CHECK(spu_region_local_step(ctx,renewed)==expected);
    ++ctx->gpr[20]._u32[0];
    ++ctx->host_depth;CHECK(!spu_region_local_step(ctx,renewed));--ctx->host_depth;
    ctx->drain_ret_pc^=4;CHECK(!spu_region_local_step(ctx,renewed));ctx->drain_ret_pc^=4;
    g_spu_trampoline_fn=inner;CHECK(!spu_region_local_step(ctx,renewed));g_spu_trampoline_fn=0;
    ctx->int_enable=1;CHECK(!spu_region_local_step(ctx,renewed));ctx->int_enable=0;
    const uint32_t inactive[]={SPU_STATUS_STOPPED,SPU_STATUS_STOPPED_BY_STOP,
        SPU_STATUS_STOPPED_BY_HALT,SPU_STATUS_WAITING_CHANNEL,SPU_STATUS_SINGLE_STEP,
        SPU_STATUS_RUNNING|SPU_STATUS_STOPPED_BY_STOP};
    for(unsigned k=0;k<sizeof(inactive)/sizeof(inactive[0]);++k) {
        ctx->status=inactive[k];CHECK(!spu_region_local_step(ctx,renewed));
    }
    ctx->status=SPU_STATUS_RUNNING;
    ctx->pc=0x30000;g_spu_trampoline_fn=inner;
    spu_drain_call(ctx,0x30004);
    ctx->pc=0x10010;
    CHECK(!spu_region_local_step(ctx,renewed)); /* nested drain invalidated scope */
    ctx->pc=0x10020;g_spu_trampoline_fn=renewed;
}
static void return_zero(spu_context* ctx) {
    ctx->pc=0;CHECK(!spu_region_local_step(ctx,caller_continuation));
    g_spu_trampoline_fn=spu_indirect_branch;
}
int main(int argc,char** argv) {
    expected=argc>1&&atoi(argv[1]);
    spu_context* ctx=calloc(1,sizeof(*ctx));CHECK(ctx!=NULL);spu_context_init(ctx,0);
    ctx->status=SPU_STATUS_RUNNING;ctx->policy_mode=1;ctx->image_id=501;ctx->host_depth=1;ctx->pc=0x10000;
    CHECK(!spu_region_local_step(ctx,start)); /* a direct C callee has no witness */
    spu_begin_image(501);spu_register_function(0x20000,caller_continuation);spu_begin_image(0);
    g_spu_trampoline_fn=start;spu_drain_call(ctx,0x20000);
    CHECK(ctx->pc==0x20000 && ctx->host_depth==1);
    CHECK(ctx->gpr[20]._u32[0]==1 && ctx->gpr[21]._u32[0]==1);
    CHECK(ctx->gpr[22]._u32[0]==0); /* owning caller executes continuation itself */
    CHECK(!spu_region_local_step(ctx,start)); /* scope ended */
    printf("SCOPE enabled=%d counters=%u,%u,%u passed=%d failed=%d\n",expected,
        ctx->gpr[20]._u32[0],ctx->gpr[21]._u32[0],ctx->gpr[22]._u32[0],g_pass,g_fail);
    if(expected) {
        ctx->host_depth=0;ctx->pc=0x10000;g_spu_trampoline_fn=return_zero;
        spu_drain_call(ctx,0);CHECK(ctx->pc==0 && !ctx->gpr[22]._u32[0]);
    }
    spu_mfc_release(ctx);spu_coh_unregister(ctx);free(ctx);return g_fail?1:0;
}
