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
    printf("Sparse verified entry overrides: %d passed, %d failed\n",g_pass,g_fail);
    free(c);return g_fail?1:0;
}
