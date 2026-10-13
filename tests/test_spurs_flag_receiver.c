/* Real guest initializer, receiver API, coherence, policy entry and poll path.
 * Only background thread creation is suppressed so each schedule is explicit. */
#include "../runtime/platform/win32_compat.h"
/* Layout/poll fixture: acknowledge thread creation without starting host
 * workers. The separate scheduler fixture verifies actual creation and joins. */
#define CreateThread(sa, stack, fn, arg, flags, id) ((HANDLE)1)
#define WaitForSingleObject(h, ms) (WAIT_OBJECT_0)
#define CloseHandle(h) (TRUE)
#include "../libs/spurs/cellSpurs.c"
#include "../runtime/spu/spu_coherency.h"
#include <assert.h>
#include <pthread.h>
extern s32 _cellSpursWorkloadFlagReceiver(CellSpurs*, u32, u32);
void spu_indirect_branch(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size=0x10000;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
uint32_t g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long ps3_ms_now(void) { return 0; }
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
#define S ((CellSpurs*)0x1000)
#define CALL(w,s) ((u32)_cellSpursWorkloadFlagReceiver(S,w,s))
static unsigned entries, expected_entry;
static void policy(spu_context* ctx)
{
    ++entries;
    assert(ctx->gpr[5]._u32[0]==expected_entry);
    assert(ctx->image_id==101);
    assert(vm_read32(0x106C)==0xFFFFFFFF);
    /* A fresh notification while this workload is already running. */
    vm_write32(0x106C,0);
    ctx->pc=SPURS_PM_SELECT_WORKLOAD_LS;
    ctx->gpr[0]._u32[0]=0x1234;
    spu_indirect_branch(ctx);
    assert(ctx->pc==0x1234);
    assert(ctx->gpr[3]._u32[0]==4 && ctx->gpr[3]._u32[1]==4);
    assert(vm_read32(0x106C)==0xFFFFFFFF);
    ctx->pc=SPURS_PM_SELECT_WORKLOAD_LS;
    spu_indirect_branch(ctx);
    assert(ctx->gpr[3]._u32[0]==4 && ctx->gpr[3]._u32[1]==0);
    ctx->pc=SPURS_PM_EXIT_TO_KERNEL_LS; spu_indirect_branch(ctx);
}
static void unrelated(spu_context* ctx)
{
    assert(ctx->gpr[5]._u32[0]==0);
    assert(vm_read32(0x106C)==0);
    ctx->pc=SPURS_PM_SELECT_WORKLOAD_LS;
    ctx->gpr[0]._u32[0]=0x1234;
    spu_indirect_branch(ctx);
    assert(ctx->gpr[3]._u32[0]==3 && ctx->gpr[3]._u32[1]==0);
    assert(vm_read32(0x106C)==0);
    ctx->pc=SPURS_PM_EXIT_TO_KERNEL_LS; spu_indirect_branch(ctx);
}
static void* compete(void* arg)
{
    return (void*)(uintptr_t)CALL((u32)(uintptr_t)arg,1);
}
int main(void)
{
    vm_base=malloc(ppu_vm_size); assert(vm_base);
    memset(vm_base,0xA5,ppu_vm_size);
    assert(cellSpursInitialize(S,3,0,0,0)==0);
    assert((u32)_cellSpursWorkloadFlagReceiver(NULL,32,1)==0x80410811);
    assert((u32)_cellSpursWorkloadFlagReceiver((CellSpurs*)0x1001,32,1)==0x80410810);
    assert(CALL(16,1)==0x80410802);
    assert(CALL(4,1)==0x80410805);
    vm_write32(0x10B0,0x0C000000); /* workloads 4 and 5 enabled */
    vm_write32(0x1D6C,1);
    assert(CALL(4,1)==0x8041080F);
    vm_write32(0x1D6C,0);
    /* Coherent registration invalidates an existing SPU reservation. */
    spu_context peer; memset(&peer,0,sizeof(peer));
    vm_write32(0x106C,0x12345678);
    peer.resv_valid=1; peer.resv_ea=0x1000;
    spu_lockline_lock(); spu_coh_reserve(&peer,0x1000); spu_lockline_unlock();
    assert(CALL(4,7)==0); /* every nonzero is_set means set */
    assert(vm_read32(0x106C)==0xFFFFFFFF && vm_base[0x1077]==4);
    assert(!peer.resv_valid && (peer.event_status & SPU_EVENT_LR));
    spu_coh_unregister(&peer);
    vm_write32(0x106C,0);
    unsigned char line[128]; memcpy(line,vm_base+0x1000,128);
    assert(CALL(4,1)==0x8041080A && CALL(5,1)==0x8041080A);
    assert(CALL(5,0)==0x80410809);
    assert(!memcmp(line,vm_base+0x1000,128));
    assert(CALL(4,0)==0);
    assert(vm_read32(0x106C)==0xFFFFFFFF && vm_base[0x1077]==0xFF);
    assert(CALL(4,0)==0x80410809);
    /* Single ownership under simultaneous valid setters. */
    pthread_t a,b; void *ar,*br;
    assert(!pthread_create(&a,NULL,compete,(void*)4));
    assert(!pthread_create(&b,NULL,compete,(void*)5));
    pthread_join(a,&ar);pthread_join(b,&br);
    assert((!ar && (uintptr_t)br==0x8041080A) || (!br && (uintptr_t)ar==0x8041080A));
    assert(CALL(vm_base[0x1077],0)==0);
    /* Unsupported SPURS2 cannot silently register a receiver. */
    vm_base[0x1074]=0x40;
    assert(CALL(32,1)==0x80410802);
    assert(CALL(4,1)==0x80410803);
    assert(vm_base[0x1077]==0xFF);
    vm_base[0x1074]=0;
    ppu_vm_size=0x1D6F;
    assert(CALL(4,1)==0x8041080D);
    ppu_vm_size=0x10000;
    assert((u32)_cellSpursWorkloadFlagReceiver((CellSpurs*)(uintptr_t)0x100001000ULL,4,1)==0x8041080D);
    assert(CALL(4,1)==0);
    vm_base[0x1083]=2;vm_base[0x1084]=2;
    vm_write32(0x10B0,0x1C000000);
    vm_write32(0x106C,0);
    assert(spurs_flag_status(0x1000,3,0)==0);
    assert(spurs_flag_status(0x1000,4,0)==4);
    assert(vm_read32(0x106C)==0); /* observing idle kick cannot consume it */
    vm_base[0x1084]=3;
    assert(spurs_flag_status(0x1000,4,1)==0 && vm_read32(0x106C)==0);
    vm_base[0x1084]=2;
    vm_write32(0x1D6C,1);
    assert(spurs_flag_status(0x1000,4,1)==0 && vm_read32(0x106C)==0);
    vm_write32(0x1D6C,0);
    uint8_t pm[16]={0};
    assert(spu_run_policy_module(unrelated,101,pm,sizeof(pm),0,3,0x1000,0)>=0);
    expected_entry=4;
    assert(spu_run_policy_module(policy,101,pm,sizeof(pm),0,4,0x1000,0)>=0);
    expected_entry=0;
    assert(spu_run_policy_module(policy,101,pm,sizeof(pm),0,4,0x1000,0)>=0);
    assert(entries==2);
    assert(CALL(4,0)==0);
    for (u32 i=0;i<3;i++) {
        assert(cellSpursAddWorkload(S,(u32*)0x8000,(void*)0x9000,16,0,NULL,1,1)==0);
        assert(vm_read32(0x8000)==i);
    }
    assert(CALL(0,1)==0);
    vm_write32(0x106C,0);
    assert(cellSpursRemoveWorkload(S,1)==0); /* nonowner must not consume */
    assert(vm_base[0x1077]==0 && vm_read32(0x106C)==0);
    assert(cellSpursRemoveWorkload(S,0)==0);
    assert(vm_base[0x1077]==0xFF && vm_read32(0x106C)==0xFFFFFFFF);
    assert(CALL(0,1)==0x80410805); /* removed workloads cannot register */
    assert(CALL(2,1)==0); /* removed owner cannot leave every new owner BUSY */
    free(vm_base);
    puts("SPURS receiver ownership and real policy entry/poll PASS");
}
