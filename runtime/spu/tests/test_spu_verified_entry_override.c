/* Sparse native entries must not claim unrelated code sharing their LS range. */
#define main override_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
#include "../spu_verified_overlay.h"
#ifdef SPU_OVERRIDE_RED
#define spu_verified_entry_override_register spu_verified_overlay_register
#endif
static void original(spu_context* c) {(void)c;}
static void replacement(spu_context* c) {(void)c;}
static void alternate(spu_context* c) {(void)c;}
static unsigned original_runs,native_runs;
static void direct_original(spu_context*c){
    if(!spu_verified_code_enter_span(c,805,0x5000,0x5040)){
        c->pc=0x5000;g_spu_trampoline_fn=spu_indirect_branch;return;
    }
    ++original_runs;c->pc=c->gpr[0]._u32[0];g_spu_trampoline_fn=spu_indirect_branch;
}
static void native_owner(spu_context*c){
    CHECK(spu_verified_code_enter(c,810,0x5000));++native_runs;
    c->pc=c->gpr[0]._u32[0];g_spu_trampoline_fn=spu_indirect_branch;
}
static void compiled_direct_override(spu_context*c){
    uint8_t code[64],constant[16];memset(code,0xa5,sizeof code);memset(constant,0x6b,sizeof constant);
    spu_verified_code_segment overlay={0x5000,sizeof code,code};
    spu_verified_code_segment native[]={{0x5000,sizeof code,code},{0x5100,sizeof constant,constant}};
    spu_begin_image(805);spu_register_function(0x5000,direct_original);spu_register_function(0x5020,alternate);spu_begin_image(810);spu_register_function(0x5000,native_owner);spu_begin_image(0);
    CHECK(spu_verified_overlay_register(801,805,&overlay,1));
    CHECK(spu_verified_entry_override_register(801,810,native,2));
    c->image_id=801;c->policy_mode=1;c->resident_code[0].image_id=0;
    memcpy(c->ls+0x5000,code,sizeof code);memcpy(c->ls+0x5100,constant,sizeof constant);
    CHECK(spu_lifted_lookup(c,0x5000)==native_owner);
    CHECK(spu_verified_code_enter(c,810,0x5000));
    CHECK(!spu_verified_code_enter(c,805,0x5000));
    CHECK(!spu_verified_code_enter_span(c,805,0x5000,0x5040));
    CHECK(spu_lifted_lookup(c,0x5020)==alternate);
    CHECK(spu_verified_code_enter_span(c,805,0x5020,0x5040));
    CHECK(!spu_lifted_lookup(c,0x5010)); /* authoritative hole stays closed */
    c->gpr[0]=spu_link(0x6000);c->gpr[1]=spu_splat_u32(0x3f000);c->status=SPU_STATUS_RUNNING;c->int_enable=0;c->host_depth=1;c->pc=0x5000;g_spu_trampoline_fn=0;
    direct_original(c);spu_drain_call(c,0x6000);
    CHECK(native_runs==1);CHECK(original_runs==0);CHECK(c->pc==0x6000);CHECK(c->host_depth==1);
    /* A remote constant mismatch disqualifies ONLY the sparse owner. The
     * original compiled body still owns its unchanged full logical span. */
    for(unsigned i=0;i<sizeof constant;++i){c->ls[0x5100+i]^=1;
      CHECK(spu_lifted_lookup(c,0x5000)==direct_original);
      CHECK(!spu_verified_code_enter(c,810,0x5000));
      CHECK(spu_verified_code_enter_span(c,805,0x5000,0x5040));
      c->ls[0x5100+i]^=1;
    }
    c->ls[0x5100]^=1;c->pc=0x5000;direct_original(c);spu_drain_call(c,0x6000);
    CHECK(original_runs==1);CHECK(native_runs==1);CHECK(c->pc==0x6000);c->ls[0x5100]^=1;
    c->policy_mode=0;CHECK(!spu_verified_code_enter(c,810,0x5000));CHECK(!spu_verified_code_enter_span(c,805,0x5000,0x5040));c->policy_mode=1;
    c->image_id=802;CHECK(!spu_verified_code_enter(c,810,0x5000));CHECK(!spu_verified_code_enter_span(c,805,0x5000,0x5040));c->image_id=801;
    uint8_t* saved=c->ls;c->ls=guest_memory;CHECK(!spu_verified_code_enter(c,810,0x5000));CHECK(!spu_verified_code_enter_span(c,805,0x5000,0x5040));c->ls=saved;
    CHECK(!spu_verified_code_enter_span(c,805,0x5000,0x5000));
    CHECK(!spu_verified_code_enter_span(c,805,0x5000,0x5044));
    /* Two exact sparse owners remain ambiguous, regardless of the
     * authoritative fallback; no legacy function is selected first. */
    spu_begin_image(811);spu_register_function(0x5000,replacement);spu_begin_image(0);
    CHECK(spu_verified_entry_override_register(801,811,native,2));
    CHECK(!spu_lifted_lookup(c,0x5000));CHECK(!spu_verified_code_enter(c,810,0x5000));
    CHECK(!spu_verified_code_enter_span(c,805,0x5000,0x5040));
    CHECK(spu_lifted_lookup(c,0x5020)==alternate);c->host_depth=0;
}
int main(void) {
    spu_context* c=calloc(1,sizeof *c);CHECK(c!=NULL);spu_context_init(c,0);
    c->policy_mode=1;c->image_id=501;
    uint8_t bytes[64];for(unsigned i=0;i<sizeof bytes;++i)bytes[i]=(uint8_t)(i*13+7);
    const spu_verified_code_segment segment={0x4000,sizeof bytes,bytes};
    spu_begin_image(501);spu_register_function(0x4010,original);spu_register_function(0x4020,original);
    spu_begin_image(702);spu_register_function(0x4010,alternate);spu_register_function(0x4020,alternate);
    spu_begin_image(701);spu_register_function(0x4010,replacement);spu_begin_image(0);
    CHECK(spu_lifted_lookup(c,0x4020)==original);
    CHECK(spu_verified_entry_override_register(501,701,&segment,1));
    /* Different streamed module: preserve both primary and bounded resident resolution. */
    CHECK(spu_lifted_lookup(c,0x4020)==original);
    CHECK(spu_lifted_lookup(c,0x4010)==original);
    c->resident_code[0].image_id=702;c->resident_code[0].lsa=0x4000;c->resident_code[0].size=64;
    CHECK(spu_lifted_lookup(c,0x4020)==alternate);
    CHECK(spu_lifted_lookup(c,0x4010)==alternate);
    memcpy(c->ls+0x4000,bytes,sizeof bytes);
    CHECK(spu_lifted_lookup(c,0x4010)==replacement);
    CHECK(spu_verified_code_enter(c,701,0x4010));
    CHECK(spu_lifted_lookup(c,0x4020)==alternate); /* exact image, non-entry still original */
    c->policy_mode=0;CHECK(spu_lifted_lookup(c,0x4010)==alternate);c->policy_mode=1;
    uint8_t* saved=c->ls;c->ls=guest_memory;
    CHECK(spu_lifted_lookup(c,0x4010)==alternate);c->ls=saved;
    /* Every immutable byte matters, including bytes far from the overridden entry. */
    for(unsigned i=0;i<sizeof bytes;++i) {
        c->ls[0x4000+i]^=1;
        CHECK(spu_lifted_lookup(c,0x4010)==alternate);
        CHECK(!spu_verified_code_enter(c,701,0x4010));
        c->ls[0x4000+i]^=1;
    }
    CHECK(spu_lifted_lookup(c,0x4010)==replacement);
    /* Expected bytes were copied; input mutation must not change the admission. */
    memset(bytes,0,sizeof bytes);CHECK(spu_verified_code_enter(c,701,0x4010));
    /* No legacy path may serve a guarded native function when its check rejects. */
    c->image_id=701;c->resident_code[0].image_id=0;
    CHECK(!spu_lifted_lookup(c,0x4010));c->image_id=501;
    /* Ambiguous exact entries fail closed while other PCs keep their resolver. */
    spu_verified_code_segment duplicate={0x4000,64,c->ls+0x4000};
    spu_begin_image(703);spu_register_function(0x4010,alternate);spu_begin_image(0);
    CHECK(spu_verified_entry_override_register(501,703,&duplicate,1));
    CHECK(!spu_lifted_lookup(c,0x4010));
    CHECK(spu_lifted_lookup(c,0x4020)==original);
    compiled_direct_override(c);
    printf("Sparse verified entry overrides: %d passed, %d failed\n",g_pass,g_fail);
    free(c);return g_fail?1:0;
}
