/* Real cellGcm FIFO walker: an NV406E semaphore acquire guards a block that
 * its producer fills late (docs/RSX_SEMAPHORE_ACQUIRE.md). The block holds
 * zeros and a stale data word until the producer writes the real commands
 * and releases the label, which here happens only after the walker has
 * reached the acquire: the field case of a loaded host. No GPU, no guest
 * content. */
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
static unsigned locked;
void ppu_resv_break_store(uint64_t ea) { (void)ea; assert(locked); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a;(void)v;(void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; assert(locked); }
void spu_lockline_lock(void) { assert(!locked); locked=1; }
void spu_lockline_unlock(void) { assert(locked); locked=0; }
int rsx_draw_engine_enabled(void) { return 1; }
int rsx_live_draw_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 m, u32 v) { (void)m; (void)v; }
void rsx_draw_engine_method_at(u32 m,u32 v,u32 ea){(void)ea;rsx_draw_engine_method(m,v);}
void rsx_live_draw_method(u32 m, u32 v) { (void)m; (void)v; }
void rsx_draw_engine_flush(void) {}
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
/* The backend never completes on its own: every label below can only reach
 * guest memory through early release, at its FIFO position. */
static u64 issued, completed;
int rsx_draw_engine_async_available(void){return 1;}
u64 rsx_draw_engine_submit_async(void){return ++issued;}
int rsx_draw_engine_completion_status(u64 ticket,int wait){
    if(wait&&ticket>completed)completed=ticket;
    return ticket<=completed;
}
void rsx_draw_engine_set_report_barrier(int(*barrier)(void)){(void)barrier;}
int rsx_draw_engine_checked_ready(void) { return 1; }
int rsx_draw_engine_flush_checked(void) { completed=issued; return 1; }
int ps3_critical_enabled(void){return 0;}
uint64_t ps3_critical_now_ns(void){return 1;}
void ps3_critical_emit_capture(uint64_t c,uint32_t k,uint64_t o,uint64_t sub,const uint64_t*w,uint32_t n){
    (void)c;(void)k;(void)o;(void)sub;(void)w;(void)n;
}

#define IO_EA(io) (0x100000u+(io))
#define JUMP(io)  (0x20000000u|(io))
#define UNTOUCHED 0xa5a5a5a5u
void (*g_host_line_write_hook)(uint32_t line);
static void raw32(u32 ea,u32 v) { v=ps3_bswap32(v);memcpy(vm_base+ea,&v,4); }
static void put_words(u32 io,const u32* w,u32 n) { for(u32 i=0;i<n;i++)raw32(IO_EA(io+i*4),w[i]); }
static u32 label_ea(u32 off) { return VM_HLE_INJECT_BASE+off; }
static void reset(void) {
    memset(&s_reports,0,sizeof(s_reports));s_reports.generation=1;
    s_deferred_reports=1;s_early_release=1;s_early_fifo_sync=1;s_reports.early_mode=1;s_frames_ahead=0;
    issued=completed=0;s_acq_t0=0;atomic_store(&s_acq_wait_ea,0u);
    for(u32 off=0x800;off<0xC00;off+=0x10)raw32(label_ea(off),UNTOUCHED);
}
static void start(u32 put) {
    cellGcm_rsx_consumer_begin();cellGcm_syscall_set_fifo_consumer_body(put,0);cellGcm_rsx_consumer_end();
}
/* The stream: release 0x900 (held until the acquire), acquire label 0x940
 * == 1, then a block at 0x100 the producer has not written yet: zeros and a
 * stale float that decodes as a JUMP into unmapped IO. */
static void stream(void) {
    const u32 head[]={0x41d6c,0x900,0x41d74,0x11, 0x40064,0x940,0x40068,0x1, JUMP(0x100)};
    put_words(0x000,head,9);
    for(u32 io=0x100;io<0x200;io+=4)raw32(IO_EA(io),0);
    raw32(IO_EA(0x180),0x3F0E170Du);
    raw32(label_ea(0x940),0);
}
/* The producer, late: it writes the block's real commands, then releases. */
static void produce(void) {
    const u32 block[]={0x41d6c,0x980,0x41d74,0x22,JUMP(0x400)};
    put_words(0x100,block,5);
    atomic_thread_fence(memory_order_release);
    raw32(label_ea(0x940),1);
}

int main(void) {
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);
    ppu_hle_inject_base=0x3000000; ppu_vm_size=0x4000000;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));
    s_ea_address_table[0]=1;
    s_gcm_syscall_mode=1;s_config.localAddress=0;

    /* 1. Switch unset (the old path): the walker passes the unsatisfied
     * acquire, walks the unwritten block's zeros and derails on the stale
     * word. The producer's commands never run. */
    s_acq_block=-1;unsetenv("GCM_SEMA_ACQUIRE");
    reset();stream();start(0x400);
    cellGcm_rsx_process_fifo();
    produce();cellGcm_rsx_process_fifo();
    assert(vm_read32(label_ea(0x980))==UNTOUCHED);

    /* 2. GCM_SEMA_ACQUIRE=1: the walker stops on the acquire, across as many
     * passes as it takes, with GET on it; the held release before it has
     * landed. Once the producer releases, the real block runs. */
    s_acq_block=-1;setenv("GCM_SEMA_ACQUIRE","1",1);unsetenv("GCM_SEMA_ACQUIRE_MS");
    reset();stream();start(0x400);
    for(int i=0;i<5;i++){
        cellGcm_rsx_process_fifo();
        assert(s_fifo_getoff==0x018);                     /* parked on the acquire packet */
        assert(vm_read32(label_ea(0x900))==0x11);          /* held release landed first */
        assert(vm_read32(label_ea(0x980))==UNTOUCHED);
    }
    const unsigned long long waits=s_acq_waits;
    produce();cellGcm_rsx_process_fifo();
    assert(vm_read32(label_ea(0x980))==0x22&&s_fifo_getoff==0x400);
    assert(s_acq_waits==waits+1);                          /* the wait was reported */

    /* 3. An SPU transfer that writes the awaited label wakes the walker; one
     * that misses it does not. */
    reset();stream();start(0x400);
    cellGcm_rsx_process_fifo();
    assert(atomic_load(&s_acq_wait_ea)==label_ea(0x940));
    { const unsigned long long k=atomic_load(&s_acq_kicks);
      gcm_note_spu_put(label_ea(0x980),16);
      assert(atomic_load(&s_acq_kicks)==k);
      produce();gcm_note_spu_put(label_ea(0x940)&~127u,128);
      assert(atomic_load(&s_acq_kicks)==k+1&&!atomic_load(&s_acq_wait_ea)); }
    cellGcm_rsx_process_fifo();
    assert(vm_read32(label_ea(0x980))==0x22);

    /* 4. The bound: a label nobody writes is waited on for
     * GCM_SEMA_ACQUIRE_MS, then consumed as on the old path. */
    s_acq_block=-1;setenv("GCM_SEMA_ACQUIRE_MS","20",1);
    reset();stream();start(0x400);
    { const u32 fill[]={0x41d6c,0x980,0x41d74,0x22,JUMP(0x400)}; put_words(0x100,fill,5); } /* ready, label never set */
    const unsigned long long timeouts=s_acq_timeouts;
    cellGcm_rsx_process_fifo();assert(s_fifo_getoff==0x018);
    { struct timespec ts={0,40*1000*1000}; nanosleep(&ts,NULL); }
    cellGcm_rsx_process_fifo();
    assert(s_acq_timeouts==timeouts+1&&s_fifo_getoff==0x400&&vm_read32(label_ea(0x980))==0x22);

    /* 5. GCM_SEMA_ACQUIRE=0 is the old path too. */
    s_acq_block=-1;setenv("GCM_SEMA_ACQUIRE","0",1);
    reset();stream();start(0x400);
    cellGcm_rsx_process_fifo();
    assert(s_fifo_getoff!=0x018);
    unsetenv("GCM_SEMA_ACQUIRE");unsetenv("GCM_SEMA_ACQUIRE_MS");

    s_deferred_reports=0;s_early_release=s_early_fifo_sync=0;
    munmap(vm_base,0x100000000ULL);
    puts("PASS semaphore acquire: late producer derails without the wait, waits across passes with it, held releases first, SPU wake-up, bound falls back, =0 is the old path");
}
