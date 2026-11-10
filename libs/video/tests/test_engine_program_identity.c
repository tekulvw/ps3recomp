/* Synthetic draw-engine test of pipeline identity across many guest program
 * pairs: each distinct transform program gets its own pipeline, revisiting
 * any of them reuses it, and a transform program rewritten in place is a new
 * pipeline. Also the inline-constant collector's slot contract. No title
 * data. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"

#define N_PROGRAMS 256u

static void upload_vp(u32 swz)
{
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 0, swz, 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
}

static u32 swizzle(u32 i)
{
    return RSX_TEST_VP_SWZ(i & 3u, (i >> 2) & 3u, (i >> 4) & 3u, (i >> 6) & 3u);
}

/* Constant rows: what the last bind_vs_constant_rows staged, and a copy of
 * the rows it was told the program reads plus the transform's. */
static int g_row_binds;
static u32 g_row_first, g_row_count, g_row_runs;
static u8 g_row_block[(RSX_DSP_NUM_CONSTANTS + 2) * 16];
static void stub_bind_vs_rows(void* u, const void* d, u32 n, const u32* mask)
{
    (void)u;
    g_row_binds++; g_row_first = 0; g_row_count = 0; g_row_runs = 0;
    memset(g_row_block, 0xEE, sizeof g_row_block);
    if (!d || n > sizeof g_row_block || !rsx_be_row_mask_fits(mask, n)) return;
    for (u32 row = 0, k = 0; rsx_be_row_run(mask, &row, &k); row += k) {
        if (!g_row_runs++) g_row_first = row;
        g_row_count += k;
        memcpy(g_row_block + row * 16u, (const u8*)d + row * 16u, k * 16u);
    }
    memcpy(g_row_block + n - 32u, (const u8*)d + n - 32u, 32u);
}
/* A program of MOVs: o0 from v0, then one constant each into o1, o7, ... */
static void upload_const_vp(const u32* regs, u32 n, int indexed)
{
    u8 vp[16 * 8];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, n == 0);
    for (u32 i = 0; i < n; i++)
        rsx_test_vp_mov_const_out(vp + 16 * (i + 1), regs[i], 7 + i, indexed && i == 0, i + 1 == n);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4 * (n + 1); i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
}
static void put_constant(u32 slot, u32 seed)
{
    m(0x1EFC, slot);                                  /* TRANSFORM_CONSTANT_LOAD */
    for (u32 w = 0; w < 4; w++) m(0x1F00 + w * 4, seed + w);
}
static int row_is(u32 slot, u32 seed)
{
    for (u32 w = 0; w < 4; w++) { u32 v; memcpy(&v, g_row_block + slot * 16u + w * 4u, 4); if (v != seed + w) return 0; }
    return 1;
}

static void test_constant_rows(void)
{
    /* The analysis on its own. */
    {
        u8 vp[16 * 4];
        rsx_vp_constant_range r;
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 1);
        CHECK(rsx_vp_analyze_constants(vp, 16, &r) == 1 && !r.all && r.count == 0,
              "a program without constants reads no rows (%u+%u all %d)", r.first, r.count, r.all);
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_const_out(vp + 16, 12, 7, 0, 0);
        rsx_test_vp_mov_const_out(vp + 32, 7, 8, 0, 1);
        CHECK(rsx_vp_analyze_constants(vp, 48, &r) == 3 && !r.all && r.first == 7 && r.count == 6,
              "constants 12 and 7: rows 7..12 (%u+%u)", r.first, r.count);
        CHECK(r.mask[0] == ((1u << 7) | (1u << 12)) && !r.mask[1] && !r.mask[15],
              "and exactly those two rows in the mask (%08X)", r.mask[0]);
        rsx_test_vp_mov_const_out(vp + 16, 3, 7, 0, 0);
        rsx_test_vp_mov_const_out(vp + 32, 500, 8, 0, 1);
        CHECK(rsx_vp_analyze_constants(vp, 48, &r) == 3 && !r.all && r.mask[0] == (1u << 3) &&
              r.mask[15] == (1u << (500 - 480)) && !r.mask[7],
              "constants 3 and 500: two rows, not the 498 between");
        /* The run walker over a mask with runs across word boundaries. */
        u32 m[16] = { 0 };
        m[0] = 0x80000001u; m[1] = 0xFFFFFFFFu; m[2] = 0x1u; m[15] = 0x80000000u;
        u32 runs[4][2], nr = 0;
        for (u32 row = 0, k = 0; nr < 4 && rsx_be_row_run(m, &row, &k); row += k) { runs[nr][0] = row; runs[nr][1] = k; nr++; }
        CHECK(nr == 3 && runs[0][0] == 0 && runs[0][1] == 1 && runs[1][0] == 31 && runs[1][1] == 34 &&
              runs[2][0] == 511 && runs[2][1] == 1, "row runs: 0, 31..64, 511 (%u runs)", nr);
        CHECK(rsx_be_row_mask_fits(m, 512u * 16u + 32u) && !rsx_be_row_mask_fits(m, 64u * 16u + 32u),
              "a mask fits a block only when its last row does");
        rsx_test_vp_mov_const_out(vp + 16, 12, 7, 0, 0);
        rsx_test_vp_mov_const_out(vp + 32, 7, 8, 1, 1);
        CHECK(rsx_vp_analyze_constants(vp, 48, &r) == 3 && r.all, "an indexed constant reads them all");
        rsx_test_vp_mov_const_out(vp + 32, 7, 8, 0, 0);
        CHECK(rsx_vp_analyze_constants(vp, 48, &r) == -1 && r.all, "an unterminated program reads them all");
        /* A branch (scalar opcode BRA) anywhere makes the range unknown. */
        rsx_test_vp_mov_const_out(vp + 32, 7, 8, 0, 1);
        u32 d1; memcpy(&d1, vp + 20, 4); d1 = (d1 & ~(0x1Fu << 27)) | (0x08u << 27); memcpy(vp + 20, &d1, 4);
        CHECK(rsx_vp_analyze_constants(vp, 48, &r) == 3 && r.all, "flow control reads them all");
    }

    /* Read once: set before the first draw that could ask. */
    setenv("PS3RECOMP_RSX_VS_CONSTANT_ROWS", "1", 1);
    g_stub_backend.bind_vs_constant_rows = stub_bind_vs_rows;
    g_stub_backend.rebind_vs_constants = stub_rebind_vs;
    g_rebind_answer = 1;
    engine_up();
    rsx_test_fp_instr(vm_base + TEX_OFFSET,
        RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, TEX_OFFSET | 2);
    m(0x1D60, 64);
    put_constant(7, 0x700); put_constant(9, 0x900); put_constant(12, 0xC00); put_constant(20, 0x1400);
    put_constant(300, 0x3000);
    const u32 a[2] = { 12, 7 };
    upload_const_vp(a, 2, 0);
    g_row_binds = 0; g_vs_binds = g_vs_rebinds = 0;
    draw_triangle();
    CHECK(g_row_binds == 1 && g_vs_binds == 0 && g_row_first == 7 && g_row_count == 2 && g_row_runs == 2,
          "a program reading constants 7 and 12 stages those two rows (%d binds, from %u, %u rows in %u runs)",
          g_row_binds, g_row_first, g_row_count, g_row_runs);
    CHECK(row_is(7, 0x700) && row_is(12, 0xC00), "with their values");
    { u8 fill[32]; memset(fill, 0xEE, sizeof fill);
      CHECK(memcmp(g_row_block + RSX_DSP_NUM_CONSTANTS * 16u, fill, sizeof fill) != 0,
            "and the transform rows"); }
    draw_triangle();
    CHECK(g_row_binds == 1 && g_vs_rebinds == 1, "unchanged constants and program: rebound (%d, %d)",
          g_row_binds, g_vs_rebinds);
    /* Same constants, a program reading a row the copy does not hold. */
    const u32 b[1] = { 20 };
    upload_const_vp(b, 1, 0);
    draw_triangle();
    CHECK(g_row_binds == 2 && g_row_first == 20 && g_row_count == 1 && row_is(20, 0x1400),
          "a program reading other rows is not served the old copy (%d, %u+%u)",
          g_row_binds, g_row_first, g_row_count);
    /* Back to rows inside a copy that holds only row 20: staged again. */
    upload_const_vp(a, 2, 0);
    draw_triangle();
    CHECK(g_row_binds == 3 && g_row_first == 7, "nor one reading rows outside it (%d)", g_row_binds);
    /* Row 9 lies between the copy's 7 and 12 but is not in it. */
    const u32 c9[1] = { 9 };
    upload_const_vp(c9, 1, 0);
    draw_triangle();
    CHECK(g_row_binds == 4 && g_row_first == 9 && g_row_count == 1 && row_is(9, 0x900),
          "nor one reading a row between the copy's rows (%d, %u)", g_row_binds, g_row_first);
    /* Indexed: every row, through bind_vs_constants. */
    upload_const_vp(a, 2, 1);
    const int full = g_vs_binds;
    draw_triangle();
    CHECK(g_vs_binds == full + 1 && g_row_binds == 4, "an indexed program binds the whole block");
    /* A full copy serves any program after it. */
    upload_const_vp(b, 1, 0);
    draw_triangle();
    CHECK(g_row_binds == 4 && g_vs_binds == full + 1, "and the whole block serves a narrower program next");
    /* A changed constant stages again. */
    put_constant(20, 0x1500);
    draw_triangle();
    CHECK(g_row_binds == 5 && row_is(20, 0x1500), "a written constant is staged again");
    unsigned long long rb = 0, rbytes = 0;
    rsx_draw_engine_vs_constant_row_counts(&rb, &rbytes);
    CHECK(rb >= 4 && rbytes > 0 && rbytes <= rb * 4u * 16u, "row binds are counted (%llu, %llu bytes)", rb, rbytes);
    engine_down();
    g_stub_backend.bind_vs_constant_rows = NULL;
    g_stub_backend.rebind_vs_constants = NULL;
    /* Without the hook the whole block is bound, as before. */
    engine_up();
    rsx_test_fp_instr(vm_base + TEX_OFFSET,
        RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, TEX_OFFSET | 2);
    m(0x1D60, 64);
    upload_const_vp(a, 2, 0);
    const int before = g_vs_binds, rows_before = g_row_binds;
    draw_triangle();
    CHECK(g_vs_binds == before + 1 && g_row_binds == rows_before, "a backend without the hook gets the whole block");
    engine_down();
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    engine_up();
    rsx_test_fp_instr(vm_base + TEX_OFFSET,
        RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, TEX_OFFSET | 2);
    m(0x1D60, 64);

    u32 handle[N_PROGRAMS];
    for (u32 i = 0; i < N_PROGRAMS; i++) {
        upload_vp(swizzle(i));
        draw_triangle();
        handle[i] = stub.bound_pipeline;
    }
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS,
          "each distinct transform program builds one pipeline (%d)", stub.n_pipeline_create);
    int distinct = 1;
    for (u32 i = 0; i < N_PROGRAMS; i++)
        for (u32 j = i + 1; j < N_PROGRAMS; j++)
            if (handle[i] == handle[j]) distinct = 0;
    CHECK(distinct, "and the pipelines are distinct");

    int same = 1;
    for (u32 r = 0; r < 2; r++)
        for (u32 k = 0; k < N_PROGRAMS; k++) {
            const u32 i = (k * 97u + r) % N_PROGRAMS;
            upload_vp(swizzle(i));
            draw_triangle();
            if (stub.bound_pipeline != handle[i]) same = 0;
        }
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS && same,
          "revisiting every program reuses its own pipeline (%d)", stub.n_pipeline_create);

    /* The same slot, the same length, different bytes: a new program. */
    upload_vp(swizzle(5));
    draw_triangle();
    CHECK(stub.bound_pipeline == handle[5], "program 5 is bound");
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 1, swizzle(5), 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    draw_triangle();
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS + 1 && stub.bound_pipeline != handle[5],
          "a program rewritten in place is not taken for the old one (%d)", stub.n_pipeline_create);
    upload_vp(swizzle(5));
    draw_triangle();
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS + 1 && stub.bound_pipeline == handle[5],
          "and restoring it finds the old pipeline again");
    engine_down();

    /* The vertex input layout follows the program's bytes: the same slot
     * rewritten to read another attribute builds a pipeline for that layout,
     * and restoring it finds the first again. */
    {
        engine_up();
        rsx_test_fp_instr(vm_base + TEX_OFFSET,
            RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        m(0x8E4, TEX_OFFSET | 2);
        m(0x1D60, 64);
        u8 vp[16];
        for (int pass = 0; pass < 3; pass++) {
            const u32 input = pass == 1 ? 3u : 0u;
            rsx_test_vp_mov_out(vp, input, swizzle(0), 0, 1);
            m(0x1E9C, 0);
            for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
            m(0x1EA0, 0);
            const int before = stub.n_pipeline_create;
            draw_triangle();
            if (pass == 0) CHECK(g_pipeline_layout_mask == 1u, "a program reading v0 lays out v0 (0x%X)", g_pipeline_layout_mask);
            if (pass == 1) CHECK(stub.n_pipeline_create == before + 1 && g_pipeline_layout_mask == 8u,
                                 "rewritten in place to read v3, it lays out v3 (0x%X)", g_pipeline_layout_mask);
            if (pass == 2) CHECK(stub.n_pipeline_create == before, "and restored, it finds the v0 pipeline again");
        }
        engine_down();
    }

    /* Only the slots a program has are written. */
    static rsx_fp_constant_block block;
    memset(&block, 0xAB, sizeof block);
    u8 fp[64] = {0};
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    CHECK(rsx_fp_collect_constants(fp, sizeof fp, &block) == 0 && block.count == 0,
          "a program without constants has no slots");
    CHECK(block.values[0][0] == 0xABABABABu, "and the unused slots are left alone");

    /* The engine keeps what a fragment program's bytes decide (sampled
     * units, inline constants, structural hash) for the programs recent draws
     * used, and must still see a constant patched into the program in place,
     * as a title does between draws. MOV r0, c0 ; END, then c0's 16 bytes. */
    {
        engine_up();
        upload_vp(swizzle(0));
        u8* fp = vm_base + TEX_OFFSET + 0x1000u;
        memset(fp, 0, 64);
        rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
                          2u /* const */ | RSX_TEST_FP_SWZ_IDENT);
        const u32 a[4] = { 0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u };   /* 1 2 3 4 */
        const u32 b[4] = { 0xBF800000u, 0x3F000000u, 0x00000000u, 0x41200000u };   /* -1 .5 0 10 */
        for (u32 i = 0; i < 4; i++) rsx_test_fp_put_word(fp + 16 + i * 4, a[i]);
        m(0x8E4, (TEX_OFFSET + 0x1000u) | 2);
        m(0x1D60, 64);
        unsigned long long hits0 = 0, misses0 = 0, hits = 0, misses = 0;
        rsx_draw_engine_fp_memo_counts(&hits0, &misses0);
        draw_triangle();
        const int pipelines = stub.n_pipeline_create;
        u32 got[4]; memcpy(got, g_ps_block, 16);
        CHECK(g_ps_bytes >= 16 && !memcmp(got, a, 16), "the program's inline constant reaches the draw");
        unsigned long long ph0 = 0, pm0 = 0, ph = 0, pm = 0;
        rsx_draw_engine_pipeline_memo_counts(&ph0, &pm0);
        draw_triangle(); draw_triangle();
        rsx_draw_engine_pipeline_memo_counts(&ph, &pm);
        CHECK(ph - ph0 == 2 && pm == pm0 && stub.n_pipeline_create == pipelines,
              "repeat draws take the previous draw's pipeline without a key (%llu hits)", ph - ph0);
        rsx_draw_engine_fp_memo_counts(&hits, &misses);
        CHECK(misses - misses0 == 1 && hits - hits0 >= 2, "repeat draws reuse the program's products (%llu hits, %llu misses)",
              hits - hits0, misses - misses0);
        memcpy(got, g_ps_block, 16);
        CHECK(!memcmp(got, a, 16), "and still bind its constant");
        for (u32 i = 0; i < 4; i++) rsx_test_fp_put_word(fp + 16 + i * 4, b[i]);
        draw_triangle();
        memcpy(got, g_ps_block, 16);
        CHECK(!memcmp(got, b, 16), "a constant patched in place reaches the next draw");
        CHECK(stub.n_pipeline_create == pipelines, "without a new pipeline (%d)", stub.n_pipeline_create);
        for (u32 i = 0; i < 4; i++) rsx_test_fp_put_word(fp + 16 + i * 4, a[i]);
        draw_triangle();
        memcpy(got, g_ps_block, 16);
        CHECK(!memcmp(got, a, 16) && stub.n_pipeline_create == pipelines, "and patching it back is seen too");
        /* A different instruction in the same bytes is a different program. */
        rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
                          2u | RSX_TEST_FP_SWZ(3, 2, 1, 0));
        draw_triangle();
        CHECK(stub.n_pipeline_create == pipelines + 1, "an instruction rewritten in place builds a pipeline (%d)",
              stub.n_pipeline_create);
        /* Render state is key input too: a blend change is a new pipeline,
         * and changing it back finds the old one. */
        const u32 before = stub.bound_pipeline;
        m(0x0310, 1u);                       /* BLEND_ENABLE */
        draw_triangle();
        CHECK(stub.n_pipeline_create == pipelines + 2 && stub.bound_pipeline != before,
              "a render-state change is not served from the previous draw (%d)", stub.n_pipeline_create);
        m(0x0310, 0u);
        draw_triangle();
        CHECK(stub.n_pipeline_create == pipelines + 2 && stub.bound_pipeline == before,
              "and restoring it finds the earlier pipeline");
        engine_down();
    }

    test_constant_rows();

    free(vm_base);
    printf("program identity failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
