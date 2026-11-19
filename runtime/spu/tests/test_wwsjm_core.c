/* Synthetic tests of the native WWS job manager core (runtime/spu/wwsjm/wwsjm_core.c).
 *
 * Every input is generated here: hand-computed vectors from the behavioural
 * specification's test section plus generated property checks. No game data.
 * Built and run by tools/test_wwsjm_core.py (which also runs the mutants). */
#include "../wwsjm/wwsjm_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static const char* group = "";

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            failures++;                                                               \
            fprintf(stderr, "FAIL [%s] %s:%d: %s\n", group, __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

#define LIST_EA   0x10000000u
#define ARRAY_EA  0x10010000u
#define SPURS_EA  0x20000000u

/* ---- builders ------------------------------------------------------------- */

static void line_blank(uint8_t l[WWSJM_LINE_BYTES])
{
    memset(l, 0, WWSJM_LINE_BYTES);
    wwsjm_put32(l + 0x00, 0x00100000u);
    wwsjm_put32(l + 0x04, 0x3000u);
    wwsjm_put16(l + 0x08, 0xABCD);              /* bytes the manager must preserve */
    wwsjm_put16(l + 0x0C, 0x1234);
    wwsjm_put16(l + 0x0E, 0xFFFF);
    for (unsigned a = 0; a < WWSJM_ARRAYS; a++)
        for (unsigned s = 0; s < WWSJM_LANES; s++)
            wwsjm_put16(l + 0x44 + 16 * a + 2 * s, WWSJM_NO_MARKER);
}

static void line_array(uint8_t l[WWSJM_LINE_BYTES], unsigned a, uint32_t mask, uint32_t ea)
{
    wwsjm_put32(l + 0x10 + 4 * a, 1);
    wwsjm_put32(l + 0x20 + 4 * a, mask);
    wwsjm_put32(l + 0x30 + 4 * a, ea);
}

/* arr a: S N l0..l5 */
static void line_markers(uint8_t l[WWSJM_LINE_BYTES], unsigned a, const uint16_t hw[8])
{
    for (unsigned i = 0; i < 8; i++)
        wwsjm_put16(l + 0x40 + 16 * a + 2 * i, hw[i]);
}

static int markers_are(const uint8_t l[WWSJM_LINE_BYTES], unsigned a, const uint16_t hw[8])
{
    for (unsigned i = 0; i < 8; i++)
        if (wwsjm_be16(l + 0x40 + 16 * a + 2 * i) != hw[i])
            return 0;
    return 1;
}

/* bytes outside [lo, hi) and outside the extra halfwords listed are unchanged */
static int same_except(const uint8_t* a, const uint8_t* b, unsigned lo, unsigned hi, unsigned x1,
                       unsigned x2)
{
    for (unsigned i = 0; i < WWSJM_LINE_BYTES; i++) {
        if (i >= lo && i < hi)
            continue;
        if (i == x1 || i == x1 + 1 || i == x2 || i == x2 + 1)
            continue;
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

static void header(uint8_t h[8], uint16_t size, uint16_t type, uint32_t ea)
{
    wwsjm_put16(h, size);
    wwsjm_put16(h + 2, type);
    wwsjm_put32(h + 4, ea);
}

static void cmd(uint8_t* rec, unsigned slot, uint32_t w0, uint32_t w1)
{
    wwsjm_put32(rec + 8 * slot, w0);
    wwsjm_put32(rec + 8 * slot + 4, w1);
}

static uint32_t rng_state = 0x2545F491u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ---- JL: job list transactions ------------------------------------------------ */

static void test_jl1_fresh_claim(void)
{
    group = "JL-1";
    uint8_t l[128], out[128], h8[8];
    line_blank(l);
    wwsjm_put16(l + 0x0A, 3);
    line_array(l, 3, 0xF, ARRAY_EA);
    const uint16_t m[8] = {3, 3, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    line_markers(l, 3, m);
    wwsjm_line dl;
    wwsjm_line_decode(l, &dl);
    wwsjm_cursor c;
    CHECK(wwsjm_cursor_of(&dl, &c) == WWSJM_OK);
    CHECK(c.array == 3 && c.index == 3 && c.header_ea == ARRAY_EA + 8 * 3);
    header(h8, 0x00D0, 1, 0x02010000u);
    wwsjm_header h;
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_OK);
    CHECK(h.size == 0xD0 && h.type == 1 && h.ea == 0x02010000u);
    CHECK(wwsjm_judge(&h, 0) == WWSJM_TAKE);
    CHECK(wwsjm_claim(l, 0, &h, out) == WWSJM_OK);
    const uint16_t want[8] = {4, 4, 3, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    CHECK(markers_are(out, 3, want));
    CHECK(same_except(l, out, 0x70, 0x80, 999, 999));
    wwsjm_context_args a = {0};
    a.lane = 0; a.dma_tag = 8; a.spurs_ea = SPURS_EA; a.wid = 6; a.index = c.index; a.array = 3;
    a.list_ea = LIST_EA; a.list_tag = 0x0A000000u; a.userdata_ls = 0x2000 + 0x60;
    uint8_t ctx[WWSJM_CONTEXT_BYTES];
    wwsjm_context_build(&a, ctx);
    CHECK(wwsjm_be32(ctx + 0x10) == (6u << 16 | 3));
}

static void jl2_line(uint8_t l[128])
{
    line_blank(l);
    wwsjm_put16(l + 0x0A, 3);
    line_array(l, 3, 0xFF, ARRAY_EA);
    const uint16_t m[8] = {0x30, 4, 3, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    line_markers(l, 3, m);
}

static void test_jl2_counted_claim(void)
{
    group = "JL-2";
    uint8_t l[128], out[128], h8[8];
    jl2_line(l);
    header(h8, 0x0080, 1, 0x02000100u);
    wwsjm_header h;
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_OK);
    CHECK(wwsjm_claim(l, 0, &h, out) == WWSJM_OK);
    const uint16_t want[8] = {0x30, 5, 3, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    CHECK(markers_are(out, 3, want));
    uint32_t r[2];
    wwsjm_finish_result(&h, r);
    CHECK(r[0] == 0x00800001u && r[1] == 0x02000100u);
    wwsjm_finish_result(NULL, r);
    CHECK(r[0] == 0 && r[1] == 0xFFFFFFFFu);
    wwsjm_line dl;
    wwsjm_line_decode(out, &dl);
    CHECK(wwsjm_ready_value(&dl, WWSJM_TAKE, 5) == 5);         /* S - N = 0x2B, next a job */
    CHECK(wwsjm_ready_value(&dl, WWSJM_STOP_EMPTY, 5) == 0);
    CHECK(wwsjm_ready_value(&dl, WWSJM_STOP_BLOCKED, 5) == 0);
}

static void test_jl3_retire(void)
{
    group = "JL-3";
    uint8_t l[128], out[128];
    jl2_line(l);
    wwsjm_put16(l + 0x72, 5);                    /* after JL-2 */
    wwsjm_held next = {1, 3, 4};
    CHECK(wwsjm_retire(l, 3, 0, &next, out) == WWSJM_OK);
    const uint16_t want[8] = {0x30, 5, 4, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    CHECK(markers_are(out, 3, want));
    CHECK(same_except(l, out, 0x74, 0x76, 999, 999));
    CHECK(wwsjm_retire(l, 3, 0, NULL, out) == WWSJM_OK);
    const uint16_t none[8] = {0x30, 5, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    CHECK(markers_are(out, 3, none));
    /* the next held job on another array leaves no marker on this one */
    wwsjm_held other = {1, 2, 9};
    CHECK(wwsjm_retire(l, 3, 0, &other, out) == WWSJM_OK);
    CHECK(markers_are(out, 3, none));
    CHECK(wwsjm_retire(l, 3, WWSJM_LANES, NULL, out) == WWSJM_E_ARG);
}

static void test_jl4_gates(void)
{
    group = "JL-4";
    uint8_t l[128], out[128], h8[8];
    line_blank(l);
    wwsjm_put16(l + 0x0A, 3);
    line_array(l, 3, 0xFF, ARRAY_EA);
    const uint16_t m[8] = {0x12, 0x12, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    line_markers(l, 3, m);
    for (uint16_t type = 2; type <= 5; type += 3) {
        header(h8, 0, type, 0x03000004u);
        wwsjm_header h;
        CHECK(wwsjm_header_decode(h8, &h) == WWSJM_OK);
        CHECK(wwsjm_header_is_gate(&h));
        CHECK(wwsjm_judge(&h, 1) == WWSJM_STOP_BLOCKED);
        CHECK(wwsjm_judge(&h, 0xFFFF) == WWSJM_STOP_BLOCKED);
        wwsjm_line dl;
        wwsjm_line_decode(l, &dl);
        CHECK(wwsjm_ready_value(&dl, wwsjm_judge(&h, 1), 5) == 0);
        CHECK(wwsjm_judge(&h, 0) == WWSJM_TAKE);
        CHECK(wwsjm_claim(l, 0, &h, out) == WWSJM_OK);
        const uint16_t want[8] = {0x13, 0x13, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
        CHECK(markers_are(out, 3, want));
    }
}

static void test_jl5_switch(void)
{
    group = "JL-5";
    uint8_t l[128], out[128], h8[8];
    line_blank(l);
    for (unsigned a = 0; a < 4; a++)
        line_array(l, a, 0xFF, ARRAY_EA + 0x1000 * a);
    wwsjm_put16(l + 0x0A, 1);
    wwsjm_put16(l + 0x0E, 0xFFDE);
    const uint16_t m[8] = {0xFF, 0xFE, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    line_markers(l, 1, m);
    header(h8, 0, 4, 0);
    wwsjm_header h;
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_OK);
    CHECK(wwsjm_judge(&h, 0) == WWSJM_TAKE);
    CHECK(wwsjm_claim(l, 2, &h, out) == WWSJM_OK);
    const uint16_t want[8] = {0xFF, 0xFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    CHECK(markers_are(out, 1, want));
    CHECK(wwsjm_be16(out + 0x0A) == 2);
    CHECK(wwsjm_be16(out + 0x0E) == 0xFFDD);
    CHECK(wwsjm_be16(out + 0x08) == 0xABCD && wwsjm_be16(out + 0x0C) == 0x1234);
    CHECK(same_except(l, out, 0x50, 0x60, 0x0A, 0x0E));
    wwsjm_line dl;
    wwsjm_line_decode(out, &dl);
    wwsjm_cursor c;
    CHECK(wwsjm_cursor_of(&dl, &c) == WWSJM_OK && c.array == 2);
    /* 3 -> 0 */
    wwsjm_put16(l + 0x0A, 3);
    line_markers(l, 3, m);
    CHECK(wwsjm_claim(l, 2, &h, out) == WWSJM_OK);
    CHECK(wwsjm_be16(out + 0x0A) == 0);
}

static void test_jl6_nop(void)
{
    group = "JL-6";
    uint8_t l[128], out[128], h8[8];
    line_blank(l);
    line_array(l, 0, 0xF, ARRAY_EA);
    const uint16_t m[8] = {4, 4, 0xFFFF, 0, 2, 3, 1, 0xFFFF};
    line_markers(l, 0, m);
    header(h8, 0, 3, 0);
    wwsjm_header h;
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_OK);
    for (unsigned lane = 0; lane < WWSJM_LANES; lane++) {
        CHECK(wwsjm_claim(l, lane, &h, out) == WWSJM_OK);
        const uint16_t want[8] = {5, 5, 0xFFFF, 0, 2, 3, 1, 0xFFFF};
        CHECK(markers_are(out, 0, want));
    }
}

static void test_jl7_wrap(void)
{
    group = "JL-7";
    uint8_t l[128];
    line_blank(l);
    line_array(l, 0, 0xF, ARRAY_EA);
    wwsjm_line dl;
    wwsjm_line_decode(l, &dl);
    CHECK(wwsjm_header_ea(&dl, 0, 0x13) == ARRAY_EA + 8 * 3);
    CHECK(wwsjm_header_ea(&dl, 0, 0x0F) == ARRAY_EA + 8 * 0xF);
    /* capacities need not be powers of two: the slot is index mod capacity */
    line_array(l, 1, 0x1FEF, ARRAY_EA + 0x10000);
    line_array(l, 2, 0x13FF, ARRAY_EA + 0x20000);
    wwsjm_line_decode(l, &dl);
    CHECK(wwsjm_header_ea(&dl, 1, 0x10) == ARRAY_EA + 0x10000 + 0x80);
    CHECK(wwsjm_header_ea(&dl, 1, 0x1FF0) == ARRAY_EA + 0x10000);
    CHECK(wwsjm_header_ea(&dl, 1, 0x1FF5) == ARRAY_EA + 0x10000 + 8 * 5);   /* spec JL-7 */
    CHECK(wwsjm_header_ea(&dl, 2, 0x0C00) == ARRAY_EA + 0x20000 + 8 * 0xC00);
    CHECK(wwsjm_header_ea(&dl, 2, 0x1400) == ARRAY_EA + 0x20000);
    /* a detached current array has no cursor */
    wwsjm_put16(l + 0x0A, 3);
    wwsjm_line_decode(l, &dl);
    wwsjm_cursor c;
    CHECK(wwsjm_cursor_of(&dl, &c) == WWSJM_E_DETACHED);
    /* a capacity field of 0xFFFFFFFF (capacity 2^32) is malformed, not a raw index */
    line_array(l, 3, 0xFFFFFFFFu, ARRAY_EA + 0x30000);
    wwsjm_line_decode(l, &dl);
    CHECK(wwsjm_cursor_of(&dl, &c) == WWSJM_E_DETACHED);
    CHECK(wwsjm_header_ea(&dl, 3, 5) == 0);
    /* capacity 1 is legal: every index is slot 0 */
    line_array(l, 3, 0, ARRAY_EA + 0x30000);
    wwsjm_line_decode(l, &dl);
    CHECK(wwsjm_cursor_of(&dl, &c) == WWSJM_OK && c.header_ea == ARRAY_EA + 0x30000);
}

/* generated: the S rule, lane rule and byte preservation on random lines */
static void test_claim_properties(void)
{
    group = "claim-gen";
    for (unsigned iter = 0; iter < 20000; iter++) {
        uint8_t l[128], out[128];
        for (unsigned i = 0; i < 128; i++)
            l[i] = (uint8_t)rnd();
        unsigned a = rnd() % 4, lane = rnd() % WWSJM_LANES;
        wwsjm_put16(l + 0x0A, (uint16_t)a);
        wwsjm_header h = {0x80, (uint16_t)(1 + rnd() % 5), 0x100000u};
        if (iter % 3 == 0)
            wwsjm_put16(l + 0x44 + 16 * a + 2 * lane, WWSJM_NO_MARKER);
        uint16_t S = wwsjm_be16(l + 0x40 + 16 * a), N = wwsjm_be16(l + 0x42 + 16 * a);
        uint16_t mine = wwsjm_be16(l + 0x44 + 16 * a + 2 * lane);
        CHECK(wwsjm_claim(l, lane, &h, out) == WWSJM_OK);
        uint16_t N1 = (uint16_t)(N + 1);
        CHECK(wwsjm_be16(out + 0x42 + 16 * a) == N1);
        uint16_t S1 = wwsjm_be16(out + 0x40 + 16 * a);
        if ((int16_t)(uint16_t)(S - N1) < 0)
            CHECK(S1 == N1);
        else
            CHECK(S1 == S);
        uint16_t mine1 = wwsjm_be16(out + 0x44 + 16 * a + 2 * lane);
        if (h.type == 1 && mine == WWSJM_NO_MARKER)
            CHECK(mine1 == N);
        else
            CHECK(mine1 == mine);
        unsigned lo = 0x40 + 16 * a;
        if (h.type == 4)
            CHECK(same_except(l, out, lo, lo + 16, 0x0A, 0x0E));
        else
            CHECK(same_except(l, out, lo, lo + 16, 999, 999));
        for (unsigned s = 0; s < WWSJM_LANES; s++)          /* other lanes untouched */
            if (s != lane)
                CHECK(wwsjm_be16(out + 0x44 + 16 * a + 2 * s) == wwsjm_be16(l + 0x44 + 16 * a + 2 * s));
    }
    /* modular: S just behind a wrapping N */
    uint8_t l[128], out[128];
    line_blank(l);
    line_array(l, 0, 0xFF, ARRAY_EA);
    const uint16_t m[8] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    line_markers(l, 0, m);
    wwsjm_header h = {0, 3, 0};
    CHECK(wwsjm_claim(l, 0, &h, out) == WWSJM_OK);
    CHECK(wwsjm_be16(out + 0x40) == 0 && wwsjm_be16(out + 0x42) == 0);
    const uint16_t m2[8] = {0x0002, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    line_markers(l, 0, m2);
    CHECK(wwsjm_claim(l, 0, &h, out) == WWSJM_OK);
    CHECK(wwsjm_be16(out + 0x40) == 2 && wwsjm_be16(out + 0x42) == 0);
    /* nothing published: not claimable */
    wwsjm_header empty = {0, 0, 0};
    CHECK(wwsjm_judge(&empty, 0) == WWSJM_STOP_EMPTY);
    CHECK(wwsjm_claim(l, 0, &empty, out) == WWSJM_E_ARG);
    CHECK(wwsjm_claim(l, WWSJM_LANES, &h, out) == WWSJM_E_ARG);
}

static void test_headers(void)
{
    group = "header";
    uint8_t h8[8];
    wwsjm_header h;
    header(h8, 0x0080, 6, 0x100);
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_E_ENTRY_TYPE);
    header(h8, 0x1080, 1, 0x100);
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_E_HEADER);
    header(h8, 0x0080, 1, 0x108);
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_E_HEADER);
    header(h8, 0x0000, 1, 0x100);
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_E_HEADER);
    header(h8, 0x0110, 1, 0x100);
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_E_HEADER);
    header(h8, 0x0088, 1, 0x100);
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_E_HEADER);
    header(h8, 0, 2, 0x101);
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_E_HEADER);
    header(h8, 0, 3, 0);
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_OK && !wwsjm_header_is_gate(&h));
    header(h8, 0x0100, 1, 0x7F00);
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_OK && h.size == 0x100);
}

/* ---- DD / RC: counters and ready counts ------------------------------------- */

static void test_dd1(void)
{
    group = "DD-1";
    uint8_t l[128], out[128];
    memset(l, 0x5A, sizeof l);
    wwsjm_put16(l + 0x44, 0x0001);
    wwsjm_put16(l + 0x46, 0x0605);
    uint16_t dv = 99;
    CHECK(wwsjm_decrement(l, 0x44, out, &dv) == WWSJM_OK && dv == 0);
    CHECK(wwsjm_be16(out + 0x44) == 0);
    CHECK(same_except(l, out, 0x44, 0x46, 999, 999));
    unsigned wid;
    uint8_t v;
    wwsjm_wake_target(wwsjm_be16(out + 0x46), &wid, &v);
    CHECK(wid == 6 && v == 5);
    uint8_t inst[128], inst2[128];
    for (unsigned i = 0; i < 128; i++)
        inst[i] = (uint8_t)(i * 7);
    CHECK(wwsjm_ready_apply(inst, wid, v, inst2) == WWSJM_OK);
    CHECK(inst2[6] == 5);
    CHECK(same_except(inst, inst2, 6, 7, 999, 999));
    wwsjm_put16(l + 0x44, 0x0004);
    CHECK(wwsjm_decrement(l, 0x44, out, &dv) == WWSJM_OK && dv == 3);
    wwsjm_put16(l + 0x44, 0x0000);
    CHECK(wwsjm_decrement(l, 0x44, out, &dv) == WWSJM_OK && dv == 0xFFFF);   /* wraps */
    CHECK(wwsjm_decrement(l, 0x7E, out, &dv) == WWSJM_OK);                    /* last u16 */
    CHECK(wwsjm_decrement(l, 0x45, out, &dv) == WWSJM_E_ARG);                 /* odd */
    CHECK(wwsjm_decrement(l, 0x80, out, &dv) == WWSJM_E_ARG);                 /* past the line */
}

static void test_rc(void)
{
    group = "RC";
    uint8_t inst[128], out[128];
    memset(inst, 0, sizeof inst);
    inst[0x26] = 4;
    CHECK(wwsjm_ready_apply(inst, 4, WWSJM_READY_ON_FLAG, out) == WWSJM_OK);   /* RC-1 */
    CHECK(out[4] == 8 && same_except(inst, out, 4, 5, 999, 999));
    CHECK(wwsjm_ready_apply(inst, 16, 1, out) == WWSJM_E_ARG);
    /* RC-2: idle */
    uint8_t l[128], h8[8];
    line_blank(l);
    line_array(l, 0, 0xFF, ARRAY_EA);
    const uint16_t m[8] = {7, 7, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    line_markers(l, 0, m);
    header(h8, 0, 0, 0);
    wwsjm_header h;
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_OK);
    wwsjm_line dl;
    wwsjm_line_decode(l, &dl);
    CHECK(wwsjm_judge(&h, 0) == WWSJM_STOP_EMPTY);
    CHECK(wwsjm_ready_value(&dl, WWSJM_STOP_EMPTY, 5) == 0);
    /* a claimable entry the publisher has not counted yet: 0 */
    CHECK(wwsjm_ready_value(&dl, WWSJM_TAKE, 5) == 0);
    /* counted: busy */
    wwsjm_put16(l + 0x40, 8);
    wwsjm_line_decode(l, &dl);
    CHECK(wwsjm_ready_value(&dl, WWSJM_TAKE, 5) == 5);
    /* S behind N in the modular sense: 0 */
    wwsjm_put16(l + 0x40, 0xFFF0);
    wwsjm_line_decode(l, &dl);
    CHECK(wwsjm_ready_value(&dl, WWSJM_TAKE, 5) == 0);
}

/* ---- CL: command lists ------------------------------------------------------- */

#define MOD_EA  0x01000000u
#define LIST_L  0x02000040u
#define DEP_Y   0x03000014u

static uint32_t cl1_record(uint8_t* rec)
{
    memset(rec, 0, 0x100);
    cmd(rec, 0, 0x00000200, 0x100C0100);    /* reserve set 0: pages 16..27 */
    cmd(rec, 1, 0x0B24A400, MOD_EA);        /* use set 0: read+shareable, 713 quadwords */
    cmd(rec, 2, 0x00000220, 0x1C110100);    /* reserve set 1: pages 28..44 */
    cmd(rec, 3, 0x0004C420, LIST_L);        /* use set 1: read+gather, 1 quadword list */
    cmd(rec, 4, 0x00000240, 0x2D100100);    /* reserve set 2 */
    cmd(rec, 5, 0x00000440, 0);             /* use set 2: uninitialised */
    cmd(rec, 6, 0x00000260, 0x3D8C0100);    /* reserve set 3 */
    cmd(rec, 7, 0x00000460, 0);
    cmd(rec, 8, 0x00000800, DEP_Y);         /* dependency */
    cmd(rec, 9, 0x00000600, 0xF);           /* unreserve */
    cmd(rec, 10, 0x00000A00, 0);            /* run */
    for (unsigned i = 0x60; i < 0x90; i++)  /* parameter data */
        rec[i] = (uint8_t)i;
    return 0x90;
}

static void check_cl1_plan(const wwsjm_plan* p)
{
    CHECK(p->dep_count == 1 && p->dep_ea[0] == DEP_Y);
    CHECK(p->unreserve_mask == 0xF);
    CHECK(p->userdata_offset == 0x60 && p->userdata_bytes == 0x30);
    CHECK(p->load_count == 2);
    int copy = -1, gather = -1;
    for (unsigned i = 0; i < p->load_count; i++) {
        if (p->load[i].fill == WWSJM_FILL_COPY)
            copy = (int)i;
        if (p->load[i].fill == WWSJM_FILL_GATHER)
            gather = (int)i;
    }
    CHECK(copy >= 0 && gather >= 0);
    if (copy >= 0) {
        CHECK(p->load[copy].ls == 0x4000 && p->load[copy].ea == MOD_EA);
        CHECK(p->load[copy].bytes == 0x2C90 && p->load[copy].shareable);
    }
    if (gather >= 0)
        CHECK(p->load[gather].ls == 0x7000 && p->load[gather].ea == LIST_L && p->load[gather].bytes == 0x10);
    uint8_t d[16];
    unsigned given = 0;
    uint8_t tag;
    CHECK(wwsjm_job_tag(&given, &tag) == WWSJM_OK && tag == 0x1F);
    CHECK(wwsjm_buffer_descriptor(p, 1, 0, tag, d) == WWSJM_OK);
    CHECK(wwsjm_be32(d) == 0x0100001Fu && wwsjm_be32(d + 4) == ((0x7000u / 4) << 16 | 0x4400 / 4));
    CHECK(wwsjm_be32(d + 8) == LIST_L && wwsjm_be32(d + 12) == 0x10);
    CHECK(wwsjm_job_tag(&given, &tag) == WWSJM_OK && tag == 0x1E);
    CHECK(wwsjm_buffer_descriptor(p, 2, 0, tag, d) == WWSJM_OK);
    CHECK(wwsjm_be32(d) == 0x0200001Eu && wwsjm_be32(d + 4) == ((0xB400u / 4) << 16 | 0x4000 / 4));
    CHECK(wwsjm_be32(d + 8) == 0 && wwsjm_be32(d + 12) == 0);
    CHECK(wwsjm_buffer_descriptor(p, 3, 0, 0, d) == WWSJM_OK);
    CHECK(wwsjm_be32(d) == 0x03000000u && wwsjm_be32(d + 4) == ((0xF400u / 4) << 16 | 0x23000 / 4));
    CHECK(wwsjm_buffer_descriptor(p, 4, 0, 0, d) == WWSJM_E_SET);
    CHECK(wwsjm_buffer_descriptor(p, 1, 1, 0, d) == WWSJM_E_SET);
}

static void test_cl1(void)
{
    group = "CL-1";
    uint8_t rec[0x100];
    uint32_t size = cl1_record(rec);
    wwsjm_plan p;
    unsigned bad = 99;
    CHECK(wwsjm_plan_commands(rec, size, 0x4000, 0x3E000, &p, &bad) == WWSJM_OK);
    check_cl1_plan(&p);
    wwsjm_context_args a = {0};
    a.lane = 2; a.dma_tag = 8; a.spurs_ea = SPURS_EA; a.wid = 6; a.index = 5; a.array = 3;
    a.dep_count = p.dep_count; a.list_ea = LIST_EA; a.list_tag = 0x0A000000u;
    a.userdata_ls = 0x2000 + p.userdata_offset;
    for (unsigned i = 0; i < WWSJM_SLOTS; i++)
        a.slot[i] = 0x900 + 16 * i;
    uint8_t ctx[WWSJM_CONTEXT_BYTES];
    wwsjm_context_build(&a, ctx);
    CHECK(wwsjm_be32(ctx + 0x00) == 2 && wwsjm_be32(ctx + 0x04) == 8 && wwsjm_be32(ctx + 0x08) == 0);
    CHECK(wwsjm_be32(ctx + 0x0C) == SPURS_EA && wwsjm_be32(ctx + 0x10) == 0x00060005u);
    CHECK(wwsjm_be32(ctx + 0x14) == 1 && wwsjm_be32(ctx + 0x18) == ((LIST_EA + 0x0A000000u) | 3));
    CHECK(wwsjm_be32(ctx + 0x1C) == 0x0006FFFFu);
    for (unsigned i = 0; i < 4; i++)
        CHECK(wwsjm_be32(ctx + 0x20 + 4 * i) == 0x2060);
    CHECK(wwsjm_be32(ctx + 0x34) == 0x910 && wwsjm_be32(ctx + 0x44) == 0x950 && wwsjm_be32(ctx + 0x48) == 0x960);
    for (unsigned i = 0x4C; i < WWSJM_CONTEXT_BYTES; i++)
        CHECK(ctx[i] == 0);
}

static void test_cl2(void)
{
    group = "CL-2";
    uint8_t rec[0x100];
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000200, 0x100C0100);
    cmd(rec, 1, 0x00000220, 0x1C110100);
    cmd(rec, 2, 0x00000240, 0x2D100100);
    cmd(rec, 3, 0x00000260, 0x3D8C0100);
    cmd(rec, 4, 0x0B24A400, MOD_EA);
    cmd(rec, 5, 0x0004C420, LIST_L);
    cmd(rec, 6, 0x00000440, 0);
    cmd(rec, 7, 0x00000460, 0);
    cmd(rec, 8, 0x00000800, DEP_Y);
    cmd(rec, 9, 0x00000600, 0xF);
    cmd(rec, 10, 0x00000A00, 0);
    wwsjm_plan p;
    CHECK(wwsjm_plan_commands(rec, 0x90, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    check_cl1_plan(&p);
}

static wwsjm_status plan_one(uint32_t w0, uint32_t w1, unsigned* bad)
{
    uint8_t rec[0x100];
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000200, 0x10040100);
    cmd(rec, 1, 0x00000220, 0x14040200);
    cmd(rec, 2, w0, w1);
    cmd(rec, 3, 0x00000A00, 0);
    wwsjm_plan p;
    return wwsjm_plan_commands(rec, 0x20, 0x4000, 0x3E000, &p, bad);
}

static void test_cl3_refusals(void)
{
    group = "CL-3";
    unsigned bad;
    CHECK(plan_one(0x00000C00, 0, &bad) == WWSJM_E_COMMAND && bad == 2);   /* end */
    CHECK(plan_one(0x00000E00, 0, &bad) == WWSJM_E_COMMAND && bad == 2);   /* invalid */
    CHECK(plan_one(0x00000000, 0, &bad) == WWSJM_OK);                      /* no-op: skipped */
    /* set-0 pages reaching into the native context area */
    uint8_t rec[0x100];
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000200, 0xF0100100);
    cmd(rec, 1, 0x00000A00, 0);
    wwsjm_plan p;
    CHECK(wwsjm_plan_commands(rec, 0x10, 0x4000, 0x3E000, &p, &bad) == WWSJM_E_RANGE && bad == 0);
    CHECK(p.load_count == 0 && p.dep_count == 0);                           /* nothing partial */
    cmd(rec, 0, 0x00000200, 0x02100100);                                    /* below the window */
    CHECK(wwsjm_plan_commands(rec, 0x10, 0x4000, 0x3E000, &p, &bad) == WWSJM_E_RANGE);
    /* every flag bit alone on a use, and the combinations outside the contract */
    CHECK(plan_one(0x00020420, 0x100, &bad) == WWSJM_E_FLAGS);   /* pad */
    CHECK(plan_one(0x00010420, 0x100, &bad) == WWSJM_E_FLAGS);   /* continue-after-discard */
    CHECK(plan_one(0x00041420, 0x100, &bad) == WWSJM_E_FLAGS);   /* write-if-discarded */
    CHECK(plan_one(0x00044420, 0x100, &bad) == WWSJM_E_FLAGS);   /* gather without read */
    CHECK(plan_one(0x00042420, 0x100, &bad) == WWSJM_E_FLAGS);   /* shareable without read */
    CHECK(plan_one(0x0004E420, 0x100, &bad) == WWSJM_E_FLAGS);   /* read+gather+shareable */
    CHECK(plan_one(0x00048420, 0x100, &bad) == WWSJM_OK);        /* read */
    CHECK(plan_one(0x0004A420, 0x100, &bad) == WWSJM_OK);        /* read+shareable */
    CHECK(plan_one(0x0004C420, 0x100, &bad) == WWSJM_OK);        /* read+gather */
    CHECK(plan_one(0x00000420, 0, &bad) == WWSJM_OK);            /* uninitialised */
    CHECK(plan_one(0x00008420, 0x100, &bad) == WWSJM_E_LENGTH);  /* read of zero quadwords */
    CHECK(plan_one(0x00048420, 0x108, &bad) == WWSJM_E_LENGTH);  /* unaligned source */
    CHECK(plan_one(0x04048420, 0x100, &bad) == WWSJM_E_LENGTH);  /* longer than the buffer */
    CHECK(plan_one(0x04008420, 0x100, &bad) == WWSJM_OK);      /* exactly the buffer */
    CHECK(plan_one(0x00008460, 0x100, &bad) == WWSJM_E_SET);     /* set never reserved */
    CHECK(plan_one(0x00048422, 0x100, &bad) == WWSJM_E_SET);     /* buffer outside the set */
    CHECK(plan_one(0x00048421, 0x100, &bad) == WWSJM_OK);        /* second buffer */
    CHECK(plan_one(0x00000220, 0x16040100, &bad) == WWSJM_E_SET);/* reserved twice */
    CHECK(plan_one(0x00000240, 0x12040100, &bad) == WWSJM_E_OVERLAP);
    CHECK(plan_one(0x00000240, 0x18048100, &bad) == WWSJM_E_COMMAND);  /* reserve pad bit */
    CHECK(plan_one(0x00008240, 0x18040100, &bad) == WWSJM_E_FLAGS);   /* reserve with flags */
    CHECK(plan_one(0x00000800, 0x101, &bad) == WWSJM_E_FLAGS);         /* odd counter */
    CHECK(plan_one(0x00000600, 0xF, &bad) == WWSJM_OK);
    /* no run-job command */
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000200, 0x10040100);
    cmd(rec, 1, 0x00000600, 0xF);
    CHECK(wwsjm_plan_commands(rec, 0x10, 0x4000, 0x3E000, &p, &bad) == WWSJM_E_NO_RUN && bad == 2);
    CHECK(wwsjm_plan_commands(rec, 0x18, 0x4000, 0x3E000, &p, &bad) == WWSJM_E_HEADER);
    CHECK(wwsjm_plan_commands(rec, 0x110, 0x4000, 0x3E000, &p, &bad) == WWSJM_E_HEADER);
    /* too many dependencies */
    memset(rec, 0, sizeof rec);
    for (unsigned i = 0; i <= WWSJM_MAX_DEPS; i++)
        cmd(rec, i, 0x00000800, 0x2000 + 2 * i);
    cmd(rec, WWSJM_MAX_DEPS + 1, 0x00000A00, 0);
    CHECK(wwsjm_plan_commands(rec, 0x60, 0x4000, 0x3E000, &p, &bad) == WWSJM_E_TOO_MANY);
    /* the run-job command ends the list: what follows is parameter data */
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000A00, 0);
    cmd(rec, 1, 0x00000E00, 0xFFFFFFFF);
    CHECK(wwsjm_plan_commands(rec, 0x20, 0x4000, 0x3E000, &p, &bad) == WWSJM_OK);
    CHECK(p.command_bytes == 8 && p.userdata_offset == 0x10 && p.userdata_bytes == 0x10);
}

/* page arithmetic, several buffers per set, zero-page sets */
static void test_sets(void)
{
    group = "sets";
    uint8_t rec[0x100];
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000200, 0x103A0100);    /* set 0: 58 pages at 16 */
    cmd(rec, 1, 0x00000220, 0x4A020100);    /* set 1: 2 pages at 74 */
    cmd(rec, 2, 0x00000240, 0x4C2C0300);    /* set 2: 3 buffers of 44 pages at 76 */
    cmd(rec, 3, 0x00000260, 0xD0280100);    /* set 3 */
    cmd(rec, 4, 0x00000280, 0xF8000100);    /* set 4: no pages */
    cmd(rec, 5, 0x39D0A400, MOD_EA);
    cmd(rec, 6, 0x0010C420, LIST_L);
    cmd(rec, 7, 0x00000440, 0);
    cmd(rec, 8, 0x00000441, 0);
    cmd(rec, 9, 0x00000442, 0);
    cmd(rec, 10, 0x00000460, 0);
    cmd(rec, 11, 0x00000480, 0);
    cmd(rec, 12, 0x00000600, 0x1F);
    cmd(rec, 13, 0x00000A00, 0);
    wwsjm_plan p;
    unsigned bad;
    CHECK(wwsjm_plan_commands(rec, 0x70, 0x4000, 0x40000, &p, &bad) == WWSJM_OK);
    CHECK(p.set[2].count == 3 && p.set[2].named == 7);
    CHECK(p.set[4].pages == 0);
    uint32_t ls, bytes;
    CHECK(wwsjm_buffer_span(&p, 2, 1, &ls, &bytes) == WWSJM_OK && ls == 0x1E000 && bytes == 0xB000);
    CHECK(wwsjm_buffer_span(&p, 4, 0, &ls, &bytes) == WWSJM_OK && ls == 0x3E000 && bytes == 0);
    /* the module copy splits into 16 KiB GETs */
    unsigned chunks = 0;
    uint32_t total = 0;
    for (unsigned i = 0; i < p.load_count; i++)
        if (p.load[i].fill == WWSJM_FILL_COPY) {
            CHECK(p.load[i].bytes <= WWSJM_CHUNK_BYTES && p.load[i].bytes % 16 == 0);
            CHECK(p.load[i].ls == 0x4000 + total && p.load[i].ea == MOD_EA + total);
            total += p.load[i].bytes;
            chunks++;
        }
    CHECK(chunks == 4 && total == 0xE740);
    /* a zero-page set must still start inside the window */
    CHECK(wwsjm_plan_commands(rec, 0x70, 0x4000, 0x3E000, &p, &bad) == WWSJM_E_RANGE && bad == 4);
    /* a set's buffer may be named only once */
    cmd(rec, 9, 0x00000441, 0);
    CHECK(wwsjm_plan_commands(rec, 0x70, 0x4000, 0x40000, &p, &bad) == WWSJM_E_SET && bad == 9);
}

/* placement: a ring of buffers starts at the lowest buffer free of busy LS */
static void test_placement(void)
{
    group = "placement";
    uint8_t rec[0x100];
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000200, 0x10100100);    /* set 0: 16 pages at 16 */
    cmd(rec, 1, 0x00000220, 0x20050200);    /* set 1: 2 x 5 pages at 32 */
    cmd(rec, 2, 0x0004C420, LIST_L);
    cmd(rec, 3, 0x00000240, 0x2A020200);    /* set 2: 2 x 2 pages at 42 */
    cmd(rec, 4, 0x00000440, 0);
    cmd(rec, 5, 0x00000260, 0x2E010100);
    cmd(rec, 6, 0x00000460, 0);
    cmd(rec, 7, 0x00000A00, 0);
    wwsjm_plan p;
    CHECK(wwsjm_plan_commands(rec, 0x40, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    uint32_t ls, bytes;
    CHECK(wwsjm_plan_place(&p, NULL, 0) == 0);
    CHECK(wwsjm_buffer_span(&p, 1, 0, &ls, &bytes) == WWSJM_OK && ls == 0x8000 && bytes == 0x1400);
    /* buffer 0 of set 1 busy: the ring starts at buffer 1; the gather follows */
    wwsjm_span busy[2] = {{0x8000, 0x8400}, {0x3F000, 0x40000}};
    CHECK(wwsjm_plan_place(&p, busy, 2) == 0);
    CHECK(p.set[1].start == 1 && p.set[2].start == 0);
    CHECK(wwsjm_buffer_span(&p, 1, 0, &ls, &bytes) == WWSJM_OK && ls == 0x9400);
    for (unsigned i = 0; i < p.load_count; i++)
        if (p.load[i].fill == WWSJM_FILL_GATHER)
            CHECK(p.load[i].ls == 0x9400);
    uint8_t d[16];
    CHECK(wwsjm_buffer_descriptor(&p, 1, 0, 0x1F, d) == WWSJM_OK);
    CHECK(wwsjm_be32(d) == 0x0100001Fu && wwsjm_be16(d + 4) == 0x9400 / 4);
    /* every buffer busy: the first buffer, reported as a conflict */
    wwsjm_span all[1] = {{0x8000, 0xA800}};
    CHECK(wwsjm_plan_place(&p, all, 1) == 1);
    CHECK(p.set[1].start == 0);
    CHECK(wwsjm_buffer_span(&p, 1, 0, &ls, &bytes) == WWSJM_OK && ls == 0x8000);
    /* held: the placed buffers of unreleased sets */
    CHECK(wwsjm_plan_place(&p, busy, 2) == 0);
    wwsjm_span held[8];
    unsigned n = 0;
    CHECK(wwsjm_plan_held(&p, 0, held, 8, &n) == WWSJM_OK);
    CHECK(n == 4);
    unsigned m = 9;
    CHECK(wwsjm_plan_held(&p, 0, held, 3, &m) == WWSJM_E_TOO_MANY);    /* never truncates */
    int saw = 0;
    for (unsigned i = 0; i < n; i++)
        if (held[i].lo == 0x9400 && held[i].hi == 0xA800)
            saw = 1;
    CHECK(saw);
    CHECK(wwsjm_plan_held(&p, 1u << 1 | 1u << 0, held, 8, &n) == WWSJM_OK && n == 2);
    /* a three-buffer ring rotates every logical buffer */
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000240, 0x4C2C0300);
    cmd(rec, 1, 0x00000440, 0);
    cmd(rec, 2, 0x00000441, 0);
    cmd(rec, 3, 0x00000442, 0);
    cmd(rec, 4, 0x00000A00, 0);
    CHECK(wwsjm_plan_commands(rec, 0x30, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    wwsjm_span b0[1] = {{0x13000, 0x13400}};
    CHECK(wwsjm_plan_place(&p, b0, 1) == 1);     /* logical 2 lands on the busy buffer */
    CHECK(p.set[2].start == 1);
    CHECK(wwsjm_buffer_descriptor(&p, 2, 0, 0x1F, d) == WWSJM_OK && wwsjm_be16(d + 4) == 0x1E000 / 4);
    CHECK(wwsjm_buffer_descriptor(&p, 2, 1, 0x1E, d) == WWSJM_OK && wwsjm_be16(d + 4) == 0x29000 / 4);
    CHECK(d[0] == 2 && d[1] == 1 && d[3] == 0x1E);
    CHECK(wwsjm_buffer_descriptor(&p, 2, 2, 0x1D, d) == WWSJM_OK && wwsjm_be16(d + 4) == 0x13000 / 4);
    /* relocation with chosen starts */
    p.set[2].start = 2;
    wwsjm_plan_relocate(&p);
    CHECK(wwsjm_buffer_span(&p, 2, 0, &ls, &bytes) == WWSJM_OK && ls == 0x29000);
}

/* spec 12.1 invariants 1 and 3: set 0 and a set with a shareable copy stay put */
static void test_pinned(void)
{
    group = "pinned";
    uint8_t rec[0x100];
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000200, 0x10040200);    /* set 0: 2 x 4 pages at 16 */
    cmd(rec, 1, 0x0040A400, MOD_EA);        /* 16 quadwords, read+shareable */
    cmd(rec, 2, 0x00000220, 0x20040200);    /* set 1: 2 x 4 pages at 32 */
    cmd(rec, 3, 0x0040A420, MOD_EA + 0x1000);
    cmd(rec, 4, 0x00000240, 0x28040200);    /* set 2: same shape, plain copy */
    cmd(rec, 5, 0x00408440, MOD_EA + 0x2000);
    cmd(rec, 6, 0x00000A00, 0);
    wwsjm_plan p;
    CHECK(wwsjm_plan_commands(rec, 0x40, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    CHECK(!p.set0_moved && p.nop_count == 0);
    wwsjm_span busy[3] = {{0x4000, 0x4400}, {0x8000, 0x8400}, {0xA000, 0xA400}};
    CHECK(wwsjm_plan_place(&p, busy, 3) == 2);
    CHECK(p.set[0].start == 0 && p.set[1].start == 0 && p.set[2].start == 1);
    for (unsigned i = 0; i < p.load_count; i++) {
        unsigned set = p.use[p.load[i].use].set;
        CHECK(p.load[i].ls == (set == 0 ? 0x4000u : set == 1 ? 0x8000u : 0xB000u));
        CHECK(p.load[i].ls % WWSJM_PAGE_BYTES == 0);
    }
    /* set 0 stays at its first page even without a shareable copy */
    cmd(rec, 1, 0x00408400, MOD_EA);        /* plain read */
    CHECK(wwsjm_plan_commands(rec, 0x40, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    CHECK(wwsjm_plan_place(&p, busy, 3) == 2 && p.set[0].start == 0);
    cmd(rec, 1, 0x0040A400, MOD_EA);
    /* a set 0 away from page 16 is allowed and noted */
    cmd(rec, 0, 0x00000200, 0x30040200);
    cmd(rec, 2, 0x00000220, 0x20040200);
    CHECK(wwsjm_plan_commands(rec, 0x40, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK && p.set0_moved);
    /* no-op commands are skipped and counted */
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000000, 0);
    cmd(rec, 1, 0x00000200, 0x10040100);
    cmd(rec, 2, 0x00000000, 0x12345678);
    cmd(rec, 3, 0x00000A00, 0);
    CHECK(wwsjm_plan_commands(rec, 0x20, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    CHECK(p.nop_count == 2 && p.set[0].reserved && p.userdata_offset == 0x20);
}

/* JL-1's command portion: 0xD0 bytes from the header's EA, userdata after the run command */
static void test_jl1_portion(void)
{
    group = "JL-1 portion";
    uint8_t rec[0x100], h8[8];
    memset(rec, 0, sizeof rec);
    cmd(rec, 0, 0x00000200, 0x10040100);
    cmd(rec, 1, 0x0100A400, MOD_EA);
    cmd(rec, 2, 0x00000220, 0x14020100);
    cmd(rec, 3, 0x0004C420, LIST_L);
    cmd(rec, 4, 0x00000A00, 0);
    for (unsigned i = 0x30; i < 0xD0; i++)
        rec[i] = (uint8_t)(0xA0 ^ i);
    header(h8, 0x00D0, 1, 0x02010000u);
    wwsjm_header h;
    CHECK(wwsjm_header_decode(h8, &h) == WWSJM_OK && h.size == 0xD0);
    wwsjm_plan p;
    CHECK(wwsjm_plan_commands(rec, h.size, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    CHECK(p.command_bytes == 0x28 && p.userdata_offset == 0x30 && p.userdata_bytes == 0xA0);
    wwsjm_context_args a = {0};
    a.lane = 0; a.dma_tag = 8; a.spurs_ea = SPURS_EA; a.wid = 6; a.index = 3; a.array = 3;
    a.list_ea = LIST_EA; a.list_tag = 0x0A000000u; a.userdata_ls = 0x2000 + p.userdata_offset;
    uint8_t ctx[WWSJM_CONTEXT_BYTES];
    wwsjm_context_build(&a, ctx);
    CHECK(wwsjm_be32(ctx + 0x20) == 0x2030 && wwsjm_be32(ctx + 0x2C) == 0x2030);
    CHECK(wwsjm_be32(ctx + 0x14) == 0);
}

/* U18: the slot rule across capacities, including where N wraps (unobserved) */
static void test_slot_modulus(void)
{
    group = "slot";
    uint8_t l[128];
    line_blank(l);
    line_array(l, 0, 0x13FF, ARRAY_EA);
    line_array(l, 1, 0x1FEF, ARRAY_EA + 0x10000);
    line_array(l, 2, 0xFF, ARRAY_EA + 0x20000);
    wwsjm_line dl;
    wwsjm_line_decode(l, &dl);
    for (uint32_t i = 0; i < 0x10000; i += 0x7F) {
        CHECK(wwsjm_header_ea(&dl, 0, (uint16_t)i) == ARRAY_EA + 8 * (i % 0x1400));
        CHECK(wwsjm_header_ea(&dl, 1, (uint16_t)i) == ARRAY_EA + 0x10000 + 8 * (i % 0x1FF0));
        CHECK(wwsjm_header_ea(&dl, 2, (uint16_t)i) == ARRAY_EA + 0x20000 + 8 * (i & 0xFF));
    }
    /* the modulus is of the u16 index: 0xFFFF and 0 are not neighbouring slots */
    CHECK(wwsjm_header_ea(&dl, 0, 0xFFFF) == ARRAY_EA + 8 * (0xFFFFu % 0x1400));
    CHECK(wwsjm_header_ea(&dl, 0, 0x0000) == ARRAY_EA);
    CHECK(0xFFFFu >= WWSJM_INDEX_WRAP_WATCH && 0xEFFFu < WWSJM_INDEX_WRAP_WATCH);
}

static void test_tags(void)
{
    group = "tags";
    unsigned given = 0;
    uint8_t tag = 0;
    for (unsigned i = 0; i < WWSJM_JOB_TAG_COUNT; i++) {
        CHECK(wwsjm_job_tag(&given, &tag) == WWSJM_OK);
        CHECK(tag == 31 - i);
    }
    CHECK(wwsjm_job_tag(&given, &tag) == WWSJM_E_SERVICE);
}

static void test_gather(void)
{
    group = "gather";
    uint8_t list[48];
    wwsjm_put32(list + 0, 0x100);  wwsjm_put32(list + 4, 0x02000000);
    wwsjm_put32(list + 8, 0x10);   wwsjm_put32(list + 12, 0x03000010);
    wwsjm_put32(list + 16, 0);     wwsjm_put32(list + 20, 0x04000000);     /* empty element */
    wwsjm_put32(list + 24, 0x8C4); wwsjm_put32(list + 28, 0x05000004);     /* odd size, EA offset 4 */
    wwsjm_put32(list + 32, 0x4);   wwsjm_put32(list + 36, 0x0600000C);
    wwsjm_put32(list + 40, 0x20);  wwsjm_put32(list + 44, 0x07000020);
    wwsjm_piece pc[6];
    unsigned n = 0;
    CHECK(wwsjm_gather_layout(list, 6, 0x5000, 0xA10, pc, 6, &n) == WWSJM_OK);
    CHECK(n == 5);
    CHECK(pc[0].ls == 0x5000 && pc[0].bytes == 0x100 && pc[0].ea == 0x02000000);
    CHECK(pc[1].ls == 0x5100 && pc[1].bytes == 0x10);
    CHECK(pc[2].ls == 0x5114 && pc[2].bytes == 0x8C4);      /* keeps its offset in the quadword */
    CHECK(pc[3].ls == 0x59EC);                              /* after 0x5114 + 0x8C4, next quadword */
    CHECK(pc[4].ls == 0x59F0);
    for (unsigned i = 0; i + 1 < n; i++)                    /* no element overlaps the next */
        CHECK(pc[i].ls + pc[i].bytes <= pc[i + 1].ls);
    CHECK(wwsjm_gather_layout(list, 6, 0x5000, 0xA0F, pc, 6, &n) == WWSJM_E_GATHER);  /* last byte out */
    CHECK(wwsjm_gather_layout(list, 6, 0x5000, 0xA10, pc, 4, &n) == WWSJM_E_TOO_MANY);
    /* an element whose quadword offset plus size crosses into the next quadword */
    uint8_t cross[16];
    wwsjm_put32(cross + 0, 8);     wwsjm_put32(cross + 4, 0x0800000C);
    wwsjm_put32(cross + 8, 0x10);  wwsjm_put32(cross + 12, 0x09000000);
    CHECK(wwsjm_gather_layout(cross, 1, 0x5000, 0x10, pc, 6, &n) == WWSJM_E_GATHER);
    CHECK(wwsjm_gather_layout(cross, 1, 0x5000, 0x14, pc, 6, &n) == WWSJM_OK && pc[0].ls == 0x500C);
    CHECK(wwsjm_gather_layout(cross, 2, 0x5000, 0x30, pc, 6, &n) == WWSJM_OK && n == 2);
    CHECK(pc[1].ls == 0x5020);                              /* not 0x5010: that would overlap */
    CHECK(wwsjm_gather_layout(cross, 2, 0x5000, 0x2F, pc, 6, &n) == WWSJM_E_GATHER);
    /* 16 KiB per element at most */
    wwsjm_put32(cross + 0, 0x4000);  wwsjm_put32(cross + 4, 0x0A000000);
    CHECK(wwsjm_gather_layout(cross, 1, 0x5000, 0x8000, pc, 6, &n) == WWSJM_OK);
    wwsjm_put32(cross + 0, 0x4010);
    CHECK(wwsjm_gather_layout(cross, 1, 0x5000, 0x8000, pc, 6, &n) == WWSJM_E_GATHER);
    wwsjm_put32(list + 8, 0x80000010);                       /* stall-and-notify */
    CHECK(wwsjm_gather_layout(list, 2, 0x5000, 0x1000, pc, 6, &n) == WWSJM_E_GATHER);
}

/* RS-1 */
static uint32_t module_record(uint8_t* rec, uint32_t module_ea)
{
    memset(rec, 0, 0x100);
    cmd(rec, 0, 0x00000200, 0x10040100);
    cmd(rec, 1, 0x0100A400, module_ea);      /* 64 quadwords, read+shareable */
    cmd(rec, 2, 0x00000220, 0x14020100);
    cmd(rec, 3, 0x0004C420, LIST_L);
    cmd(rec, 4, 0x00000600, 0x2);
    cmd(rec, 5, 0x00000A00, 0);
    return 0x30;
}

static int module_loaded(const wwsjm_plan* p)
{
    for (unsigned i = 0; i < p->load_count; i++)
        if (p->load[i].fill == WWSJM_FILL_COPY)
            return !p->load[i].skip;
    return -1;
}

static void test_rs1(void)
{
    group = "RS-1";
    const uint32_t M = 0x01900000u, M2 = 0x01910000u;
    const uint32_t jobs[4] = {M, M, M2, M};
    const int want[4] = {1, 0, 1, 1};
    wwsjm_residency r;
    wwsjm_residency_reset(&r);
    uint8_t rec[0x100];
    wwsjm_plan p;
    for (unsigned j = 0; j < 4; j++) {
        uint32_t size = module_record(rec, jobs[j]);
        CHECK(wwsjm_plan_commands(rec, size, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
        wwsjm_plan_place(&p, NULL, 0);
        wwsjm_plan_residency(&p, &r);
        CHECK(module_loaded(&p) == want[j]);
        for (unsigned i = 0; i < p.load_count; i++)                 /* gathers always load */
            if (p.load[i].fill == WWSJM_FILL_GATHER)
                CHECK(!p.load[i].skip);
    }
    wwsjm_residency_reset(&r);                                      /* a new run */
    module_record(rec, M);
    CHECK(wwsjm_plan_commands(rec, 0x30, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    wwsjm_plan_residency(&p, &r);
    CHECK(module_loaded(&p) == 1);
    /* the same source with another length, or something else placed over it: reload */
    wwsjm_plan_residency(&p, &r);
    CHECK(module_loaded(&p) == 0);
    cmd(rec, 1, 0x0080A400, M);
    CHECK(wwsjm_plan_commands(rec, 0x30, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    wwsjm_plan_residency(&p, &r);
    CHECK(module_loaded(&p) == 1);
    wwsjm_residency_clobber(&r, 0x4400, 0x10);
    module_record(rec, M);
    CHECK(wwsjm_plan_commands(rec, 0x30, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    wwsjm_plan_residency(&p, &r);
    CHECK(module_loaded(&p) == 1);
    /* a copy that is not shareable always loads */
    cmd(rec, 1, 0x01008400, M);
    CHECK(wwsjm_plan_commands(rec, 0x30, 0x4000, 0x3E000, &p, NULL) == WWSJM_OK);
    wwsjm_plan_residency(&p, &r);
    CHECK(module_loaded(&p) == 1);
    wwsjm_plan_residency(&p, &r);
    CHECK(module_loaded(&p) == 1);
}

int main(void)
{
    test_jl1_fresh_claim();
    test_jl2_counted_claim();
    test_jl3_retire();
    test_jl4_gates();
    test_jl5_switch();
    test_jl6_nop();
    test_jl7_wrap();
    test_claim_properties();
    test_headers();
    test_dd1();
    test_rc();
    test_cl1();
    test_cl2();
    test_cl3_refusals();
    test_sets();
    test_placement();
    test_pinned();
    test_jl1_portion();
    test_slot_modulus();
    test_tags();
    test_gather();
    test_rs1();
    if (failures) {
        fprintf(stderr, "test_wwsjm_core: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_wwsjm_core: all checks passed\n");
    return 0;
}
