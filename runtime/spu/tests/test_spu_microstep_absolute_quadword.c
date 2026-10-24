/* Real dispatcher/microstep and full-interpreter differential regression.
 * Hand-encoded instructions and synthetic registers only; no game assets. */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, void (*)(spu_context*));
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
extern void spu_halt(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size;
static spu_context micro, interp;
static int failures, checks;
static void check(int ok, const char* label, unsigned imm) {
    checks++;
    if (!ok) { failures++; fprintf(stderr, "FAIL %s immediate=%04X\n", label, imm); }
}
static void word(spu_context* c, uint32_t pc, uint32_t w) {
    c->ls[pc]=w>>24; c->ls[pc+1]=w>>16; c->ls[pc+2]=w>>8; c->ls[pc+3]=w;
}
static u128 sentinel(unsigned r) {
    u128 v;
    for (unsigned j=0;j<4;j++) v._u32[j]=0xA1053C79u ^ (r*0x01020408u) ^ (j*0x12345679u);
    return v;
}
static void finish(spu_context* c) {
    c->status=SPU_STATUS_STOPPED_BY_STOP;
    spu_halt(c);
}
static void entry(spu_context* c) { g_spu_trampoline_fn=spu_indirect_branch; }
static void init(spu_context* c) {
    spu_context_init(c,0); c->image_id=77; c->policy_mode=1;
    memset(c->ls,0x5A,SPU_LS_SIZE);
    for(unsigned r=0;r<128;r++) c->gpr[r]=sentinel(r);
    c->gpr[79]=spu_link(0x400);
}
static void run(spu_context* c, unsigned pc, int adapter) {
    c->status=SPU_STATUS_RUNNING; c->pc=pc;
    if(adapter) { assert(spu_run_with_halt(entry,c)==1); }
    else { assert(spu_interp_run_until(c,pc,0)==0x400); }
    assert(c->pc==0x400);
}
static void single(unsigned imm, int store) {
    unsigned ea=(imm*4u)&0x3FFF0u, wrong=(imm*16u)&0x3FFF0u;
    init(&micro); init(&interp);
    u128 value=sentinel(19), canary=sentinel(29);
    for(unsigned n=0;n<2;n++) {
        spu_context* c=n?&interp:&micro;
        if(wrong!=ea) spu_ls_write128(c,wrong,canary);
        spu_ls_write128(c,ea,value);
        c->gpr[73]=store?sentinel(73):sentinel(33);
        word(c,0x30000,(store?0x20800000u:0x30800000u)|(imm<<7)|73);
        word(c,0x30004,0x35002780); /* bi r79 */
    }
    run(&interp,0x30000,0); run(&micro,0x30000,1);
    check(!memcmp(micro.gpr,interp.gpr,sizeof micro.gpr),store?"STQA registers":"LQA full vector",imm);
    check(!memcmp(micro.ls,interp.ls,SPU_LS_SIZE),store?"STQA memory/canaries":"LQA no stores",imm);
    u128 expected=store?sentinel(73):value;
    u128 got=store?spu_ls_read128(&interp,ea):interp.gpr[73];
    check(!memcmp(&expected,&got,16),"interpreter literal oracle",imm);
    if(wrong!=ea) {
        got=spu_ls_read128(&micro,wrong);
        check(!memcmp(&got,&canary,16),"old wrong address untouched",imm);
    }
}
static void stub(spu_context* c, int store) {
    for(unsigned r=0;r<76;r++) {
        unsigned ea=0x3F9C0+r*16;
        word(c,0x3FEC0+r*4,(store?0x20800000u:0x30800000u)|((ea/4)<<7)|r);
    }
    word(c,0x3FFF0,0x35002780); /* bi r79, synthetic continuation */
}
static void save_restore(void) {
    init(&micro); init(&interp);
    stub(&micro,1); stub(&interp,1);
    run(&interp,0x3FEC0,0); run(&micro,0x3FEC0,1);
    check(!memcmp(micro.ls,interp.ls,SPU_LS_SIZE),"76-register save area and stack canaries",0);
    for(unsigned r=0;r<76;r++) {
        u128 expected=sentinel(r), actual=spu_ls_read128(&micro,0x3F9C0+r*16);
        check(!memcmp(&actual,&expected,16),"saved full vector",r);
        micro.gpr[r]=interp.gpr[r]=sentinel(r+128);
    }
    stub(&micro,0); stub(&interp,0);
    run(&interp,0x3FEC0,0); run(&micro,0x3FEC0,1);
    for(unsigned r=0;r<76;r++) {
        u128 expected=sentinel(r);
        check(!memcmp(&micro.gpr[r],&expected,16),"restored full vector",r);
    }
    check(!memcmp(micro.gpr,interp.gpr,sizeof micro.gpr),"restore preserves unsaved registers",0);
    check(!memcmp(micro.ls,interp.ls,SPU_LS_SIZE),"restore leaves stack canaries intact",0);
}
int main(void) {
    spu_begin_image(77); spu_register_function(0x400,finish);
    const unsigned immediates[]={0,1,2,3,4,0x7FFF,0x8000,0xBFFF,0xFE50,0xFF94,0xFFFF};
    /* imm FF94 with rt73 is exact LQA 30FFCA49 / STQA 20FFCA49.
     * Correct EA 3FE50, old wrong EA 3F940. */
    for(unsigned i=0;i<sizeof immediates/sizeof *immediates;i++)
        for(int store=0;store<2;store++) single(immediates[i],store);
    save_restore();
    printf("absolute quadword: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
/* Link-only boundaries outside this CPU-only fixture. Fail if any is reached. */
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
void ppu_resv_break_store(uint64_t ea) { (void)ea; abort(); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { abort(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) {
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
