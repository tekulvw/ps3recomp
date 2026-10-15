/* Test the real registry/interpreter and count calls to the unchanged resolver.
 * All guest instructions are synthetic. Existing handoff controls/stubs are reused. */
#define main handoff_control_main
#include "test_spu_interpreter_handoff.c"
#undef main
extern int spu_lifted_entry_maybe(uint32_t);
extern spu_lifted_fn spu_lifted_lookup_real(const spu_context*, uint32_t);
static unsigned resolver_calls;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t pc) {
    ++resolver_calls;
    return spu_lifted_lookup_real(c, pc);
}
int main(int argc, char** argv) {
    if (argc != 2 || strcmp(argv[1], "prefilter")) return handoff_control_main(argc, argv);
    spu_context_init(&ctx, 0);ctx.image_id=77;ctx.policy_mode=1;ctx.status=SPU_STATUS_RUNNING;
    spu_begin_image(77);spu_register_function(0x200,helper);
    spu_register_function(0x204,wrong_continuation);spu_register_function(0x240,finish);
    spu_register_function(0x3fffc, finish);
    spu_register_function(0, finish);
    spu_begin_image(81);
    spu_register_function(0x10100, finish);
    unsigned checks=0;
    for (uint32_t pc=0;pc<SPU_LS_SIZE;pc+=4) {
        int expected=pc==0 || pc==0x200 || pc==0x204 || pc==0x240 || pc==0x10100 || pc==0x3fffc;
        assert(spu_lifted_entry_maybe(pc)==expected); ++checks;
    }
    assert(spu_lifted_lookup(&ctx,0x10100)==NULL);
    assert(spu_lifted_entry_maybe(0x10100));
    assert(!spu_lifted_entry_maybe(0x18000));
    spu_begin_image(77);spu_register_function(0x18000, helper);
    assert(spu_lifted_entry_maybe(0x18000));
    /* Noncanonical registrations retain the old resolver path. */
    assert(spu_lifted_entry_maybe(0x40000));
    assert(spu_lifted_entry_maybe(0xffffffff));
    assert(spu_lifted_entry_maybe(1));
    for(uint32_t pc=0x4000;pc<0x4180;pc+=4) word(pc,0x04000082);
    resolver_calls=0;
    uint32_t result=spu_interp_run_until(&ctx,0x4000,0x4180);
    assert(result==0x4180 && ctx.pc==0x4180 && ctx.status==SPU_STATUS_RUNNING);
    const char* enabled=getenv("SPU_INTERP_REJOIN_PREFILTER");
    assert(resolver_calls==((enabled && atoi(enabled)) ? 0u : 96u));
    printf("prefilter PASS checks=%u resolver_calls=%u pc=%X\n",checks,resolver_calls,result);
    return 0;
}
