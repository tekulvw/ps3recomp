/* Real HLE guest bytes, workload registration, PM context and async SPU worker.
 * Only the lifted title callback is supplied by this asset-free fixture. */
#include "../libs/spurs/cellSpurs.c"
#include "../runtime/spu/spu_helpers.h"
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x100000;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w) {(void)a;(void)v;(void)w;}
/* Title-specific CRI wake bridge must remain unused in this generic worker. */
void ydkj_wake_all_event_flags(void) { abort(); }
unsigned long long ps3_ms_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000;
}
extern void spu_halt(spu_context*);
#define S ((CellSpurs*)0x1000)
#define TS ((CellSpursTaskset2*)0x10000)
#define A ((CellSpursTasksetAttribute2*)0x8000)
#define CALL(s,t,a) ((u32)cellSpursCreateTaskset2(s,t,a))
static atomic_int entered, release_worker;
static void worker(spu_context* c) {
    assert(c->image_id == 101);
    assert(spu_ls_read32(c, STC_TASKSET_PTR+4) == 0x10000);
    assert(spu_ls_read32(c, STC_TASK_ID) == 0);
    assert(spu_ls_read32(c, STC_TEMP_TASKINFO) == 0x12345678);
    assert(spu_ls_read32(c, 0x1C4) == 0x1000);
    assert(spu_ls_read32(c, 0x3000) == 0x11223344);
    atomic_store(&entered, 1);
    while (!atomic_load(&release_worker)) usleep(100);
    spu_halt(c);
}
static void make_elf(void) {
    /* One PT_LOAD containing four bytes at LS 0x3000. */
    memset(vm_base+0x90000,0,0x104);
    vm_write32(0x90000,0x7F454C46); vm_base[0x90004]=1;
    vm_base[0x90005]=2; vm_base[0x90006]=1;
    vm_write16(0x90010,2); vm_write16(0x90012,23);
    vm_write32(0x90014,1); vm_write32(0x90018,0x3000);
    vm_write32(0x9001C,52); vm_write16(0x90028,52);
    vm_write16(0x9002A,32); vm_write16(0x9002C,1);
    vm_write32(0x90034,1); vm_write32(0x90038,0x100);
    vm_write32(0x9003C,0x3000); vm_write32(0x90044,4);
    vm_write32(0x90048,4); vm_write32(0x9004C,5);
    vm_write32(0x90050,16); vm_write32(0x90100,0x11223344);
    assert(spu_elf_image_size(vm_base+0x90000,0x104)==0x104);
    spu_workload_register_img(spu_workload_fingerprint(vm_base+0x90000,0x104),
                              worker,101,"taskset2 fixture");
}
static void rejected(u32 expected, CellSpurs* s, CellSpursTaskset2* t,
                     const CellSpursTasksetAttribute2* a) {
    unsigned char before[0x2900]; memcpy(before,vm_base+0x10000,sizeof(before));
    u32 enabled=vm_read32(0x1000+SPURS_WKL_ENABLED);
    assert(CALL(s,t,a)==expected);
    assert(!memcmp(before,vm_base+0x10000,sizeof(before)));
    assert(vm_read32(0x1000+SPURS_WKL_ENABLED)==enabled);
}
int main(void) {
    vm_base=malloc(ppu_vm_size); assert(vm_base);
    memset(vm_base,0xA5,ppu_vm_size);
    memset(vm_base+0x1000,0,0x1000); s_inst[0].ea=0x1000; s_inst[0].kernel_live=1;
    rejected(0x80410911,NULL,TS,NULL);
    rejected(0x80410911,S,NULL,NULL);
    rejected(0x80410910,(CellSpurs*)0x1001,TS,NULL);
    rejected(0x80410910,S,(CellSpursTaskset2*)0x10001,NULL);
    rejected(0x80410910,S,TS,(CellSpursTasksetAttribute2*)0x8001);
    rejected(CELL_SPURS_CORE_ERROR_STAT,(CellSpurs*)0x3000,TS,NULL);
    rejected(0x8041090D,S,(CellSpursTaskset2*)0xFF000,NULL);
    rejected(0x8041090D,S,TS,(CellSpursTasksetAttribute2*)0xFFF00);
    rejected(0x8041090D,S,(CellSpursTaskset2*)(uintptr_t)0x100010000ULL,NULL);
    vm_base[0x1074]=0x40; rejected(0x80410903,S,TS,NULL); vm_base[0x1074]=0;
    assert(CALL(S,TS,NULL)==0);
    u32 wid=vm_read32(0x10000+CSTS_WID);
    assert(vm_read32(0x10000+CSTS_SIZE_FIELD)==0x2900);
    assert(vm_read64(0x10000+CSTS_SPURS)==0x1000);
    assert(vm_read64(0x10000+CSTS_ARGS)==0);
    assert(vm_base[0x10000+CSTS_WKL_FLAG_WAIT]==0x80);
    for(u32 off=0;off<0x2900;off++) {
        if((off>=0x60 && off<0x68) || off==0x72 ||
           (off>=0x74 && off<0x78) || (off>=0x1890 && off<0x1894)) continue;
        assert(vm_base[0x10000+off]==0);
    }
    assert(vm_base[0xFFFF]==0xA5 && vm_base[0x12900]==0xA5);
    assert(s_workloads[wid].is_taskset && s_workloads[wid].spurs_ea==0x1000);
    assert(s_workloads[wid].maxContention==8);
    for(int i=0;i<8;i++) {
        assert(s_workloads[wid].priority[i]==1);
        assert(vm_base[0x1000+SPURS_WKL_INFO1+32*wid+0x18+i]==1);
    }
    assert(vm_read64(0x1000+SPURS_WKL_INFO1+32*wid+8)==0x10000);
    assert(cellSpursGetWorkloadInfo(0x1000,wid,0x8300)==0);
    assert(vm_read64(0x8300)==0x10000);
    assert(vm_read32(0x8310)==0x200 && vm_read32(0x8314)==0x1E40);
    assert(vm_base[0x8321]==8 && vm_base[0x8322]==8);
    rejected(0x8041090A,S,TS,NULL);
    make_elf(); vm_write32(0x8100,0x12345678);
    assert(cellSpursCreateTask((CellSpursTaskset*)TS,(u32*)0x8200,
                               (void*)0x90000,NULL,0,0,0x8100)==0);
    u64 deadline=ps3_ms_now()+1000;
    while(!atomic_load(&entered) && ps3_ms_now()<deadline) usleep(100);
    assert(atomic_load(&entered) && vm_read32(0x8200)==0);
    assert(spurs_bitset_test(0x10000+CSTS_ENABLED,0));
    atomic_store(&release_worker,1);
    deadline=ps3_ms_now()+1000;
    while(spurs_bitset_test(0x10000+CSTS_ENABLED,0) && ps3_ms_now()<deadline) usleep(100);
    assert(!spurs_bitset_test(0x10000+CSTS_ENABLED,0));
    assert(cellSpursDestroyTaskset((CellSpursTaskset*)TS)==0);
    assert(!(vm_read32(0x1000+SPURS_WKL_ENABLED)&(0x80000000u>>wid)));
    assert(vm_read64(0x1000+SPURS_WKL_INFO1+32*wid+8)==0);
    assert(!s_workloads[wid].in_use);
    /* Explicit attributes are guest BE bytes, never a native host struct. */
    assert((u32)_cellSpursTasksetAttribute2Initialize(NULL,0)==0x80410911);
    assert((u32)_cellSpursTasksetAttribute2Initialize((CellSpursTasksetAttribute2*)0x8001,0)==0x80410910);
    assert((u32)_cellSpursTasksetAttribute2Initialize((CellSpursTasksetAttribute2*)0xFFF00,0)==0x8041090D);
    vm_base[0x8200]=0xA5;
    assert(_cellSpursTasksetAttribute2Initialize(A,0)==0);
    assert(vm_read32(0x8018)==8 && vm_read64(0x8008)==0);
    assert(vm_base[0x81FF]==0);
    assert(vm_base[0x7FFF]==0xA5 && vm_base[0x8200]==0xA5);
    for(int i=0;i<8;i++) assert(vm_base[0x8010+i]==1);
    vm_write64(0x8008,0x0123456789ABCDEFULL);
    vm_write32(0x8004,0x8400);
    vm_write32(0x8018,3); vm_write32(0x801C,1); vm_base[0x8010]=7;
    assert(CALL(S,TS,A)==0);
    assert(vm_read64(0x10000+CSTS_ARGS)==0x0123456789ABCDEFULL);
    assert(vm_base[0x10000+CSTS_ENABLE_CLEAR_LS]==1);
    wid=vm_read32(0x10000+CSTS_WID);
    assert(s_workloads[wid].maxContention==3 && s_workloads[wid].priority[0]==7);
    assert(cellSpursGetWorkloadInfo(0x1000,wid,0x8300)==0);
    assert(vm_read32(0x831C)==0x8400);
    assert(cellSpursDestroyTaskset((CellSpursTaskset*)TS)==0);
    vm_write32(0x8020,0x7001); rejected(0x80410910,S,TS,A);
    vm_write32(0x8020,0x7000); rejected(0x80410903,S,TS,A);
    vm_write32(0x8020,0); vm_write32(0x8018,99);
    assert(CALL(S,TS,A)==0);
    wid=vm_read32(0x10000+CSTS_WID);
    assert(s_workloads[wid].maxContention==8);
    assert(cellSpursDestroyTaskset((CellSpursTaskset*)TS)==0);
    vm_write32(0x8018,8); vm_base[0x8010]=16;
    rejected(0x80410902,S,TS,A);
    vm_base[0x8010]=1;
    /* A different taskset and instance gets a different live workload id. */
    memset(vm_base+0x3000,0,0x1000); s_inst[1].ea=0x3000; s_inst[1].kernel_live=1;
    assert(CALL(S,TS,NULL)==0); wid=vm_read32(0x10000+CSTS_WID);
    assert(CALL((CellSpurs*)0x3000,(CellSpursTaskset2*)0x30000,NULL)==0);
    u32 other=vm_read32(0x30000+CSTS_WID); assert(other!=wid);
    assert(cellSpursDestroyTaskset((CellSpursTaskset*)TS)==0);
    assert(vm_read64(0x3000+SPURS_WKL_INFO1+32*other+8)==0x30000);
    assert(cellSpursDestroyTaskset((CellSpursTaskset*)0x30000)==0);
    for(unsigned i=0;i<CELL_SPURS_MAX_WORKLOAD;i++) s_workloads[i].in_use=1;
    rejected(CELL_SPURS_CORE_ERROR_NOMEM,S,TS,A);
    puts("Taskset2 BE layout, defaults, validation, registration and real worker PASS");
    /* The production worker is detached; process teardown owns its allocations. */
    return 0;
}
