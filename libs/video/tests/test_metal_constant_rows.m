/* bind_vs_constant_rows on a real Metal device, headless: a transform
 * program that takes its colour from one constant stages only that row (and
 * the viewport transform's), and draws that colour; the next program, reading
 * another row, draws its own; a rewritten constant is seen. The rows a
 * program does not read are left as whatever staging held. Synthetic inputs
 * only; no window. */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"
float cellAvconfExt_display_gamma(void) { return 1.0f; }   /* identity present gamma */
u8* vm_base; u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < ppu_vm_size ? offset : 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
int cellGcm_native_consumer_enabled(void) { return 0; }
void cellGcm_fifo_kick_notify(void) {}
void cellGcm_rsx_consumer_begin(void) {}
void cellGcm_rsx_consumer_end(void) {}
void rsx_ring_progress_metal_committed(uint64_t ticket, uint64_t records) { (void)ticket; (void)records; }
void rsx_ring_progress_metal_completed(uint64_t ticket, uint64_t status) { (void)ticket; (void)status; }

static unsigned checks, failures;
#define T(x, label) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL %s\n", label); } } while (0)
static void method(u32 a, u32 v) { rsx_draw_engine_method(a, v); }
static void be_float(u8* p, float f) { u32 v; memcpy(&v, &f, 4); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static u32 fbits(float f) { u32 v; memcpy(&v, &f, 4); return v; }
enum { W = 16, H = 16, VTX = 0x10000 };

static void constant(u32 slot, float r, float g, float b)
{
    method(0x1EFC, slot);
    method(0x1F00, fbits(r)); method(0x1F04, fbits(g)); method(0x1F08, fbits(b)); method(0x1F0C, fbits(1));
}
/* o0 = v0; o1 (colour 0) = c[reg] */
static void program(u32 reg)
{
    u8 vp[32];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_const_out(vp + 16, reg, 1, 0, 1);
    method(0x1E9C, 0);
    for (u32 i = 0; i < 8; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0xB80 + i * 4, w); }
    method(0x1EA0, 0);
}
static void draw(void) { method(0x1808, RSX_PRIMITIVE_TRIANGLES); method(0x1814, 2u << 24); method(0x1808, 0); }
static u32 centre(void)
{
    u8 px[W * H * 4];
    T(s_engine_backend.submit_and_wait_checked(NULL, 0), "GPU completion");
    s_engine_backend.readback(NULL, g.surfaces[0].handle, 0, 0, W, H, px, W * 4);
    u32 v; memcpy(&v, px + ((H / 2) * W + W / 2) * 4, 4);
    return v & 0x00FFFFFFu;
}
static int is_rgb(u32 px, u8 r, u8 g, u8 b)
{
    const u8* c = (const u8*)&px;   /* either channel order */
    return c[1] == g && ((c[0] == r && c[2] == b) || (c[0] == b && c[2] == r));
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("SKIP: no Metal device"); return 0; }
        s_queue = [s_dev newCommandQueue]; s_headless = 1; s_guest_shaders = 1;
        vm_base = calloc(1, 1u << 20); ppu_vm_size = 1u << 20;
        setenv("PS3RECOMP_RSX_VS_CONSTANT_ROWS", "1", 1);
        rsx_draw_engine_set_backend(&s_engine_backend);
        T(rsx_draw_engine_init(W, H) == 0, "initialization");
        method(0x194, 0xFEED0000); method(0x210, 0x40000); method(0x20C, W * 4);
        method(0x200, (u32)W << 16); method(0x204, (u32)H << 16); method(0x208, 0x148);
        method(0x1740, 2u | (4u << 4) | (16u << 8)); method(0x1680, 0x80000000u | VTX);
        static const float pos[3][4] = { { -1, -1, 0.5f, 1 }, { 3, -1, 0.5f, 1 }, { -1, 3, 0.5f, 1 } };
        for (u32 v = 0; v < 3; v++) for (u32 i = 0; i < 4; i++) be_float(vm_base + VTX + v * 16 + i * 4, pos[v][i]);
        rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
        method(0x8E4, 0x60002); method(0x1D60, 64);
        method(0xA6C, 0x207); method(0xA70, 0); method(0xA74, 0);
        /* Staging full of something no constant holds. */
        u32 off = 0;
        if (eng_stage_reserve(1u << 16, &off)) memset(s_eng_stage + off, 0x7F, 1u << 16);
        T(s_engine_backend.submit_and_wait_checked(NULL, 0), "seed staging");

        constant(40, 1, 0, 0); constant(3, 0, 1, 0); constant(300, 0, 0, 1);
        unsigned long long rb0 = 0, bytes0 = 0, rb = 0, bytes = 0;
        rsx_draw_engine_vs_constant_row_counts(&rb0, &bytes0);
        program(40); draw();
        T(is_rgb(centre(), 255, 0, 0), "constant 40 draws red");
        program(3); draw();
        T(is_rgb(centre(), 0, 255, 0), "constant 3 draws green");
        program(300); draw();
        T(is_rgb(centre(), 0, 0, 255), "constant 300 draws blue");
        constant(300, 1, 1, 0); draw();
        T(is_rgb(centre(), 255, 255, 0), "a rewritten constant is drawn");
        rsx_draw_engine_vs_constant_row_counts(&rb, &bytes);
        T(rb - rb0 == 4 && bytes - bytes0 == 4u * 3u * 16u, "each draw staged one row and the transform's");
        T(!atomic_load(&s_eng_checked_failed), "no backend failure");
        rsx_draw_engine_shutdown();
        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
        printf("test_metal_constant_rows: checks=%u failures=%u row_binds=%llu staged_bytes=%llu\n",
               checks, failures, rb - rb0, bytes - bytes0);
        return failures ? 1 : 0;
    }
}
