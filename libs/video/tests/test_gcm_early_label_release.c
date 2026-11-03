/* Real cellGcm FIFO drain with PS3RECOMP_RSX_EARLY_LABEL_RELEASE and
 * PS3RECOMP_RSX_EARLY_FIFO_SYNC semantics. Synthetic backend: tickets
 * complete only when the fixture says so; no GPU, no guest content. */
#include "../cellGcmSys.c"
#include <assert.h>
#include <sys/mman.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active = 0;
uint32_t g_ww_lo, g_ww_hi;
ps3_guest_caller_fn g_ps3_guest_caller;
u32 g_rsx_last_reference;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_dump_guest_stack(ppu_context* c,const char* t) {(void)c;(void)t;}
void ppu_dump_bctrl_ring(uint32_t id,const char* t) {(void)id;(void)t;}
static unsigned locked, notified, flushed, checked_flushes;
void ppu_resv_break_store(uint64_t ea) { (void)ea; assert(locked); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a;(void)v;(void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; assert(locked); notified++; }
void spu_lockline_lock(void) { assert(!locked); locked=1; }
void spu_lockline_unlock(void) { assert(locked); locked=0; }
int rsx_draw_engine_enabled(void) { return 1; }
int rsx_live_draw_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 m, u32 v) { (void)m; (void)v; }
void rsx_draw_engine_method_at(u32 m,u32 v,u32 ea){(void)ea;rsx_draw_engine_method(m,v);}
void rsx_live_draw_method(u32 m, u32 v) { (void)m; (void)v; }
void rsx_draw_engine_flush(void) { flushed++; }
void rsx_live_draw_flush(void) {}
void rsx_state_init(rsx_state* s) { memset(s,0,sizeof(*s)); }
int rsx_process_method(rsx_state* s,u32 m,u32 v) { (void)s; if(m==0x50u)g_rsx_last_reference=v; return 0; }
void rsx_raise_user_cmd(u32 arg) { (void)arg; }
void rsx_live_draw_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
int rsx_live_draw_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return 0; }
int rsx_live_draw_resolve_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h,u32 i,u32 j,u32 k,u32 l) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;return 0; }
void rsx_live_draw_note_resolve(u32 a,u32 b,u32 c,u32 d) { (void)a;(void)b;(void)c;(void)d; }
void rsx_draw_engine_guest_read(u32 a,u32 b,u32 c,u32 d,u32 e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void rsx_draw_engine_guest_wrote(u32 a,u32 b,u32 c,u32 d,u32 e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void rsx_draw_engine_note_flip(u32 a) {(void)a;}
void rsx_draw_engine_mark_guest_dirty(u32 ea,u32 len) { (void)ea; (void)len; }
void rsx_live_draw_method_at(u32 method,u32 arg,u32 data_ea) { (void)data_ea;rsx_live_draw_method(method,arg); }
static int gpu_error;
static u64 issued, completed, blocking_waits;
int rsx_draw_engine_async_available(void){return 1;}
u64 rsx_draw_engine_submit_async(void){return ++issued;}
/* A blocking wait is a GPU wait: the fixture completes on demand and counts it. */
int rsx_draw_engine_completion_status(u64 ticket,int wait){
    if(gpu_error)return -1;
    if(wait&&ticket>completed){blocking_waits++;completed=ticket;}
    return ticket<=completed;
}
void rsx_draw_engine_set_report_barrier(int(*barrier)(void)){(void)barrier;}
int rsx_draw_engine_checked_ready(void) { return !gpu_error; }
int rsx_draw_engine_flush_checked(void) { checked_flushes++; completed=issued; return gpu_error?0:1; }

static void raw32(u32 ea,u32 v) { v=ps3_bswap32(v);memcpy(vm_base+ea,&v,4); }
static void fifo(const u32* w,u32 n) {
    for(u32 i=0;i<n;i++)raw32(0x100000+i*4,w[i]);
    /* The consumer body only: the API wrapper's own barrier would add a
     * checked flush that is not part of the FIFO methods under test. */
    cellGcm_rsx_consumer_begin();cellGcm_syscall_set_fifo_consumer_body(n*4,0);cellGcm_rsx_consumer_end();
    cellGcm_rsx_process_fifo();
    assert(vm_read32(GCM_CONTROL_GUEST_ADDR+4)==n*4);
}
static void observe(uint32_t k,uint32_t ea,uint32_t v,uint64_t t,uint64_t a){(void)k;(void)ea;(void)v;(void)t;(void)a;}
static void reset(int early,int sync) {
    memset(&s_reports,0,sizeof(s_reports));s_reports.generation=3;
    s_deferred_reports=1;s_early_release=early;s_early_fifo_sync=sync;s_reports.early_mode=(u32)early;
    issued=completed=blocking_waits=0;flushed=checked_flushes=notified=0;gpu_error=0;
    atomic_store(&s_checked_ref_failed,0);s_ref_qhead=s_ref_qtail=0;
}
int main(void) {
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);
    ppu_hle_inject_base=0x3000000; ppu_vm_size=0x4000000;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));
    s_ea_address_table[0]=1; s_ea_address_table[0xE0]=0x20;
    s_gcm_syscall_mode=1;s_config.localAddress=0;

    /* 1. Flag off: a backend release stays unpublished until completion. */
    reset(0,0);
    u32 label=VM_HLE_INJECT_BASE+0x900;raw32(label,0xa5a5a5a5);
    { const u32 w[]={0x41d6c,0x900,0x41d74,0x11223344}; fifo(w,4); }
    assert(vm_read32(label)==0xa5a5a5a5&&s_reports.count==1&&s_reports.gated==1);
    completed=issued;cellGcm_rsx_process_fifo();
    assert(vm_read32(label)==0x11223344&&!s_reports.count);

    /* 2. Early: both NV4097 releases publish at their FIFO position with
     * their usual value encodings, before the backend completes. */
    reset(1,0);
    u32 tex=VM_HLE_INJECT_BASE+0x910;raw32(label,0xa5a5a5a5);raw32(tex,0xa5a5a5a5);
    { const u32 w[]={0x41d6c,0x900,0x41d74,0x55667788,0x41d6c,0x910,0x41d70,0x11223344}; fifo(w,8); }
    assert(vm_read32(label)==0x55667788&&vm_read32(tex)==0x11443322);
    assert(s_reports.count==2&&!s_reports.gated&&issued>=1&&completed<issued&&!blocking_waits);
    /* Completion only verifies: a later guest write is not overwritten. */
    raw32(label,0x01010101);completed=issued;cellGcm_rsx_process_fifo();
    assert(!s_reports.count&&vm_read32(label)==0x01010101&&s_reports.published==2);

    /* 3. Early NV406E release: GET is past the RELEASE when the label lands. */
    u32 sem=GCM_LABEL_GUEST_BASE+0x920;raw32(sem,0xaaaaaaaa);
    { const u32 w[]={0x40064,0x920,0x4006c,0x0badf00d}; fifo(w,4); }
    assert(vm_read32(sem)==0x0badf00d&&s_reports.count==1&&completed<issued);
    completed=issued;cellGcm_rsx_process_fifo();assert(!s_reports.count);

    /* 4. Early GET_REPORT: the synthesized record lands at once, completion
     * word last, under the lock-line lock. */
    u32 rep=0x2000430;memset(vm_base+rep,0xA5,16);raw32(rep+12,0x00010000);
    { const u32 w[]={0x401A8,0xBAD68000,0x41800,0x01000430}; fifo(w,4); }
    assert(vm_read32(rep+8)==0xffff&&vm_read32(rep+12)==0&&vm_read64(rep)>0&&notified==1);
    assert(s_reports.count==1&&completed<issued);
    completed=issued;cellGcm_rsx_process_fifo();assert(!s_reports.count);

    /* 5. An observed progress word stays completion-gated, and a later
     * eligible release keeps stream order behind it. */
    u32 watched=VM_HLE_INJECT_BASE+0x930;raw32(watched,0);raw32(label,0);
    rsx_ring_progress_observer_set(watched,observe);
    { const u32 w[]={0x41d6c,0x930,0x41d74,0x77,0x41d6c,0x900,0x41d74,0x78}; fifo(w,8); }
    assert(vm_read32(watched)==0&&vm_read32(label)==0&&s_reports.gated==2);
    completed=issued;cellGcm_rsx_process_fifo();
    assert(vm_read32(watched)==0x77&&vm_read32(label)==0x78&&!s_reports.count);
    rsx_ring_progress_observer_set(0,NULL);

    /* 6. Checked SET_REFERENCE. Without early FIFO sync the reference waits
     * for a checked backend completion; with it, the recorded work is
     * committed without waiting and the reference queues immediately. */
    setenv("PS3RECOMP_GCM_CHECKED_REF","1",1);
    reset(1,0);
    { const u32 w[]={0x40050,0x101}; fifo(w,2); }
    assert(checked_flushes==1&&s_ref_qtail==1&&s_ref_q[0]==0x101);
    reset(1,1);
    { const u32 w[]={0x41d6c,0x900,0x41d74,0x99,0x40050,0x102}; fifo(w,6); }
    assert(!checked_flushes&&!blocking_waits&&s_ref_qtail==1&&s_ref_q[0]==0x102);
    assert(vm_read32(label)==0x99&&s_reports.count==2&&completed<issued&&!s_reports.unissued);
    assert(!atomic_load(&s_checked_ref_failed));
    completed=issued;cellGcm_rsx_process_fifo();assert(!s_reports.count);

    /* 7. NV406E ACQUIRE: the default barrier waits for the GPU; early FIFO
     * sync keeps only the ordering of unapplied records. */
    raw32(sem,0x5);
    reset(1,0);{ const u32 w[]={0x40064,0x920,0x40068,0x5}; fifo(w,4); }
    assert(checked_flushes==1);
    reset(1,1);{ const u32 w[]={0x40064,0x920,0x40068,0x5}; fifo(w,4); }
    assert(!checked_flushes&&!blocking_waits);

    /* 8. A backend failure discovered after early publication freezes later
     * publication and checked references; nothing is reported as complete. */
    reset(1,1);raw32(label,0);
    { const u32 w[]={0x41d6c,0x900,0x41d74,0x31}; fifo(w,4); }
    assert(vm_read32(label)==0x31);
    gpu_error=1;cellGcm_rsx_process_fifo();
    assert(s_reports.failed&&atomic_load(&s_checked_ref_failed));
    { const u32 w[]={0x41d6c,0x900,0x41d74,0x32,0x40050,0x103};
      for(u32 i=0;i<6;i++)raw32(0x100000+i*4,w[i]);
      cellGcm_rsx_consumer_begin();cellGcm_syscall_set_fifo_consumer_body(24,0);cellGcm_rsx_consumer_end();
      cellGcm_rsx_process_fifo(); }
    assert(vm_read32(label)==0x31&&s_ref_qtail==0);
    unsetenv("PS3RECOMP_GCM_CHECKED_REF");
    s_deferred_reports=0;s_early_release=s_early_fifo_sync=0;
    munmap(vm_base,0x100000000ULL);
    puts("PASS early label release: FIFO-position labels/reports, ordering, checked reference, acquire, sticky failure");
}
