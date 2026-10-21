/* Synthetic data only. Exercise the real DMA path; platform/coherence dependencies
 * are inert because the fabricated pages are committed and unreserved. */
#include "spu_dma.h"
#include "spu_dma_publication_impl.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
static uint8_t memory[0x40000];
uint8_t* vm_base = memory;
uint8_t g_vm_page_bitmap[65536 / 8];
int g_cri_video_dma;
uint32_t g_ydkj_real_taskset_ea;
uint32_t g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long clock_ms = 1000;
unsigned long long ps3_ms_now(void) { return clock_ms; }
static int reserved, locks, notifications;
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return reserved; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea, const void* self) { (void)ea; (void)self; ++notifications; }
void spu_lockline_lock(void) { ++locks; }
void spu_lockline_unlock(void) { --locks; }
void spu_lockguard_check(spu_context* s, uint32_t ea, const uint8_t* p, uint32_t n, const char* k)
{ (void)s; (void)ea; (void)p; (void)n; (void)k; }
void SetLastError(DWORD e) { (void)e; }
LPVOID VirtualAlloc(LPVOID a, SIZE_T n, DWORD t, DWORD p)
{ (void)a; (void)n; (void)t; (void)p; assert(0); return NULL; }
SIZE_T VirtualQuery(LPCVOID a, PMEMORY_BASIC_INFORMATION m, SIZE_T n)
{ (void)a; (void)m; (void)n; return 0; }
void (*g_spu_put_hook)(uint32_t, uint32_t);
/* Job I/O recorder (runtime/diagnostics): never armed here, so the fixture
 * only needs the inactive pointer and an event sink that is never reached. */
void* ps3_jobio_active;
void ps3_jobio_event(struct spu_context* c, uint32_t k, uint32_t cmd, uint32_t lsa, uint64_t ea, uint32_t n, const uint8_t* b, uint32_t x)
{ (void)c; (void)k; (void)cmd; (void)lsa; (void)ea; (void)n; (void)b; (void)x; }
void* volatile g_sn_defer_ctx;
volatile unsigned g_sn_defer;
unsigned spu_sn_defer_ticks(void) { return 0; }
void spu_ch_wake(spu_context* s) { (void)s; }
/* Reached only by the identical-transfer watchdog, which no fixture trips. */
void spu_halt(spu_context* s) { (void)s; abort(); }
unsigned* spu_ls_watch_list(int* n) { *n=0; return NULL; }
void spu_overlay_note_get(spu_context* s, uint32_t ea, const uint8_t* p, uint32_t n)
{ (void)s; (void)ea; (void)p; (void)n; }
#include <errno.h>
#include <sys/stat.h>
static void* transfer_thread(void* p) {
    spu_context* s=p;
    for(int i=0;i<64;i++) assert(mfc_do_transfer(s,0x100,0x12000,32,0x40)==0);
    return NULL;
}
int main(int argc,char** argv) {
    static spu_context s;
    static uint8_t ls[SPU_LS_SIZE];
    memset(g_vm_page_bitmap,255,sizeof g_vm_page_bitmap);
    s.ls=ls; s.image_id=7; s.spu_id=3; s.pc=0x2340;
    for(int i=0;i<8;i++) { spu_ls_write32(&s,0x100+i*4,0x3F800000u+i); memory[0x12000+i*4]=0x55; }
    if(argc>1 && !strcmp(argv[1],"disabled")) {
        spu_dma_disposition_token t;
        spu_dma_disposition_begin(&t,0x100,0x12000,32,0x20);
        spu_dma_disposition_before(&t,(const uint8_t*)1,0x12000,32);
        spu_dma_disposition_after(&t,(const uint8_t*)1);
        assert(spu_dma_disposition_finish(&t,(spu_context*)1,0x100,0x12000,32,0x20,0,SPU_DMA_DISPOSITION_PUT)==0);
        return 0;
    }
    if(argc>1 && !strcmp(argv[1],"tag-failure")) {
        mfc_engine m; memset(&m,0,sizeof m);
        s.mfc_lsa=SPU_LS_SIZE-16; s.mfc_eal=0x12000; s.mfc_size=32; s.mfc_tag=5;
        assert(mfc_submit(&m,&s,0x20)==-1);
        assert(m.tag_completed&(1u<<5)); /* Existing completion despite failure. */
        return 0;
    }
    if(argc>1 && !strcmp(argv[1],"zeros")) {
        memset(memory+0x12000,0x55,128);
        assert(mfc_do_transfer(&s,0xFFFFFFFF,0x12000,128,MFC_SDCRZ_CMD)==0);
        for(int i=0;i<128;i++)assert(memory[0x12000+i]==0);
        assert(locks==0 && notifications==1);
        memset(ls+0x100,0x55,32);
        assert(mfc_do_transfer(&s,0x100,0x1000,32,0x40)==0);
        for(int i=0;i<32;i++)assert(ls[0x100+i]==0);
        return 0;
    }
    if(argc>1 && !strcmp(argv[1],"reserved")) {
        reserved=1;
        assert(mfc_do_transfer(&s,0x100,0x12000,32,0x20)==0);
        assert(!memcmp(memory+0x12000,ls+0x100,32));
        assert(locks==0 && notifications==1);
        return 0;
    }
    if(argc>1 && !strcmp(argv[1],"errno")) {
        spu_dma_disposition_token t; errno=EDOM;
        spu_dma_disposition_begin(&t,0x100,0x12000,32,0x20); assert(errno==EDOM);
        spu_dma_disposition_before(&t,ls+0x100,0x12000,32); assert(errno==EDOM);
        spu_dma_disposition_after(&t,ls+0x100); assert(errno==EDOM);
        assert(spu_dma_disposition_finish(&t,&s,0x100,0x12000,32,0x20,0,SPU_DMA_DISPOSITION_PUT)==0); assert(errno==EDOM);
        return 0;
    }
    if(argc>1 && !strcmp(argv[1],"threads")) {
        pthread_t threads[8]; spu_context contexts[8]; uint8_t* stores[8];
        for(int i=0;i<8;i++) { stores[i]=calloc(1,SPU_LS_SIZE); assert(stores[i]); contexts[i]=s; contexts[i].ls=stores[i]; assert(!pthread_create(&threads[i],NULL,transfer_thread,&contexts[i])); }
        for(int i=0;i<8;i++) { assert(!pthread_join(threads[i],NULL)); free(stores[i]); }
        return 0;
    }
    if(argc>1 && !strcmp(argv[1],"trigger")) {
        for(int i=0;i<300;i++) assert(mfc_do_transfer(&s,0x100,0x12000,32,0x20)==0);
        const char* path=getenv("SPU_DMA_DISPOSITION_TRIGGER"); assert(path);
        FILE* f=fopen(path,"w"); assert(f); fclose(f);
        /* Host-owned clock delay only in fixture; no observer counter reset. */
        struct timespec pause={0,300000000}; nanosleep(&pause,NULL);
    }
    assert(mfc_do_transfer(&s,0x100,0x12000,32,0x20)==0);
    assert(!memcmp(memory+0x12000,ls+0x100,32));
    memset(ls+0x100,0,32);
    assert(mfc_do_transfer(&s,0x100,0x12000,32,0x40)==0);
    assert(spu_ls_read32(&s,0x100)==0x3F800000);
    s.mfc_lsa=0x100; s.mfc_tag=1;
    spu_ls_write32(&s,0x200,32); spu_ls_write32(&s,0x204,0x12000);
    assert(mfc_do_list_transfer(&s,0x200,0,8,0x24)==0);
    uint8_t prior[32]; memcpy(prior,memory+0x12000,32);
    assert(mfc_do_transfer(&s,0x104,0x12000,32,0x20)==0); /* malformed no-copy rc0 */
    assert(!memcmp(prior,memory+0x12000,32));
    assert(mfc_do_transfer(&s,SPU_LS_SIZE-16,0x12000,32,0x20)==-1);
    g_vm_page_bitmap[1/8] &= (uint8_t)~(1u<<(1%8));
    assert(mfc_do_transfer(&s,0x100,0x12000,32,0x40)==-1);
    assert(!memcmp(prior,memory+0x12000,32));
    return 0;
}
