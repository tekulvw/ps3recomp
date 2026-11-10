/* The Vulkan backend driven by the register-file engine, with and without
 * PS3RECOMP_RSX_ENCODE_THREAD, headless under the validation layer: 64 draws,
 * each its own colour into its own 8x8 cell, a clear part-way, completion
 * tickets between draws, vertex bytes rewritten after every draw, and draws
 * that sample the target they write (colour snapshots); the frame is stored
 * to guest memory and written to argv[2] for the runner to compare across
 * modes. argv[1] is "direct" or "thread". Synthetic inputs only; no window.
 * python3 tools/test_vulkan_encode_thread.py */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "../rsx_vulkan_backend.c"
#include "../rsx_test_programs.h"
#include <assert.h>

#define GUEST_BYTES (1u << 20)
u8* vm_base; u32 ppu_vm_size, ppu_hle_inject_base;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset < GUEST_BYTES ? offset : 0; }
u32 cellGcmResolveIO(u32 offset) { return offset < GUEST_BYTES ? offset : 0; }
u32 gcm_io_offset_to_ea(u32 offset) { return offset < GUEST_BYTES ? offset : 0; }
void vm_write32(uint32_t a, uint32_t v)
{
    if (a + 4u <= GUEST_BYTES) { vm_base[a] = (u8)(v >> 24); vm_base[a + 1] = (u8)(v >> 16); vm_base[a + 2] = (u8)(v >> 8); vm_base[a + 3] = (u8)v; }
}
void ps3_ms(const char* key) { (void)key; }
unsigned long long ps3_ms_now(void) { return 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
u32 cellGcm_flip_request_count(void) { return 0; }
void cellGcm_fifo_kick_notify(void) {}
rsx_frontend_device* rsx_frontend_vulkan_create(const rsx_frontend_vulkan_owner* owner) { (void)owner; return NULL; }
void rsx_frontend_close(rsx_frontend_device* device) { (void)device; }

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } } while (0)

enum { W = 64, H = 64, CELLS = 64, VTX = 0x10000, SURFACE = 0x40000 };
static void m(u32 a, u32 v) { rsx_draw_engine_method(a, v); }
static void be_float(u8* p, float f) { u32 v; memcpy(&v, &f, 4); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void write_vertices(float r, float g, float b)
{
    static const float pos[3][2] = { { -1, -1 }, { 3, -1 }, { -1, 3 } };
    for (u32 v = 0; v < 3; v++) {
        const float f[12] = { pos[v][0], pos[v][1], 0.5f, 1, r, g, b, 1, 0.5f, 0.5f, 0, 1 };
        for (u32 i = 0; i < 12; i++) be_float(vm_base + VTX + v * 48 + i * 4, f[i]);
    }
}
static void draw(void) { m(0x1808, RSX_PRIMITIVE_TRIANGLES); m(0x1814, 2u << 24); m(0x1808, 0); }

int main(int argc, char** argv)
{
    if (argc != 3) { fprintf(stderr, "usage: fixture direct|thread OUT\n"); return 2; }
    const int threaded = !strcmp(argv[1], "thread");
    if (threaded) setenv("PS3RECOMP_RSX_ENCODE_THREAD", "1", 1); else unsetenv("PS3RECOMP_RSX_ENCODE_THREAD");
    setenv("PS3RECOMP_RSX_ENGINE", "dispatch", 1);
    setenv("PS3RECOMP_VK_WINDOW", "0", 1);
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 1);
    vm_base = calloc(1, GUEST_BYTES); ppu_vm_size = GUEST_BYTES;
    assert(vm_base);
    assert(rsx_vulkan_backend_init(W, H, "encode thread synthetic") == 0 && s_vk.eng_active);
    CHECK(rsx_encq_active() == threaded, "encode thread as asked");
    if (!s_g.on) { fprintf(stderr, "SKIP: no guest programs (glslang)\n"); rsx_vulkan_backend_shutdown(); return 0; }

    m(0x194, 0xFEED0000); m(0x210, SURFACE); m(0x20C, W * 4);
    m(0x200, (u32)W << 16); m(0x204, (u32)H << 16); m(0x208, 0x148);
    m(0x1740, 2u | (4u << 4) | (48u << 8)); m(0x1680, 0x80000000u | VTX);
    m(0x1744, 2u | (4u << 4) | (48u << 8)); m(0x1684, 0x80000000u | (VTX + 16));
    m(0x1760, 2u | (4u << 4) | (48u << 8)); m(0x16A0, 0x80000000u | (VTX + 32));
    u8 vp[48];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_tex_r0_tc0(vm_base + 0x60040, 0);
    m(0xA6C, 0x207); m(0xA70, 0); m(0xA74, 0);
    m(0x1D90, 0xFF000000u); m(0x1D94, 0xF3);
    u64 tickets[CELLS]; u32 nt = 0;
    for (u32 c = 0; c < CELLS; c++) {
        if (c == CELLS / 2) {
            m(0x8C0, (u32)W << 16); m(0x8C4, (u32)H << 16);
            m(0x1D90, 0xFF808080u); m(0x1D94, 0xF0);
        }
        const u32 x = (c % 8u) * 8u, y = (c / 8u) * 8u;
        m(0x8C0, x | (8u << 16)); m(0x8C4, y | (8u << 16));
        if (c % 11 == 10) {
            /* Sample the target being drawn (its centre): a colour snapshot
             * of everything before this draw. */
            m(0x8E4, 0x60042); m(0x1D60, 64);
            m(0x1A00, SURFACE); m(0x1A04, 1 | (2u << 4) | (0xA5u << 8) | (1u << 16));
            m(0x1A0C, 0x80000000u); m(0x1A10, 0xAAE4); m(0x1A18, ((u32)W << 16) | H); m(0x1840, W * 4);
        } else {
            m(0x8E4, 0x60002); m(0x1D60, 64);
            m(0x1A0C, 0);
        }
        write_vertices((float)(c % 4u) / 3.0f, (float)((c / 4u) % 4u) / 3.0f, (float)(c / 16u) / 3.0f);
        draw();
        write_vertices(0.25f, 0.5f, 0.75f);   /* reused at once; the draw copied them */
        if (c % 4 == 3) {
            const u64 t = rsx_draw_engine_submit_async();
            CHECK(t != 0, "ticket");
            if (t) tickets[nt++] = t;
        }
    }
    for (u32 i = 0; i < nt; i++) CHECK(rsx_draw_engine_completion_status(tickets[i], 1) == 1, "ticket completes");
    rsx_draw_engine_present();
    CHECK(rsx_draw_engine_flush_checked(), "checked flush");
    /* The frame into guest memory, then out. */
    memset(vm_base + SURFACE, 0, W * H * 4);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, SURFACE, W * 4, W, H);
    int nonzero = 0;
    for (u32 i = 0; i < W * H * 4; i++) nonzero |= vm_base[SURFACE + i] != 0;
    CHECK(nonzero, "the frame reached guest memory");
    FILE* f = fopen(argv[2], "wb");
    assert(f && fwrite(vm_base + SURFACE, 1, W * H * 4, f) == W * H * 4); fclose(f);
    if (threaded) {
        rsx_encq_stats st; rsx_encq_get_stats(&st);
        printf("%s: ops=%llu immediate=%llu ordered=%llu snapshots=%llu tickets=%llu drains=%llu\n", argv[1],
               st.ops, st.immediates, st.ordered_calls, st.snapshots_split, st.tickets, st.drains);
        CHECK(st.ops > CELLS && st.tickets == nt, "the queue carried the frame");
    }
    rsx_vulkan_backend_shutdown();
    CHECK(!rsx_encq_active(), "stopped with the backend");
    printf("%s: checks=%d failures=%d\n", argv[1], checks, failures);
    return failures ? 1 : 0;
}
