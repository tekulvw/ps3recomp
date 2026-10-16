/* Neutral raw-ISA leaf blocks: no whole-image classifier dependencies. */
#include <string.h>
#include <stdint.h>
static uint64_t compared_bytes, compare_calls;
static int counted_compare(const void* a,const void* b,size_t n) {
    compared_bytes+=n;++compare_calls;return memcmp(a,b,n);
}
#define memcmp counted_compare
#define main leaf_unused_mfc_main
#include "test_spu_mfc_slots.c"
#undef main
#undef memcmp
#include "../spu_verified_overlay.h"
static int enabled;
static unsigned raw_entries;
static void raw_leaf(spu_context* c) {
    if(!spu_verified_leaf_enter(c,701,0x10010,0x10018)) {
        c->pc=0x10010;g_spu_trampoline_fn=spu_indirect_branch;return;
    }
    ++raw_entries;c->gpr[20]=spu_ai(c->gpr[20],1);
    c->pc=c->gpr[6]._u32[0]&0x3fffc;
    g_spu_trampoline_fn=spu_indirect_branch;
}
static void legacy(spu_context* c) {
    c->gpr[20]=spu_ai(c->gpr[20],1);SPU_RET_REG(c,6);
}
static void other(spu_context* c) {(void)c;}
static void premature(spu_context* c) {++c->gpr[22]._u32[0];}
static unsigned irq_entries;
static void handler(spu_context* c) {
    ++irq_entries;spu_wrch(c,SPU_WrEventAck,spu_pref_u32(1));
    c->int_enable=1;c->pc=c->srr0;g_spu_trampoline_fn=spu_indirect_branch;
}
static void put(uint8_t* b,uint32_t w) {for(unsigned i=0;i<4;++i)b[i]=(uint8_t)(w>>(24-8*i));}
int main(int argc,char** argv) {
    enabled=argc>1&&atoi(argv[1]);
    uint8_t bytes[32]={0};put(bytes+16,0x1c000000u|(1u<<14)|(20u<<7)|20u);
    put(bytes+20,0x35000000u|(6u<<7));
    spu_context* c=calloc(1,sizeof(*c));CHECK(c!=NULL);spu_context_init(c,0);
    c->policy_mode=1;c->image_id=501;c->status=SPU_STATUS_RUNNING;
    /* Guard-only queries model an existing owner; execution tests use real dispatch. */
    c->region_dispatch_valid=1;c->region_dispatch_depth=0;c->region_has_return=1;
    c->region_return_pc=c->drain_ret_pc=0x20000;
    memcpy(c->ls+0x10000,bytes,sizeof(bytes));
    spu_verified_code_segment segment={0x10000,sizeof(bytes),bytes};
    CHECK(spu_verified_overlay_register(501,701,&segment,1));
    spu_begin_image(701);spu_register_function(0x10010,legacy);spu_begin_image(0);
    const spu_verified_leaf_unit unit={0x10010,0x10018,raw_leaf};
    CHECK(spu_verified_leaf_register(701,&unit,1));
    CHECK(!spu_verified_leaf_register(701,&unit,1));
    CHECK(!spu_verified_leaf_register(999,&unit,1));
    const spu_verified_leaf_unit invalid[]={
        {0x10010,0x10010,raw_leaf},{0x10010,0x10019,raw_leaf},
        {0x1000c,0x10014,raw_leaf},{0x10010,0x10024,raw_leaf},{0x10010,0x10018,0}};
    for(unsigned k=0;k<sizeof(invalid)/sizeof(invalid[0]);++k)
        CHECK(!spu_verified_leaf_register(701,&invalid[k],1));
    c->region_dispatch_valid=0;c->region_has_return=0;c->drain_ret_pc=0;
    CHECK(spu_lifted_lookup(c,0x10010)!=raw_leaf);
    CHECK(!spu_verified_leaf_enter(c,701,0x10010,0x10018));
    c->region_dispatch_valid=1;c->region_has_return=1;
    c->region_return_pc=c->drain_ret_pc=0x20000;
    compared_bytes=compare_calls=0;
    for(unsigned k=0;k<10000;++k)CHECK(spu_lifted_lookup(c,0x10010)==(enabled?raw_leaf:legacy));
    CHECK(compared_bytes==(enabled?80000:320000) && compare_calls==10000);
    printf("COMPARE mode=%d calls=%llu requested_bytes=%llu\n",enabled,
        (unsigned long long)compare_calls,(unsigned long long)compared_bytes);
    CHECK(spu_verified_leaf_enter(c,701,0x10010,0x10018)==enabled);
    CHECK(!spu_verified_leaf_enter(c,701,0x10010,0x10014));
    /* Outside code/classifier changes do not alter this raw-ISA translation. */
    c->ls[0x10000]^=1;
    CHECK(spu_lifted_lookup(c,0x10010)==(enabled?raw_leaf:NULL));
    c->ls[0x10017]^=1;CHECK(spu_lifted_lookup(c,0x10010)==NULL);
    CHECK(!spu_verified_leaf_enter(c,701,0x10010,0x10018));
    c->ls[0x10017]^=1;c->ls[0x10000]^=1;
    c->int_enable=1;CHECK(spu_lifted_lookup(c,0x10010)==(enabled?raw_leaf:legacy));
    c->event_status=c->event_mask=1;CHECK(spu_lifted_lookup(c,0x10010)!=raw_leaf);
    CHECK(!spu_verified_leaf_enter(c,701,0x10010,0x10018));
    c->event_status=c->event_mask=0;c->int_enable=0;
    c->irq_saved=1;CHECK(spu_lifted_lookup(c,0x10010)!=raw_leaf);c->irq_saved=0;
    c->irq_frame=c;CHECK(spu_lifted_lookup(c,0x10010)!=raw_leaf);c->irq_frame=0;
    c->region_dispatch_valid=0;c->status=0;CHECK(spu_lifted_lookup(c,0x10010)!=raw_leaf);c->status=SPU_STATUS_RUNNING;c->region_dispatch_valid=1;
    c->policy_mode=0;CHECK(spu_lifted_lookup(c,0x10010)!=raw_leaf);c->policy_mode=1;
    c->ls=bytes;CHECK(!spu_verified_leaf_enter(c,701,0x10010,0x10018));c->ls=c->ls_store;
    c->host_depth=1;
    CHECK(spu_lifted_lookup(c,0x10010)!=raw_leaf); /* no owning dispatch yet */
    c->region_dispatch_valid=1;c->region_dispatch_depth=1;c->region_has_return=1;
    c->region_return_pc=c->drain_ret_pc=0x10014;
    CHECK(spu_lifted_lookup(c,0x10010)!=raw_leaf); /* continuation inside block */
    c->region_return_pc=c->drain_ret_pc=0x20000;
    c->region_dispatch_depth=2;CHECK(spu_lifted_lookup(c,0x10010)!=raw_leaf);
    c->region_dispatch_depth=1;c->region_has_return=0;
    CHECK(spu_lifted_lookup(c,0x10010)!=raw_leaf);
    c->region_dispatch_valid=0;c->drain_ret_pc=0;
    /* Raw return through r6 must unwind the owning C call exactly once. */
    spu_begin_image(501);spu_register_function(0x20000,premature);spu_begin_image(0);
    c->host_depth=1;c->gpr[6]=spu_link(0x20000);c->gpr[0]=spu_link(0x30000);
    c->pc=0x10010;g_spu_trampoline_fn=spu_indirect_branch;spu_drain_call(c,0x20000);
    CHECK(raw_entries==(unsigned)enabled);
    CHECK(c->pc==0x20000 && c->gpr[20]._u32[0]==1 && c->gpr[22]._u32[0]==0);
    CHECK(!g_spu_trampoline_fn);++c->gpr[22]._u32[0];CHECK(c->gpr[22]._u32[0]==1);
    c->pc=0x10010;spu_verified_leaf_indirect_call(c,0x20000);
    CHECK(!c->region_dispatch_valid && c->drain_ret_pc==0);
    spu_drain_call(c,0x20000);
    CHECK(raw_entries==2u*(unsigned)enabled && c->gpr[20]._u32[0]==2);
    spu_begin_image(501);spu_register_function(0x8000,handler);spu_begin_image(0);
    spu_ls_write32(c,0,0x30100000u); /* neutral BRA vector */
    c->int_enable=1;c->event_status=c->event_mask=1;c->pc=0x10010;
    g_spu_trampoline_fn=spu_indirect_branch;spu_drain_call(c,0x20000);
    CHECK(irq_entries==1 && c->gpr[20]._u32[0]==3);
    CHECK(c->int_enable==1 && !c->irq_saved && !c->irq_frame && !c->event_status);
    const unsigned before_zero=raw_entries;
    c->status=0;c->pc=0x10010;
    spu_verified_leaf_indirect_call(c,0x20000);spu_drain_call(c,0x20000);
    CHECK(c->status==0 && c->gpr[20]._u32[0]==4 && c->gpr[22]._u32[0]==1);
    CHECK(raw_entries==before_zero+(unsigned)enabled);
    c->status=SPU_STATUS_RUNNING;c->host_depth=0;
    c->region_dispatch_valid=1;c->region_dispatch_depth=0;c->region_has_return=1;
    c->region_return_pc=c->drain_ret_pc=0x20000;
    uint8_t other_bytes[32];memcpy(other_bytes,bytes,sizeof(bytes));
    put(other_bytes+16,0x1c000000u|(2u<<14)|(20u<<7)|20u);
    spu_verified_code_segment other_segment={0x10000,sizeof(other_bytes),other_bytes};
    CHECK(spu_verified_overlay_register(501,703,&other_segment,1));
    spu_begin_image(703);spu_register_function(0x10010,other);spu_begin_image(0);
    memcpy(c->ls+0x10000,other_bytes,sizeof(other_bytes));
    CHECK(spu_lifted_lookup(c,0x10010)==other); /* a leaf miss must retain legacy admission */
    memcpy(c->ls+0x10000,bytes,sizeof(bytes));
    /* Duplicate eligible byte matches must deopt rather than choose an owner. */
    CHECK(spu_verified_overlay_register(501,702,&segment,1));
    spu_begin_image(702);spu_register_function(0x10010,other);spu_begin_image(0);
    const spu_verified_leaf_unit ambiguous={0x10010,0x10018,other};
    CHECK(spu_verified_leaf_register(702,&ambiguous,1));
    CHECK(spu_lifted_lookup(c,0x10010)==NULL);
    CHECK(!spu_verified_leaf_enter(c,701,0x10010,0x10018));
    printf("LEAF mode=%d entries=%u passed=%d failed=%d\n",enabled,raw_entries,g_pass,g_fail);
    spu_mfc_release(c);spu_coh_unregister(c);free(c);return g_fail?1:0;
}
