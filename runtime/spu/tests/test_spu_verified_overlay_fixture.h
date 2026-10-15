/* Neutral generated code; this fixture is enabled only by the dedicated runner. */
#include "spu_verified_overlay.h"
#include "../spu_dma.h"
extern unsigned test_verified_compares;
#include "spu_recomp.h"
#include "verified_program.h"
static void verified_stale(spu_context* c) { c->gpr[7]._u32[0] = 555; }
static void test_verified_overlay(void)
{
    spu_context* c = calloc(1, sizeof *c);
    check(c != NULL, "allocate verified overlay context");
    if (!c) return;
    spu_context_init(c, 0); c->image_id = 501; c->policy_mode = 1;
    unsigned char expected_caller[16], expected_callee[8];
    memcpy(expected_caller, verified_program, 16);
    memcpy(expected_callee, verified_program+32, 8);
    spu_verified_code_segment segs[] = {{0x10000,16,expected_caller},{0x10020,8,expected_callee}};
    check(!spu_verified_overlay_register(0,701,segs,2), "reject missing policy identity");
    check(!spu_verified_overlay_register(501,0,segs,2), "reject wildcard compiled image");
    check(!spu_verified_overlay_register(501,501,segs,2), "reject compiled-primary identity alias");
    check(!spu_verified_overlay_register(501,701,segs,0), "reject no executable bytes");
    check(!spu_verified_overlay_register(501,701,segs,9), "reject excessive segment count before reads");
    spu_verified_code_segment null_bytes = {0x10000,4,NULL};
    check(!spu_verified_overlay_register(501,701,&null_bytes,1), "reject null executable bytes");
    spu_verified_code_segment bad = {0x3fffc,8,expected_callee};
    check(!spu_verified_overlay_register(501,701,&bad,1), "reject executable LS overflow");
    bad = (spu_verified_code_segment){0x10000,0,expected_caller};
    check(!spu_verified_overlay_register(501,701,&bad,1), "reject zero segment");
    bad = (spu_verified_code_segment){0x10001,8,expected_callee};
    check(!spu_verified_overlay_register(501,701,&bad,1), "reject unaligned segment");
    spu_verified_code_segment overlap[] = {segs[0],{0x10004,8,expected_callee}};
    check(!spu_verified_overlay_register(501,701,overlap,2), "reject overlapping segments");
    spu_begin_image(701); verified_test_spu_recomp_register();
    spu_begin_image(501); spu_register_function(0x10000,verified_stale);
    spu_register_function(0x10010,verified_stale); /* hole within authoritative region */
    spu_begin_image(0);
    check(spu_verified_overlay_register(501,701,segs,2), "register exact executable segments");
    check(!spu_verified_overlay_register(501,701,segs,2), "duplicate registration fails atomically");
    check(!spu_lifted_lookup(c,0x10000), "unloaded image cannot execute compiled primary collision");
    unsigned char* saved_vm = vm_base;
    vm_base = calloc(1,GUEST_SIZE);
    check(vm_base != NULL, "allocate neutral DMA carrier");
    if (!vm_base) { free(c); return; }
    memcpy(vm_base+0x30000,verified_program,16);
    memcpy(vm_base+0x30020,verified_program+32,8);
    check(mfc_do_transfer(c,0x10000,0x30000,16,0x40)==0, "actual first GET copies partial code");
    check(!spu_lifted_lookup(c,0x10000), "first partial load does not admit whole image");
    /* One-element LIST completion supplies the remaining executable range. */
    spu_ls_write32(c,0x200,8);
    spu_ls_write32(c,0x204,0x30020);
    c->mfc_lsa=0x10020; c->mfc_tag=2;
    check(mfc_do_list_transfer(c,0x200,0,8,0x44)==0, "actual GET LIST finishes executable bytes");
    check(spu_lifted_lookup(c,0x10000)==verified_test_spu_func_00010000, "complete exact image admits correct function");
    memset(expected_caller,0,sizeof expected_caller); memset(expected_callee,0,sizeof expected_callee);
    check(spu_verified_code_enter(c,701,0x10000), "expected bytes are immutable registry copies");
    unsigned compares = test_verified_compares;
    for (unsigned n=0;n<100;++n) (void)spu_lifted_lookup(c,0x10010);
    check(test_verified_compares==compares, "non-entry interpreter PCs do not compare entire image");
    check(!spu_lifted_lookup(c,0x10010), "registered region holes reject stale primary");
    check(!spu_verified_code_enter(c,701,0x10010), "function guard rejects non-entry");
    check(!spu_verified_code_enter(NULL,701,0x10000), "null context fails closed");
    check(!spu_verified_code_enter(c,701,0x10001), "unaligned transfer fails closed");
    spu_context* unchanged = malloc(sizeof *unchanged);
    check(unchanged != NULL, "allocate state preservation snapshot");
    if (unchanged) {
        memcpy(unchanged,c,sizeof *c);
        (void)spu_lifted_lookup(c,0x10000);(void)spu_verified_code_enter(c,701,0x10020);
        check(!memcmp(unchanged,c,sizeof *c), "lookup and function gate do not mutate context or LS");
        free(unchanged);
    }
    c->image_id=502;
    check(!spu_verified_code_enter(c,701,0x10000), "function guard requires exact policy");
    c->image_id=701;
    check(!spu_lifted_lookup(c,0x10000), "wrong primary binding cannot recursively serve guarded function");
    c->image_id=0;
    check(!spu_lifted_lookup(c,0x10020), "wildcard primary cannot repeatedly select a rejected guarded body");
    c->image_id=501;
    c->policy_mode=0;
    check(!spu_lifted_lookup(c,0x10000), "opted non-policy context cannot bypass guarded entry");
    check(!spu_verified_code_enter(c,701,0x10000), "non-policy function gate rejects activation");
    c->policy_mode=1;
    /* An ordinary data write leaves the exact module valid. */
    c->gpr[11]._u32[0]=0x20000; c->gpr[0]=spu_link(0x900); c->host_depth=1;
    c->status=SPU_STATUS_RUNNING;
    verified_test_spu_func_00010000(c); spu_drain_call(c,0x900);
    check(c->gpr[7]._u32[0]==23 && c->pc==0x900 && c->host_depth==1,
          "exact generated direct caller and callee preserve return state");
    /* The running caller writes different callee bytes before its direct C
     * call. The callee entry guard must defer real-LS interpretation through
     * the caller drain, then resume the caller at its explicit return PC. */
    c->gpr[10]._u32[0]=0x40800000u|(99u<<7)|7;
    c->gpr[10]._u32[1]=0x35000000u|(6u<<7);
    c->gpr[10]._u32[2]=c->gpr[10]._u32[3]=0;
    c->gpr[11]._u32[0]=0x10020; c->gpr[0]=spu_link(0x900);
    c->pc=0x10000; c->status=SPU_STATUS_RUNNING;
    verified_test_spu_func_00010000(c); spu_drain_call(c,0x900);
    check(c->gpr[7]._u32[0]==100 && c->pc==0x900 && c->host_depth==1,
          "guarded generated direct callee interprets modified LS and preserves caller return");
    check(!spu_lifted_lookup(c,0x10000), "foreign SPU store invalidates entry without cached trust");
    memcpy(c->ls+0x10020,verified_program+32,8);
    check(spu_lifted_lookup(c,0x10000)==verified_test_spu_func_00010000, "restored bytes permit fresh validation");
    c->ls[0x10024]^=1;
    check(!spu_lifted_lookup(c,0x10000), "changed non-entry executable byte rejects entire image");
    memcpy(c->ls+0x10020,verified_program+32,8);
    vm_base[0x30020]^=1;
    check(mfc_do_transfer(c,0x10020,0x30020,8,0x40)==0, "foreign GET replaces already admitted executable bytes");
    check(!spu_lifted_lookup(c,0x10000), "foreign GET cannot retain previous entry admission");
    vm_base[0x30020]^=1;
    check(mfc_do_transfer(c,0x10020,0x30020,8,0x40)==0 &&
          spu_lifted_lookup(c,0x10000)==verified_test_spu_func_00010000,
          "restored GET is freshly validated");
    /* Exact bytes with only compatibility wildcard registration must fail. */
    unsigned char wildcard_bytes[] = {0,0,0,0};
    spu_verified_code_segment ws={0x18000,4,wildcard_bytes};
    spu_begin_image(0);spu_register_function(0x18000,verified_stale);
    check(spu_verified_overlay_register(501,703,&ws,1), "register candidate before exact table exists");
    check(!spu_lifted_lookup(c,0x18000), "wildcard-only function never qualifies verified code");
    /* Two indistinguishable opt-in images cannot select by registration order. */
    spu_begin_image(702);spu_register_function(0x10000,verified_stale);spu_begin_image(0);
    spu_verified_code_segment same[]={{0x10000,16,verified_program},{0x10020,8,verified_program+32}};
    check(spu_verified_overlay_register(501,702,same,2), "register second candidate");
    check(!spu_lifted_lookup(c,0x10000) && !spu_verified_code_enter(c,701,0x10000), "ambiguous exact matches never execute compiled code");
    /* Invalid descriptors did not consume slots; fill the remaining bounded
     * startup registry and reject the first excess entry. */
    unsigned slots=0;
    while (spu_verified_overlay_register(601,800+(int)slots,&ws,1)) ++slots;
    check(slots==13, "registry has exact fixed image capacity without partial invalid registrations");
    check(!spu_verified_overlay_register(601,999,&ws,1), "registry overflow fails closed");
    free(vm_base);vm_base=saved_vm;
    g_spu_trampoline_fn=0;free(c);
}
