/* Synthetic fixture: an SPU write must reach guest memory so that, in each
 * 128-byte line, the first word the write stores there is the last to change
 * (docs/SPU_PUT_LINE_ORDER.md).
 *
 * Every host copy and fill the MFC path makes into the watched range goes
 * through a recorder that writes byte by byte, ascending. After each byte the
 * recorder checks every line of the destination as a FIFO walker would see
 * it if it ran at that instant, the producer thread having lost its CPU. A
 * line whose first word has changed must already hold all of its new bytes.
 * The destination starts the way MAG prepares a slot: a park (a JUMP to its
 * own address) at the start of each line and at the start of the body, and
 * stale command data everywhere else. No guest content. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* fixture_memcpy(void* d, const void* s, size_t n);
static void* fixture_memset(void* d, int c, size_t n);
#undef memcpy   /* fortified string.h defines them as macros */
#undef memset
#define memcpy fixture_memcpy
#define memset fixture_memset
#include "spu_dma.h"
#include "spu_dma_publication_impl.h"
#undef memcpy
#undef memset

static uint8_t memory[0x40000];
uint8_t* vm_base = memory;
uint8_t g_vm_page_bitmap[65536 / 8];
int g_cri_video_dma;
uint32_t g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long ps3_ms_now(void) { return 1000; }
/* Lines reported as reserved, by index from the watched base: this selects
 * the PUT path's plain, partly locked and fully locked copy branches. */
static uint32_t reserved_mask;
static uint32_t watch_ea, watch_size;
int spu_coh_is_reserved(uint32_t ea)
{
    if (ea < (watch_ea & ~127u) || ea >= watch_ea + watch_size) return 0;
    const uint32_t line = (ea - (watch_ea & ~127u)) >> 7;
    return line < 32 && ((reserved_mask >> line) & 1u);
}
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
void* ps3_jobio_active;
void ps3_jobio_event(struct spu_context* c, uint32_t k, uint32_t cmd, uint32_t lsa, uint64_t ea, uint32_t n, const uint8_t* b, uint32_t x)
{ (void)c; (void)k; (void)cmd; (void)lsa; (void)ea; (void)n; (void)b; (void)x; }
void* volatile g_sn_defer_ctx;
volatile unsigned g_sn_defer;
unsigned spu_sn_defer_ticks(void) { return 0; }
void spu_ch_wake(spu_context* s) { (void)s; }
unsigned* spu_ls_watch_list(int* n) { *n = 0; return NULL; }
void spu_overlay_note_get(spu_context* s, uint32_t ea, const uint8_t* p, uint32_t n)
{ (void)s; (void)ea; (void)p; (void)n; }

static spu_context spu;
static uint8_t local_store[SPU_LS_SIZE];
static uint8_t old_image[0x1000], new_image[0x1000];
static unsigned checks, failures;
static const char* current_case;

static void store_be32(uint8_t* p, uint32_t v)
{ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* What a walker sees right now: per line piece of the write, a first word
 * that differs from before means the whole piece must be the new one. */
static void observe(void)
{
    checks++;
    for (uint32_t off = 0; off < watch_size;) {
        uint32_t piece = 128u - ((watch_ea + off) & 127u);
        if (piece > watch_size - off) piece = watch_size - off;
        const uint8_t* now = memory + watch_ea + off;
        if (piece >= 8u && memcmp(now, old_image + off, 4) != 0 &&
            memcmp(now, new_image + off, piece) != 0) {
            if (failures++ < 4)
                fprintf(stderr, "FAIL %s: line at ea=0x%05X shows its new first word with old bytes behind it\n",
                        current_case, watch_ea + off);
            return;
        }
        off += piece;
    }
}

static int in_watch(const void* d, size_t n)
{
    const uint8_t* p = (const uint8_t*)d;
    return watch_size && p < memory + watch_ea + watch_size && p + n > memory + watch_ea;
}

static void* fixture_memcpy(void* d, const void* s, size_t n)
{
    uint8_t* dst = (uint8_t*)d; const uint8_t* src = (const uint8_t*)s;
    if (!in_watch(d, n)) { for (size_t i = 0; i < n; i++) dst[i] = src[i]; return d; }
    for (size_t i = 0; i < n; i++) { dst[i] = src[i]; observe(); }
    return d;
}

static void* fixture_memset(void* d, int c, size_t n)
{
    uint8_t* dst = (uint8_t*)d;
    if (!in_watch(d, n)) { for (size_t i = 0; i < n; i++) dst[i] = (uint8_t)c; return d; }
    for (size_t i = 0; i < n; i++) { dst[i] = (uint8_t)c; observe(); }
    return d;
}

/* The slot as MAG prepares it: a park at the body start and at every line
 * start, stale command and float data between them. */
static void prepare(uint32_t ea, uint32_t size)
{
    watch_ea = 0; watch_size = 0;
    for (uint32_t off = 0; off < size; off += 4) {
        const uint32_t a = ea + off;
        uint32_t v = (off == 0 || (a & 127u) == 0) ? 0x20000000u | a : 0x3F7FFFFFu - off;
        store_be32(memory + a, v);
    }
    memcpy(old_image, memory + ea, size);
}

static void expect_done(uint32_t ea, uint32_t size)
{
    observe();
    assert(!memcmp(memory + ea, new_image, size));
    watch_ea = 0; watch_size = 0;
}

static void run_put(const char* name, uint32_t ea, uint32_t size, uint32_t reserved, int list)
{
    current_case = name;
    prepare(ea, size);
    for (uint32_t off = 0; off < size; off += 4) {
        const uint32_t v = (off & 0x7Fu) == 0x20u ? 0x00041D74u : 0x00041710u + off;
        store_be32(local_store + 0x1000 + off, v);   /* EA and LS agree mod 16 */
    }
    memcpy(new_image, local_store + 0x1000, size);
    reserved_mask = reserved;
    watch_ea = ea; watch_size = size;
    spu.mfc_tag = 3;
    if (list) {
        /* Two list elements, split at a line boundary: each element is a
         * write of its own, and the rule holds per write. */
        const uint32_t first = 0xE0u;
        spu_ls_write32(&spu, 0x200, first);
        spu_ls_write32(&spu, 0x204, ea);
        spu_ls_write32(&spu, 0x208, size - first);
        spu_ls_write32(&spu, 0x20C, ea + first);
        spu.mfc_lsa = 0x1000;
        assert(mfc_do_list_transfer(&spu, 0x200, 0, 16, 0x24) == 0);
    } else {
        assert(mfc_do_transfer(&spu, 0x1000, ea, size, 0x22) == 0);   /* PUTF, as MAG's body job */
    }
    expect_done(ea, size);
}

static void run_zero(const char* name, uint32_t ea, uint32_t size)
{
    current_case = name;
    prepare(ea, size);
    memset(new_image, 0, size);
    reserved_mask = 0;
    watch_ea = ea; watch_size = size;
    spu.mfc_tag = 4;
    assert(mfc_do_transfer(&spu, 0, ea, size, MFC_SDCRZ_CMD) == 0);
    expect_done(ea, size);
}

int main(void)
{
    memset(g_vm_page_bitmap, 255, sizeof g_vm_page_bitmap);
    spu.ls = local_store;
    /* A 752-byte body that starts 0x20 into a line, like MAG's. */
    run_put("PUTF body, plain copy", 0x12020, 752, 0, 0);
    run_put("PUTF body, one line reserved", 0x13020, 752, 1u << 2, 0);
    run_put("PUTF one line, reserved", 0x14000, 128, 1u, 0);
    run_put("PUTL body in two elements", 0x15020, 752, 0, 1);
    run_zero("SDCRZ", 0x16000, 1024);
    printf("checks=%u failures=%u\n", checks, failures);
    if (failures) return 1;
    printf("PASS: direct, list, partly and fully locked PUTs and SDCRZ store each line's first word last\n");
    return 0;
}
