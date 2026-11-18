/* The single-precision compares (fceq/fcgt/fcmeq/fcmgt) and estimates (frest/frsqest) the build
 * selects -- the SPU_FLOAT_X86_RZ AVX-512 forms when built with -march=x86-64-v4
 * -DSPU_FLOAT_X86_RZ=1, the default forms otherwise -- against the scalar references, bit for bit.
 *  - compares: every pair from a word set with every biased exponent, both signs and edge fractions,
 *    then random pairs (raw words, and words sharing exponents so magnitudes tie);
 *  - estimates: every sign, biased exponent and 6-bit table index, with random low fraction bits.
 * Build: cc -O2 -I include -I runtime/spu runtime/spu/tests/test_spu_float_avx512.c -o t && ./t */
#include "spu_helpers.h"
#include <stdio.h>

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)(s_rng >> 16); }
static long s_bad, s_checks;

static u128 w4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 r; r._u32[0] = a; r._u32[1] = b; r._u32[2] = c; r._u32[3] = d; return r; }
static void report(const char* op, u128 a, u128 b, u128 got, u128 ref)
{
    if (s_bad++ < 20)
        for (int i = 0; i < 4; i++)
            if (got._u32[i] != ref._u32[i])
                printf("MISMATCH %s lane %d: a=%08X b=%08X got=%08X ref=%08X\n", op, i, a._u32[i], b._u32[i], got._u32[i], ref._u32[i]);
}
static int ne(u128 x, u128 y) { return memcmp(&x, &y, 16) != 0; }
static void cmp4(u128 a, u128 b)
{
    u128 g, r;
    s_checks++;
    if (ne(g = spu_fceq(a, b), r = spu_fceq_ref(a, b))) report("fceq", a, b, g, r);
    if (ne(g = spu_fcgt(a, b), r = spu_fcgt_ref(a, b))) report("fcgt", a, b, g, r);
    if (ne(g = spu_fcmeq(a, b), r = spu_fcmeq_ref(a, b))) report("fcmeq", a, b, g, r);
    if (ne(g = spu_fcmgt(a, b), r = spu_fcmgt_ref(a, b))) report("fcmgt", a, b, g, r);
}
static void est4(u128 a)
{
    u128 g, r;
    s_checks++;
    if (ne(g = spu_frest(a), r = spu_frest_ref(a))) report("frest", a, a, g, r);
    if (ne(g = spu_frsqest(a), r = spu_frsqest_ref(a))) report("frsqest", a, a, g, r);
}

int main(void)
{
    static const uint32_t frac[] = { 0, 1, 2, 0x400000, 0x7FFFFF, 0x7FFFFE, 0x3FFFFF, 0x555555 };
    enum { NF = sizeof frac / sizeof frac[0] };
    static uint32_t set[2 * 256 * NF];
    int n = 0;
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t e = 0; e < 256; e++)
            for (int f = 0; f < NF; f++) set[n++] = (s << 31) | (e << 23) | frac[f];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j += 4)
            cmp4(w4(set[i], set[i], set[i], set[i]), w4(set[j], set[(j + 1) % n], set[(j + 2) % n], set[(j + 3) % n]));
    for (long k = 0; k < 4000000; k++) {
        const uint32_t a0 = rnd32(), a1 = rnd32(), a2 = rnd32(), a3 = rnd32();
        cmp4(w4(a0, a1, a2, a3), w4(rnd32(), rnd32(), rnd32(), rnd32()));
        const uint32_t m = 0xFF800000u;   /* same sign and exponent: the fractions decide */
        cmp4(w4(a0, a1, a2, a3), w4((a0 & m) | (rnd32() & ~m), (a1 & m) | (rnd32() & ~m), a2 ^ 0x80000000u, a3 & 0x807FFFFFu));
    }
    for (uint32_t hi = 0; hi < (1u << 15); hi++)          /* sign, 8 exponent bits, 6 table-index bits */
        for (int k = 0; k < 4; k++) {
            const uint32_t base = hi << 17;
            est4(w4(base | (rnd32() & 0x1FFFF), base | (k ? rnd32() & 0x1FFFF : 0), base | 0x1FFFF, base));
        }
#if defined(SPU_FLOAT_X86_RZ_ACTIVE)
    const char* form = "AVX-512 (SPU_FLOAT_X86_RZ)";
#else
    const char* form = "default";
#endif
    printf("%s forms: %ld vector checks, %ld mismatching\n", form, s_checks, s_bad);
    return s_bad != 0;
}
