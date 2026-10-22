/* Synthetic draw-engine test of RSX_ENGINE_VTX_PROBE_FP: a draw through the
 * named fragment program reports each attribute's array format and current
 * (constant) value, the vertices exactly as fetched, and the transform
 * constants; a draw through another program and draws past the per-program
 * limit report nothing. No title data. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_fp_decompiler.h"
#include <unistd.h>

static void store_be_f32(u8* p, float f)
{
    u32 w; memcpy(&w, &f, 4);
    p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w;
}

static void upload_vp_from_attr(u32 input)
{
    u8 vp[16];
    rsx_test_vp_mov_out(vp, input, RSX_TEST_VP_SWZ(0, 1, 2, 3), 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
}

static u32 f2u(float f) { u32 w; memcpy(&w, &f, 4); return w; }

int main(int argc, char** argv)
{
    const int any = argc > 1 && !strcmp(argv[1], "any");
    const int trig = argc > 1 && !strcmp(argv[1], "trigger");
    char trigger_path[1024];
    const char* tmpdir = getenv("TMPDIR");
    snprintf(trigger_path, sizeof trigger_path, "%s/vtx_probe_trigger_XXXXXX",
             tmpdir && *tmpdir ? tmpdir : "/tmp");
    if (trig) {
        const int fd = mkstemp(trigger_path);
        if (fd < 0) return 2;
        close(fd);
        unlink(trigger_path);                          /* armed only once it exists */
        setenv("RSX_ENGINE_VTX_PROBE_TRIGGER", trigger_path, 1);
    }
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;

    /* Two fragment programs: MOV r0, COL0 and MOV r0, TC0. */
    const u32 fp_a = TEX_OFFSET, fp_b = TEX_OFFSET + 0x100u;
    rsx_test_fp_instr(vm_base + fp_a,
        RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_instr(vm_base + fp_b,
        RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    const u64 hash_a = rsx_fp_structural_hash(vm_base + fp_a, 16, 1469598103934665603ull);
    char env[64];
    snprintf(env, sizeof env, "%016llx", (unsigned long long)hash_a);
    setenv("RSX_ENGINE_VTX_PROBE_FP", any ? "*" : env, 1);
    setenv("RSX_ENGINE_VTX_PROBE_MAX", "1", 1);
    setenv("RSX_ENGINE_VTX_PROBE_CONST", "5,7-8", 1);

    /* Capture stderr. */
    FILE* cap = tmpfile();
    if (!cap) return 2;
    fflush(stderr);
    const int saved = dup(2);
    dup2(fileno(cap), 2);

    engine_up();
    /* attr0: one float2 per four vertices (divided), as a particle record;
     * attr3 disabled and fed from its current value. */
    for (u32 i = 0; i < 2; i++) {
        store_be_f32(vm_base + VTX_OFFSET + i * 8u, 10.0f + (float)i);
        store_be_f32(vm_base + VTX_OFFSET + i * 8u + 4u, -1.0f);
    }
    m(M_VTXFMT, 2u | (2u << 4) | (8u << 8) | (4u << 16));
    m(M_VTXFMT + 3u * 4u, 2u);                        /* attr3 size 0: disabled */
    m(0x1C30, f2u(2.0f)); m(0x1C34, f2u(3.0f)); m(0x1C38, f2u(0.0f)); m(0x1C3C, f2u(1.0f));
    m(0x1FC0, 0u);
    m(0x1EFC, 5u);                                     /* constant load slot 5 */
    m(0x1F00, f2u(0.5f)); m(0x1F04, f2u(0.25f)); m(0x1F08, 0); m(0x1F0C, f2u(1.0f));

    m(0x8E4, fp_b | 2);
    m(0x1D60, 64);
    upload_vp_from_attr(0);
    draw_triangle();                                   /* other program: silent */
    m(0x8E4, fp_a | 2);
    if (trig) {
        draw_triangle();                               /* not armed yet: silent */
        FILE* t = fopen(trigger_path, "wb");
        if (t) fclose(t);
        draw_triangle();                               /* same frame: not re-checked */
        rsx_draw_engine_present();                     /* next frame checks and arms */
    }
    if (any) {
        draw_triangle();                               /* unblended: silent under "*" */
        m(0x0310, 1);                                  /* BLEND_ENABLE */
    }
    draw_triangle();                                   /* probed */
    upload_vp_from_attr(3);
    draw_triangle();                                   /* over the limit: silent */
    engine_down();

    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    static char out[1 << 16];
    rewind(cap);
    const size_t n = fread(out, 1, sizeof out - 1, cap);
    out[n] = 0;
    fclose(cap);

    char head[96];
    snprintf(head, sizeof head, "[vtx probe] fp %s pipe", env);
    int heads = 0, ours = 0;
    for (const char* p = out; (p = strstr(p, "[vtx probe] fp ")); p++) {
        const char* eol = strchr(p, '\n');
        if (eol && eol - p > 4 && !strncmp(eol - 4, " end", 4)) continue;
        heads++;
        if (!strncmp(p, head, strlen(head))) ours++;
    }
    CHECK(heads == 1 && ours == 1, "exactly one probed draw, through the named program (%d, %d)",
          heads, ours);
    CHECK(strstr(out, "prim 5 batches arr 1 idx 0 refs 3 verts 3") != NULL, "the batch summary");
    CHECK(strstr(out, "arr[0] first 0 count 3") != NULL, "the array batch");
    CHECK(strstr(out, "a0  fmt 0x00040822 type 2 size 2 stride 8 freq 4 div") != NULL,
          "attr0's divided array format");
    CHECK(strstr(out, "a3  fmt 0x00000002 type 2 size 0") != NULL &&
          strstr(out, "current (2 3 0 1) writes 4") != NULL,
          "attr3 disabled, with its current value and write count");
    CHECK(strstr(out, "v0 ref 0: a0(10 -1 0 1)") != NULL &&
          strstr(out, "v2 ref 2: a0(10 -1 0 1)") != NULL,
          "vertices 0..3 share the divided element 0");
    CHECK(strstr(out, "c5 (0.5 0.25 0 1)") != NULL && strstr(out, "c7 (0 0 0 0)") != NULL &&
          strstr(out, "c6 (") == NULL,
          "the listed constant rows only");
    CHECK(strstr(out, "viewport scale") != NULL, "the viewport");
    if (trig) {
        unlink(trigger_path);
        CHECK(strstr(out, "[vtx probe] armed at frame 1") != NULL, "armed by the trigger file a frame later");
    }
    if (g_failures) fputs(out, stdout);
    printf("%s\n", g_failures ? "FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
