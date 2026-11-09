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
    if (s_bad) { printf("test_spu_int_x86: %ld mismatches of %ld checks\n", s_bad, s_checks); return 1; }
    printf("test_spu_int_x86: ok (%ld checks, 78 helpers)\n", s_checks);
    return 0;
#endif
}
