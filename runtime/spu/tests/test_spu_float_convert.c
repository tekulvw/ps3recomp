/* spu_cflts, spu_cfltu, spu_csflt, spu_cuflt and spu_fi (the x86, NEON or scalar form this build
 * selects) against their scalar references (spu_*_ref in spu_helpers.h), lane for lane, bit for bit.
 *
 *  1. float to integer, every scale immediate 0..255: every sign and biased exponent 0..255 with
 *     edge and random fractions, so every saturation edge, exponent-0 and exponent-255 word meets
 *     every scale;
 *  2. integer to float, every scale immediate: every bit length 0..32 with the values around the
 *     24-bit truncation edge, both signs, INT_MIN/INT_MAX/UINT_MAX, plus random words;
 *  3. fi: every sign, biased exponent and 13-bit base fraction, with steps and Y values picked
 *     on both sides of the borrow edge (step * Y around base << 19) and at random;
 *  4. random words and immediates for all five.
 * "full" as the first argument adds exhaustive sweeps over all 2^32 words for a set of scales.
 *
 * Build (x86: at -msse4.1 and -march=x86-64-v3 / v4 for the SSE4.1 and AVX forms):
 *   cc -std=gnu17 -O2 -I include -I runtime/spu runtime/spu/tests/test_spu_float_convert.c -lm -o t && ./t [full] */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t s_rng = 0xD1B54A32D192ED03ull;
static inline uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

enum { OP_CFLTS, OP_CFLTU, OP_CSFLT, OP_CUFLT, OP_FI, OP_N };
static const char* const k_ops[OP_N] = { "cflts", "cfltu", "csflt", "cuflt", "fi" };
static unsigned long long s_lanes[OP_N], s_bad;

static void run(int op, u128 a, u128 b, int i8)
{
    u128 got, ref;
    switch (op) {
    case OP_CFLTS: got = spu_cflts(a, i8); ref = spu_cflts_ref(a, i8); break;
    case OP_CFLTU: got = spu_cfltu(a, i8); ref = spu_cfltu_ref(a, i8); break;
    case OP_CSFLT: got = spu_csflt(a, i8); ref = spu_csflt_ref(a, i8); break;
    case OP_CUFLT: got = spu_cuflt(a, i8); ref = spu_cuflt_ref(a, i8); break;
    default:       got = spu_fi(a, b);     ref = spu_fi_ref(a, b); break;
    }
    s_lanes[op] += 4;
    for (int i = 0; i < 4; i++)
        if (got._u32[i] != ref._u32[i] && s_bad++ < 20)
            printf("MISMATCH %s i8=%d a=%08X b=%08X got=%08X ref=%08X\n", k_ops[op], i8,
                   a._u32[i], b._u32[i], got._u32[i], ref._u32[i]);
}

/* Lanes are queued and run four at a time, so neighbouring lanes come from different cases. */
static u128 s_qa, s_qb;
static int s_qn;
static void lane(int op, uint32_t a, uint32_t b, int i8)
{
    s_qa._u32[s_qn] = a; s_qb._u32[s_qn] = b;
    if (++s_qn == 4) { run(op, s_qa, s_qb, i8); s_qn = 0; }
}
static void flush(int op, int i8)
{
    if (!s_qn) return;
    for (int i = s_qn; i < 4; i++) { s_qa._u32[i] = rnd32(); s_qb._u32[i] = rnd32(); }
    run(op, s_qa, s_qb, i8);
    s_qn = 0;
}

static const uint32_t k_frac[] = { 0x000000u, 0x000001u, 0x000002u, 0x400000u, 0x7FFFFFu, 0x7FFFFEu,
                                   0x3FFFFFu, 0x555555u, 0x000080u, 0x0000FFu, 0x7FFF00u, 0x100000u };
#define NFRAC (sizeof k_frac / sizeof k_frac[0])

/* integer words around the edges of bit length n (1..32) */
static int int_edges(int n, uint32_t* out)
{
    const uint64_t lo = 1ull << (n - 1), hi = (n == 32) ? 0x100000000ull : (1ull << n);
    int k = 0;
    out[k++] = (uint32_t)lo;
    out[k++] = (uint32_t)(hi - 1);
    out[k++] = (uint32_t)(lo + 1);
    if (n > 24) {
        const uint64_t ulp = 1ull << (n - 24);         /* last bit kept by a 24-bit truncation */
        out[k++] = (uint32_t)(lo + ulp - 1);
        out[k++] = (uint32_t)(lo + ulp);
        out[k++] = (uint32_t)(lo + ulp + 1);
        out[k++] = (uint32_t)(hi - ulp);
        out[k++] = (uint32_t)(hi - ulp - 1);
    }
    for (int r = 0; r < 6; r++) out[k++] = (uint32_t)(lo + (rnd32() % lo));
    return k;
}

static void sweep_all(int op, int i8)
{
    u128 a, b = { 0 };
    for (uint64_t w = 0; w < 0x100000000ull; w += 4) {
        for (int i = 0; i < 4; i++) a._u32[i] = (uint32_t)w + (uint32_t)i;
        run(op, a, b, i8);
    }
}

int main(int argc, char** argv)
{
    const int full = argc > 1 && !strcmp(argv[1], "full");

    /* 1. float to integer: every scale x sign x biased exponent x fraction */
    for (int op = OP_CFLTS; op <= OP_CFLTU; op++)
        for (int i8 = 0; i8 < 256; i8++) {
            for (uint32_t s = 0; s < 2; s++)
                for (uint32_t be = 0; be < 256; be++) {
                    for (unsigned f = 0; f < NFRAC; f++) lane(op, s << 31 | be << 23 | k_frac[f], 0, i8);
                    for (int r = 0; r < 4; r++) lane(op, s << 31 | be << 23 | (rnd32() & 0x7FFFFFu), 0, i8);
                }
            flush(op, i8);
        }

    /* 2. integer to float: every scale x bit length x edge value x sign */
    for (int op = OP_CSFLT; op <= OP_CUFLT; op++)
        for (int i8 = 0; i8 < 256; i8++) {
            static const uint32_t k_special[] = { 0u, 1u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x80000001u,
                                                  0x00FFFFFFu, 0x01000000u, 0x01000001u, 0xFF000000u, 0xFEFFFFFFu };
            for (unsigned k = 0; k < sizeof k_special / sizeof k_special[0]; k++) lane(op, k_special[k], 0, i8);
            for (int n = 1; n <= 32; n++) {
                uint32_t e[32];
                const int ne = int_edges(n, e);
                for (int k = 0; k < ne; k++) { lane(op, e[k], 0, i8); lane(op, 0u - e[k], 0, i8); }
            }
            for (int r = 0; r < 256; r++) lane(op, rnd32() >> (rnd32() & 31), 0, i8);
            flush(op, i8);
        }

    /* 3. fi: every sign x exponent x base, step and Y around the borrow edge and at random */
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t be = 0; be < 256; be++)
            for (uint32_t base = 0; base < 0x2000u; base++) {
                const uint32_t hi = s << 31 | be << 23 | base << 10;
                const uint32_t u0 = base << 19;
                const uint32_t step = 1 + rnd32() % 0x3FFu;
                uint32_t y = u0 / step;                       /* step * y just at or below u0 */
                for (int d = -1; d <= 1; d++) {
                    const uint32_t yy = (y + (uint32_t)d) & 0x7FFFFu;
                    lane(OP_FI, (rnd32() & 0xFFF80000u) | yy, hi | step, 0);
                }
                lane(OP_FI, rnd32(), hi | (rnd32() & 0x3FFu), 0);
                lane(OP_FI, rnd32() | 0x7FFFFu, hi | 0x3FFu, 0);  /* the largest step * y */
                lane(OP_FI, rnd32() & ~0x7FFFFu, hi | (rnd32() & 0x3FFu), 0);   /* y = 0 */
            }
    flush(OP_FI, 0);

    /* 4. random words and immediates */
    for (long it = 0; it < 4000000; it++) {
        u128 a, b;
        for (int i = 0; i < 4; i++) { a._u32[i] = rnd32(); b._u32[i] = rnd32(); }
        const int i8 = (int)(rnd32() & 0xFF);
        for (int op = 0; op < OP_N; op++) run(op, a, b, i8);
    }

    if (full) {
        static const int k_fl[] = { 173, 158, 46, 0, 255 };         /* scales 0, 15, 127, 173, -82 */
        static const int k_if[] = { 155, 140, 0, 255, 28 };          /* scales 0, 15, 155, -100, 127 */
        for (unsigned k = 0; k < sizeof k_fl / sizeof k_fl[0]; k++) { sweep_all(OP_CFLTS, k_fl[k]); sweep_all(OP_CFLTU, k_fl[k]); }
        for (unsigned k = 0; k < sizeof k_if / sizeof k_if[0]; k++) { sweep_all(OP_CSFLT, k_if[k]); sweep_all(OP_CUFLT, k_if[k]); }
    }

    unsigned long long n = 0;
    for (int op = 0; op < OP_N; op++) { printf("  %-6s %llu lanes\n", k_ops[op], s_lanes[op]); n += s_lanes[op]; }
#if defined(__SSE4_1__) && !defined(SPU_SCALAR_HELPERS)
    const char* form = "x86 SSE4.1";
#if defined(__AVX__)
    form = "x86 SSE4.1/AVX";
#endif
#elif defined(__aarch64__) && defined(__ARM_NEON) && !defined(SPU_SCALAR_HELPERS)
    const char* form = "NEON (cflts, fi)";
#else
    const char* form = "scalar";
#endif
    printf("test_spu_float_convert [%s]%s: %llu lanes, %llu mismatches\n", form, full ? " full" : "", n, s_bad);
    return s_bad ? 1 : 0;
}
