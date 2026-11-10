/*
 * test_pamf_uninit_reader - cellPamf calls on a reader that was never
 * successfully initialised.
 *
 * A title can ask a PAMF reader for its presentation start time before
 * cellPamfReaderInitialize has run on it, for example when its movie never
 * opened. This runtime keeps the reader in guest memory and re-validates it
 * (header pointer, nested lengths, stream cursor) on every call, so a reader
 * that was never initialised, or whose Initialize failed, is refused with
 * CELL_PAMF_ERROR_INVALID_ARG or CELL_PAMF_ERROR_INVALID_HEADER instead of
 * being read through.
 *
 * Checks, all through guest EAs:
 *   - each reader call on an EA Initialize never saw fails cleanly and
 *     leaves its output untouched;
 *   - a reader whose Initialize failed (bad magic) stays invalid.
 * Well-formed headers are covered by tests/test_pamf_reader.cpp.
 *
 *   cc -std=gnu17 -I include -o test_pamf_uninit_reader libs/codec/tests/test_pamf_uninit_reader.c
 * Driven by tools/test_pamf_uninit_reader.py.
 */
#include "../cellPamf.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           fflush(stdout); if (!(c)) fails++; } while (0)

/* A refused reader: bad argument or a reader record that does not validate. */
#define INVALID_R(r) ((r) == (s32)CELL_PAMF_ERROR_INVALID_ARG || (r) == (s32)CELL_PAMF_ERROR_INVALID_HEADER)
#define EA(x)   ((CellPamfReader*)(uintptr_t)(x))
#define OUT(x)  ((void*)(uintptr_t)(x))

static const u32 READER_NEW  = 0x1000;   /* never initialised */
static const u32 READER_BAD  = 0x1080;   /* Initialize failed */
static const u32 OUT_EA      = 0x2000;   /* output scratch */
static const u32 BAD_EA      = 0x20000;  /* header with the wrong magic */

static void put32(u32 ea, u32 v) { vm_write32(ea, v); }
static void put16(u32 ea, u16 v) { vm_base[ea] = (u8)(v >> 8); vm_base[ea + 1] = (u8)v; }

/* A header whose magic is wrong; Initialize must refuse it. */
static void build_header(void)
{
    memset(vm_base + BAD_EA, 0, 0x800);
    put32(BAD_EA, 0x4E4F5045);  /* "NOPE" */
    put32(BAD_EA + 4, 0x30303431);
}

static void poison_out(void) { memset(vm_base + OUT_EA, 0xAA, 0x100); }
static int out_untouched(void)
{
    for (int i = 0; i < 0x100; i++)
        if (vm_base[OUT_EA + i] != 0xAA) return 0;
    return 1;
}

static void expect_invalid(u32 reader, const char* what)
{
    s32 r;
    poison_out();
    r = cellPamfReaderGetPresentationStartTime(EA(reader), OUT(OUT_EA));
    CHECK(INVALID_R(r) && out_untouched(), "%s: GetPresentationStartTime -> 0x%08X", what, (u32)r);
    poison_out();
    r = cellPamfReaderGetPresentationEndTime(EA(reader), OUT(OUT_EA));
    CHECK(INVALID_R(r) && out_untouched(), "%s: GetPresentationEndTime -> 0x%08X", what, (u32)r);
    u32 mux = cellPamfReaderGetMuxRateBound(EA(reader));   /* the error code, not a rate */
    CHECK(INVALID_R((s32)mux), "%s: GetMuxRateBound -> 0x%08X", what, mux);
    r = cellPamfReaderGetNumberOfStreams(EA(reader));
    CHECK(INVALID_R(r), "%s: GetNumberOfStreams -> 0x%08X", what, (u32)r);
    r = cellPamfReaderGetNumberOfSpecificStreams(EA(reader), CELL_PAMF_STREAM_TYPE_AVC);
    CHECK(INVALID_R(r), "%s: GetNumberOfSpecificStreams -> 0x%08X", what, (u32)r);
    r = cellPamfReaderSetStreamWithType(EA(reader), CELL_PAMF_STREAM_TYPE_AVC, 0);
    CHECK(INVALID_R(r), "%s: SetStreamWithType -> 0x%08X", what, (u32)r);
    r = cellPamfReaderSetStreamWithTypeAndChannel(EA(reader), CELL_PAMF_STREAM_TYPE_AVC, 0);
    CHECK(r != CELL_OK, "%s: SetStreamWithTypeAndChannel -> 0x%08X", what, (u32)r);
    r = cellPamfReaderSetStreamWithIndex(EA(reader), 0);
    CHECK(INVALID_R(r), "%s: SetStreamWithIndex -> 0x%08X", what, (u32)r);
    r = cellPamfReaderGetStreamIndex(EA(reader));
    CHECK(INVALID_R(r), "%s: GetStreamIndex -> 0x%08X", what, (u32)r);
    poison_out();
    r = cellPamfReaderGetStreamInfo(EA(reader), OUT(OUT_EA), 0x40);
    CHECK(INVALID_R(r) && out_untouched(), "%s: GetStreamInfo -> 0x%08X", what, (u32)r);
    r = (s32)cellPamfReaderGetNumberOfEp(EA(reader));   /* the error code, not a count */
    CHECK(INVALID_R(r), "%s: GetNumberOfEp -> 0x%08X", what, (u32)r);
    poison_out();
    r = cellPamfReaderGetEp(EA(reader), 0, OUT(OUT_EA));
    /* The legacy GetEp is NOT_AVAILABLE for every reader here; it must fail without writing. */
    CHECK(r != CELL_OK && out_untouched(), "%s: GetEp -> 0x%08X", what, (u32)r);
}

int main(void)
{
    ppu_vm_size = 0x40000;
    vm_base = calloc(1, ppu_vm_size);
    if (!vm_base) return 2;
    build_header();

    /* Before any Initialize at all. */
    expect_invalid(READER_NEW, "never initialised");

    /* A failed Initialize leaves that reader invalid. */
    s32 r = cellPamfReaderInitialize(EA(READER_BAD), OUT(BAD_EA), 0x800, 0);
    CHECK(r != CELL_OK, "bad magic: Initialize -> 0x%08X", (u32)r);
    expect_invalid(READER_BAD, "failed Initialize");
    expect_invalid(READER_NEW, "never initialised, after a failed one");

    printf(fails ? "FAIL: %d check(s)\n" : "PASS: cellPamf reader calls on uninitialised readers\n", fails);
    return fails ? 1 : 0;
}
