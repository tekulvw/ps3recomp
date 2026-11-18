/* PS3RECOMP_RSX_PIPELINE_REUSE: a draw whose pipeline inputs cannot have
 * changed keeps the previous draw's pipeline. The same synthetic stream of
 * methods, in-memory fragment-program edits and draws is run with reuse off,
 * on, and in verify mode (2); every draw must bind the same pipeline and the
 * same fragment constants as with reuse off, verify mode must see no
 * disagreement, and reuse must actually keep pipelines. No title data.
 *
 * python3 tools/test_engine_pipeline_reuse.py */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"

#define DRAWS 3000u
#define FP_AT (TEX_OFFSET + 0x2000u)
static u32 g_seed;
static u32 g_last[3];   /* what the stream last wrote to 0x1D60, unit 0 format, unit 0 size */
static u32 rnd(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

static void upload_vp(u32 swz)
{
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 0, swz, 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
}
/* Two programs at one address: MOV r0, c0 (inline constant) or TEX r0, unit 0. */
static void write_fp(int tex, u32 c)
{
    u8* fp = vm_base + FP_AT;
    memset(fp, 0, 64);
    if (tex) rsx_test_fp_tex_r0_tc0(fp, 0);
    else {
        rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
                          2u /* const */ | RSX_TEST_FP_SWZ_IDENT);
        for (u32 i = 0; i < 4; i++) rsx_test_fp_put_word(fp + 16 + i * 4, c + i);
    }
}

typedef struct { u32 pipeline; u8 ps[16]; } draw_rec;
static draw_rec g_rec[3][DRAWS];

/* One run of the stream; returns the pipelines created. */
static int run(int mode, draw_rec* rec)
{
    if (mode) { char v[2] = { (char)('0' + mode), 0 }; setenv("PS3RECOMP_RSX_PIPELINE_REUSE", v, 1); }
    else unsetenv("PS3RECOMP_RSX_PIPELINE_REUSE");
    g_seed = 12345u;
    engine_up();
    upload_vp(RSX_TEST_VP_SWZ(0, 1, 2, 3));
    write_fp(0, 0x3F800000u);
    m(0x8E4, FP_AT | 2);
    m(0x1D60, 64);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    g_last[0] = 64; g_last[1] = 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16); g_last[2] = (4u << 16) | 4u;
    u32 draws = 0;
    while (draws < DRAWS) {
        switch (rnd() % 17u) {
        /* Methods the key never reads: kept pipelines must survive them. */
        case 0: m(0x1EFC, rnd() % 8u); m(0x1F00 + (rnd() % 32u) * 4u, rnd()); break;
        case 1: m(M_VTXBUF_OFFSET, 0x80000000u | VTX_OFFSET); break;
        case 2: m(M_TEX_UNIT + 0x00, TEX_OFFSET + (rnd() % 4u) * 0x40u); break;
        case 3: m(0x1D6C, 0x10u); m(0x1D74, rnd()); m(0x181C, rnd() & ~3u); m(0x1820, (rnd() & 1u) | ((rnd() & 1u) << 4)); break;
        /* Inputs the key is made of. */
        case 4: upload_vp(RSX_TEST_VP_SWZ(rnd() & 3u, rnd() & 3u, rnd() & 3u, 3)); break;
        case 5: write_fp((int)(rnd() & 1u), 0x3F800000u + (rnd() % 7u)); break;   /* in memory, no method */
        case 6: { u8* fp = vm_base + FP_AT; if (!fp[1]) rsx_test_fp_put_word(fp + 16, rnd()); } break; /* constant patch */
        case 7: m(0x1D60, g_last[0] = (rnd() & 1u) ? 64u : 64u | 0x40u); break;
        case 8: m(M_TEX_UNIT + 0x04, g_last[1] = 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16) | ((rnd() & 1u) << 2)); break;
        case 9: m(M_TEX_UNIT + 0x18, g_last[2] = (rnd() & 1u) ? ((4u << 16) | 4u) : ((8u << 16) | 4u)); break;
        case 10: m(M_DEPTH_TEST, rnd() & 1u); break;
        /* The same value again, as MAG rewrites whole units between draws:
         * nothing changes, so the pipeline may be kept. */
        case 11: m(0x1D60, g_last[0]); m(M_TEX_UNIT + 0x04, g_last[1]); m(M_TEX_UNIT + 0x18, g_last[2]); break;
        default: {
            draw_triangle();
            rec[draws].pipeline = stub.bound_pipeline;
            memcpy(rec[draws].ps, g_ps_block, 16);
            draws++;
        }
        }
    }
    const int created = stub.n_pipeline_create;
    engine_down();
    return created;
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    unsigned long long h0, m0, x0, h1, m1, x1, h2, m2, x2;
    rsx_draw_engine_pipeline_reuse_counts(&h0, &m0, &x0);
    const int off = run(0, g_rec[0]);
    rsx_draw_engine_pipeline_reuse_counts(&h1, &m1, &x1);
    CHECK(h1 == h0 && m1 == m0, "off: nothing is kept or counted");
    const int on = run(1, g_rec[1]);
    rsx_draw_engine_pipeline_reuse_counts(&h2, &m2, &x2);
    const unsigned long long kept = h2 - h1, gathered = m2 - m1;
    CHECK(kept > DRAWS / 8u, "on: draws keep the previous pipeline (%llu kept, %llu gathered)", kept, gathered);
    int same = 1, first = -1;
    for (u32 i = 0; i < DRAWS; i++)
        if (g_rec[0][i].pipeline != g_rec[1][i].pipeline || memcmp(g_rec[0][i].ps, g_rec[1][i].ps, 16)) {
            same = 0; if (first < 0) first = (int)i;
        }
    CHECK(same && on == off, "every draw binds the same pipeline and fragment constants (first difference %d; %d vs %d pipelines)",
          first, on, off);
    run(2, g_rec[2]);
    unsigned long long h3, m3, x3;
    rsx_draw_engine_pipeline_reuse_counts(&h3, &m3, &x3);
    CHECK(x3 == x2 && h3 - h2 == kept, "verify mode: the full gather agrees every time it was skipped (%llu checks)", h3 - h2);
    same = 1;
    for (u32 i = 0; i < DRAWS; i++)
        if (g_rec[0][i].pipeline != g_rec[2][i].pipeline || memcmp(g_rec[0][i].ps, g_rec[2][i].ps, 16)) same = 0;
    CHECK(same, "and binds what reuse off binds");
    unsetenv("PS3RECOMP_RSX_PIPELINE_REUSE");
    printf("[engine_pipeline_reuse] %s (%d failed)\n", g_failures ? "FAIL" : "PASS", g_failures);
    free(vm_base);
    return g_failures ? 1 : 0;
}
