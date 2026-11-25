/* Real cellGcm FIFO walker: a label release must not become guest-visible
 * before the walker has fetched past the control transfer that follows it
 * (docs/RSX_RELEASE_AFTER_FETCH.md). A synthetic producer reacts to a label
 * the instant it is written -- the worst case of a walker thread that loses
 * its CPU right after publishing -- and recycles the segment that holds the
 * release, branch included. No GPU, no guest content. */
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
#define CALL(io)  ((io)|2u)
#define RET       0x00020000u
#define UNTOUCHED 0xa5a5a5a5u
static void raw32(u32 ea,u32 v) { v=ps3_bswap32(v);memcpy(vm_base+ea,&v,4); }
static void put_words(u32 io,const u32* w,u32 n) { for(u32 i=0;i<n;i++)raw32(IO_EA(io+i*4),w[i]); }
static u32 label_ea(u32 off) { return VM_HLE_INJECT_BASE+off; }

/* The producer: it waits on one label and, the moment that label holds the
 * value it waits for, recycles the segment the release sat in -- here by
 * re-pointing the segment's closing branch at a block it has not written
 * yet. It runs from the label's publication itself (the SPURS line-commit
 * seam), so no host scheduling can put the walker's next fetch first. */
static u32 producer_label, producer_value, producer_patch_io, producer_patch_word;
static unsigned producer_runs;
static u32 producer_seen_get;
static u32 producer_clear_from, producer_clear_to;
void (*g_host_line_write_hook)(uint32_t line);
static void producer(uint32_t line) {
    if(!producer_label||producer_runs||line!=(producer_label&~127u))return;   /* it recycles once */
    if(vm_read32(producer_label)!=producer_value)return;
    producer_runs++;
    producer_seen_get=vm_read32(GCM_CONTROL_GUEST_ADDR+4);
    if(producer_clear_to){
        /* Recycle the whole block: clear it, then start filling it with
         * data (a float that decodes as a JUMP into unmapped IO). */
        for(u32 io=producer_clear_from;io<producer_clear_to;io+=4)raw32(IO_EA(io),0);
        raw32(IO_EA(producer_clear_to-4),0x3F7FFFFFu);
        return;
    }
    raw32(IO_EA(producer_patch_io),producer_patch_word);
}

static void reset(void) {
    memset(&s_reports,0,sizeof(s_reports));s_reports.generation=1;
    s_deferred_reports=1;s_early_release=1;s_early_fifo_sync=1;s_reports.early_mode=1;s_frames_ahead=0;
    issued=completed=0;producer_label=0;producer_runs=0;producer_seen_get=0;
    for(u32 off=0x800;off<0xC00;off+=0x10)raw32(label_ea(off),UNTOUCHED);
}
static void run(u32 put) {
    cellGcm_rsx_consumer_begin();cellGcm_syscall_set_fifo_consumer_body(put,0);cellGcm_rsx_consumer_end();
    cellGcm_rsx_process_fifo();
}

int main(void) {
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);
    ppu_hle_inject_base=0x3000000; ppu_vm_size=0x4000000;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));
    s_ea_address_table[0]=1;s_ea_address_table[1]=2;
    s_gcm_syscall_mode=1;s_config.localAddress=0;
    setenv("PS3RECOMP_RSX_REPORT_SPURS_KICK","1",1);
    g_host_line_write_hook=producer;

    /* 1. A segment that ends in a backend release and a JUMP out of it. The
     * producer recycles the segment on that release and re-points the JUMP
     * at a block it has not filled. The walker must already have taken the
     * JUMP it was given: segment A runs, the unfilled block does not. */
    reset();
    { const u32 seg[]={0x41d6c,0x900,0x41d74,0x11,JUMP(0x100)}; put_words(0x000,seg,5); }
    { const u32 a[]={0x41d6c,0x980,0x41d74,0x22,JUMP(0x400)};   put_words(0x100,a,5); }
    { const u32 junk[]={0x41d6c,0xA00,0x41d74,0x33,JUMP(0x400)};put_words(0x200,junk,5); }
    producer_label=label_ea(0x900);producer_value=0x11;producer_patch_io=0x010;producer_patch_word=JUMP(0x200);
    run(0x400);
    assert(producer_runs==1);
    assert(vm_read32(label_ea(0x900))==0x11);
    assert(vm_read32(label_ea(0x980))==0x22);
    assert(vm_read32(label_ea(0xA00))==UNTOUCHED);
    /* GET already leads the release when the producer sees it. */
    assert(producer_seen_get==0x100);

    /* 1b. The release sits early in a long segment. On the label the
     * producer clears the whole segment and starts writing data into it, so
     * every word the walker had not fetched yet is gone. The walker must
     * still execute the original tail: its closing release lands, and no
     * data word is decoded as a command. */
    reset();
    { const u32 head[]={0x41d6c,0x900,0x41d74,0x11}; put_words(0x000,head,4); }
    for(u32 io=0x010;io<0x400;io+=8){ const u32 nop2[]={0x40100,0}; put_words(io,nop2,2); }
    { const u32 tail[]={0x41d6c,0x980,0x41d74,0x22,JUMP(0x800)}; put_words(0x400,tail,5); }
    producer_label=label_ea(0x900);producer_value=0x11;producer_patch_io=0;producer_patch_word=0;
    producer_clear_from=0x000;producer_clear_to=0x414;
    run(0x800);
    assert(producer_runs==1&&vm_read32(label_ea(0x980))==0x22&&s_fifo_getoff==0x800);
    producer_clear_to=0;

    /* 1c. A segment that releases thousands of times before its branch (a
     * release per draw) is held as a whole: none of it lands, and nothing
     * is forced, until the JUMP is taken. */
    reset();
    { u32 io=0x1000;
      for(u32 i=0;i<3000;i++){ const u32 rel[]={0x41d6c,0x900+(i&1)*0x80,0x41d74,i+1}; put_words(io,rel,4); io+=16; }
      raw32(IO_EA(io),JUMP(0x100)); producer_patch_io=io; }
    { const u32 a[]={0x41d6c,0xA80,0x41d74,0x22,JUMP(0x400)};   put_words(0x100,a,5); }
    { const u32 junk[]={0x41d6c,0xA00,0x41d74,0x33,JUMP(0x400)};put_words(0x200,junk,5); }
    producer_label=label_ea(0x900);producer_value=1;producer_patch_word=JUMP(0x200);
    { const unsigned long long forced=s_release_hold_forced;
      cellGcm_rsx_consumer_begin();cellGcm_syscall_set_fifo_consumer_body(0x400,0x1000);cellGcm_rsx_consumer_end();
      cellGcm_rsx_process_fifo();
      assert(s_release_hold_forced==forced); }
    assert(producer_runs==1&&vm_read32(label_ea(0xA80))==0x22&&vm_read32(label_ea(0xA00))==UNTOUCHED);
    assert(vm_read32(label_ea(0x900))==2999&&vm_read32(label_ea(0x980))==3000);

    /* 2. The same with NV406E (0x6C) releases and a CALL/RET pair: the
     * release before the CALL is not visible until the CALL is taken, and
     * the one inside the called block not until the RET is. */
    reset();
    { const u32 seg[]={0x40064,0x910,0x4006c,0x44,CALL(0x100),0x40064,0x990,0x4006c,0x55}; put_words(0x000,seg,9); }
    { const u32 sub[]={0x40064,0x920,0x4006c,0x66,RET};   put_words(0x100,sub,5); }
    { const u32 junk[]={0x40064,0xA10,0x4006c,0x77,RET}; put_words(0x200,junk,5); }
    producer_label=GCM_LABEL_GUEST_BASE+0x910;producer_value=0x44;producer_patch_io=0x010;producer_patch_word=CALL(0x200);
    run(0x024);
    assert(producer_runs==1);
    assert(vm_read32(label_ea(0x920))==0x66&&vm_read32(label_ea(0x990))==0x55);
    assert(vm_read32(label_ea(0xA10))==UNTOUCHED);
    reset();
    { const u32 seg[]={CALL(0x100),0x40064,0x990,0x4006c,0x55}; put_words(0x000,seg,5); }
    { const u32 sub[]={0x40064,0x920,0x4006c,0x66,RET};   put_words(0x100,sub,5); }
    /* The producer replaces the RET with a branch into its unfilled block. */
    { const u32 junk[]={0x40064,0xA10,0x4006c,0x77,JUMP(0x014)}; put_words(0x200,junk,5); }
    producer_label=GCM_LABEL_GUEST_BASE+0x920;producer_value=0x66;producer_patch_io=0x110;producer_patch_word=JUMP(0x200);
    run(0x014);
    assert(producer_runs==1&&vm_read32(label_ea(0x990))==0x55&&vm_read32(label_ea(0xA10))==UNTOUCHED);

    /* 3. Visibility is held, never lost or reordered: a pass that ends on
     * `put` right after a release publishes it, and releases keep stream
     * order (the second write to one label wins). */
    reset();
    { const u32 seg[]={0x41d6c,0x900,0x41d74,0x1,0x41d6c,0x900,0x41d74,0x2,0x41d6c,0x940,0x41d74,0x3}; put_words(0x000,seg,12); }
    run(0x030);
    assert(vm_read32(label_ea(0x900))==0x2&&vm_read32(label_ea(0x940))==0x3);

    /* 4. An ACQUIRE reads label memory, so held releases land before it is
     * evaluated: a stream that releases and then acquires its own label must
     * not wait on itself. */
    setenv("GCM_SEMA_ACQUIRE","1",1);
    reset();
    { const u32 seg[]={0x40064,0x930,0x4006c,0x5,0x40068,0x5,0x40064,0x9a0,0x4006c,0x6}; put_words(0x000,seg,10); }
    run(0x028);
    assert(vm_read32(label_ea(0x9a0))==0x6&&s_fifo_getoff==0x028);
    unsetenv("GCM_SEMA_ACQUIRE");

    /* 5. A long branch-free stretch does not hold a release for ever: once
     * the walker has fetched GCM_RELEASE_FETCH_WINDOW bytes past it, it is
     * published even though no branch has come. */
    reset();
    { const u32 rel[]={0x41d6c,0x950,0x41d74,0x7}; put_words(0x000,rel,4); }
    for(u32 io=0x010;io<0x010+GCM_RELEASE_FETCH_WINDOW+0x20;io+=4)raw32(IO_EA(io),0); /* NOPs */
    producer_label=label_ea(0x950);producer_value=0x7;producer_patch_io=0x0FFF0;producer_patch_word=0;
    s_release_hold_published_at=0;
    run(0x010+GCM_RELEASE_FETCH_WINDOW+0x20);
    assert(producer_runs==1&&vm_read32(label_ea(0x950))==0x7);
    assert(s_release_hold_published_at>=GCM_RELEASE_FETCH_WINDOW&&s_release_hold_published_at<0x010+GCM_RELEASE_FETCH_WINDOW+0x20);

    /* 6. PS3RECOMP_GCM_RELEASE_AFTER_FETCH=0 restores publication at the
     * release itself; the producer then wins the race, as before. */
    s_release_after_fetch=0;
    reset();
    { const u32 seg[]={0x41d6c,0x900,0x41d74,0x11,JUMP(0x100)}; put_words(0x000,seg,5); }
    { const u32 a[]={0x41d6c,0x980,0x41d74,0x22,JUMP(0x400)};   put_words(0x100,a,5); }
    { const u32 junk[]={0x41d6c,0xA00,0x41d74,0x33,JUMP(0x400)};put_words(0x200,junk,5); }
    producer_label=label_ea(0x900);producer_value=0x11;producer_patch_io=0x010;producer_patch_word=JUMP(0x200);
    run(0x400);
    assert(producer_runs==1&&vm_read32(label_ea(0xA00))==0x33&&vm_read32(label_ea(0x980))==UNTOUCHED);
    s_release_after_fetch=-1;

    s_deferred_reports=0;s_early_release=s_early_fifo_sync=0;
    munmap(vm_base,0x100000000ULL);
    puts("PASS release after fetch: segment-tail JUMP, CALL and RET recycled on their release, cleared segment, thousands of releases before a branch, stream order, acquire, fetch window, switch off");
}
