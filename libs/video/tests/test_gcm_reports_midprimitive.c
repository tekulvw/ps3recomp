/* The deferred-report queue when a FIFO pass stops inside a primitive, and
 * what a failure does to publication. Real cellGcm FIFO drain, deferred
 * reports with early label release and early FIFO sync (MAG's defaults) and
 * checked REF; a synthetic engine that, like the real one, takes no ticket
 * between BEGIN and END. No GPU, no guest content.
 *
 * python3 tools/test_gcm_reports_midprimitive.py
 */
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
/* The primitive state as the engine keeps it: BEGIN_END with a primitive
 * opens one, with 0 closes it. */
static int in_prim;
void rsx_draw_engine_method(u32 m, u32 v) { if((m&0x1FFCu)==0x1808u)in_prim=v!=0; }
int rsx_draw_engine_in_primitive(void){return in_prim;}
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
static int gpu_error, refuse_once, status_error_once;
static u64 issued, completed, blocking_waits, refused;
int rsx_draw_engine_async_available(void){return 1;}
/* As the engine: no ticket inside a primitive (its draw is not recorded),
 * and a backend refusal on demand. */
u64 rsx_draw_engine_submit_async(void){
    if(in_prim){refused++;return 0;}
    if(refuse_once){refuse_once=0;refused++;return 0;}
    return ++issued;
}
int rsx_draw_engine_completion_status(u64 ticket,int wait){
    if(gpu_error)return -1;
    if(status_error_once){status_error_once=0;return -1;}
    if(wait&&ticket>completed){blocking_waits++;completed=ticket;}
    return ticket<=completed;
}
void rsx_draw_engine_set_report_barrier(int(*barrier)(void)){(void)barrier;}
int rsx_draw_engine_checked_ready(void) { return !gpu_error; }
static unsigned full_waits;
int rsx_draw_engine_flush_checked(void) { if(in_prim)return 0; checked_flushes++; full_waits++; completed=issued; return gpu_error?0:1; }
int ps3_critical_enabled(void){return 0;}
uint64_t ps3_critical_now_ns(void){return 1;}
void ps3_critical_emit_capture(uint64_t c,uint32_t k,uint64_t o,uint64_t sub,const uint64_t*w,uint32_t n){(void)c;(void)k;(void)o;(void)sub;(void)w;(void)n;}
static void raw32(u32 ea,u32 v) { v=ps3_bswap32(v);memcpy(vm_base+ea,&v,4); }
static u32 s_at;   /* where the next words go: the FIFO is written on, as a title does */
static void fifo(const u32* w,u32 n) {
    for(u32 i=0;i<n;i++)raw32(0x100000+s_at+i*4,w[i]);
    s_at+=n*4;
    cellGcm_rsx_consumer_begin();cellGcm_syscall_set_fifo_consumer_body(s_at,s_fifo_getoff);cellGcm_rsx_consumer_end();
    cellGcm_rsx_process_fifo();
}
static void observe(uint32_t k,uint32_t ea,uint32_t v,uint64_t t,uint64_t a){(void)k;(void)ea;(void)v;(void)t;(void)a;}
static void reset(void) {
    memset(&s_reports,0,sizeof(s_reports));s_reports.generation=3;
    s_deferred_reports=1;s_early_release=1;s_early_fifo_sync=1;s_reports.early_mode=1;
    issued=completed=blocking_waits=refused=0;flushed=checked_flushes=notified=full_waits=0;
    gpu_error=refuse_once=status_error_once=0;in_prim=0;
    atomic_store(&s_checked_ref_failed,0);s_ref_qhead=s_ref_qtail=0;
    s_at=0;s_fifo_getoff=0;vm_write32(GCM_CONTROL_GUEST_ADDR+4,0);
}
#define LABEL(off,v) 0x41d6c,(off),0x41d74,(v)
#define BEGIN(p)     0x41808,(p)
#define END          0x41808,0
int main(void) {
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);
    ppu_hle_inject_base=0x3000000; ppu_vm_size=0x4000000;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));
    s_ea_address_table[0]=1; s_ea_address_table[0xE0]=0x20;
    s_gcm_syscall_mode=1;s_config.localAddress=0;
    setenv("PS3RECOMP_GCM_CHECKED_REF","1",1);
    const u32 label=VM_HLE_INJECT_BASE+0x900, watched=VM_HLE_INJECT_BASE+0x930;

    /* 1. The wedge: labels queue, then put lands inside a primitive. The end
     * of the pass issues; the engine takes no ticket inside a primitive.
     * That used to fail the queue (issue-zero), freeze publication and
     * checked REF, and stop every later pass. Now the issue waits. */
    reset();raw32(label,0);
    { const u32 w[]={LABEL(0x900,0x31),LABEL(0x900,0x32),BEGIN(5)}; fifo(w,10); }
    assert(in_prim&&refused==0&&issued==0);                  /* nothing asked of the engine */
    assert(!s_reports.failed&&!atomic_load(&s_checked_ref_failed));
    assert(vm_read32(label)==0x32&&s_reports.unissued==2&&s_reports.issue_deferrals>=1);
    printf("[PASS] a pass that stops inside a primitive defers the issue: no failure, labels still land\n");
    /* The primitive ends and the stream goes on: the records get their
     * ticket, later labels land, and completion verifies them all. */
    { const u32 w[]={END,LABEL(0x900,0x33)}; fifo(w,6); }
    assert(!in_prim&&!s_reports.failed&&!s_reports.unissued&&issued>=1&&vm_read32(label)==0x33);
    completed=issued;{ const u32 w[]={LABEL(0x900,0x34)}; fifo(w,4); }
    completed=issued;cellGcm_rsx_process_fifo();
    assert(!s_reports.count&&s_reports.published>=4&&vm_read32(label)==0x34&&!atomic_load(&s_checked_ref_failed));
    printf("[PASS] after END the deferred records are ticketed and verified\n");

    /* 2. Degrade instead of wedging: a backend refusal outside a primitive
     * still fails the queue, but a full checked wait then publishes what was
     * queued, in order, and publication resumes. A completion-gated label
     * (an observed progress word) shows the queued record really lands. */
    reset();raw32(label,0);raw32(watched,0);
    rsx_ring_progress_observer_set(watched,observe);
    refuse_once=1;
    { const u32 w[]={LABEL(0x930,0x41),LABEL(0x900,0x42)}; fifo(w,8); }
    assert(refused==1&&full_waits==1&&s_reports.recoveries==1&&!s_reports.failed);
    assert(vm_read32(watched)==0x41&&vm_read32(label)==0x42&&!s_reports.count);
    assert(!atomic_load(&s_checked_ref_failed));
    { const u32 w[]={LABEL(0x930,0x43)}; fifo(w,4); }        /* later releases flow */
    completed=issued;cellGcm_rsx_process_fifo();
    assert(vm_read32(watched)==0x43&&!s_reports.count&&!s_reports.failed);
    rsx_ring_progress_observer_set(0,NULL);
    printf("[PASS] a failed queue recovers after a full GPU wait; labels keep flowing\n");

    /* 3. A failure seen inside a primitive: the full wait cannot run there,
     * so recovery waits for the primitive to end -- checked REF stays up and
     * the FIFO keeps moving to reach the END. */
    reset();raw32(label,0);raw32(watched,0);
    rsx_ring_progress_observer_set(watched,observe);
    { const u32 w[]={LABEL(0x930,0x51)}; fifo(w,4); }        /* issued, gated */
    assert(s_reports.count==1&&issued==1&&vm_read32(watched)==0);
    { const u32 w[]={BEGIN(5)}; fifo(w,2); }
    status_error_once=1;cellGcm_rsx_process_fifo();          /* its completion fails */
    assert(s_reports.failed&&in_prim&&!full_waits&&!atomic_load(&s_checked_ref_failed));
    /* The walker goes on to the END (a release after it stops it while the
     * queue is failed); the same pass then recovers. */
    { const u32 w[]={END,LABEL(0x900,0x52)}; fifo(w,6); }
    assert(!in_prim&&!s_reports.failed&&s_reports.recoveries==1&&vm_read32(watched)==0x51);
    cellGcm_rsx_process_fifo();                              /* and the release it stopped at lands */
    assert(s_fifo_getoff==s_at&&vm_read32(label)==0x52);
    { const u32 w[]={LABEL(0x900,0x53)}; fifo(w,4); }
    completed=issued;cellGcm_rsx_process_fifo();
    assert(vm_read32(label)==0x53&&!atomic_load(&s_checked_ref_failed));
    rsx_ring_progress_observer_set(0,NULL);
    printf("[PASS] a failure inside a primitive recovers once it ends; checked REF stays up\n");

    /* 4. A failed backend cannot be waited on: publication stays stopped and
     * checked REF fails, as before -- nothing is reported as complete. */
    reset();raw32(label,0);
    { const u32 w[]={LABEL(0x900,0x61)}; fifo(w,4); }
    gpu_error=1;cellGcm_rsx_process_fifo();
    assert(s_reports.failed&&!s_reports.recoveries&&atomic_load(&s_checked_ref_failed));
    printf("[PASS] a failed backend still stops publication and checked REF\n");
    unsetenv("PS3RECOMP_GCM_CHECKED_REF");
    printf("PASS reports inside a primitive and failure recovery\n");
    return 0;
}
