/* Synthetic draw-engine test: the text the engine compiles is the text the
 * shared builders (rsx_vp_build_text, rsx_fp_build_text) make from the raw
 * draw state, and state on units a program does not read leaves it alone.
 *
 * An offline warm-up of the SPIR-V cache (rsx_shader_prewarm) calls the same
 * builders with a state it describes itself. Its files are only useful if
 * their names -- a hash of the text -- are the names the engine asks for, so
 * every draw below compares the engine's own text with the builders' text for
 * the registers the draw set, given as a warm-up would give them: per-unit
 * state for every bound unit, not only the sampled ones. No title data. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
#include "rsx_texture_layout.h"

static char g_vs[256 * 1024], g_ps[256 * 1024];
static char s_vs_built[256 * 1024], s_ps_built[256 * 1024];
static int g_creates;
static u32 capture_pipeline_create(void* u, const char* vs, const char* ps,
                                   const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                   u32 stride, rsx_be_format rt, u32 rt_count)
{
    g_creates++;
    snprintf(g_vs, sizeof g_vs, "%s", vs ? vs : "");
    snprintf(g_ps, sizeof g_ps, "%s", ps ? ps : "");
    return stub_pipeline_create(u, vs, ps, rs, layout, stride, rt, rt_count);
}

#define FP_OFFSET  0x00030000u   /* main memory */
#define TEXD_BASE  0x00050000u   /* texture bytes, main memory */
#define FMT_ARGB8  (0x85u | 0x20u)

/* What a warm-up knows: the registers, decoded per unit. */
static rsx_fp_text_state g_raw;
static u32 g_vp_words[16], g_vp_instrs, g_vp_start, g_vtex_enabled;
static u8 g_fp[64];
static u32 g_fp_size;

/* The render-state registers the draws below write, as a warm-up reads them. */
enum { R_PACKER, R_SURFACE, R_ALPHA_EN, R_ALPHA_FUNC, R_COUNT };
static u32 g_reg[R_COUNT];
static void reg(u32 method, u32 value)
{
    m(method, value);
    if (method == 0x1FECu) g_reg[R_PACKER] = value;
    if (method == 0x0208u) g_reg[R_SURFACE] = value;
    if (method == 0x0304u) g_reg[R_ALPHA_EN] = value;
    if (method == 0x0308u) g_reg[R_ALPHA_FUNC] = value;
}

static u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void set_vp(const u8* vp, u32 instrs, u32 slot)
{
    m(0x1E9C, slot);
    for (u32 i = 0; i < instrs * 4u; i++) { g_vp_words[i] = rd32(vp + i * 4); m(0xB80 + i * 4, g_vp_words[i]); }
    m(0x1EA0, slot);
    g_vp_instrs = instrs; g_vp_start = slot;
}
static void set_fp(const u8* fp, u32 size, u32 ctrl)
{
    memcpy(vm_base + FP_OFFSET, fp, size);
    memcpy(g_fp, fp, size); g_fp_size = size;
    m(0x8E4, FP_OFFSET | 2);
    m(0x1D60, ctrl);
    g_raw.ctrl = ctrl;
}
/* A 2D (or, with `cube`, six-face) ARGB8 texture of w x h on `unit`. */
static void bind_unit(u32 unit, int cube, u32 w, u32 h, u32 wrap)
{
    const u32 b = 0x1A00u + unit * 0x20u;
    const u32 fmt = 2u | ((cube ? 1u : 0u) << 2) | (2u << 4) | (FMT_ARGB8 << 8) | (1u << 16);
    m(b + 0x00, TEXD_BASE + unit * 0x4000u);
    m(b + 0x04, fmt);
    m(b + 0x08, wrap);
    m(b + 0x0C, 0x80000000u);
    m(b + 0x10, 0xAAE4u);
    m(b + 0x14, 0);
    m(b + 0x18, (w << 16) | h);
    if (cube && w == h) g_raw.cube_mask |= 1u << unit; else g_raw.cube_mask &= ~(1u << unit);
    g_raw.gamma_masks[unit] = rsx_texture_gamma_mask(FMT_ARGB8, 0xAAE4u, 0, wrap);
    g_raw.expand_masks[unit] = rsx_texture_expand_mask(FMT_ARGB8, 0xAAE4u, 0, wrap);
}
static void unbind_unit(u32 unit)
{
    m(0x1A00u + unit * 0x20u + 0x0C, 0);
    g_raw.cube_mask &= ~(1u << unit);
    g_raw.gamma_masks[unit] = g_raw.expand_masks[unit] = 0;
}
static void bind_vertex_unit(u32 unit)
{
    const u32 b = 0x0900u + unit * 0x20u;
    m(b + 0x00, TEXD_BASE + 0x40000u);
    m(b + 0x04, 2u | (2u << 4) | (FMT_ARGB8 << 8) | (1u << 16));
    m(b + 0x0C, 0x80000000u);
    m(b + 0x18, (4u << 16) | 4u);
    g_vtex_enabled |= 1u << unit;
}

/* Draw, then build both texts from the raw state and compare. */
static int draw_and_compare(const char* what)
{
    const int before = g_creates;
    draw_triangle();
    rsx_fp_text_state st = g_raw;
    st.srgb_output = (u32)rsx_fp_srgb_output_enabled(g_reg[R_PACKER], g_raw.ctrl, g_reg[R_SURFACE] & 0x1Fu);
    st.alpha_test = g_reg[R_ALPHA_EN] & 1u;
    st.alpha_func = g_reg[R_ALPHA_FUNC];
    u32 nconst = 0;
    const int fi = rsx_fp_build_text(g_fp, g_fp_size, &st, s_ps_built, sizeof s_ps_built, &nconst);
    const int vi = rsx_vp_build_text((const u8*)g_vp_words, g_vp_instrs * 16u, g_vp_start,
                                     g_vtex_enabled, 0xFFFFu, s_vs_built, sizeof s_vs_built);
    const int same = fi > 0 && vi > 0 && !strcmp(g_ps, s_ps_built) && !strcmp(g_vs, s_vs_built);
    CHECK(same, "%s: the engine's text is the builders' text (fp %d, vp %d, %s)", what, fi, vi,
          strcmp(g_ps, s_ps_built) ? "ps differs" : strcmp(g_vs, s_vs_built) ? "vs differs" : "same");
    return g_creates - before;
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    g_stub_backend.pipeline_create = capture_pipeline_create;
    engine_up();
    reg(0x0208u, 0x08u);                                   /* A8R8G8B8 colour */

    u8 vp[16 * 2];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);   /* o7 (tc0) <- v8 */
    set_vp(vp, 2, 0);
    u8 fp[16];
    rsx_test_fp_tex_r0_tc0(fp, 0);
    set_fp(fp, 16, 0x40);

    printf("-- a fresh engine whose first draw has a stale cube and vertex texture\n");
    bind_unit(0, 0, 4, 4, 0);
    bind_unit(14, 1, 8, 8, 0);
    bind_vertex_unit(1);
    CHECK(draw_and_compare("stale state first") == 1, "one pipeline");
    CHECK(strstr(g_ps, "Texture2D    rsx_tex[16]") && !strstr(g_ps, "TextureCube") && !strstr(g_vs, "rsx_vtex"),
          "declared as if neither were bound");
    engine_down();
    memset(&g_raw, 0, sizeof g_raw); g_vtex_enabled = 0; memset(g_reg, 0, sizeof g_reg);
    engine_up();
    reg(0x0208u, 0x08u);
    set_vp(vp, 2, 0);
    set_fp(fp, 16, 0x40);

    printf("-- unit 0 sampled, nothing else bound\n");
    bind_unit(0, 0, 4, 4, 0);
    CHECK(draw_and_compare("plain") == 1, "one pipeline");
    static char plain[sizeof g_ps];
    snprintf(plain, sizeof plain, "%s", g_ps);
    CHECK(strstr(plain, "Texture2D    rsx_tex[16]") != NULL, "the shared 2D bank");

    printf("-- a cube map left bound on unit 14, which the program does not sample\n");
    bind_unit(14, 1, 8, 8, 0);
    CHECK(draw_and_compare("stale cube") == 0, "the same pipeline, not a new one");
    draw_triangle();
    CHECK(strcmp(g_ps, plain) == 0 && !strstr(g_ps, "TextureCube"),
          "and the same text: no TextureCube, the shared 2D bank");
    /* The same for a stale vertex texture and a program that fetches none. */
    bind_vertex_unit(1);
    draw_and_compare("stale vertex texture");
    CHECK(!strstr(g_vs, "rsx_vtex"), "a vertex texture it does not fetch is not declared");

    printf("-- the program samples the cube unit\n");
    u8 fpc[16];
    rsx_test_fp_tex_r0_tc0(fpc, 14);
    set_fp(fpc, 16, 0x40);
    CHECK(draw_and_compare("sampled cube") == 1, "a pipeline of its own");
    CHECK(strstr(g_ps, "TextureCube rsx_tex14 : register(t14);") &&
          strstr(g_ps, "Texture2D   rsx_tex0 : register(t0);"),
          "unit 14 is declared TextureCube, each unit on its own register");
    unbind_unit(14);

    printf("-- sRGB texture on the sampled unit, sRGB export, alpha test\n");
    set_fp(fp, 16, 0x40);
    bind_unit(0, 0, 4, 4, 0x7u << 20);                   /* gamma on R, G, B */
    CHECK(draw_and_compare("gamma") == 1 && strstr(g_ps, "_rsx_gamma_input"), "gamma decode");
    bind_unit(3, 0, 4, 4, 0x7u << 20);                   /* bound, not sampled */
    CHECK(draw_and_compare("gamma on an unsampled unit") == 0, "an unsampled sRGB unit changes nothing");
    set_fp(fp, 16, 0x00);                                /* 16-bit exports */
    reg(0x1FECu, 1u);                                      /* SHADER_PACKER: sRGB */
    CHECK(draw_and_compare("sRGB export") == 1 && strstr(g_ps, "_rsx_srgb_output"), "sRGB export");
    reg(0x0304u, 1u); reg(0x0308u, 0x0204u);                 /* alpha test GREATER */
    CHECK(draw_and_compare("alpha test") == 1, "alpha test");
    reg(0x0304u, 0u); reg(0x1FECu, 0u);

    printf("-- a branching vertex program off slot 0\n");
    {
        u8 bvp[16 * 3];
        rsx_test_vp_mov_out(bvp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(bvp + 16, 0, RSX_TEST_VP_SWZ(1, 0, 2, 3), 1, 0);
        rsx_test_vp_mov_out(bvp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
        u32 d0 = rd32(bvp), d1 = rd32(bvp + 4), d2 = rd32(bvp + 8), d3 = rd32(bvp + 12);
        const u32 target = 9u + 2u;
        d1 = (d1 & ~(0x1Fu << 27)) | (0x08u << 27);
        d0 = (d0 & ~(7u << 10)) | (7u << 10);
        d2 = (d2 & ~0x3Fu) | ((target >> 3) & 0x3Fu);
        d3 = (d3 & ~(7u << 29)) | ((target & 7u) << 29);
        rsx_test_put_le32(bvp, d0); rsx_test_put_le32(bvp + 4, d1);
        rsx_test_put_le32(bvp + 8, d2); rsx_test_put_le32(bvp + 12, d3);
        set_vp(bvp, 3, 9);
        CHECK(draw_and_compare("branch at slot 9") == 1 && strstr(g_vs, "if (_pc == 2)"),
              "its blocks are its own instructions");
    }

    engine_down();
    free(vm_base);
    if (g_failures) { printf("%d FAILED\n", g_failures); return 1; }
    printf("all passed\n");
    return 0;
}
