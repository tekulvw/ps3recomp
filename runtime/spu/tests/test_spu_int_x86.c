/* The x86 integer/permute helpers must be bit-identical to their scalar
 * references for random operands and every shift count, including counts
 * outside the architectural range. Build on x86-64 with SSSE3 or later:
 *   cc -O2 -mssse3 -I../../../include -I.. test_spu_int_x86.c -o t && ./t
 * (also at -msse4.1 and -march=x86-64-v3). Without SSSE3 the helpers are the
 * scalar references themselves and the test reports that it compared nothing. */
#include "../spu_helpers.h"
#include <stdio.h>

#if defined(SPU_X86_VECTOR)
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}
static u128 rnd128(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = rnd32(); return r; }

static long s_bad, s_checks;
static void check(const char* op, u128 got, u128 ref, u128 a, long arg)
{
    s_checks++;
    if (!memcmp(&got, &ref, 16)) return;
    if (s_bad++ < 12)
        printf("MISMATCH %s a=%08X%08X%08X%08X arg=%ld got=%08X%08X%08X%08X ref=%08X%08X%08X%08X\n",
               op, a._u32[0], a._u32[1], a._u32[2], a._u32[3], arg,
               got._u32[0], got._u32[1], got._u32[2], got._u32[3], ref._u32[0], ref._u32[1], ref._u32[2], ref._u32[3]);
}

#endif

static int test_upstream_sse41(void);

int main(void)
{
#if !defined(SPU_X86_VECTOR)
    puts("test_spu_int_x86: no x86 vector forms in this build (needs SSSE3 on x86), nothing to compare");
    return 0;
#else
    /* Shift and rotate counts: every count from -300 to 300 (register forms read the low
     * bits of the preferred word, so wide values must wrap exactly as the scalar forms do),
     * plus random full-width words. */
    for (int it = 0; it < 400; it++) {
        const u128 a = rnd128();
        for (long k = -300; k <= 300; k++) {
            u128 b = rnd128();
            b._u32[0] = (it & 1) ? (uint32_t)k : (uint32_t)k ^ (rnd32() & 0xFFFFFE00u);
            check("rotqby", spu_rotqby(a, b), spu_rotqby_ref(a, b), a, k);
            check("rotqbybi", spu_rotqbybi(a, b), spu_rotqbybi_ref(a, b), a, k);
            check("shlqby", spu_shlqby(a, b), spu_shlqby_ref(a, b), a, k);
            check("shlqbybi", spu_shlqbybi(a, b), spu_shlqbybi_ref(a, b), a, k);
            check("rotqmby", spu_rotqmby(a, b), spu_rotqmby_ref(a, b), a, k);
            check("rotqmbybi", spu_rotqmbybi(a, b), spu_rotqmbybi_ref(a, b), a, k);
            check("rotqbyi", spu_rotqbyi(a, (int)k), spu_rotqbyi_ref(a, (int)k), a, k);
            check("shlqbyi", spu_shlqbyi(a, (int)k), spu_shlqbyi_ref(a, (int)k), a, k);
            check("rotqmbyi", spu_rotqmbyi(a, (int)k), spu_rotqmbyi_ref(a, (int)k), a, k);
        }
    }
    /* Insertion controls: every base address residue and immediate. */
    for (long base = -64; base < 64; base++) {
        for (int i7 = -64; i7 < 64; i7++) {
            u128 a = rnd128();
            a._u32[0] = (uint32_t)base + ((uint32_t)rnd32() << 8);
            const int pos = (int)a._u32[0] + i7;
            check("cbd", spu_cbd(a, i7), spu_cbd_pos(pos), a, pos);
            check("chd", spu_chd(a, i7), spu_chd_pos(pos), a, pos);
            check("cwd", spu_cwd(a, i7), spu_cwd_pos(pos), a, pos);
            check("cdd", spu_cdd(a, i7), spu_cdd_pos(pos), a, pos);
        }
    }
    /* Whole-register logic, random operands and immediates. */
    for (long it = 0; it < 200000; it++) {
        const u128 a = rnd128(), b = (it % 7 == 0) ? a : rnd128(), c = rnd128();
        const int32_t imm = (it & 1) ? (int32_t)rnd32() : (int32_t)(rnd32() & 0x3FF) - 0x200;
        check("and", spu_and(a, b), spu_and_ref(a, b), a, it);
        check("or", spu_or(a, b), spu_or_ref(a, b), a, it);
        check("xor", spu_xor(a, b), spu_xor_ref(a, b), a, it);
        check("nand", spu_nand(a, b), spu_nand_ref(a, b), a, it);
        check("nor", spu_nor(a, b), spu_nor_ref(a, b), a, it);
        check("andc", spu_andc(a, b), spu_andc_ref(a, b), a, it);
        check("orc", spu_orc(a, b), spu_orc_ref(a, b), a, it);
        check("andi", spu_andi(a, imm), spu_andi_ref(a, imm), a, imm);
        check("ori", spu_ori(a, imm), spu_ori_ref(a, imm), a, imm);
        check("xori", spu_xori(a, imm), spu_xori_ref(a, imm), a, imm);
        check("selb", spu_selb(a, b, c), spu_selb_ref(a, b, c), a, it);
    }
    /* Per-lane families: random operands, plus operands that share lanes with each other
     * (equal lanes for the compares), every immediate class, and the 0x80/0xC0/0xE0
     * shufb selectors. */
    for (long it = 0; it < 300000; it++) {
        u128 a = rnd128(), b = rnd128(), c = rnd128();
        if (it % 5 == 0) for (int i = 0; i < 16; i++) if (rnd32() & 1) b._u8[i] = a._u8[i];
        if (it % 11 == 0) { a._u32[it % 4] = 0x80000000u; b._u32[(it + 1) % 4] = 0x7FFFFFFFu; }
        const int32_t imm = (it & 1) ? (int32_t)rnd32() : (int32_t)(rnd32() & 0x3FF) - 0x200;
#define C2(op) check(#op, spu_##op(a, b), spu_##op##_ref(a, b), a, it)
#define CI(op) check(#op, spu_##op(a, imm), spu_##op##_ref(a, imm), a, imm)
#define C1(op) check(#op, spu_##op(a), spu_##op##_ref(a), a, it)
#define C3(op) check(#op, spu_##op(a, b, c), spu_##op##_ref(a, b, c), a, it)
        C2(a); C2(sf); C2(ceq); C2(cgt); C2(clgt); CI(ai); CI(sfi); CI(ceqi); CI(cgti); CI(clgti);
        C2(absdb); C2(avgb); C2(ceqb); C2(cgtb); C2(clgtb); C2(ceqh); C2(cgth); C2(clgth); C2(ah); C2(sfh);
        C2(mpy); C2(mpyu); C2(mpyh); C2(mpyhh); C2(mpyhhu); C2(mpys);
        CI(ceqbi); CI(cgtbi); CI(clgtbi); CI(ceqhi); CI(cgthi); CI(clgthi); CI(ahi); CI(sfhi); CI(mpyi); CI(mpyui);
        C3(mpya); C3(mpyhha); C3(mpyhhau);
        C1(cntb); C1(xsbh); C1(xshw); C1(xswd); C1(gb); C1(gbh); C1(gbb); C1(fsm); C1(fsmh); C1(fsmb);
        if (it % 3 == 0) for (int i = 0; i < 16; i++) { const uint8_t r8 = (uint8_t)rnd32(); if (r8 < 96) c._u8[i] = (uint8_t)(0x80 | (r8 & 0x7F)); }
        check("shufb", spu_shufb(a, b, c), spu_shufb_ref(a, b, c), a, it);
#undef C2
#undef CI
#undef C1
#undef C3
    }
    /* Immediate rotates and shifts: every count, including out-of-range ones. */
    for (int it = 0; it < 2000; it++) {
        const u128 a = rnd128();
        for (int k = -130; k <= 130; k++) {
            check("rothi", spu_rothi(a, k), spu_rothi_ref(a, k), a, k);
            check("roti", spu_roti(a, k), spu_roti_ref(a, k), a, k);
            check("rotmahi", spu_rotmahi(a, k), spu_rotmahi_ref(a, k), a, k);
            check("rotmai", spu_rotmai(a, k), spu_rotmai_ref(a, k), a, k);
        }
    }
    if (test_upstream_sse41()) s_bad++;
    if (s_bad) { printf("test_spu_int_x86: %ld mismatches of %ld checks\n", s_bad, s_checks); return 1; }
    printf("test_spu_int_x86: ok (%ld checks, 78 helpers)\n", s_checks);
    return 0;
#endif
}

/* Upstream SSE4.1/AVX2 variable shifts, carry/borrow and scalar control oracle. */
#if SPU_X86
/* x86 (SSE4.1/AVX2) SPU helpers vs their scalar spu_X_ref, on random and edge inputs.
 *
 *   clang -O2 -mavx2 -Iruntime/spu -Iruntime -Iinclude \
 *         runtime/spu/tests/test_spu_int_x86.c -o test_spu_int_x86 && ./test_spu_int_x86
 *
 * With -msse4.1 in place of -mavx2 it checks the SSE4.1-only set (the variable shifts are
 * then their own references).
 *
 * The generate-controls (spu_cbd_pos & co.) lost their byte loops on every host; they are
 * checked here against those loops, kept below as the reference. */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

#if !SPU_X86
#error "build with -msse4.1 or -mavx2 (SPU_X86 off)"
#endif

static uint64_t up_s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t up_rnd(void) { up_s_rng ^= up_s_rng << 13; up_s_rng ^= up_s_rng >> 7; up_s_rng ^= up_s_rng << 17; return (uint32_t)up_s_rng; }
/* Mostly random words, with the small and boundary values shifts and compares care about. */
static uint32_t up_rword(void) {
    static const uint32_t edge[] = {0, 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 0x7F, 0x80, 0xFF,
                                    0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu,
                                    0xFFFFFFF0u, 0xFFFFFFE0u, 0x00800080u, 0x80008000u, 0x7F7F7F7Fu};
    switch (up_rnd() & 3) {
    case 0: return edge[up_rnd() % (sizeof edge / sizeof edge[0])];
    case 1: return up_rnd() & 0x3F;
    default: return up_rnd();
    }
}
static u128 up_rv(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = up_rword(); return r; }
static int up_s_fail;
static void up_cmp(const char* name, u128 got, u128 want, u128 a, u128 b, u128 c, int imm) {
    if (memcmp(&got, &want, 16) == 0) return;
    if (up_s_fail++ < 20)
        printf("FAIL %s imm=%d a=%08X%08X%08X%08X b=%08X%08X%08X%08X c=%08X..\n  got  %08X %08X %08X %08X\n  want %08X %08X %08X %08X\n",
               name, imm, a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3], c._u32[0],
               got._u32[0], got._u32[1], got._u32[2], got._u32[3], want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
}

/* The loops the generate-controls had. */
static u128 up_ref_cbd_pos(int pos) { u128 r; for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    r._u8[SPU_W(pos & 0xF)] = 0x03; return r; }
static u128 up_ref_chd_pos(int pos) { u128 r; for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~1; r._u8[SPU_W(p)] = 0x02; r._u8[SPU_W(p + 1)] = 0x03; return r; }
static u128 up_ref_cwd_pos(int pos) { u128 r; for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~3; for (int k = 0; k < 4; k++) r._u8[SPU_W(p + k)] = (uint8_t)k; return r; }
static u128 up_ref_cdd_pos(int pos) { u128 r; for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~7; for (int k = 0; k < 8; k++) r._u8[SPU_W(p + k)] = (uint8_t)k; return r; }

#define T2(f)  up_cmp(#f, f(a, b), f##_ref(a, b), a, b, z, 0)
#define T3(f)  up_cmp(#f, f(a, b, c), f##_ref(a, b, c), a, b, c, 0)
#define T1(f)  up_cmp(#f, f(a), f##_ref(a), a, z, z, 0)
#define TI(f)  up_cmp(#f, f(a, imm), f##_ref(a, imm), a, z, z, imm)

static int test_upstream_sse41(void) {
    const u128 z = spu_zero();
    for (long n = 0; n < 2000000; n++) {
        const u128 a = up_rv(), b = up_rv(), c = up_rv();
        const int imm = (int)(up_rnd() % 3 == 0 ? (int32_t)up_rword() : (int32_t)(up_rnd() % 256) - 128);
        T2(spu_a); T2(spu_sf); T2(spu_and); T2(spu_or); T2(spu_xor);
        T2(spu_ceq); T2(spu_cgt); T2(spu_clgt); T3(spu_selb);
        TI(spu_ai); TI(spu_sfi); TI(spu_andi); TI(spu_ori); TI(spu_xori);
        TI(spu_ceqi); TI(spu_cgti); TI(spu_clgti);
        TI(spu_shlqbyi); TI(spu_rotqbyi); TI(spu_rotqmbyi);
        T2(spu_shlqby); T2(spu_rotqby); T2(spu_shlqbybi); T2(spu_rotqbybi); T2(spu_rotqmby); T2(spu_rotqmbybi);
        TI(spu_cwd);
        T2(spu_absdb); T2(spu_avgb); T2(spu_ceqb); T2(spu_cgtb); T2(spu_clgtb);
        T2(spu_ceqh); T2(spu_cgth); T2(spu_clgth); T2(spu_ah); T2(spu_sfh);
        T2(spu_shl); T2(spu_rot); T2(spu_rotm); T2(spu_rotma); T2(spu_cg); T2(spu_bg);
        T2(spu_mpy); T2(spu_mpyu); T2(spu_mpyh); T2(spu_mpyhh); T2(spu_mpyhhu); T2(spu_mpys);
        TI(spu_ceqbi); TI(spu_cgtbi); TI(spu_clgtbi); TI(spu_ceqhi); TI(spu_cgthi); TI(spu_clgthi);
        TI(spu_ahi); TI(spu_sfhi); TI(spu_mpyi); TI(spu_mpyui);
        TI(spu_rothi); TI(spu_roti); TI(spu_rotmahi); TI(spu_rotmai);
        T3(spu_mpya); T3(spu_mpyhha); T3(spu_mpyhhau); T3(spu_addx); T3(spu_sfx);
        T1(spu_cntb); T1(spu_xsbh); T1(spu_xshw);
#if defined(__AVX2__) && !defined(SPU_SCALAR_HELPERS)
        /* Retain the fork public float paths while checking upstream AVX2 variants. */
        up_cmp("upstream fi", spu__up_avx2_fi(a, b), spu_fi_ref(a, b), a, b, c, imm);
        up_cmp("upstream cflts", spu__up_avx2_cflts(a, imm), spu_cflts_ref(a, imm), a, b, c, imm);
        up_cmp("upstream cfltu", spu__up_avx2_cfltu(a, imm), spu_cfltu_ref(a, imm), a, b, c, imm);
        up_cmp("upstream csflt", spu__up_avx2_csflt(a, imm), spu_csflt_ref(a, imm), a, b, c, imm);
        up_cmp("upstream cuflt", spu__up_avx2_cuflt(a, imm), spu_cuflt_ref(a, imm), a, b, c, imm);
#endif
        const int pos = (int)up_rnd();
        up_cmp("cbd_pos", spu_cbd_pos(pos), up_ref_cbd_pos(pos), z, z, z, pos);
        up_cmp("chd_pos", spu_chd_pos(pos), up_ref_chd_pos(pos), z, z, z, pos);
        up_cmp("cwd_pos", spu_cwd_pos(pos), up_ref_cwd_pos(pos), z, z, z, pos);
        up_cmp("cdd_pos", spu_cdd_pos(pos), up_ref_cdd_pos(pos), z, z, z, pos);
    }
    printf(up_s_fail ? "%d FAILURES\n" : "all x86 SPU helpers match their references\n", up_s_fail);
    return up_s_fail != 0;
}

#else
static int test_upstream_sse41(void) { return 0; }
#endif
