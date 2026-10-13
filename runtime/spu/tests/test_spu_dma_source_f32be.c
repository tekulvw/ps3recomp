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
uint32_t g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long clock_ms = 1000;
unsigned long long ps3_ms_now(void) { return clock_ms; }
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
void* volatile g_sn_defer_ctx;
volatile unsigned g_sn_defer;
unsigned spu_sn_defer_ticks(void) { return 0; }
void spu_ch_wake(spu_context* s) { (void)s; }
unsigned* spu_ls_watch_list(int* n) { *n=0; return NULL; }
void spu_overlay_note_get(spu_context* s, uint32_t ea, const uint8_t* p, uint32_t n)
{ (void)s; (void)ea; (void)p; (void)n; }
static volatile LONG ready_threads, start_threads;
static void* concurrent_observe(void* opaque)
{
    InterlockedIncrement(&ready_threads);
    while (!InterlockedCompareExchange(&start_threads, 0, 0)) sched_yield();
    mfc_trace_source_f32be((spu_context*)opaque, 0x100, 0x12000, 32, 0x20);
    return NULL;
}
int main(int argc, char** argv)
{
    static spu_context s;
    static uint8_t local_store[SPU_LS_SIZE], before[32];
    const uint32_t words[] = {0, 0x80000000, 0x3F800000, 0xC0200000,
                             0x7FC00001, 0x7F800000, 0x00000001, 0xFF800000};
    memset(g_vm_page_bitmap, 255, sizeof g_vm_page_bitmap);
    s.ls = local_store; s.pc = 0x2340; s.image_id = 7;
    if (argc > 1 && !strcmp(argv[1], "full")) {
        const uint32_t at = SPU_LS_SIZE - 16384;
        spu_ls_write32(&s, SPU_LS_SIZE-4, 0xBFA00000); /* last word, -1.25 */
        assert(mfc_do_transfer(&s, at, 0x12000, 16384, 0x20) == 0);
        assert(memcmp(memory+0x12000, local_store+at, 16384) == 0);
        assert(spu_ls_read32(&s, SPU_LS_SIZE-4) == 0xBFA00000);
        clock_ms = 6000;
        /* Overrun and EA wrap are rejected before any observer read/log. */
        assert(mfc_do_transfer(&s, at+16, 0x12000, 16384, 0x20) == -1);
        assert(mfc_do_transfer(&s, at, 0xFFFFFFF0u, 32, 0x20) == -1);
        return 0;
    }
    for (unsigned i=0; i<8; ++i) spu_ls_write32(&s, 0x100 + i*4, words[i]);
    if (argc > 1 && !strcmp(argv[1], "threads")) {
        pthread_t threads[8];
        for (unsigned i=0; i<8; ++i) assert(pthread_create(&threads[i], NULL, concurrent_observe, &s) == 0);
        while (InterlockedCompareExchange(&ready_threads, 0, 0) != 8) sched_yield();
        InterlockedExchange(&start_threads, 1);
        for (unsigned i=0; i<8; ++i) assert(pthread_join(threads[i], NULL) == 0);
        return 0;
    }
    memcpy(before, local_store+0x100, sizeof before);
    memset(memory+0x12000, 0x5A, sizeof before);
    /* A two-byte watched PUT has no complete f32 word and cannot claim the window. */
    assert(mfc_do_transfer(&s, 0x106, 0x12006, 2, 0x20) == 0);
    assert(mfc_do_transfer(&s, SPU_LS_SIZE + 0x100, 0x12000, 32, 0x20) == 0);
    assert(memcmp(local_store+0x100, before, sizeof before) == 0);
    assert(memcmp(memory+0x12000, before, sizeof before) == 0);
    /* Same window is suppressed; invalid LS span must never be scanned. */
    assert(mfc_do_transfer(&s, 0x100, 0x12000, 32, 0x20) == 0);
    assert(mfc_do_transfer(&s, SPU_LS_SIZE-16, 0x12000, 32, 0x20) == -1);
    clock_ms = 6000;
    if (argc > 1) {
        /* Real PUTL normalizes each element through mfc_do_transfer. */
        s.mfc_lsa = 0x100; s.mfc_tag = 1;
        spu_ls_write32(&s, 0x200, 32);
        spu_ls_write32(&s, 0x204, 0x12000);
        assert(mfc_do_list_transfer(&s, 0x200, 0, 8, 0x24) == 0);
    } else {
        assert(mfc_do_transfer(&s, 0x100, 0x12000, 32, 0x20) == 0);
    }
    /* GET and unrelated PUT must not consume the next logging window. */
    clock_ms = 11000;
    assert(mfc_do_transfer(&s, 0x100, 0x12000, 32, 0x40) == 0);
    assert(mfc_do_transfer(&s, 0x100, 0x13000, 32, 0x20) == 0);
    assert(mfc_do_transfer(&s, 0x100, 0x12000, 32, 0x20) == 0);
    assert(memcmp(local_store+0x100, before, sizeof before) == 0);
    return 0;
}
