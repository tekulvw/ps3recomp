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
int rsx_process_method(rsx_state* s,u32 m,u32 v) { (void)s; if(m==0x50)g_rsx_last_reference=v;return 0; }
void rsx_raise_user_cmd(u32 arg) { (void)arg; }
void rsx_live_draw_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
int rsx_live_draw_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return 0; }
int rsx_live_draw_resolve_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h,u32 i,u32 j,u32 k,u32 l) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;return 0; }
void rsx_live_draw_note_resolve(u32 a,u32 b,u32 c,u32 d) { (void)a;(void)b;(void)c;(void)d; }
static u32 peek_ref(void) {u32 v;memcpy(&v,vm_base+GCM_CONTROL_GUEST_ADDR+8,4);return ps3_bswap32(v);}
static unsigned checked_calls; static int complete=1;
static pthread_mutex_t hold_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond=PTHREAD_COND_INITIALIZER;
static int held,pending,released;
int rsx_draw_engine_flush_checked(void) {
    checked_calls++;
    if(held){pthread_mutex_lock(&hold_mutex);pending=1;pthread_cond_broadcast(&hold_cond);while(!released)pthread_cond_wait(&hold_cond,&hold_mutex);pthread_mutex_unlock(&hold_mutex);}
    assert(peek_ref()==0xA5A5A5A5u);
    return complete;
}
static void raw32(u32 ea,u32 v) { v=ps3_bswap32(v);memcpy(vm_base+ea,&v,4); }
static void* drain_thread(void* unused){(void)unused;cellGcm_rsx_process_fifo();return NULL;}
int main(int argc,char**argv) {
    assert(argc==2);setenv("PS3RECOMP_GCM_CHECKED_REF","1",1);
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);ppu_vm_size=0x4000000;ppu_hle_inject_base=0x3000000;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));s_ea_address_table[0]=1;
    s_gcm_syscall_mode=1;raw32(GCM_CONTROL_GUEST_ADDR+8,0xA5A5A5A5u);
    if(!strcmp(argv[1],"held")) {
        held=1;raw32(0x100000,0x00040050);raw32(0x100004,0xCAFEBABE);cellGcm_syscall_set_fifo(8,0);
        pthread_t t;assert(!pthread_create(&t,NULL,drain_thread,NULL));
        pthread_mutex_lock(&hold_mutex);while(!pending)pthread_cond_wait(&hold_cond,&hold_mutex);
        assert(peek_ref()==0xA5A5A5A5u);assert(s_ref_qhead==s_ref_qtail);
        released=1;pthread_cond_broadcast(&hold_cond);pthread_mutex_unlock(&hold_mutex);
        assert(!pthread_join(t,NULL));assert(peek_ref()==0xCAFEBABE);
    } else if(!strcmp(argv[1],"park_patch") || !strcmp(argv[1],"park_branch") || !strcmp(argv[1],"park_wrap")) {
        /* A producer parks the GPU while its next segment is incomplete.
         * put can be ahead, or behind the park after a ring wrap. */
        int wrap=!strcmp(argv[1],"park_wrap");
        u32 park=wrap?0x100:0, put=wrap?8:16;
        raw32(0x100000+park,0x20000000u|park);
        raw32(0x100008,0x00040050);raw32(0x10000C,0xCAFEBABE);
        cellGcm_syscall_set_fifo(put,park);
        for(unsigned i=0;i<3;i++) {
            cellGcm_rsx_process_fifo();
            assert(s_fifo_getoff==park && !checked_calls);
            assert(peek_ref()==0xA5A5A5A5u);
            assert(s_ref_qhead==s_ref_qtail);
            assert(!atomic_load(&s_checked_ref_failed));
            assert(cellGcm_checked_reference_ready());
        }
        if(!strcmp(argv[1],"park_patch")) {
            raw32(0x100000,0x00040050);raw32(0x100004,0xCAFEBABE);
            cellGcm_syscall_set_fifo(8,park);
        } else if(wrap) {
            raw32(0x100000,0x00040050);raw32(0x100004,0xCAFEBABE);
            raw32(0x100000+park,0x20000000u);
        } else raw32(0x100000,0x20000008u);
        cellGcm_rsx_process_fifo();
        assert(s_fifo_getoff==(wrap||!strcmp(argv[1],"park_patch")?8:16));
        assert(checked_calls==1 && peek_ref()==0xCAFEBABE);
        assert(s_checked_ref_receipt==1 && !atomic_load(&s_checked_ref_failed));
    } else if(!strcmp(argv[1],"repeat_wrap")) {
        u32 values[]={0xFFFFFFFFu,0xFFFFFFFFu,0,1};
        for(unsigned i=0;i<4;i++){raw32(0x100000+8*i,0x00040050);raw32(0x100004+8*i,values[i]);}
        cellGcm_syscall_set_fifo(32,0);cellGcm_rsx_process_fifo();assert(checked_calls==4);
        for(unsigned i=0;i<4;i++){assert(peek_ref()==values[i]);assert(s_ref_receipts[i]==i+1);gcm_ref_publish_one();}
    } else if(!strcmp(argv[1],"malformed")) {
        raw32(0x100000,0x00080050);raw32(0x100004,0xCAFEBABE);raw32(0x100008,0);
        cellGcm_syscall_set_fifo(12,0);cellGcm_rsx_process_fifo();assert(!checked_calls);assert(peek_ref()==0xA5A5A5A5u);
    } else if(!strcmp(argv[1],"rescue")) {
        raw32(0x100000,0x00040050);raw32(0x100004,0xCAFEBABE);
        u32 get=0;gcm_fifo_resync_why("test-skipped",&get,8);gcm_ref_publish_one();
        assert(peek_ref()==0xA5A5A5A5u);assert(!checked_calls);
    } else if(!strcmp(argv[1],"incomplete")) {
        raw32(0x100000,0x00040050);raw32(0x100004,0xCAFEBABE);
        raw32(0x100008,0x20000004);
        cellGcm_syscall_set_fifo(4,0);cellGcm_rsx_process_fifo();
        assert(!checked_calls);assert(peek_ref()==0xA5A5A5A5u);
    } else if(!strcmp(argv[1],"overflow")) {
        for(unsigned i=0;i<GCM_REF_QLEN+1;i++)gcm_ref_push_at(i,i*8);
        gcm_ref_publish_one();assert(peek_ref()==0xA5A5A5A5u);
    } else {
        if(!strcmp(argv[1],"failure")) complete=0;
        raw32(0x100000,0x00040050);raw32(0x100004,0xCAFEBABE);
        cellGcm_syscall_set_fifo(8,0);cellGcm_rsx_process_fifo();
        assert(checked_calls==1);
        gcm_ref_publish_one();
        assert(peek_ref()==(complete?0xCAFEBABE:0xA5A5A5A5u));
    }
    puts("checked reference scenario passed");munmap(vm_base,0x100000000ULL);return 0;
}

int rsx_draw_engine_checked_ready(void) { return 1; }

/* Backend boundary required by the production guest-memory transfer path. */
void rsx_draw_engine_mark_guest_dirty(u32 ea,u32 len) { (void)ea; (void)len; }
