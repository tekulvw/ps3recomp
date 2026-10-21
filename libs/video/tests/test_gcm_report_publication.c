/* Real cellGcm FIFO drain; backend calls record ordering without a GPU. */
#include "../cellGcmSys.c"
#include <assert.h>
#include <sys/mman.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active = 1;
uint32_t g_ww_lo, g_ww_hi;
ps3_guest_caller_fn g_ps3_guest_caller;
u32 g_rsx_last_reference;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_dump_guest_stack(ppu_context* c,const char* t) {(void)c;(void)t;}
void ppu_dump_bctrl_ring(uint32_t id,const char* t) {(void)id;(void)t;}
static unsigned locked, notified, broken, flushed, live_flushed, checked_flushes;
static u32 watched = 0x2000430;
static unsigned watch_writes;
static int expect_publication = 1;
void ppu_resv_break_store(uint64_t ea) { if (ea >= watched && ea < (u64)watched + 16) { assert(locked); broken++; } }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) {
    assert(locked && flushed && live_flushed && w==4);
    assert(a==watched+watch_writes*4 && watch_writes<4);
    assert(vm_read32(watched+12)==0x00010000); /* Completion still pending. */
    if (watch_writes==3) assert(v==0 && vm_read64(watched)>0);
    watch_writes++;
}
int spu_coh_is_reserved(uint32_t a) { return a / 128 == watched / 128; }
void spu_coh_notify_write(uint32_t a) { if (a / 128 == watched / 128) { assert(locked && flushed && live_flushed && expect_publication); assert(vm_read32(watched+12)==0); notified++; } }
void spu_lockline_lock(void) { assert(!locked); locked=1; }
void spu_lockline_unlock(void) { assert(locked); locked=0; }
int rsx_draw_engine_enabled(void) { return 1; }
static int fixture_live=1;
int rsx_live_draw_enabled(void) { return fixture_live; }
void rsx_draw_engine_method(u32 m, u32 v) { (void)m; (void)v; }
void rsx_draw_engine_method_at(u32 m,u32 v,u32 ea){(void)ea;rsx_draw_engine_method(m,v);}
void rsx_live_draw_method(u32 m, u32 v) { (void)m; (void)v; }
void rsx_draw_engine_flush(void) { assert(!locked); assert(vm_read32(watched+12)==0x00010000); flushed++; }
void rsx_live_draw_flush(void) { assert(!locked); assert(vm_read32(watched+12)==0x00010000); live_flushed++; }
void rsx_state_init(rsx_state* s) { memset(s,0,sizeof(*s)); }
int rsx_process_method(rsx_state* s,u32 m,u32 v) { (void)s; (void)m; (void)v; return 0; }
void rsx_raise_user_cmd(u32 arg) { (void)arg; }
void rsx_live_draw_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
int rsx_live_draw_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return 0; }
int rsx_live_draw_resolve_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h,u32 i,u32 j,u32 k,u32 l) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;return 0; }
void rsx_live_draw_note_resolve(u32 a,u32 b,u32 c,u32 d) { (void)a;(void)b;(void)c;(void)d; }
static void raw32(u32 ea,u32 v) { v=ps3_bswap32(v);memcpy(vm_base+ea,&v,4); }
static void run_report(u32 context,u32 offset) {
    raw32(0x100000,0x401A8); raw32(0x100004,context);
    raw32(0x100008,0x41800); raw32(0x10000C,0x01000000|offset);
    cellGcm_syscall_set_fifo(16,0); cellGcm_rsx_process_fifo();
    assert(vm_read32(GCM_CONTROL_GUEST_ADDR+4)==16);
}
static void prepare(u32 address) {
    watched=address; flushed=live_flushed=notified=broken=watch_writes=0;
    expect_publication=1;
    memset(vm_base+address,0xA5,16); raw32(address+12,0x00010000);
    g_ww_lo=address; g_ww_hi=address+16; /* High final record wraps; omit its watch. */
}
static void check_report(u32 expected) {
    assert(vm_read32(watched+12)==0);
    assert(vm_read32(watched+8)==expected && vm_read64(watched)>0);
    assert(notified==1 && broken==4 && flushed==1 && live_flushed==1 && !locked);
    assert(watch_writes==(g_ww_hi ? 4 : 0));
}
static void reject(u32 context,u32 offset) {
    prepare(0x2000430);
    u8 before[16]; memcpy(before,vm_base+watched,16);
    run_report(context,offset);
    assert(memcmp(before,vm_base+watched,16)==0);
    assert(!flushed && !live_flushed && !notified && !broken && !watch_writes);
}
static u64 fixture_issued,fixture_completed;
static int fixture_gpu_error;
int rsx_draw_engine_async_available(void){return s_deferred_reports;}
u64 rsx_draw_engine_submit_async(void){return ++fixture_issued;}
int rsx_draw_engine_completion_status(u64 ticket,int wait){(void)wait;return fixture_gpu_error?-1:ticket<=fixture_completed;}
void rsx_draw_engine_set_report_barrier(int(*barrier)(void)){(void)barrier;}
int main(void) {
    /* Sparse 4-GiB mapping also exercises the final valid guest record. */
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);
    ppu_hle_inject_base=0x3000000; ppu_vm_size=0x4000000;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));
    s_ea_address_table[0]=1; s_ea_address_table[0xE0]=0x20;
    s_gcm_syscall_mode=1;
    memset(vm_base+0x2000420,0xA5,48); prepare(0x2000430);
    run_report(0xBAD68000,0x430);
    check_report(0xFFFF);
    assert(vm_read64(watched)==s_report_data[0x43].timestamp);
    for(unsigned i=0;i<16;i++) assert(vm_base[watched-16+i]==0xA5 && vm_base[watched+16+i]==0xA5);
    /* Workload-flag completion record in the same mapped region. */
    prepare(0x2000560); run_report(0xBAD68000,0x560); check_report(0xFFFF);
    /* HOST_BUFFER must use raw IO, not the dedicated report-main window. */
    prepare(0x100430); run_report(0xFEED0001,0x430); check_report(0xFFFF);
    assert(vm_read32(0x200043C)==0);
    /* Context persists across drains; unrelated context register leaves it. */
    prepare(0x100560);
    raw32(0x100000,0x40194); raw32(0x100004,0xBAD68000);
    raw32(0x100008,0x41800); raw32(0x10000C,0x01000560);
    cellGcm_syscall_set_fifo(16,0); cellGcm_rsx_process_fifo(); check_report(0xFFFF);
    /* Local report storage remains host-only; no invented guest window. */
    prepare(0x2000430); expect_publication=0;
    run_report(0x66626660,0x430);
    assert(vm_read32(watched+12)==0x00010000 && !notified && !broken && !watch_writes);
    assert(flushed==1 && live_flushed==1 && s_report_data[0x43].value==0xFFFF);
    reject(0xDEADBEEF,0x430); reject(0xBAD68000,0x431);
    /* Upstream accepts the generic local DMA handle as a local-report alias.
     * It must never invent a guest report-memory window. */
    prepare(0x2000430);expect_publication=0;
    u64 local_before=s_report_data[0x43].timestamp;
    run_report(0xFEED0000,0x430);
    assert(s_report_dma==GCM_REPORT_DMA_LOCAL);
    assert(vm_read32(watched+12)==0x10000&&!notified&&!broken&&!watch_writes);
    assert(flushed==1&&live_flushed==1&&s_report_data[0x43].value==0xffff);
    assert(s_report_data[0x43].timestamp>local_before);
    reject(0xBAD68000,0x100000); /* Next IO page is unmapped. */
    s_ea_address_table[0xE1]=0x1000; reject(0xBAD68000,0x100000); /* Invalid EA page. */
    s_ea_address_table[0xE0]=0x40; reject(0xBAD68000,0x430); /* Beyond VM size. */
    s_ea_address_table[0xE0]=0x20;
    ppu_vm_size=0x200043F; reject(0xBAD68000,0x430); /* Truncated record. */
    ppu_vm_size=0x4000000;
    /* Guest DMA accepts the full 24-bit offset independently of host cache. */
    s_ea_address_table[0xE1]=0x21; prepare(0x2100000);
    run_report(0xBAD68000,0x100000); check_report(0xFFFF);
    /* VM size zero means 4 GiB, and EA+16 must not wrap in validation. */
    ppu_vm_size=0; s_ea_address_table[0xEF]=0xFFF;
    prepare(0xFFFFFFF0); run_report(0xBAD68000,0xFFFFF0); check_report(0xFFFF);
    /* Last valid record of the first mapped page. */
    prepare(0x20FFFF0); run_report(0xBAD68000,0xFFFF0); check_report(0xFFFF);
    /* A valid mapping to guest page zero must not be mistaken for failure. */
    s_ea_address_table[0xE0]=0; prepare(0);
    run_report(0xBAD68000,0); check_report(0xFFFF);
    /* The actual deferred GCM path leaves completion pending until a genuine
     * backend receipt, including an otherwise empty FIFO drain. */
    s_deferred_reports=1;fixture_live=0;g_ww_lo=g_ww_hi=0;
    s_report_dma=GCM_REPORT_DMA_MAIN;s_ea_address_table[0xe0]=0x20;
    memset(&s_reports,0,sizeof(s_reports));s_reports.generation=7;
    watched=0x2000430;prepare(watched);g_ww_lo=g_ww_hi=0;
    flushed=live_flushed=1; /* Existing publication assertions remain active. */
    cellGcm_rsx_consumer_begin();gcm_get_report(0x01000430);cellGcm_rsx_consumer_end();
    assert(s_reports.count==1&&s_reports.unissued==1&&!notified);
    u64 before_issue=fixture_issued;
    (void)cellGcmGetReport(1,0x43);(void)cellGcmGetTimeStamp(0x43);
    (void)cellGcmGetReportDataAddress(0x43);
    assert(fixture_issued==before_issue&&s_reports.unissued==1);
    assert(vm_read32(watched+12)==0x10000);
    assert(!gcm_report_vm_begin(watched+4,4)); /* No mutation lease granted. */
    cellGcm_rsx_process_fifo();
    assert(s_reports.count==1&&!s_reports.unissued&&!notified);
    assert(vm_read32(watched+12)==0x10000);
    fixture_completed=fixture_issued;
    cellGcm_rsx_process_fifo();
    assert(!s_reports.count&&s_reports.published==1&&notified==1);
    assert(vm_read32(watched+8)==0xffff&&vm_read32(watched+12)==0);
    assert(gcm_report_vm_begin(watched,16));cellGcm_rsx_consumer_end();
    /* Actual FIFO NV4097 release keeps the original byte swap, but leaves
     * the label untouched until its genuine command-buffer receipt. */
    u32 label_ea=VM_HLE_INJECT_BASE+0x900;raw32(label_ea,0xa5a5a5a5);
    raw32(0x100000,(2u<<18)|0x1d6cu);raw32(0x100004,0x900);raw32(0x100008,0x11223344);
    cellGcm_syscall_set_fifo(12,0);cellGcm_rsx_process_fifo();
    assert(vm_read32(label_ea)==0xa5a5a5a5&&s_reports.count==1);
    assert(rsx_reports_overlap(&s_reports,label_ea+3,1)&&!rsx_reports_overlap(&s_reports,label_ea+4,1));
    fixture_completed=fixture_issued;cellGcm_rsx_process_fifo();
    assert(vm_read32(label_ea)==0x11443322&&!s_reports.count);
    /* NV406E release publishes the raw value using the same ordered queue. */
    label_ea=GCM_LABEL_GUEST_BASE+0x920;raw32(label_ea,0xaaaaaaaa);
    raw32(0x100000,0x40064);raw32(0x100004,0x920);
    raw32(0x100008,0x4006c);raw32(0x10000c,0x55667788);
    cellGcm_syscall_set_fifo(16,0);cellGcm_rsx_process_fifo();
    assert(vm_read32(label_ea)==0xaaaaaaaa&&s_reports.count==1);
    fixture_completed=fixture_issued;cellGcm_rsx_process_fifo();
    assert(vm_read32(label_ea)==0x55667788&&!s_reports.count);
    /* NV308A inline data lands at its stream position after an earlier
     * deferred NV406E release has published. The default barrier also drains
     * GPU work once; the opt-in shortcut keeps only that report ordering. */
    s_config.localAddress=0;
    for(int mode=0;mode<2;mode++){
        s_inline_no_gpu_wait=mode;
        u32 dst=0x2200000+(u32)mode*0x1000;raw32(dst,0);raw32(dst+4,0);
        label_ea=GCM_LABEL_GUEST_BASE+0x940;raw32(label_ea,0xaaaaaaaa);
        const u32 w[]={0x40064,0x940,0x4006c,0x01020304,
            (1u<<18)|(2u<<13)|0x184u,0xfeed0000,(1u<<18)|(2u<<13)|0x30cu,dst,
            (1u<<18)|(2u<<13)|0x304u,0x01000000,(1u<<18)|(4u<<13)|0x304u,0,
            (2u<<18)|(4u<<13)|0x400u,0xcafef00d,0x12345678};
        for(unsigned i=0;i<sizeof w/sizeof w[0];i++)raw32(0x100000+i*4,w[i]);
        fixture_completed=UINT64_MAX;cellGcm_syscall_set_fifo((u32)sizeof w,0);
        unsigned before=checked_flushes;cellGcm_rsx_process_fifo();
        assert(vm_read32(label_ea)==0x01020304&&!s_reports.count);
        assert(vm_read32(dst)==0xcafef00d&&vm_read32(dst+4)==0x12345678);
        assert(checked_flushes-before==(mode?0u:1u));
    }
    s_inline_no_gpu_wait=0;fixture_completed=fixture_issued;
    /* GPU error is sticky and never publishes success or grants a VM lease. */
    raw32(watched+12,0x10000);
    cellGcm_rsx_consumer_begin();gcm_get_report(0x01000430);cellGcm_rsx_consumer_end();
    fixture_gpu_error=1;cellGcm_rsx_process_fifo();
    assert(s_reports.failed&&vm_read32(watched+12)==0x10000&&notified==1);
    assert(!gcm_report_vm_begin(0,UINT64_C(0x100000000)));
    s_deferred_reports=0;
    munmap(vm_base,0x100000000ULL); puts("GCM guest report publication checks passed");
}

/* Upstream renderer hooks are outside this focused FIFO fixture. */
void rsx_draw_engine_guest_read(u32 a,u32 b,u32 c,u32 d,u32 e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void rsx_draw_engine_guest_wrote(u32 a,u32 b,u32 c,u32 d,u32 e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void rsx_draw_engine_note_flip(u32 a) {(void)a;}
int rsx_draw_engine_checked_ready(void) { return 1; }
int rsx_draw_engine_flush_checked(void) { checked_flushes++; return fixture_gpu_error?0:1; }

/* Backend boundary required by the production guest-memory transfer path. */
void rsx_draw_engine_mark_guest_dirty(u32 ea,u32 len) { (void)ea; (void)len; }
