/* rsx_draw_engine_methods_at (a packet's data words as one run) against
 * rsx_draw_engine_method_at (the same words one at a time).
 *
 * A seeded synthetic stream of method packets is laid out in guest memory
 * the way a FIFO holds it (big-endian words) -- texture units, vertex
 * formats and offsets, transform constant and program windows, render
 * state, label registers, clears and draws, incrementing and not -- and
 * applied to the engine twice: word by word, and as runs cut at arbitrary
 * points. Every draw must bind the same pipeline, textures and state and
 * hand the backend the same vertices; the register file (values and write
 * counts) and the pipeline-reuse counts must agree afterwards; the
 * picture-identity bookkeeping must see each word's own data address. With
 * pipeline reuse off and on. No title data.
 *
 * python3 tools/test_engine_method_runs.py */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "../rsx_picture_identity.h"

#define FIFO_EA   0x00300000u          /* the stream's words, in guest memory */
#define FP_AT     (TEX_OFFSET + 0x2000u)
#define PACKETS   20000u
#define DRAW_MAX  2048u

typedef struct { u32 subch, method, count, ea; int incrementing; } packet;
static packet g_pk[PACKETS];
static u32 g_npk, g_fifo;
static u32 g_seed;
static u32 rnd(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

static void be32(u32 ea, u32 v)
{
    u8* p = vm_base + ea;
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static u32 rd32(u32 ea)
{
    const u8* p = vm_base + ea;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
/* One packet: its words into the stream, its shape into the list. */
static u32 emit(int incrementing, u32 subch, u32 method, u32 count, const u32* args)
{
    if (g_npk >= PACKETS) { printf("[FAIL] stream full\n"); exit(1); }
    packet* p = &g_pk[g_npk++];
    p->subch = subch; p->method = method; p->count = count; p->incrementing = incrementing; p->ea = g_fifo;
    for (u32 i = 0; i < count; i++) be32(g_fifo + i * 4u, args[i]);
    g_fifo += count * 4u;
    return p->ea;
}
static void emit1(u32 method, u32 arg) { emit(1, 0, method, 1, &arg); }

/* A fragment program at FP_AT: MOV r0, c0 (inline constant) or TEX r0, unit 0. */
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

static u32 g_unit_fmt[4];
static void build_stream(void)
{
    g_npk = 0; g_fifo = FIFO_EA; g_seed = 20261119u;
    u32 a[40];
    /* A guest program pair, through the upload windows as libgcm writes them:
     * load pointer, then the program words in one incrementing packet. */
    { u8 vp[16]; rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ(0, 1, 2, 3), 0, 1);
      emit1(0x1E9C, 0);
      for (u32 i = 0; i < 4; i++) memcpy(&a[i], vp + i * 4, 4);
      emit(1, 0, 0x0B80, 4, a);
      emit1(0x1EA0, 0); }
    write_fp(1, 0x3F800000u);
    emit1(0x08E4, FP_AT | 2u);
    emit1(0x1D60, 64);
    for (u32 u = 0; u < 4; u++) g_unit_fmt[u] = 2u | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16);
    u32 draws = 0;
    while (draws < DRAW_MAX) {
        switch (rnd() % 16u) {
        case 0: case 1: {
            /* A whole texture unit, as MAG rewrites them between draws. */
            const u32 u = rnd() % 4u;
            if (rnd() % 3u == 0) g_unit_fmt[u] ^= 1u << 2;
            a[0] = TEX_OFFSET + (rnd() % 4u) * 0x40u; a[1] = g_unit_fmt[u]; a[2] = 0; a[3] = 0x80000000u;
            a[4] = 0xAAE4u; a[5] = 0; a[6] = ((rnd() & 1u) ? 8u : 4u) << 16 | 4u; a[7] = 0;
            emit(1, 0, M_TEX_UNIT + u * 0x20u, 8, a);
            break; }
        case 2:
            /* Transform constants: the load pointer and a window of them. */
            a[0] = rnd() % 8u;
            { const u32 n = 1u + 4u * (1u + rnd() % 8u);
              for (u32 i = 1; i < n; i++) a[i] = rnd();
              emit(1, 0, M_VP_UPLOAD_CONST_ID, n, a); }
            break;
        case 3:
            /* The same window non-incrementing: every word the same method. */
            { const u32 n = 4u * (1u + rnd() % 3u);
              for (u32 i = 0; i < n; i++) a[i] = rnd();
              emit(0, 0, M_VP_UPLOAD_CONST + (rnd() % 8u) * 4u, n, a); }
            break;
        case 4:
            /* Vertex attribute formats 1..3 (0 stays the position), and the offsets. */
            { const u32 at = 1u + rnd() % 3u;
              emit1(M_VTXFMT + at * 4u, (rnd() & 1u) ? (2u | (4u << 4) | (16u << 8)) : 0u);
              emit1(M_VTXBUF_OFFSET + at * 4u, 0x80000000u | VTX_OFFSET); }
            break;
        case 5:
            /* Render state, in one incrementing packet. */
            a[0] = rnd() & 1u; a[1] = rnd() & 1u; a[2] = rnd() % 8u;
            emit(1, 0, 0x0A6C, 3, a);          /* alpha test enable, alpha func, alpha ref */
            emit1(M_SCISSOR_H, (rnd() % 256u) << 16 | (rnd() % 64u));
            emit1(M_STENCIL_FUNC_REF, rnd() & 0xFFu);
            break;
        case 6:
            /* Label registers and report DMA, as the FIFO carries them. */
            emit1(0x1D6C, 0x10u); emit1(0x1D74, rnd()); emit1(0x01A8, 0xFEED0000u);
            break;
        case 7:
            /* Constant-window and program-load registers again with the
             * values already there (reuse's comparable writes). */
            emit1(0x1D60, 64); emit1(0x08E4, FP_AT | 2u);
            break;
        case 8:
            /* A long packet of plain registers (the vertex data 4f window). */
            { const u32 n = 16u + rnd() % 24u;
              for (u32 i = 0; i < n; i++) a[i] = rnd();
              emit(1, 0, 0x1C00 + (rnd() % 16u) * 16u, n > 40u ? 40u : n, a); }
            break;
        case 9:
            if (rnd() % 4u == 0) emit1(M_CLEAR_BUFFERS, 0xF0u);
            break;
        case 10:
            /* A subchannel 1 packet (3D state bound elsewhere). */
            a[0] = rnd() & 0xFFu; a[1] = rnd() & 0xFFu;
            emit(1, 1, 0x0A00, 2, a);
            break;
        default: {
            /* A draw: begin, one or two batches (non-incrementing), end. */
            const u32 prim = RSX_PRIMITIVE_TRIANGLES;
            emit1(M_BEGIN_END, prim);
            a[0] = 0u | (2u << 24); a[1] = 0u | (2u << 24);
            emit(0, 0, M_DRAW_ARRAYS, 1u + (rnd() & 1u), a);
            emit1(M_BEGIN_END, 0);
            draws++;
        } }
    }
}

/* What a run of the stream leaves behind. */
typedef struct { u32 pipeline, tex0, mask, verts, scissor[4], stencil; float v0[4]; } draw_rec;
static draw_rec g_rec[2][DRAW_MAX + 64];
static u32 g_ndraw[2];
static u64 g_regs_hash[2];
static unsigned long long g_kept[2], g_gathered[2];

static u64 mix(u64 h, u64 v) { h ^= v; h *= 0x9E3779B97F4A7C15ull; return h ^ (h >> 31); }

/* Apply the stream in one form: 0 = word by word, 1 = runs cut at random
 * points (the cuts drawn from their own generator, so both forms see the
 * same stream). */
static void apply(int form, int reuse)
{
    if (reuse) setenv("PS3RECOMP_RSX_PIPELINE_REUSE", "1", 1);
    else unsetenv("PS3RECOMP_RSX_PIPELINE_REUSE");
    unsigned long long h0, m0, x0, h1, m1, x1;
    rsx_draw_engine_pipeline_reuse_counts(&h0, &m0, &x0);
    engine_up();
    u32 cut_seed = 7u;
    g_ndraw[form] = 0;
    for (u32 k = 0; k < g_npk; k++) {
        const packet* p = &g_pk[k];
        const int before = stub.n_draw;
        if (form == 0) {
            for (u32 i = 0; i < p->count; i++)
                rsx_draw_engine_method_at((p->subch << 13) | (p->incrementing ? p->method + i * 4u : p->method),
                                          rd32(p->ea + i * 4u), p->ea + i * 4u);
        } else {
            u32 i = 0;
            while (i < p->count) {
                cut_seed = cut_seed * 1664525u + 1013904223u;
                u32 n = (cut_seed >> 8) % 3u == 0 ? 1u + (cut_seed >> 16) % 5u : p->count - i;
                if (n > p->count - i) n = p->count - i;
                rsx_draw_engine_methods_at(p->subch, p->incrementing ? p->method + i * 4u : p->method, p->incrementing,
                                           (const u32*)(vm_base + p->ea + i * 4u), n, p->ea + i * 4u);
                i += n;
            }
        }
        if (stub.n_draw != before && g_ndraw[form] < DRAW_MAX + 64u) {
            draw_rec* r = &g_rec[form][g_ndraw[form]++];
            r->pipeline = stub.bound_pipeline; r->tex0 = stub.bound_tex[0]; r->mask = stub.bound_mask;
            r->verts = stub.draw_vertices; memcpy(r->scissor, stub.scissor, sizeof r->scissor);
            r->stencil = stub.stencil_ref; memcpy(r->v0, stub.draw_first_vertex, sizeof r->v0);
        }
    }
    u64 h = 0x243F6A8885A308D3ull;
    for (u32 ridx = 0; ridx < RSX_DSP_NUM_REGS; ridx++) {
        u32 seen; const u32 v = rsx_draw_engine_register(ridx, &seen);
        h = mix(h, ((u64)seen << 32) | v);
    }
    g_regs_hash[form] = h;
    rsx_draw_engine_pipeline_reuse_counts(&h1, &m1, &x1);
    g_kept[form] = h1 - h0; g_gathered[form] = m1 - m0;
    engine_down();
}

static int compare(const char* what)
{
    int ok = g_ndraw[0] == g_ndraw[1] && g_ndraw[0] >= DRAW_MAX;
    int first = -1;
    for (u32 i = 0; ok && i < g_ndraw[0]; i++)
        if (memcmp(&g_rec[0][i], &g_rec[1][i], sizeof g_rec[0][i])) { ok = 0; first = (int)i; }
    CHECK(ok, "%s: %u draws bind the same pipeline, textures, state and vertices either way (first difference %d)",
          what, g_ndraw[0], first);
    CHECK(g_regs_hash[0] == g_regs_hash[1], "%s: the register file (values and write counts) agrees", what);
    CHECK(g_kept[0] == g_kept[1] && g_gathered[0] == g_gathered[1],
          "%s: pipeline reuse kept %llu / gathered %llu either way (runs: %llu / %llu)", what,
          g_kept[0], g_gathered[0], g_kept[1], g_gathered[1]);
    return ok;
}

/* The picture-identity bookkeeping reads each word's own data address: a
 * run whose second word is a unit's offset at a captured command address
 * must bind the picture's bytes, as the per-word path does. */
static u8 g_uploaded[64];
static void capture_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h, const void* p, u32 pitch, u32 rows)
{
    if (pitch * rows == 64u) memcpy(g_uploaded, p, 64);
    stub_texture_upload(u, t, face, mip, w, h, p, pitch, rows);
}
static void picture_case(int form)
{
    u8 picture[64]; memset(picture, 0x5A + form, 64);
    memset(vm_base + TEX_OFFSET, 0x11, 64);
    g_stub_backend.texture_upload = capture_upload;
    engine_up();
    (void)rsx_picture_reset();   /* a picture the previous form left live stays; a fresh one is made beside it */
    const u32 words_ea = FIFO_EA + 0x100000u;
    const u32 cmd_ea = words_ea + 4u;               /* the second word of the run */
    const u64 id = rsx_picture_create(TEX_OFFSET, RSX_LOCATION_MAIN, TEX_OFFSET, 64, picture);
    CHECK(id != 0, "form %d: a picture", form);
    CHECK(rsx_picture_bind_descriptor(0x3000, id) == 1, "form %d: bound to a descriptor", form);
    CHECK(rsx_picture_capture_command(cmd_ea, 0x3000, RSX_LOCATION_MAIN, TEX_OFFSET) == 1, "form %d: command captured", form);
    rsx_picture_release(id);
    /* A two-word incrementing packet starting one register before unit 0's
     * offset register: 0x19FC (a plain register), then 0x1A00 (the offset,
     * whose data word sits at cmd_ea). */
    be32(words_ea, 0u); be32(words_ea + 4u, TEX_OFFSET);
    if (form == 0) {
        rsx_draw_engine_method_at(0x19FC, 0u, words_ea);
        rsx_draw_engine_method_at(M_TEX_UNIT, TEX_OFFSET, cmd_ea);
    } else
        rsx_draw_engine_methods_at(0, 0x19FC, 1, (const u32*)(vm_base + words_ea), 2, words_ea);
    m(M_TEX_UNIT + 0x04, 2u | (2u << 4) | (0xA5u << 8) | (1u << 16));
    m(M_TEX_UNIT + 0x0C, 0x80000000u); m(M_TEX_UNIT + 0x10, 0xAAE4u); m(M_TEX_UNIT + 0x18, (4u << 16) | 4u);
    memset(g_uploaded, 0, sizeof g_uploaded);
    draw_triangle();
    CHECK(rsx_picture_ready(), "form %d: the picture command resolved", form);
    CHECK(!memcmp(g_uploaded, picture, 64), "form %d: the draw sampled the picture's bytes, not guest memory", form);
    engine_down();
    g_stub_backend.texture_upload = stub_texture_upload;
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    build_stream();
    printf("stream: %u packets, %u words\n", g_npk, (g_fifo - FIFO_EA) / 4u);
    apply(0, 0); apply(1, 0);
    compare("reuse off");
    apply(0, 1); apply(1, 1);
    compare("reuse on");
    CHECK(g_kept[0] > DRAW_MAX / 8u, "reuse on kept pipelines in this stream (%llu)", g_kept[0]);
    picture_case(0);
    picture_case(1);
    unsetenv("PS3RECOMP_RSX_PIPELINE_REUSE");
    printf("[engine_method_runs] %s (%d failed)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
