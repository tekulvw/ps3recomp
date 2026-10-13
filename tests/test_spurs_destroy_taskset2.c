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
static int check_release, release_checked;
static u32 release_wid;
void ppu_resv_break_store(uint64_t ea) {
    if(check_release && ea==0x1000+SPURS_WKL_STATE1+release_wid) {
        /* Production RemoveWorkload's last guest publication precedes its
         * in_use=0. No retiring data may survive to the reuse boundary. */
        SpursWorkload* w=&s_workloads[release_wid];
        assert(w->in_use && w->is_taskset && w->spurs_ea==0x1000);
        assert(!w->data && !w->pm && !w->sizePm && !w->name_ea && !w->started);
        assert(!vm_read64(0x1000+SPURS_WKL_INFO1+32*release_wid+8));
        release_checked=1;
    }
}
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

#define DESTROY(t) ((u32)cellSpursDestroyTaskset2(t))
static void unchanged(u32 expected, CellSpursTaskset2* t) {
    unsigned char before[0x2900]; memcpy(before,vm_base+0x10000,sizeof before);
    u32 enabled=vm_read32(0x1000+SPURS_WKL_ENABLED);
    assert(DESTROY(t)==expected);
    assert(!memcmp(before,vm_base+0x10000,sizeof before));
    assert(vm_read32(0x1000+SPURS_WKL_ENABLED)==enabled);
}
static atomic_int race_go;
static u32 race_create, race_destroy;
static void* race_submit(void* unused) {
    (void)unused; while(!atomic_load(&race_go)) {}
    race_create=(u32)cellSpursCreateTask((CellSpursTaskset*)TS,(u32*)0x8200,
                                        NULL,NULL,0,0,0);
    return NULL;
}
static void* race_retire(void* unused) {
    (void)unused; while(!atomic_load(&race_go)) {}
    race_destroy=DESTROY(TS); return NULL;
}
static void admission_races(void) {
    for(unsigned i=0;i<64;i++) {
        assert(CALL(S,TS,NULL)==0);
        vm_write32(0x8200,0xA5A5A5A5); atomic_store(&race_go,0);
        pthread_t a,b;
        assert(!pthread_create(&a,NULL,race_submit,NULL));
        assert(!pthread_create(&b,NULL,race_retire,NULL));
        atomic_store(&race_go,1); pthread_join(a,NULL); pthread_join(b,NULL);
        if(race_destroy==0) {
            assert(race_create==0x8041090F && vm_read32(0x8200)==0xA5A5A5A5);
            for(u32 off=0;off<0x60;off++) assert(!vm_base[0x10000+off]);
        } else {
            assert(race_destroy==0x8041090A && race_create==0);
            assert(cellSpursDestroyTaskset((CellSpursTaskset*)TS)==0);
        }
    }
}
int main(void) {
    vm_base=malloc(ppu_vm_size); assert(vm_base);
    memset(vm_base,0xA5,ppu_vm_size);
    memset(vm_base+0x1000,0,0x1000); s_inst[0].ea=0x1000; s_inst[0].kernel_live=1;
    unchanged(0x80410911,NULL);
    unchanged(0x80410910,(CellSpursTaskset2*)0x10001);
    unchanged(0x8041090D,(CellSpursTaskset2*)0xFF000);
    unchanged(0x8041090D,(CellSpursTaskset2*)(uintptr_t)0x100010000ULL);
    assert(CALL(S,TS,NULL)==0);
    u32 wid=vm_read32(0x10000+CSTS_WID);
    vm_write32(0x10000+CSTS_SIZE_FIELD,0x1900); unchanged(0x8041090F,TS);
    vm_write32(0x10000+CSTS_SIZE_FIELD,0x2900);
    vm_write32(0x10000+CSTS_EVENT_FLAG_ID1,1); unchanged(0x80410903,TS);
    vm_write32(0x10000+CSTS_EVENT_FLAG_ID1,0);
    vm_write64(0x10000+CSTS_SPURS,0x100001000ULL); unchanged(0x8041090F,TS);
    vm_write64(0x10000+CSTS_SPURS,0x1000);
    vm_write32(0x10000+CSTS_WID,32); unchanged(0x80410902,TS);
    vm_write32(0x10000+CSTS_WID,wid);
    /* Pending/ready/waiting state cannot be dismissed as an empty workload. */
    for(u32 off=0;off<0x60;off+=0x10) {
        vm_base[0x10000+off]=0x80; unchanged(0x8041090A,TS);
        vm_base[0x10000+off]=0;
    }
    release_wid=wid; check_release=1;
    assert(DESTROY(TS)==0);
    check_release=0; assert(release_checked);
    assert(vm_read32(0x10000+CSTS_WID)==32);
    assert(!s_workloads[wid].in_use);
    assert(!(vm_read32(0x1000+SPURS_WKL_ENABLED)&(0x80000000u>>wid)));
    assert(!vm_read64(0x1000+SPURS_WKL_INFO1+32*wid+8));
    assert(vm_base[0xFFFF]==0xA5 && vm_base[0x12900]==0xA5);
    unchanged(0x80410902,TS);
    /* A closed generation rejects submission before touching guest arguments
     * (even deliberately unmapped argument pointers), task bits or output. */
    vm_write32(0x8200,0xA5A5A5A5);
    assert((u32)cellSpursCreateTask((CellSpursTaskset*)TS,(u32*)0x8200,
             (void*)0x90000,NULL,0,0xFFFFFFFF,0xFFFFFFFF)==0x8041090F);
    assert(vm_read32(0x8200)==0xA5A5A5A5);
    assert((u32)cellSpursCreateTask((CellSpursTaskset*)(uintptr_t)0x100010000ULL,
             (u32*)0x8200,(void*)0x90000,NULL,0,0xFFFFFFFF,0xFFFFFFFF)==0x8041090D);
    assert(vm_read32(0x8200)==0xA5A5A5A5);
    assert(CALL(S,TS,NULL)==0); /* valid recreation resets the closed generation */
    make_elf(); vm_write32(0x8100,0x12345678);
    assert(cellSpursCreateTask((CellSpursTaskset*)TS,(u32*)0x8200,
             (void*)0x90000,NULL,0,0,0x8100)==0);
    /* Admission occurs synchronously before host thread creation, so destroy
     * rejects the task even when its worker has not reached the callback. */
    unchanged(0x8041090A,TS);
    u64 deadline=ps3_ms_now()+1000;
    while(!atomic_load(&entered) && ps3_ms_now()<deadline) usleep(100);
    assert(atomic_load(&entered)); unchanged(0x8041090A,TS);
    atomic_store(&release_worker,1);
    deadline=ps3_ms_now()+1000;
    while(spu_taskset_lifecycle_active(0x10000) && ps3_ms_now()<deadline) usleep(100);
    assert(!spu_taskset_lifecycle_active(0x10000));
    assert(!spurs_bitset_test(0x10000+CSTS_ENABLED,0));
    /* Empty enabled bits after worker completion are not proof of supported
     * Taskset2 shutdown: ever-submitted sets remain explicitly unsupported. */
    unchanged(0x8041090A,TS);
    atomic_store(&entered,0); atomic_store(&release_worker,0);
    assert(cellSpursCreateTask((CellSpursTaskset*)TS,(u32*)0x8200,
             (void*)0x90000,NULL,0,0,0x8100)==0);
    deadline=ps3_ms_now()+1000;
    while(!atomic_load(&entered) && ps3_ms_now()<deadline) usleep(100);
    assert(atomic_load(&entered));
    /* Legacy removal cannot permit valid Create2 to reset active references. */
    assert(cellSpursDestroyTaskset((CellSpursTaskset*)TS)==0);
    unsigned char before[0x2900]; memcpy(before,vm_base+0x10000,sizeof before);
    assert(CALL(S,TS,NULL)==0x8041090A);
    assert(!memcmp(before,vm_base+0x10000,sizeof before));
    atomic_store(&release_worker,1);
    deadline=ps3_ms_now()+1000;
    while(spu_taskset_lifecycle_active(0x10000) && ps3_ms_now()<deadline) usleep(100);
    assert(!spu_taskset_lifecycle_active(0x10000));
    unchanged(0x80410905,TS); /* legacy removal made the host owner unavailable */
    assert(CALL(S,TS,NULL)==0); assert(DESTROY(TS)==0);
    admission_races();
    /* WakeUp policy attempts own the selected workload, even with stale globals
     * from an unrelated Taskset1 and a policy fingerprint MISS. */
    assert(CALL(S,TS,NULL)==0);
    assert(cellSpursCreateTaskset(S,(CellSpursTaskset*)0x20000,0,NULL,0)==0);
    assert(g_ydkj_real_taskset_ea==0x20000);
    assert(cellSpursCreateTask((CellSpursTaskset*)0x20000,(u32*)0x8200,
                            NULL,NULL,0,0,0)==0); /* legacy admission is unchanged */
    assert(cellSpursWakeUp(S)==0);
    unchanged(0x8041090A,TS);
    puts("PASS: Taskset2 bounded destroy, invalid inputs, pending and real worker lifetime");
    free(vm_base); return 0;
}
