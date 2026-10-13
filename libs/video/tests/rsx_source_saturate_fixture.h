/* Synthetic programs only: clamp one source, add a positive offset, then
 * LG2/EX2. A missing clamp sends negative values through log2 and yields
 * nonfinite output instead of the known finite gray. */
#ifndef RSX_SOURCE_SATURATE_FIXTURE_H
#define RSX_SOURCE_SATURATE_FIXTURE_H
#include "../rsx_test_programs.h"

static inline void source_saturate_fp(u8 fp[64], u32 operand, u32 modifier,
                                      int absolute, int negate,
                                      int destination_sat, float offset)
{
    u32 src[3] = {2u, 2u, 2u}; /* shared inline constant */
    src[operand] = RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ(0,0,0,0);
    if (negate) src[operand] |= 1u << 17;
    if (absolute && operand == 0) src[0] |= 1u << 29;
    if (absolute && operand == 1) src[1] |= 1u << 18;
    src[1] |= modifier << (19 + 3 * operand); /* all source modifiers in word 2 */
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(operand == 2 ? 4 : 3) |
        RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_OUTREG(1) | RSX_TEST_FP_INSRC_TC0 |
        (destination_sat ? 1u << 31 : 0), src[0]);
    rsx_test_fp_put_word(fp + 8, src[1]);
    rsx_test_fp_put_word(fp + 12, src[2]);
    union { float f; u32 u; } constant = {.f = offset};
    for (u32 lane = 0; lane < 4; lane++)
        rsx_test_fp_put_word(fp + 16 + 4 * lane, constant.u);
    rsx_test_fp_instr(fp + 32, RSX_TEST_FP_OPC(0x1D) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_OUTREG(1), (1u << 2) | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_instr(fp + 48, RSX_TEST_FP_OPC(0x1C) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_END, (1u << 2) | RSX_TEST_FP_SWZ_IDENT);
}
#endif
