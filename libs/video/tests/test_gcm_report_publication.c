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
static unsigned locked, notified, broken, flushed, live_flushed;
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
int rsx_live_draw_enabled(void) { return 1; }
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
    reject(0xFEED0000,0x430); /* Generic VRAM context is not a report context. */
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
    munmap(vm_base,0x100000000ULL); puts("GCM guest report publication checks passed");
}

int rsx_draw_engine_checked_ready(void) { return 1; }
int rsx_draw_engine_flush_checked(void) { return 0; }
