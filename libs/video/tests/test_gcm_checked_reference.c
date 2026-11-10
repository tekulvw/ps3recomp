/* Real cellGcm FIFO drain; backend calls record ordering without a GPU. */
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <time.h>
#define CLOCK_MONOTONIC 1
static int clock_gettime(int clock_id,struct timespec* out) {
    (void)clock_id;LARGE_INTEGER now,frequency;
    if(!QueryPerformanceCounter(&now)||!QueryPerformanceFrequency(&frequency))return -1;
    out->tv_sec=(time_t)(now.QuadPart/frequency.QuadPart);
    out->tv_nsec=(long)((now.QuadPart%frequency.QuadPart)*1000000000/frequency.QuadPart);return 0;
}
#endif
#include "../cellGcmSys.c"
#include <assert.h>
#ifdef _WIN32
#include <stdlib.h>
#define setenv(key,value,overwrite) ((void)(overwrite),_putenv_s(key,value))
#define MAP_FAILED ((void*)-1)
static void* fixture_map(void) {
    void* memory=VirtualAlloc(NULL,UINT64_C(0x100000000),MEM_RESERVE,PAGE_READWRITE);
    if(!memory || !VirtualAlloc(memory,0x4000000,MEM_COMMIT,PAGE_READWRITE)) return MAP_FAILED;
    return memory;
}
#define mmap(...) fixture_map()
#define munmap(address,size) ((void)(size),VirtualFree(address,0,MEM_RELEASE)?0:-1)
typedef HANDLE pthread_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT
static int pthread_mutex_lock(pthread_mutex_t* p){AcquireSRWLockExclusive(p);return 0;}
static int pthread_mutex_unlock(pthread_mutex_t* p){ReleaseSRWLockExclusive(p);return 0;}
static int pthread_cond_wait(pthread_cond_t* c,pthread_mutex_t* m){return SleepConditionVariableSRW(c,m,INFINITE,0)?0:1;}
static int pthread_cond_broadcast(pthread_cond_t* c){WakeAllConditionVariable(c);return 0;}
struct fixture_start {void*(*entry)(void*);void* argument;};
static DWORD WINAPI fixture_thread(void* value){struct fixture_start copy=*(struct fixture_start*)value;free(value);copy.entry(copy.argument);return 0;}
static int pthread_create(pthread_t* t,void* attributes,void*(*entry)(void*),void* arg){
    (void)attributes;struct fixture_start* p=malloc(sizeof(*p));if(!p)return 1;p->entry=entry;p->argument=arg;
    *t=CreateThread(NULL,0,fixture_thread,p,0,NULL);if(!*t){free(p);return 1;}return 0;
}
static int pthread_join(pthread_t t,void* status){(void)status;DWORD result=WaitForSingleObject(t,INFINITE);CloseHandle(t);return result==WAIT_OBJECT_0?0:1;}
#else
#include <sys/mman.h>
#endif

uint8_t* vm_base;
void (*g_spu_put_hook)(uint32_t,uint32_t);
uint32_t ppu_vm_size;
int g_resv_store_active = 1;
uint32_t g_ww_lo, g_ww_hi;
ps3_guest_caller_fn g_ps3_guest_caller;
u32 g_rsx_last_reference;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
void ppu_dump_guest_stack(ppu_context* c,const char* t) {(void)c;(void)t;}
void ppu_dump_bctrl_ring(uint32_t id,const char* t) {(void)id;(void)t;}
static unsigned locked, notified, broken, flushed, live_flushed;
static int external_backend,external_ready=1;
static unsigned checked_calls;
static int expect_live_ea;static unsigned live_ea_words;
static u32 watched = 0x2000430;
static unsigned watch_writes;
static int expect_publication = 1;
static unsigned draw_words;static int expect_draw;
static void (*fixture_live_method_observer)(u32,u32,u32);
static void (*fixture_user_observer)(u32);
static void (*fixture_write_observer)(u32,u64,int);
void ppu_resv_break_store(uint64_t ea) { if (ea >= watched && ea < (u64)watched + 16) { assert(locked); broken++; } }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) {
    if(fixture_write_observer){fixture_write_observer(a,v,w);return;}
    assert(locked && (external_backend ? checked_calls : flushed && live_flushed) && w==4);
    assert(a==watched+watch_writes*4 && watch_writes<4);
    assert(vm_read32(watched+12)==0x00010000); /* Completion still pending. */
    if (watch_writes==3) assert(v==0 && vm_read64(watched)>0);
    watch_writes++;
}
int spu_coh_is_reserved(uint32_t a) { return a / 128 == watched / 128; }
void spu_coh_notify_write(uint32_t a) { if (a / 128 == watched / 128) { assert(locked && (external_backend ? checked_calls : flushed && live_flushed) && expect_publication); assert(vm_read32(watched+12)==0); notified++; } }
void spu_lockline_lock(void) { assert(!locked); locked=1; }
void spu_lockline_unlock(void) { assert(locked); locked=0; }
int rsx_draw_engine_enabled(void) { return !external_backend; }
int rsx_live_draw_enabled(void) { return 1; }
void rsx_draw_engine_method(u32 m, u32 v) { if(m==0x1814u) { assert(v==0x02000000u);draw_words++; } }
void rsx_draw_engine_method_at(u32 m,u32 v,u32 ea){(void)ea;rsx_draw_engine_method(m,v);}
void rsx_live_draw_method(u32 m, u32 v) { (void)m; (void)v; }
void rsx_draw_engine_flush(void) { assert(!locked); assert(vm_read32(watched+12)==0x00010000); flushed++; }
void rsx_live_draw_flush(void) { assert(!locked); assert(vm_read32(watched+12)==0x00010000); live_flushed++; }
void rsx_state_init(rsx_state* s) { memset(s,0,sizeof(*s)); }
int rsx_process_method(rsx_state* s,u32 m,u32 v) { (void)s; if(m==0x50)g_rsx_last_reference=v;return 0; }
void rsx_raise_user_cmd(u32 arg) { if(fixture_user_observer)fixture_user_observer(arg); }
void rsx_live_draw_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
int rsx_live_draw_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return 0; }
int rsx_live_draw_resolve_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h,u32 i,u32 j,u32 k,u32 l) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;return 0; }
void rsx_live_draw_note_resolve(u32 a,u32 b,u32 c,u32 d) { (void)a;(void)b;(void)c;(void)d; }
static u32 peek_ref(void) {u32 v;memcpy(&v,vm_base+GCM_CONTROL_GUEST_ADDR+8,4);return ps3_bswap32(v);}
static int complete=1;
static pthread_mutex_t hold_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond=PTHREAD_COND_INITIALIZER;
static int held,pending,released;
int rsx_draw_engine_flush_checked(void) {
    checked_calls++;
    if(expect_draw) assert(draw_words==1);
    if(held){pthread_mutex_lock(&hold_mutex);pending=1;pthread_cond_broadcast(&hold_cond);while(!released)pthread_cond_wait(&hold_cond,&hold_mutex);pthread_mutex_unlock(&hold_mutex);}
    assert(peek_ref()==0xA5A5A5A5u);
    return complete;
}
static void raw32(u32 ea,u32 v) { v=ps3_bswap32(v);memcpy(vm_base+ea,&v,4); }
static int external_is_ready(void* user){assert(user==&external_backend);return external_ready;}
static int external_flush(void* user){assert(user==&external_backend);return rsx_draw_engine_flush_checked();}
static void* drain_thread(void* unused){(void)unused;cellGcm_rsx_process_fifo();return NULL;}
int main(int argc,char**argv) {
    assert(argc==2);setenv("PS3RECOMP_GCM_CHECKED_REF","1",1);
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);ppu_vm_size=0x4000000;ppu_hle_inject_base=0x3000000;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));s_ea_address_table[0]=1;
    s_gcm_syscall_mode=1;raw32(GCM_CONTROL_GUEST_ADDR+8,0xA5A5A5A5u);
    if(!strcmp(argv[1],"live_data_ea")) {
        expect_live_ea=1;
        raw32(0x100000,0x00087A00);raw32(0x100004,0x30000);raw32(0x100008,0x1A521);
        cellGcm_syscall_set_fifo(12,0);cellGcm_rsx_process_fifo();
        assert(live_ea_words==2);
    } else if(!strncmp(argv[1],"external_",9)) {
        external_backend=1;
        assert(!cellGcm_checked_reference_ready());
        assert(!cellGcm_set_checked_completion(&external_backend,external_is_ready,NULL));
        assert(!cellGcm_set_checked_completion(&external_backend,NULL,external_flush));
        assert(cellGcm_set_checked_completion(&external_backend,external_is_ready,external_flush));
        assert(cellGcm_checked_reference_ready());
        assert(cellGcm_set_checked_completion(NULL,NULL,NULL));
        assert(!cellGcm_checked_reference_ready());
        assert(cellGcm_set_checked_completion(&external_backend,external_is_ready,external_flush));
        complete=!strstr(argv[1],"failure");external_ready=!strstr(argv[1],"notready");
        if(strstr(argv[1],"held")) {
            held=1;raw32(0x100000,0x00040050);raw32(0x100004,0xCAFEBABE);cellGcm_syscall_set_fifo(8,0);
            pthread_t t;assert(!pthread_create(&t,NULL,drain_thread,NULL));
            pthread_mutex_lock(&hold_mutex);while(!pending)pthread_cond_wait(&hold_cond,&hold_mutex);
            assert(peek_ref()==0xA5A5A5A5u && s_ref_qhead==s_ref_qtail);
            released=1;pthread_cond_broadcast(&hold_cond);pthread_mutex_unlock(&hold_mutex);
            assert(!pthread_join(t,NULL) && peek_ref()==0xCAFEBABE);
        } else if(strstr(argv[1],"report")) {
            s_ea_address_table[0xE0]=0x20;
            memset(vm_base+watched,0xA5,16);raw32(watched+12,0x00010000);
            g_ww_lo=watched;g_ww_hi=watched+16;
            raw32(0x100000,0x401A8);raw32(0x100004,0xBAD68000);
            raw32(0x100008,0x41800);raw32(0x10000C,0x01000430);
            cellGcm_syscall_set_fifo(16,0);cellGcm_rsx_process_fifo();
            assert(checked_calls==1);
            assert(vm_read32(watched+12)==(complete?0:0x00010000));
            assert(watch_writes==(complete?4:0));
        } else {
            raw32(0x100000,0x00040050);raw32(0x100004,0xCAFEBABE);
            cellGcm_syscall_set_fifo(8,0);cellGcm_rsx_process_fifo();gcm_ref_publish_one();
            assert(checked_calls==(external_ready?1:0));
            assert(peek_ref()==(complete&&external_ready?0xCAFEBABE:0xA5A5A5A5u));
        }
        assert(cellGcm_checked_reference_ready()==(complete&&external_ready));
    } else if(!strcmp(argv[1],"held")) {
        held=1;raw32(0x100000,0x00040050);raw32(0x100004,0xCAFEBABE);cellGcm_syscall_set_fifo(8,0);
        pthread_t t;assert(!pthread_create(&t,NULL,drain_thread,NULL));
        pthread_mutex_lock(&hold_mutex);while(!pending)pthread_cond_wait(&hold_cond,&hold_mutex);
        assert(peek_ref()==0xA5A5A5A5u);assert(s_ref_qhead==s_ref_qtail);
        released=1;pthread_cond_broadcast(&hold_cond);pthread_mutex_unlock(&hold_mutex);
        assert(!pthread_join(t,NULL));assert(peek_ref()==0xCAFEBABE);
    } else if(!strcmp(argv[1],"park_patch") || !strcmp(argv[1],"park_branch") || !strcmp(argv[1],"park_wrap") || !strcmp(argv[1],"park_invalid")) {
        /* A producer parks the GPU while its next segment is incomplete.
         * put can be ahead, or behind the park after a ring wrap. */
        int wrap=!strcmp(argv[1],"park_wrap");
        u32 park=wrap?0x100:0, put=wrap?16:24;
        expect_draw=1;
        raw32(0x100000+park,0x20000000u|park);
        raw32(0x100008,0x00041814);raw32(0x10000C,0x02000000);
        raw32(0x100010,0x00040050);raw32(0x100014,0xCAFEBABE);
        cellGcm_syscall_set_fifo(put,park);
        for(unsigned i=0;i<12;i++) {
            cellGcm_rsx_process_fifo();
            assert(s_fifo_getoff==park && !checked_calls && !draw_words);
            assert(peek_ref()==0xA5A5A5A5u);
            assert(s_ref_qhead==s_ref_qtail);
            assert(!atomic_load(&s_checked_ref_failed));
            assert(cellGcm_checked_reference_ready());
        }
        if(!strcmp(argv[1],"park_invalid")) {
            raw32(0x100000,0x20200000u);cellGcm_rsx_process_fifo();
            assert(atomic_load(&s_checked_ref_failed)&&!checked_calls&&!draw_words);
            assert(peek_ref()==0xA5A5A5A5u && !cellGcm_checked_reference_ready());
        } else {
            if(!strcmp(argv[1],"park_patch")) {
                raw32(0x100000,0x00041814);raw32(0x100004,0x02000000);
                raw32(0x100008,0x00040050);raw32(0x10000C,0xCAFEBABE);
                cellGcm_syscall_set_fifo(16,park);
            } else if(wrap) {
                raw32(0x100000,0x00041814);raw32(0x100004,0x02000000);
                raw32(0x100008,0x00040050);raw32(0x10000C,0xCAFEBABE);
                raw32(0x100000+park,0x20000000u);
            } else raw32(0x100000,0x20000008u);
            cellGcm_rsx_process_fifo();
            assert(s_fifo_getoff==(wrap||!strcmp(argv[1],"park_patch")?16:24));
            assert(checked_calls==1 && draw_words==1 && peek_ref()==0xCAFEBABE);
            assert(s_checked_ref_receipt==1 && !atomic_load(&s_checked_ref_failed));
        }
    } else if(!strcmp(argv[1],"circling") || !strcmp(argv[1],"invalid_jump")) {
        int invalid=!strcmp(argv[1],"invalid_jump");
        raw32(0x100000,invalid?0x20200000u:0x20000008u);
        raw32(0x100008,0x20000000u);cellGcm_syscall_set_fifo(16,0);
        for(unsigned i=0;i<12;i++)cellGcm_rsx_process_fifo();
        assert(atomic_load(&s_checked_ref_failed)&&!cellGcm_checked_reference_ready());assert(!checked_calls&&!draw_words);
        assert(peek_ref()==0xA5A5A5A5u && s_ref_qhead==s_ref_qtail);
    } else if(!strcmp(argv[1],"repeat_wrap")) {
        u32 values[]={0xFFFFFFFFu,0xFFFFFFFFu,0,1};
        for(unsigned i=0;i<4;i++){raw32(0x100000+8*i,0x00040050);raw32(0x100004+8*i,values[i]);}
        cellGcm_syscall_set_fifo(32,0);cellGcm_rsx_process_fifo();assert(checked_calls==4);
        assert(!cellGcm_set_checked_completion(&external_backend,external_is_ready,external_flush));
        for(unsigned i=0;i<4;i++){assert(peek_ref()==values[i]);assert(s_ref_receipts[i]==i+1);gcm_ref_publish_one();}
    } else if(!strcmp(argv[1],"unknown_word")) {
        /* An unknown command word under checked REF stands checked REF down for
         * the run; the walker skips the word and REF and labels keep advancing. */
        raw32(0x100000,0x60000000u);                                /* not a command word */
        raw32(0x100004,0x00040050);raw32(0x100008,0xCAFEBABE);      /* SET_REFERENCE */
        raw32(0x10000C,0x00040064);raw32(0x100010,0x00000020);      /* SEMAPHORE_OFFSET */
        raw32(0x100014,0x0004006C);raw32(0x100018,0x12345678);      /* SEMAPHORE_RELEASE */
        cellGcm_syscall_set_fifo(0x1C,0);cellGcm_rsx_process_fifo();
        assert(s_fifo_getoff==0x1C);
        assert(atomic_load(&s_checked_ref_off)&&!atomic_load(&s_checked_ref_failed));
        assert(!checked_calls);                                       /* plain paced REF from here on */
        gcm_ref_publish_one();assert(peek_ref()==0xCAFEBABE);
        assert(vm_read32(GCM_LABEL_GUEST_BASE+0x20)==0x12345678u);
        /* Later work keeps draining, and a second unknown word reports nothing new. */
        raw32(0x10001C,0x60000000u);
        raw32(0x100020,0x00040050);raw32(0x100024,0xBEEF0001u);
        cellGcm_syscall_set_fifo(0x28,0x1C);cellGcm_rsx_process_fifo();
        assert(s_fifo_getoff==0x28 && !atomic_load(&s_checked_ref_failed));
        gcm_ref_publish_one();assert(peek_ref()==0xBEEF0001u);
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

int rsx_draw_engine_checked_ready(void) { return !external_backend; }

/* Backend boundary required by the production guest-memory transfer path. */
void rsx_draw_engine_mark_guest_dirty(u32 ea,u32 len) { (void)ea; (void)len; }

/* Upstream renderer hooks are outside this focused FIFO fixture. */
void rsx_draw_engine_guest_read(u32 a,u32 b,u32 c,u32 d,u32 e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void rsx_draw_engine_guest_wrote(u32 a,u32 b,u32 c,u32 d,u32 e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void rsx_draw_engine_note_flip(u32 a) {(void)a;}

void rsx_live_draw_method_at(u32 method,u32 arg,u32 data_ea) {
    if(fixture_live_method_observer)fixture_live_method_observer(method,arg,data_ea);
    if(expect_live_ea){
        assert(live_ea_words<2 && data_ea==0x100004+4*live_ea_words);
        assert(method==0x7A00+4*live_ea_words);
        assert(arg==(live_ea_words?0x1A521:0x30000));live_ea_words++;
    }
    rsx_live_draw_method(method,arg);
}
