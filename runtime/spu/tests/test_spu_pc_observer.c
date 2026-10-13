/* Synthetic actual dispatcher/interpreter observer fixture; no title assets. */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, void (*)(spu_context*));
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
extern void spu_halt(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size;
static spu_context contexts[8];
static uint8_t expected_ls[8][SPU_LS_SIZE];
static u128 expected_gpr[8][128];
static int zero;
static void finish(spu_context* c) { c->status=SPU_STATUS_STOPPED_BY_STOP; spu_halt(c); }
static void entry(spu_context* c) { g_spu_trampoline_fn=spu_indirect_branch; }
static void* run_context(void* arg) {
    unsigned k=(unsigned)(uintptr_t)arg;
    {
        spu_context* c=&contexts[k]; spu_context_init(c,0);
        c->image_id=77; c->policy_mode=1;
        c->gpr[0]._u32[0]=0xABC000+k; c->gpr[1]._u32[0]=0xDEF000+k;
        /* Other lanes intentionally differ: observer must use preferred lane. */
        c->gpr[0]._u32[3]=0xBAD000+k; c->gpr[1]._u32[3]=0xBAD100+k;
        c->gpr[3]._u32[0]=0x1000; c->gpr[4]._u32[0]=16;
        c->gpr[5]._u32[0]=0x1000; c->gpr[6]._u32[0]=32;
        c->gpr[79]=spu_link(0x400);
        for(unsigned j=0;j<4;j++) spu_ls_write32(c,0x1000+j*4,zero?0:0x3F800000+j*0x800000);
        /* Load (microstep), shuffle (full fallback), store in place, nop, bi. */
        spu_ls_write32(c,0x30000,0x34000000|(3<<7)|20);
        spu_ls_write32(c,0x30004,0xB0000000|(20<<21)|(20<<14)|(20<<7)|22);
        spu_ls_write32(c,0x30008,0x24000000|(3<<7)|20);
        spu_ls_write32(c,0x3000C,0x40200000);
        spu_ls_write32(c,0x30010,0x35002780);
        /* Swap two words: nonzero in-place conversion with preserved finite count. */
        const unsigned char mask[16]={4,5,6,7,0,1,2,3,12,13,14,15,8,9,10,11};
        for(unsigned j=0;j<16;j++) c->gpr[22]._u8[SPU_W(j)]=mask[j];
        memcpy(expected_ls[k],c->ls,SPU_LS_SIZE);
        memcpy(expected_gpr[k],c->gpr,sizeof c->gpr);
        const unsigned order[4]={1,0,3,2};
        for(unsigned j=0;j<4;j++) {
            uint32_t bits=zero?0:0x3F800000+order[j]*0x800000;
            expected_gpr[k][20]._u32[j]=bits;
            for(unsigned b=0;b<4;b++) expected_ls[k][0x1000+j*4+b]=(uint8_t)(bits>>(24-b*8));
        }
        fprintf(stdout,"context%u=%p\n",k,(void*)c);
        c->pc=0x30000; c->status=SPU_STATUS_RUNNING;
        if(k==0) assert(spu_run_with_halt(entry,c)==1);
        else assert(spu_interp_run_until(c,0x30000,0)==0x400);
        assert(!memcmp(c->ls,expected_ls[k],SPU_LS_SIZE));
        assert(!memcmp(c->gpr,expected_gpr[k],sizeof c->gpr));
        for(unsigned j=0;j<4;j++) assert(spu_ls_read32(c,0x1000+j*4)==(zero?0:0x3F800000+order[j]*0x800000));
        assert(c->gpr[0]._u32[0]==0xABC000+k && c->gpr[1]._u32[0]==0xDEF000+k);
    }
    return NULL;
}
int main(int argc, char** argv) {
    zero=argc>1 && !strcmp(argv[1],"zero");
    spu_begin_image(77); spu_register_function(0x400,finish);
    if(argc>1 && !strcmp(argv[1],"threads")) {
        pthread_t threads[8];
        for(unsigned i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,run_context,(void*)(uintptr_t)i));
        for(unsigned i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
    } else { run_context((void*)0); run_context((void*)1); }
    return 0;
}
/* Link-only boundaries outside this CPU-only fixture. Fail if any is reached. */
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
