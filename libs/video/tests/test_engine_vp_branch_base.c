/* Synthetic draw-engine test: a branching transform program loaded off slot 0.
 *
 * Branch targets in a transform program are absolute slots of the 512-slot
 * program memory. The engine hands the decompiler the program's bytes from
 * its start slot, so the decompiler has to be told that slot to turn each
 * target into an instruction of this program. The same program loaded at
 * slot 0 and at slot 5 (its targets moved with it, as a loader relocates
 * them) is one program: both must decompile to the same text, which branches
 * to its own third instruction. No title data. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_vp_decompiler.h"

static char g_vs[256 * 1024];
static int g_creates;
static u32 capture_pipeline_create(void* u, const char* vs, const char* ps,
                                   const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                   u32 stride, rsx_be_format rt, u32 rt_count)
{
    g_creates++;
    snprintf(g_vs, sizeof g_vs, "%s", vs ? vs : "");
    return stub_pipeline_create(u, vs, ps, rs, layout, stride, rt, rt_count);
}

static u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

/* Make instruction `p` also an unconditional BRA to absolute slot `target`. */
static void make_branch(u8* p, u32 target)
{
    u32 d0 = rd32(p), d1 = rd32(p + 4), d2 = rd32(p + 8), d3 = rd32(p + 12);
    d1 = (d1 & ~(0x1Fu << 27)) | (0x08u << 27);         /* scalar opcode BRA */
    d0 = (d0 & ~(7u << 10)) | (7u << 10);                /* condition: always */
    d0 = (d0 & ~(1u << 23)) | (((target >> 9) & 1u) << 23);
    d2 = (d2 & ~0x3Fu) | ((target >> 3) & 0x3Fu);
    d3 = (d3 & ~(7u << 29)) | ((target & 7u) << 29);
    rsx_test_put_le32(p, d0); rsx_test_put_le32(p + 4, d1);
    rsx_test_put_le32(p + 8, d2); rsx_test_put_le32(p + 12, d3);
}

/* Three instructions loaded at `slot`: the first branches over the second to
 * the third, which ends the program. */
static void upload_branching_vp(u32 slot)
{
    u8 vp[16 * 3];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 0, RSX_TEST_VP_SWZ(1, 0, 2, 3), 1, 0);
    rsx_test_vp_mov_out(vp + 32, 0, RSX_TEST_VP_SWZ_IDENT, 0, 1);
    make_branch(vp, slot + 2u);
    m(0x1E9C, slot);                                  /* TRANSFORM_PROGRAM_LOAD */
    for (u32 i = 0; i < 12; i++) m(0xB80 + i * 4, rd32(vp + i * 4));
    m(0x1EA0, slot);                                  /* TRANSFORM_PROGRAM_START */
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    g_stub_backend.pipeline_create = capture_pipeline_create;
    engine_up();
    rsx_test_fp_instr(vm_base + TEX_OFFSET,
        RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, TEX_OFFSET | 2);
    m(0x1D60, 64);

    upload_branching_vp(0);
    draw_triangle();
    static char at0[sizeof g_vs];
    snprintf(at0, sizeof at0, "%s", g_vs);
    CHECK(g_creates == 1 && strstr(at0, "if (_pc == 2)") && strstr(at0, "-> 2 "),
          "at slot 0 the program branches to its third instruction");

    upload_branching_vp(5);
    draw_triangle();
    CHECK(g_creates == 2, "the program at slot 5 is a pipeline of its own (%d)", g_creates);
    CHECK(strstr(g_vs, "if (_pc == 2)") && strstr(g_vs, "-> 2 ") && !strstr(g_vs, "-> 7 "),
          "at slot 5 its target (slot 7) is still its third instruction");
    CHECK(strcmp(g_vs, at0) == 0, "and the two decompile to the same text");

    /* Back to slot 0: the first pipeline again, not the slot-5 one. */
    upload_branching_vp(0);
    draw_triangle();
    CHECK(g_creates == 2, "returning to slot 0 reuses its pipeline (%d)", g_creates);

    /* The decompiler on its own: the base names the slot `ucode` starts at. */
    {
        u8 vp[16 * 3];
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp + 16, 0, RSX_TEST_VP_SWZ(1, 0, 2, 3), 1, 0);
        rsx_test_vp_mov_out(vp + 32, 0, RSX_TEST_VP_SWZ_IDENT, 0, 1);
        make_branch(vp, 42u);
        static char a[256 * 1024], b[256 * 1024];
        const int na = rsx_vp_decompile_compact_base_ex(vp, sizeof vp, 0, 0xFFFFu, 40u, a, sizeof a);
        make_branch(vp, 2u);
        const int nb = rsx_vp_decompile_compact_base_ex(vp, sizeof vp, 0, 0xFFFFu, 0u, b, sizeof b);
        CHECK(na == 3 && nb == 3 && strcmp(a, b) == 0,
              "slot 42 from a program at 40 is slot 2 from one at 0");
        rsx_vp_set_branch_base(40u);
        const int nc = rsx_vp_decompile_compact_base_ex(vp, sizeof vp, 0, 0xFFFFu, 0u, a, sizeof a);
        rsx_vp_set_branch_base(0u);
        CHECK(nc == 3 && strcmp(a, b) == 0, "and the explicit base ignores rsx_vp_set_branch_base");
    }

    engine_down();
    free(vm_base);
    if (g_failures) { printf("%d FAILED\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
