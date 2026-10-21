/* Independent synthetic fixture: real direct/list PUTs from two contexts. */
#include "spu_dma.h"
#include "spu_dma_publication_impl.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
static uint8_t memory[0x40000];
uint8_t* vm_base = memory;
uint8_t g_vm_page_bitmap[65536 / 8];
int g_cri_video_dma;
uint32_t g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
static unsigned long long clock_ms = 1000;
static volatile LONG clock_calls;
unsigned long long ps3_ms_now(void) { InterlockedIncrement(&clock_calls); return clock_ms; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea, const void* self) { (void)ea; (void)self; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockguard_check(spu_context* s, uint32_t ea, const uint8_t* p, uint32_t n, const char* k)
{ (void)s; (void)ea; (void)p; (void)n; (void)k; }
void SetLastError(DWORD e) { (void)e; }
LPVOID VirtualAlloc(LPVOID a, SIZE_T n, DWORD t, DWORD p)
{ (void)a; (void)n; (void)t; (void)p; assert(0); return NULL; }
SIZE_T VirtualQuery(LPCVOID a, PMEMORY_BASIC_INFORMATION m, SIZE_T n)
{ (void)a; (void)m; (void)n; assert(0); return 0; }
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
unsigned* spu_ls_watch_list(int* n) { *n=0; return NULL; }
void spu_overlay_note_get(spu_context* s, uint32_t ea, const uint8_t* p, uint32_t n)
{ (void)s; (void)ea; (void)p; (void)n; }
static spu_context contexts[2];
static uint8_t stores[2][SPU_LS_SIZE], before[2][32];
static const uint32_t payload[2][8] = {
    {0, 0x80000000, 0x3F800000, 0xC0200000, 0x7FC00001, 0x7F800000, 1, 0xFF800000},
    {0x40800000, 0, 0, 0, 0, 0, 0, 0}
};
static void put(unsigned index, int list)
{
    spu_context* s = &contexts[index];
    uint32_t ea = 0x12000 + index*0x100;
    memset(memory+ea, 0x5A, 32);
    if (list) {
        s->mfc_lsa = 0x100; s->mfc_tag = 1;
        spu_ls_write32(s, 0x200, 32);
        spu_ls_write32(s, 0x204, ea);
        assert(mfc_do_list_transfer(s, 0x200, 0, 8, 0x24) == 0);
    } else {
        assert(mfc_do_transfer(s, 0x100, ea, 32, 0x20) == 0);
    }
    assert(!memcmp(memory+ea, before[index], 32));
    assert(!memcmp(stores[index]+0x100, before[index], 32));
}
static volatile LONG ready_threads, start_threads;
static void* concurrent_observe(void* opaque)
{
    InterlockedIncrement(&ready_threads);
    while (!InterlockedCompareExchange(&start_threads, 0, 0)) sched_yield();
    spu_context* s = (spu_context*)opaque;
    mfc_trace_source_f32be(s, 0x100, s == &contexts[0] ? 0x12000 : 0x12100, 32, 0x20);
    return NULL;
}
int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "disabled")) {
        mfc_trace_source_f32be((spu_context*)(uintptr_t)1, 0, 0x12000, 32, 0x20);
        contexts[0].ls = (uint8_t*)(uintptr_t)1;
        mfc_trace_source_f32be(&contexts[0], 0, 0x12000, 32, 0x20);
        assert(clock_calls == 0);
        return 0;
    }
    memset(g_vm_page_bitmap, 255, sizeof g_vm_page_bitmap);
    for (unsigned i=0; i<2; ++i) {
        contexts[i].ls = stores[i]; contexts[i].pc = 0x2340; contexts[i].image_id = 7;
        /* Other lanes are different: a lane reversal cannot pass these checks. */
        for (unsigned lane=0; lane<4; ++lane) {
            contexts[i].gpr[0]._u32[lane] = 0xA1234567 + i*0x10000000 + lane;
            contexts[i].gpr[1]._u32[lane] = 0xC7654321 + i*0x10000000 + lane;
        }
        for (unsigned j=0; j<8; ++j) spu_ls_write32(&contexts[i], 0x100+j*4, payload[i][j]);
        memcpy(before[i], stores[i]+0x100, 32);
        printf("fixture-context index=%u ctx=%p\n", i, (void*)&contexts[i]);
    }
    if (argc > 1 && !strcmp(argv[1], "threads")) {
        for (unsigned round=0; round<3; ++round) {
            pthread_t threads[8]; ready_threads=0; start_threads=0;
            for (unsigned i=0; i<8; ++i)
                assert(!pthread_create(&threads[i], NULL, concurrent_observe, &contexts[i%2]));
            while (InterlockedCompareExchange(&ready_threads, 0, 0) != 8) sched_yield();
            InterlockedExchange(&start_threads, 1);
            for (unsigned i=0; i<8; ++i) assert(!pthread_join(threads[i], NULL));
            clock_ms += 5000;
        }
        return 0;
    }
    put(0, 0);
    clock_ms=5999; put(1, 1); /* Suppressed by the global window; no sequence consumed. */
    clock_ms=6000; put(1, 1);
    clock_ms=11000; put(0, 1);
    clock_ms=15999; put(0, 0);
    clock_ms=16000; put(1, 0);
    clock_ms=21000;
    assert(mfc_do_transfer(&contexts[0], SPU_LS_SIZE-16, 0x12000, 32, 0x20) == -1);
    assert(mfc_do_transfer(&contexts[0], 0x100, 0xFFFFFFF0u, 32, 0x20) == -1);
    assert(mfc_do_transfer(&contexts[0], 0x100, 0x12000, 32, 0x40) == 0);
    assert(mfc_do_transfer(&contexts[0], 0x100, 0x13000, 32, 0x20) == 0);
    put(0, 0);
    return 0;
}
