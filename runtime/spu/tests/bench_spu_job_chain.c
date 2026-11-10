/* Synthetic SPU job chain through the real channel, MFC and lock-line paths.
 *
 * A "job" here is a fixed sequence of the channel operations a small SPURS
 * job makes, written by hand (no lifted code, no game data): a descriptor
 * GET, two input GETs, a little work in local store, an output PUT, a tag
 * wait after each group, and a GETLLAR/PUTLLC increment of a completion
 * counter. Every operation goes through spu_wrch/spu_rdch exactly as lifted
 * code calls them, so the numbers are the runtime's fixed cost per operation
 * and per job, next to a floor that moves the same bytes with memcpy.
 *
 * Two chains are timed:
 *  - serial: one context runs N jobs back to back (the serial tail);
 *  - hop: two contexts on two threads alternate, each polling the completion
 *    counter with GETLLAR until it is its turn (a dependent chain).
 *
 * argv[1] = jobs (default 200000). Every job's output and the final counter
 * are checked, so the fixture also fails if a change breaks the transfers. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size = 0;
#include "../spu_channels.c"

/* Inert stand-ins for the PPU/HLE side this fixture does not link. */
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
unsigned long long ps3_ms_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
}
void spurs_ef_set_from_spu(uint32_t a, uint16_t b) { (void)a; (void)b; }
uint64_t spurs_pm_build_context(uint8_t* a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
void ydkj_wake_all_event_flags(void) {}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static u128 u32v(uint32_t v) { u128 r; memset(&r, 0, sizeof r); r._u32[0] = v; return r; }
static void ch_w(spu_context* c, uint32_t ch, uint32_t v) { spu_wrch(c, ch, u32v(v)); }
static uint32_t ch_r(spu_context* c, uint32_t ch) { return spu_rdch(c, ch)._u32[0]; }

enum { DESC = 0x10000, IN = 0x20000, OUT = 0x40000, LINE = 0x80000, GUEST = 1u << 20 };
enum { LS_DESC = 0x1000, LS_IN = 0x2000, LS_OUT = 0x3000, LS_LINE = 0x3F000, LS_POLL = 0x3E000 };

static void dma(spu_context* c, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag)
{
    ch_w(c, MFC_LSA, lsa); ch_w(c, MFC_EAH, 0); ch_w(c, MFC_EAL, ea);
    ch_w(c, MFC_Size, size); ch_w(c, MFC_TagID, tag); ch_w(c, MFC_Cmd, cmd);
}
static void tag_wait(spu_context* c, uint32_t mask)
{
    ch_w(c, MFC_WrTagMask, mask); ch_w(c, MFC_WrTagUpdate, 2);   /* all */
    assert((ch_r(c, MFC_RdTagStat) & mask) == mask);
}
static void getllar(spu_context* c, uint32_t lsa, uint32_t ea)
{
    ch_w(c, MFC_LSA, lsa); ch_w(c, MFC_EAH, 0); ch_w(c, MFC_EAL, ea); ch_w(c, MFC_Cmd, MFC_GETLLAR_CMD);
    assert(ch_r(c, MFC_RdAtomicStat) == 4);
}
/* GETLLAR/PUTLLC increment of the big-endian word at the line's start. */
static uint32_t atomic_inc(spu_context* c, uint32_t line_ea)
{
    for (;;) {
        getllar(c, LS_LINE, line_ea);
        uint32_t v = spu_ls_read32(c, LS_LINE) + 1;
        spu_ls_write32(c, LS_LINE, v);
        ch_w(c, MFC_LSA, LS_LINE); ch_w(c, MFC_EAH, 0); ch_w(c, MFC_EAL, line_ea); ch_w(c, MFC_Cmd, MFC_PUTLLC_CMD);
        if (ch_r(c, MFC_RdAtomicStat) == 0) return v;
    }
}
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* One small job: its output word is (descriptor word ^ input word ^ i). */
static void job(spu_context* c, uint32_t i, uint32_t line_ea)
{
    const uint32_t d = DESC + (i & 63) * 128, in = IN + (i & 15) * 0x800, out = OUT + (i & 15) * 0x200;
    dma(c, MFC_GET_CMD, LS_DESC, d, 128, 1); tag_wait(c, 1u << 1);
    dma(c, MFC_GET_CMD, LS_IN, in, 1024, 2);
    dma(c, MFC_GET_CMD, LS_IN + 1024, in + 1024, 1024, 2); tag_wait(c, 1u << 2);
    for (uint32_t o = 0; o < 512; o += 4)
        spu_ls_write32(c, LS_OUT + o, spu_ls_read32(c, LS_DESC + (o & 127)) ^ spu_ls_read32(c, LS_IN + o) ^ i);
    dma(c, MFC_PUT_CMD, LS_OUT, out, 512, 3); tag_wait(c, 1u << 3);
    assert(be32(vm_base + out + 508) == (be32(vm_base + d + 124) ^ be32(vm_base + in + 508) ^ i));
    if (line_ea) (void)atomic_inc(c, line_ea);
}

static spu_context* new_ctx(int id)
{
    spu_context* c = (spu_context*)calloc(1, sizeof(spu_context));
    assert(c);
    spu_context_init(c, id);
    c->image_id = 7; c->spu_id = (uint32_t)id;
    return c;
}

typedef struct { spu_context* c; uint32_t parity, n; } hop_arg;
static void* hop_thread(void* p)
{
    hop_arg* a = (hop_arg*)p;
    for (uint32_t k = a->parity; k < a->n; k += 2) {
        for (;;) {   /* poll the counter as a job manager does */
            getllar(a->c, LS_POLL, LINE);
            if (spu_ls_read32(a->c, LS_POLL) == k) break;
            YieldProcessor();
        }
        job(a->c, k, LINE);
    }
    return NULL;
}

#define TIME(label, n, body) do { double t0_ = now_s(); for (uint32_t i = 0; i < (n); i++) { body; } \
    printf(" %s=%.1f", label, (now_s() - t0_) * 1e9 / (n)); } while (0)

int main(int argc, char** argv)
{
    const uint32_t n = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 0) : 200000u;
    assert(n >= 16);
    vm_base = (uint8_t*)calloc(1, GUEST);
    assert(vm_base);
    { extern uint8_t g_vm_page_bitmap[65536 / 8]; memset(g_vm_page_bitmap, 255, sizeof g_vm_page_bitmap); }
    for (uint32_t i = 0; i < 0x30000; i++) vm_base[DESC + i] = (uint8_t)(i * 131u + 7u);
    spu_context* c = new_ctx(0);
    for (uint32_t i = 0; i < 1000; i++) job(c, i, LINE + 256);   /* warm */

    printf("ns:");
    TIME("wrch_param", n, ch_w(c, MFC_LSA, LS_DESC));
    TIME("tag_wait", n, tag_wait(c, 2));
    TIME("get128_and_wait", n, { dma(c, MFC_GET_CMD, LS_DESC, DESC + (i & 63) * 128, 128, 1); tag_wait(c, 2); });
    TIME("getllar_putllc", n, (void)atomic_inc(c, LINE + 128));
    TIME("job", n, job(c, i, 0));
    TIME("job_with_completion", n, job(c, i, LINE + 256));
    TIME("memcpy_floor", n, {
        memcpy(c->ls + LS_DESC, vm_base + DESC + (i & 63) * 128, 128);
        memcpy(c->ls + LS_IN, vm_base + IN + (i & 15) * 0x800, 2048);
        for (uint32_t o = 0; o < 512; o += 4)
            spu_ls_write32(c, LS_OUT + o, spu_ls_read32(c, LS_DESC + (o & 127)) ^ spu_ls_read32(c, LS_IN + o) ^ i);
        memcpy(vm_base + OUT + (i & 15) * 0x200, c->ls + LS_OUT, 512);
        __asm__ volatile("" ::: "memory"); });
    assert(be32(vm_base + LINE + 128) == n + 0 && be32(vm_base + LINE + 256) == 1000 + n);

    memset(vm_base + LINE, 0, 128);
    const uint32_t hops = n / 4;
    hop_arg a = { new_ctx(1), 0, hops }, b = { new_ctx(2), 1, hops };
    pthread_t ta, tb;
    double t0 = now_s();
    assert(!pthread_create(&ta, NULL, hop_thread, &a) && !pthread_create(&tb, NULL, hop_thread, &b));
    assert(!pthread_join(ta, NULL) && !pthread_join(tb, NULL));
    printf(" hop_two_threads=%.1f\n", (now_s() - t0) * 1e9 / hops);
    assert(be32(vm_base + LINE) == hops);
    return 0;
}
