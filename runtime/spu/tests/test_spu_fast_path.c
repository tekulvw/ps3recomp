/* Synthetic check of the compact MFC/channel form (PS3RECOMP_MFC_FAST=1,
 * spu_context.h spu_fast_path_on), run by tools/test_spu_fast_path.py in both
 * forms: the same DMA sequence must leave local store, guest memory, tag
 * status and the atomic status identical, malformed and null-page transfers
 * must be refused the same way, and a plain PUT over a line another context
 * holds a reservation on must break that reservation in both forms. The
 * fixture prints a digest of every result for the runner to compare. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
#include "../spu_channels.c"
uint32_t g_barrier_sync_watch, g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
unsigned long long ps3_ms_now(void) { return 1000; }
void spurs_ef_set_from_spu(uint32_t a, uint16_t b) { (void)a; (void)b; }
uint64_t spurs_pm_build_context(uint8_t* a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e; return 0;
}
void ydkj_wake_all_event_flags(void) {}

static uint64_t digest = 1469598103934665603ull;
static void mix(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { digest ^= b[i]; digest *= 1099511628211ull; }
}
static void mix32(uint32_t v) { mix(&v, 4); }

static void wr(spu_context* c, uint32_t ch, uint32_t v) { spu_wrch(c, ch, spu_make_preferred_u32(v)); }
static uint32_t rd(spu_context* c, uint32_t ch) { return spu_rdch(c, ch)._u32[0]; }

/* One MFC command as a job issues it: five parameter latches, the command,
 * then wait for the tag and read the status. */
static uint32_t dma(spu_context* c, uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t tag)
{
    wr(c, MFC_LSA, lsa); wr(c, MFC_EAH, 0); wr(c, MFC_EAL, ea); wr(c, MFC_Size, size);
    wr(c, MFC_TagID, tag); wr(c, MFC_Cmd, cmd);
    wr(c, MFC_WrTagMask, 1u << tag); wr(c, MFC_WrTagUpdate, MFC_TAG_UPDATE_ALL);
    uint32_t st = rd(c, MFC_RdTagStat);
    mix32(st);
    return st;
}

int main(void)
{
    vm_base = calloc(1, 1 << 20); assert(vm_base);
    ppu_vm_size = 1 << 20;
    spu_context* a = calloc(1, sizeof(*a)); spu_context* b = calloc(1, sizeof(*b));
    assert(a && b);
    spu_context_init(a, 0); spu_context_init(b, 1);
    printf("fast=%d\n", spu_fast_path_on());

    /* A patterned 64 KiB source region well above the null page. */
    const uint32_t src = 0x40000, dst = 0x60000;
    for (uint32_t i = 0; i < 0x10000; ++i) vm_base[src + i] = (uint8_t)(i * 7 + (i >> 8));

    /* GET 16 KiB, PUT it elsewhere, then a few small transfers at odd sizes. */
    assert(dma(a, MFC_GET_CMD, 0x1000, src, 0x4000, 3) == (1u << 3));
    assert(memcmp(&a->ls[0x1000], vm_base + src, 0x4000) == 0);
    assert(dma(a, MFC_PUT_CMD, 0x1000, dst, 0x4000, 5) == (1u << 5));
    assert(memcmp(vm_base + dst, vm_base + src, 0x4000) == 0);
    for (uint32_t s = 1; s <= 8; s <<= 1) assert(dma(a, MFC_GET_CMD, 0x8000 + s * 16, src + 0x100 + s * 16, s, 1) == 2u);
    assert(dma(a, MFC_PUT_CMD, 0x1000, dst + 0x4000, 0x3FF0, 7) == (1u << 7));

    /* Refused: a PUT with a size that is not a multiple of 16, and a PUT into
     * the null page. Neither may move data. */
    memset(vm_base + 0x80000, 0xAA, 0x100);
    dma(a, MFC_PUT_CMD, 0x1000, 0x80000, 24, 2);
    for (uint32_t i = 0; i < 0x100; ++i) assert(vm_base[0x80000 + i] == 0xAA);
    dma(a, MFC_PUT_CMD, 0x1000, 0x8000, 0x100, 2);
    for (uint32_t i = 0; i < 0x100; ++i) assert(vm_base[0x8000 + i] == 0);
    /* An EA outside committed memory: refused too. */
    dma(a, MFC_GET_CMD, 0x2000, 0x900000, 0x100, 4);

    /* Reservation: b takes a line, a's plain PUT covers it, b's conditional
     * store must then fail (atomic status 1); without the PUT it succeeds. */
    const uint32_t line = 0x70000;
    wr(b, MFC_LSA, 0x3000); wr(b, MFC_EAH, 0); wr(b, MFC_EAL, line); wr(b, MFC_Cmd, MFC_GETLLAR_CMD);
    rd(b, MFC_RdAtomicStat);
    wr(b, MFC_LSA, 0x3000); wr(b, MFC_EAH, 0); wr(b, MFC_EAL, line); wr(b, MFC_Cmd, MFC_PUTLLC_CMD);
    const uint32_t ok = rd(b, MFC_RdAtomicStat);
    wr(b, MFC_LSA, 0x3000); wr(b, MFC_EAH, 0); wr(b, MFC_EAL, line); wr(b, MFC_Cmd, MFC_GETLLAR_CMD);
    rd(b, MFC_RdAtomicStat);
    assert(dma(a, MFC_PUT_CMD, 0x1000, line - 0x80, 0x200, 6) == (1u << 6));   /* spans the reserved line */
    wr(b, MFC_LSA, 0x3000); wr(b, MFC_EAH, 0); wr(b, MFC_EAL, line); wr(b, MFC_Cmd, MFC_PUTLLC_CMD);
    const uint32_t lost = rd(b, MFC_RdAtomicStat);
    printf("putllc held=%u lost=%u\n", ok, lost);
    mix32(ok); mix32(lost);

    /* Reads of latched state through the channel entry. */
    mix32(rd(a, MFC_RdTagMask)); mix32(rd(a, SPU_RdMachStat)); mix32(rd(a, MFC_RdAtomicStat));
    a->int_enable = 1; mix32(rd(a, SPU_RdMachStat)); a->int_enable = 0;

    /* The verified-cover test's two forms, compared in one process: mark a
     * few ranges, then sweep every span start and several lengths across
     * the map's byte boundaries with the gate forced each way. */
    {
        const int saved = g_spu_dispatch_gates.fast_path;
        spu_verified_cover_mark(0x0A00, 0x80);          /* one 128-byte unit        */
        spu_verified_cover_mark(0x2000, 0x1000);        /* a run of 32 units        */
        spu_verified_cover_mark(0x3FF80, 0x80);         /* the last unit of LS      */
        spu_verified_cover_mark(0x12345, 0x3);          /* an unaligned sliver      */
        unsigned agree = 0, hits = 0;
        static const uint32_t sizes[] = { 1, 4, 16, 0x7F, 0x80, 0x81, 0x100, 0x3FF, 0x400, 0x1000, 0x4000 };
        for (uint32_t lsa = 0; lsa < SPU_LS_SIZE; lsa += 0x40)
            for (unsigned si = 0; si < sizeof sizes / sizeof sizes[0]; ++si) {
                g_spu_dispatch_gates.fast_path = 0; const int slow = spu_verified_cover_any(lsa, sizes[si]);
                g_spu_dispatch_gates.fast_path = 1; const int fast = spu_verified_cover_any(lsa, sizes[si]);
                assert(slow == fast);
                agree++; hits += slow;
            }
        g_spu_dispatch_gates.fast_path = saved;
        printf("cover sweep=%u hits=%u\n", agree, hits);
        mix32(hits);
    }

    mix(a->ls, SPU_LS_SIZE); mix(b->ls, SPU_LS_SIZE); mix(vm_base, 1 << 20);
    mix32(a->mfc_tag_mask); mix32(a->mfc_tag_status); mix32(a->mfc_lsa); mix32(a->mfc_eal); mix32(a->mfc_size); mix32(a->mfc_tag);
    printf("digest=%016llx\n", (unsigned long long)digest);
    free(a); free(b);
    return 0;
}
