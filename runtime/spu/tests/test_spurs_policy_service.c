/* Manufactured policy service request using real PM, channel and LV2 paths. */
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint8_t guest[1<<20];
uint8_t* vm_base=guest;
uint32_t ppu_vm_size=sizeof guest;
static _Thread_local uint32_t captured_queue;
static _Thread_local uint64_t captured_event[4];
static int push_result;
static unsigned vm_result_writes;
#include "../../syscalls/lv2_register.c"
#include "../spu_workload.h"
extern SPU_THREAD_LOCAL volatile unsigned g_spurs_pm_exited;
#include "../spu_helpers.h"
#include "../spurs_policy_service.h"
extern void spu_wrch(spu_context*,uint32_t,u128);
extern u128 spu_rdch(spu_context*,uint32_t);
extern int (*g_spu_user_event_hook)(spu_context*,uint32_t);
static uint32_t ids[16];
static unsigned calls;
static int delayed_mode, delayed_started, delayed_returned, delayed_early;
struct Formatter { uint32_t id; };
static void* formatter(void* arg) {
    struct Formatter* f=arg;
    while(!__atomic_load_n(&delayed_started,__ATOMIC_ACQUIRE)) { struct timespec t={0,1000000};nanosleep(&t,NULL); }
    spu_context* actual=spurs_policy_service_acquire(f->id);assert(actual);
    int observed=0;
    for(unsigned i=0;i<2000;i++) {
        delayed_early=__atomic_load_n(&delayed_returned,__ATOMIC_ACQUIRE);
        uint32_t status=(uint32_t)InterlockedCompareExchange((LONG*)&actual->status,0,0);
        if(delayed_early || status==SPU_STATUS_WAITING_CHANNEL){observed=1;break;}
        struct timespec tick={0,1000000};nanosleep(&tick,NULL);
    }
    assert(observed); /* Never publish merely because a timer expired. */
    spurs_policy_service_release(actual);
    ppu_context p={0};p.gpr[3]=f->id;p.gpr[4]=0x200;p.gpr[5]=0x800;p.gpr[6]=4;
    assert(!sys_spu_thread_read_ls_handler(&p));
    p.gpr[3]=f->id;p.gpr[4]=0x76543210;assert(!sys_spu_thread_write_spu_mb_handler(&p));
    return NULL;
}
static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv=PTHREAD_COND_INITIALIZER;
static int gate_enabled,gate_arrived,gate_release;
static uint32_t be32(const uint8_t* p){return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];}

static void policy(spu_context* c) {
    const uint32_t off=0x200;
    c->ls[off]=0x12;c->ls[off+1]=0x34;c->ls[off+2]=0x56;c->ls[off+3]=0x78;
    spu_wrch(c,SPU_WrOutMbox,spu_make_preferred_u32(off));
    spu_wrch(c,SPU_WrOutIntrMbox,spu_make_preferred_u32(0x01000000));
    assert(captured_queue==77 && captured_event[0]==0xFFFFFFFF53505501ull);
    assert(captured_event[1]==spurs_policy_service_identity(c) && captured_event[3]==off);
    if(push_result) {
        assert(!spurs_policy_service_reply_pending(c));
        assert(spu_rdch(c,SPU_RdInMbox)._u32[0]==CELL_EBUSY);
        /* No pending service: preserve the existing latched empty-read path. */
        assert(spu_rdch(c,SPU_RdInMbox)._u32[0]==CELL_EBUSY);
        g_spurs_pm_exited=1;return;
    }
    /* Each physical lane has a different result destination for concurrent runs. */
    uint32_t output=0x400+be32(c->ls+0x1c8)*16;
    ppu_context p={0};p.gpr[3]=captured_event[1];p.gpr[4]=off;p.gpr[5]=output;p.gpr[6]=4;
    int64_t read_result=sys_spu_thread_read_ls_handler(&p);
    if(read_result) { fprintf(stderr,"FAIL: PM service event identity %u cannot resolve actual LS (read=%lld)\n",c->spu_id,(long long)read_result);exit(1); }
    assert(__atomic_load_n(&vm_result_writes,__ATOMIC_RELAXED)>0);
    assert(guest[output+4]==0x12 && guest[output+5]==0x34 && guest[output+6]==0x56 && guest[output+7]==0x78);
    assert(spu_rdch(c,SPU_RdInMbox)._u32[0]==CELL_OK); /* Kernel enqueue ack is distinct. */
    if(delayed_mode) {
        struct Formatter f={(uint32_t)captured_event[1]};pthread_t worker;
        assert(!pthread_create(&worker,NULL,formatter,&f));
        __atomic_store_n(&delayed_started,1,__ATOMIC_RELEASE);
        uint32_t reply=spu_rdch(c,SPU_RdInMbox)._u32[0];
        __atomic_store_n(&delayed_returned,1,__ATOMIC_RELEASE);
        assert(!pthread_join(worker,NULL));
        if(delayed_early || reply!=0x76543210) {
            fprintf(stderr,"FAIL: delayed PPU reply bypassed (early=%d reply=%08X)\n",delayed_early,reply);exit(1);
        }
    } else {
        p.gpr[3]=captured_event[1];p.gpr[4]=0x76543210;
        assert(sys_spu_thread_write_spu_mb_handler(&p)==0);
        assert(spu_channel_has_data(&c->ch_in_mbox));
        assert(spu_rdch(c,SPU_RdInMbox)._u32[0]==0x76543210);
    }
    assert(!spurs_policy_service_reply_pending(c));
    ids[__atomic_fetch_add(&calls,1,__ATOMIC_RELAXED)]=spurs_policy_service_identity(c);
    assert(c->spu_id==0); /* No change to the existing physical/context ABI. */
    assert(spurs_policy_release_workload(be32(c->ls+0x1c4),be32(c->ls+0x1dc))==-1);
    pthread_mutex_lock(&gate_lock);
    if(gate_enabled){gate_arrived++;pthread_cond_broadcast(&gate_cv);while(!gate_release)pthread_cond_wait(&gate_cv,&gate_lock);}
    pthread_mutex_unlock(&gate_lock);
    g_spurs_pm_exited=1;
}
static const uint8_t pm[16]={0};
struct Run { uint32_t ea,wid,lane; uint64_t data; int result; };
static void* run(void* value){struct Run* r=value;r->result=spu_run_policy_module(policy,77,pm,sizeof pm,r->data,r->wid,r->ea,r->lane);return NULL;}
static void* teardown(void* unused){(void)unused;spurs_policy_release_instance(0x2000);return NULL;}
int main(void) {
    g_spu_user_event_hook=spu_deliver_user_event;
    assert(spu_run_policy_module(policy,77,pm,sizeof pm,0x9000,0,0x1000,0)==0);
    assert(spu_run_policy_module(policy,77,pm,sizeof pm,0x9000,0,0x1000,0)==0);
    assert(ids[0] && ids[0]==ids[1]);
    spu_context* held=spurs_policy_service_acquire(ids[0]);assert(held);
    assert(spurs_policy_release_workload(0x1000,0)==-1); /* No partial retirement. */
    spu_context* check=spurs_policy_service_acquire(ids[0]);assert(check==held);spurs_policy_service_release(check);
    spurs_policy_service_release(held);
    assert(spurs_policy_release_workload(0x1000,0)==0);
    ppu_context p={0};p.gpr[3]=ids[0];p.gpr[4]=0x200;p.gpr[5]=0x400;p.gpr[6]=4;
    assert(sys_spu_thread_read_ls_handler(&p)!=0);
    p.gpr[3]=ids[0];p.gpr[4]=7;assert(sys_spu_thread_write_spu_mb_handler(&p)==(int32_t)CELL_ESRCH);
    gate_enabled=1;
    struct Run a={0x1000,0,0,0x9000,0},b={0x2000,0,1,0x9000,0};pthread_t ta,tb;
    assert(!pthread_create(&ta,NULL,run,&a));assert(!pthread_create(&tb,NULL,run,&b));
    pthread_mutex_lock(&gate_lock);while(gate_arrived!=2)pthread_cond_wait(&gate_cv,&gate_lock);gate_release=1;pthread_cond_broadcast(&gate_cv);pthread_mutex_unlock(&gate_lock);
    assert(!pthread_join(ta,NULL) && !pthread_join(tb,NULL));assert(!a.result && !b.result);
    gate_enabled=0;
    assert(ids[2]!=ids[0] && ids[3]!=ids[2]);
    assert(spu_run_policy_module(policy,77,pm,sizeof pm,0x9000,1,0x2000,1)==0);
    assert(ids[4]!=ids[2] && ids[4]!=ids[3]); /* Different workload on the same lane. */
    held=spurs_policy_service_acquire(ids[4]);assert(held);
    struct Run reset={0x2000,1,1,0xA000,0};assert(!pthread_create(&ta,NULL,run,&reset));
    /* Reset invalidates new lookups but waits until the actual PPU reference ends. */
    int retired=0;
    for(unsigned i=0;i<1000;i++){
        check=spurs_policy_service_acquire(ids[4]);
        if(!check){retired=1;break;}spurs_policy_service_release(check);
        struct timespec tick={0,1000000};nanosleep(&tick,NULL);
    }
    assert(retired);
    assert(spu_run_policy_module(policy,77,pm,sizeof pm,0x9000,2,0x1000,0)==0); /* Other instance progresses. */
    spurs_policy_service_release(held);
    assert(!pthread_join(ta,NULL) && !reset.result);
    assert(ids[6]!=ids[4]);
    check=spurs_policy_service_acquire(ids[6]);assert(check);
    p.gpr[3]=ids[6];p.gpr[4]=SPU_LS_SIZE-1;p.gpr[5]=0x400;p.gpr[6]=8;assert(sys_spu_thread_read_ls_handler(&p)==(int32_t)CELL_EFAULT);
    p.gpr[3]=ids[6];p.gpr[4]=0x200;p.gpr[5]=0xAB;p.gpr[6]=1;assert(sys_spu_thread_write_ls_handler(&p)==0 && check->ls[0x200]==0xAB);
    for(unsigned i=0;i<SPU_IN_MBOX_HW_DEPTH;i++){p.gpr[3]=ids[6];p.gpr[4]=i+1;assert(!sys_spu_thread_write_spu_mb_handler(&p));}
    p.gpr[3]=ids[6];p.gpr[4]=99;assert(sys_spu_thread_write_spu_mb_handler(&p)==(int32_t)CELL_EBUSY);
    captured_queue=0;
    spu_channel_write(&check->ch_out_mbox,0x200);
    assert(spu_deliver_user_event(check,0x01000000)==1);
    assert(!captured_queue && check->ch_in_mbox.count==SPU_IN_MBOX_HW_DEPTH);
    for(unsigned i=0;i<SPU_IN_MBOX_HW_DEPTH;i++)assert(spu_channel_read(&check->ch_in_mbox)==i+1);
    /* A referenced instance teardown rejects stale acquisitions while other
     * instances remain runnable and callback release can finish independently. */
    assert(!pthread_create(&ta,NULL,teardown,NULL));
    retired=0;
    for(unsigned i=0;i<1000;i++){
        held=spurs_policy_service_acquire(ids[6]);
        if(!held){retired=1;break;}spurs_policy_service_release(held);
        struct timespec tick={0,1000000};nanosleep(&tick,NULL);
    }
    assert(retired);
    spurs_policy_release_instance(0x2000); /* Second retire must not free first caller's generation. */
    assert(check->ls[0x200]==0xAB);
    assert(spu_run_policy_module(policy,77,pm,sizeof pm,0x9000,3,0x1000,0)==0);
    spurs_policy_service_release(check);
    assert(!pthread_join(ta,NULL));
    spurs_policy_release_instance(0x1000);
    assert(!spurs_policy_service_acquire(ids[6]));
    delayed_mode=1;
    assert(spu_run_policy_module(policy,77,pm,sizeof pm,0x9000,4,0x3000,0)==0);
    delayed_mode=0;push_result=1;
    assert(spu_run_policy_module(policy,77,pm,sizeof pm,0x9000,5,0x3000,0)==0);
    spurs_policy_release_instance(0x3000);
    puts("PASS: delayed PPU formatter blocks the actual second RdInMbox until real reply");
    puts("PASS: real PM service / actual LS / separate replies / concurrent identity / referenced retirement and reset");
}

void sys_ppu_thread_init(lv2_syscall_table* t) { (void)t; }
void sys_mutex_init(lv2_syscall_table* t)      { (void)t; }
void sys_cond_init(lv2_syscall_table* t)       { (void)t; }
void sys_semaphore_init(lv2_syscall_table* t)  { (void)t; }
void sys_rwlock_init(lv2_syscall_table* t)     { (void)t; }
void sys_event_init(lv2_syscall_table* t)      { (void)t; }
void sys_timer_init(lv2_syscall_table* t)      { (void)t; }
void sys_memory_init(lv2_syscall_table* t)     { (void)t; }
void sys_vm_init(lv2_syscall_table* t)         { (void)t; }
void sys_fs_init(lv2_syscall_table* t)         { (void)t; }
const char* np_psnr_identity(void)            { return NULL; }

int sys_event_queue_push_by_id(uint32_t q, uint64_t d0, uint64_t d1,
                               uint64_t d2, uint64_t d3)
{
    captured_queue = q;
    captured_event[0] = d0; captured_event[1] = d1;
    captured_event[2] = d2; captured_event[3] = d3;
    return push_result;
}
void sys_fs_translate_path(const char* ps3_path, char* host, int size)
{
    (void)ps3_path;
    if (host && size > 0) host[0] = 0;
}
void sys_process_exit(int32_t status)            { exit((int)status); }
int32_t sys_process_getpid(void)                 { return 1; }

void ppu_dump_guest_stack(ppu_context* c, const char* tag) { (void)c; (void)tag; }
void ppu_guard_page(uint32_t ea)                 { (void)ea; }
void ppu_guest_caller(char* out, size_t n)       { if (out && n) out[0] = 0; }
void ppu_guest_callstack(const char* tag)        { (void)tag; }
void ppu_log_host_chain(const char* tag)         { (void)tag; }
void ps3_hle_register_ctx(uint32_t nid, const char* name,
                          void (*fn)(ppu_context*))
{
    (void)nid; (void)name; (void)fn;
}
void ppu_resv_break_store(uint64_t ea)           { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)              { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
int      g_barrier_sync_watch = 0;

/* Title-specific SPURS hooks the SPU half offers but nothing here installs. */
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint32_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

/* Upstream raw-SPU and RSX services are outside this group-start fixture. */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t ps3_spu_image_source_ea(uint32_t ea) { return ea; }
/* cellSpurs owns this; 0 = "not a SPURS port", the plain lv2 event path. */
uint32_t spurs_port_queue(uint32_t port) { return port == 1 ? 77 : 0; }
void sys_raw_spu_init(lv2_syscall_table* t) { (void)t; }
void sys_rsx_init(lv2_syscall_table* t) { (void)t; }
void spu_raw_note_image(uint32_t ea, uint32_t src) { (void)ea; (void)src; }
int32_t spu_registry_fallback(uint32_t tid, uint32_t args, uint32_t size, void* user)
{ (void)tid; (void)args; (void)size; (void)user; return -1; }

int64_t sys_event_flag_set(ppu_context* c) { (void)c;return -1; }

/* The service must use the existing VM writer, not bypass its runtime hooks. */
void vm_write64(uint64_t ea,uint64_t v){
    __atomic_fetch_add(&vm_result_writes,1,__ATOMIC_RELAXED);
    for(unsigned i=0;i<8;i++)guest[ea+i]=(uint8_t)(v>>((7-i)*8));
}
