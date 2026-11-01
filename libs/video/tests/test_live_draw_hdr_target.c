/* Synthetic HDR render targets through the actual D3D12 renderer. */
#ifndef HDR_RENDERER_SOURCE
#define HDR_RENDERER_SOURCE "../rsx_live_draw.c"
#endif
#include HDR_RENDERER_SOURCE
#include "live_draw_test_harness.h"
#define T "test_live_draw_hdr_target"
#define HDR 0x30000u
#define OUT 0x10000u
#define FP_WRITE 0x1000u
#define FP_SAMPLE 0x1100u

static void constant_program(u32 address, float r, float g1, float b, float a)
{
    const float values[4] = { r, g1, b, a };
    rsx_test_fp_instr(ldh_main + address,
        RSX_TEST_FP_OPC(1) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
        2u | RSX_TEST_FP_SWZ_IDENT);
    for (u32 i = 0; i < 4; i++) {
        u32 bits; memcpy(&bits, &values[i], 4);
        rsx_test_fp_put_word(ldh_main + address + 16 + i * 4, bits);
    }
}

static void sample_program(void)
{
    rsx_test_fp_tex_r0_tc0(ldh_main + FP_SAMPLE, 0);
    rsx_test_fp_put_word(ldh_main + FP_SAMPLE,
        RSX_TEST_FP_OPC(RSX_TEST_FP_OP_TEX) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_TC0);
    rsx_test_fp_instr(ldh_main + FP_SAMPLE + 16,
        RSX_TEST_FP_OPC(0x04) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END, RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(ldh_main + FP_SAMPLE + 24, 2u | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(ldh_main + FP_SAMPLE + 28, 2u | RSX_TEST_FP_SWZ_IDENT);
    u32 bits; float scale = .25f; memcpy(&bits, &scale, 4);
    for (u32 i = 0; i < 4; i++) rsx_test_fp_put_word(ldh_main + FP_SAMPLE + 32 + i * 4, bits);
}

static int near_byte(u8 value, float expected)
{
    const int difference = (int)value - (int)(expected * 255.f + .5f);
    return difference >= -2 && difference <= 2;
}

static void write_target(u32 offset, u32 format, u32 width, u32 height)
{
    ldh_m(NV4097_SET_TEXTURE_CONTROL0, 0);
    ldh_surface(format | (1u << 8), offset, width * (format == 12 ? 16 : format == 11 ? 8 : 4),
                0x200000, 64, width, height);
    ldh_draw(FP_WRITE, 0x40, 0, 0, 0x207);
}

static void sample_check(u32 offset, u32 format, u32 width, u32 height,
                         u32 pitch, const char* name, float r, float green, float b)
{
    ldh_texture0(offset, format, width, height, pitch);
    ldh_surface(8u | (1u << 8), OUT, 32, 0x200000, 64, 8, 8);
    ldh_draw(FP_SAMPLE, 0x40, 0, 0, 0x207);
    u8 rgb[8 * 8 * 3]; u32 w = 0, h = 0;
    const int read = ldh_read_rgb(OUT, "hdr-target.ppm", rgb, 8, 8, &w, &h) == 0 && w == 8 && h == 8;
    ldh_check("read actual GPU result", read);
    if (read) {
        const u8* pixel = rgb + (4 * 8 + 4) * 3;
        printf("[sample] %u %u %u expected %.3f %.3f %.3f\n", pixel[0], pixel[1], pixel[2], r, green, b);
        ldh_check(name, near_byte(pixel[0], r) && near_byte(pixel[1], green) && near_byte(pixel[2], b));
    }
}

int main(int argc, char** argv)
{
    const char* scenario = argc > 1 ? argv[1] : "hdr";
    if (ldh_init(T)) return 2;
    ShowWindow((HWND)rsx_null_backend_get_hwnd(), SW_HIDE);
    ldh_setup_quad(0, 1, 1, 0, 0);
    constant_program(FP_WRITE, -.5f, .25f, 2.f, .75f);
    sample_program();
    if (!strcmp(scenario, "identity")) {
        write_target(HDR, 8, 4, 4);
        sample_check(HDR, 0xA5, 4, 4, 16, "UNORM baseline", .25f, .3125f, .5f);
        const u32 before_half = g.n_psos;
        write_target(HDR, 11, 4, 4);
        ldh_check("FP16 selects a distinct PSO", g.n_psos == before_half + 1);
        sample_check(HDR, 0xBA, 4, 4, 32, "same address selects FP16 surface", .125f, .3125f, .75f);
        const u32 pipelines = g.n_psos;
        write_target(HDR, 12, 4, 4);
        ldh_check("RGBA32F selects a distinct PSO", g.n_psos == pipelines + 1);
        sample_check(HDR, 0xBB, 4, 4, 64, "same address selects float surface", .125f, .3125f, .75f);
        const u32 after = g.n_psos;
        write_target(HDR, 8, 4, 4);
        ldh_check("switching back reuses UNORM PSO", g.n_psos == after);
        sample_check(HDR, 0xA5, 4, 4, 16, "switching back selects UNORM surface", .25f, .3125f, .5f);
    } else if (!strcmp(scenario, "subrect")) {
        write_target(HDR, 12, 4, 4);
        constant_program(FP_WRITE, 1, -.5f, .25f, .75f);
        ldh_m(NV4097_SET_SCISSOR_HORIZONTAL, 2u << 16);
        ldh_draw(FP_WRITE, 0x40, 0, 0, 0x207);
        sample_check(HDR, 0xBB, 2, 2, 64, "float subrectangle selects top-left texels", .5f, .125f, .3125f);
    } else if (!strcmp(scenario, "copy")) {
        write_target(HDR, 12, 4, 4);
        ldh_check("copy HDR subrectangle on GPU", ld_blit_gpu(0, HDR + 16, 64, 0, HDR + 0x1000, 32, 2, 2) == 1);
        sample_check(HDR + 0x1000, 0xBB, 2, 2, 32, "GPU copy retains signed HDR values", .125f, .3125f, .75f);
        ldh_check("unaligned float source rejected", ld_blit_gpu(0, HDR + 1, 64, 0, HDR + 0x2000, 32, 2, 2) < 0);
        ldh_check("overflow-sized source rejected", ld_blit_gpu(0, HDR, 64, 0, HDR + 0x2000, 32, UINT32_MAX, 2) < 0);
        ldh_check("invalid float destination pitch rejected", ld_blit_gpu(0, HDR, 64, 0, HDR + 0x2000, 15, 1, 1) < 0);
    } else if (!strcmp(scenario, "copy-alias")) {
        write_target(HDR, 8, 4, 4);
        ldh_m(NV4097_SET_SURFACE_PITCH_A, 64);
        ldh_draw(FP_WRITE, 0x40, 0, 0, 0x207);
        write_target(HDR, 12, 4, 4);
        ldh_check("copy latest written alias at same address and padded pitch",
            ld_blit_gpu(0, HDR, 64, 0, HDR + 0x1000, 64, 4, 4) == 1);
        sample_check(HDR + 0x1000, 0xBB, 4, 4, 64, "copy chooses latest numeric surface", .125f, .3125f, .75f);
    } else if (!strcmp(scenario, "copy-destination")) {
        write_target(HDR, 12, 4, 4);
        constant_program(FP_WRITE, 0, 0, 0, 1);
        write_target(HDR + 0x1000, 12, 4, 4);
        write_target(HDR + 0x1000, 8, 4, 4);
        ldh_m(NV4097_SET_SURFACE_PITCH_A, 64);
        ldh_draw(FP_WRITE, 0x40, 0, 0, 0x207);
        ldh_check("copy destination selects compatible typed alias",
            ld_blit_gpu(0, HDR, 64, 0, HDR + 0x1000, 64, 4, 4) == 1);
        sample_check(HDR + 0x1000, 0xBB, 4, 4, 64, "typed destination receives HDR copy", .125f, .3125f, .75f);
    } else if (!strcmp(scenario, "resolve-alias")) {
        write_target(HDR, 8, 4, 4);
        ldh_m(NV4097_SET_SURFACE_PITCH_A, 64); ldh_draw(FP_WRITE, 0x40, 0, 0, 0x207);
        write_target(HDR, 12, 4, 4);
        write_target(HDR, 8, 4, 4);
        ldh_m(NV4097_SET_SURFACE_PITCH_A, 64); ldh_draw(FP_WRITE, 0x40, 0, 0, 0x207);
        rsx_live_draw_set_display_buffer(0, 0, HDR + 0x10000, 32, 8, 8);
        ldh_check("resolve selects last written source alias",
            rsx_live_draw_resolve_blit(0, HDR, 64, 1u << 20, 1u << 20,
                                      0, HDR + 0x10000, 32, 0, 0, 2, 2) == 1);
        u8 rgb[8 * 8 * 3]; u32 width = 0, height = 0;
        const int read = ldh_read_rgb(HDR + 0x10000, "resolve-alias.ppm", rgb, 8, 8, &width, &height) == 0;
        ldh_check("resolve copies newest UNORM pixels instead of stale FP32", read &&
            near_byte(rgb[0], 0) && near_byte(rgb[1], .25f) && near_byte(rgb[2], 1));
    } else if (!strcmp(scenario, "resolve-offset")) {
        for (u32 format = 11; format <= 12; format++) {
            const u32 source = HDR + (format - 11) * 0x2000;
            const u32 destination = HDR + 0x10000 + (format - 11) * 0x2000;
            const u32 bytes = format == 12 ? 16 : 8;
            write_target(source, format, 4, 4);
            ldh_surface(format | (1u << 8), destination, 8 * bytes, 0x200000, 64, 8, 8);
            ldh_m(NV4097_SET_COLOR_CLEAR_VALUE, 0xFF0000FFu);
            ldh_m(NV4097_CLEAR_SURFACE, 0xF0u);
            rsx_live_draw_set_display_buffer(0, 0, destination, 32, 8, 8);
            ldh_check("display destination +4 is column one for float source",
                rsx_live_draw_resolve_blit(0, source + bytes, 4 * bytes, 1u << 20, 1u << 20,
                                           0, destination + 4, 32, 0, 0, 2, 2) == 1);
            u8 rgb[8 * 8 * 3]; u32 width = 0, height = 0;
            const int read = ldh_read_rgb(destination, "resolve-float.ppm", rgb, 8, 8, &width, &height) == 0;
            ldh_check("float resolve writes requested display column", read && width == 8 && height == 8 &&
                near_byte(rgb[3], 0) && near_byte(rgb[4], .2f) && near_byte(rgb[5], 2.f / 3.f) &&
                near_byte(rgb[0], 0) && near_byte(rgb[1], 0) && near_byte(rgb[2], .5f));
        }
    } else if (!strcmp(scenario, "precision")) {
        constant_program(FP_WRITE, 4097, .25f, 2, .75f);
        write_target(HDR, 12, 1, 1);
        rsx_test_fp_put_word(ldh_main + FP_SAMPLE + 16,
            RSX_TEST_FP_OPC(0x03) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END);
        const float subtract[4] = {-4096, -.25f, -2, 0};
        for (u32 i = 0; i < 4; i++) {
            u32 bits; memcpy(&bits, &subtract[i], 4);
            rsx_test_fp_put_word(ldh_main + FP_SAMPLE + 32 + i * 4, bits);
        }
        sample_check(HDR, 0xBB, 1, 1, 16, "FP32 precision is not reduced to FP16", 1, 0, 0);
    } else if (!strcmp(scenario, "readback")) {
        write_target(HDR, 12, 4, 4);
        u8 rgb[4 * 4 * 3]; u32 width = 0, height = 0;
        int good = ldh_read_rgb(HDR, "float-dump.ppm", rgb, 4, 4, &width, &height) == 0 && width == 4 && height == 4;
        for (u32 i = 0; good && i < 16; i++)
            good = near_byte(rgb[i * 3], 0) && near_byte(rgb[i * 3 + 1], .2f) && near_byte(rgb[i * 3 + 2], 2.f / 3.f);
        ldh_check("FP32 readback uses 16-byte texels and diagnostic tonemap", good);
    } else if (!strcmp(scenario, "clear")) {
        write_target(HDR, 12, 4, 4);
        ldh_m(NV4097_SET_COLOR_CLEAR_VALUE, 0x80402010u);
        ldh_m(NV4097_CLEAR_SURFACE, 0xF0u);
        sample_check(HDR, 0xBB, 4, 4, 64, "FP32 clear remains numeric RGBA", .25f + 16.f / 255.f, .25f + 8.f / 255.f, .25f + 4.f / 255.f);
    } else if (!strcmp(scenario, "mrt")) {
        rsx_test_fp_put_word(ldh_main + FP_WRITE, RSX_TEST_FP_OPC(1) | RSX_TEST_FP_MASK_XYZW);
        constant_program(FP_WRITE + 32, -.25f, 1, 3, .5f);
        rsx_test_fp_put_word(ldh_main + FP_WRITE + 32,
            RSX_TEST_FP_OPC(1) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END | (2u << 1));
        ldh_m(NV4097_SET_TEXTURE_CONTROL0, 0);
        ldh_surface(12u | (1u << 8), HDR, 64, 0x200000, 64, 4, 4);
        ldh_m(NV4097_SET_CONTEXT_DMA_COLOR_B, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
        ldh_m(NV4097_SET_SURFACE_COLOR_BOFFSET, HDR + 0x1000);
        ldh_m(NV4097_SET_SURFACE_PITCH_B, 64);
        ldh_m(NV4097_SET_SURFACE_COLOR_TARGET, 0x13);
        ldh_draw(FP_WRITE, 0x40, 0, 0, 0x207);
        sample_check(HDR, 0xBB, 4, 4, 64, "MRT A keeps signed HDR", .125f, .3125f, .75f);
        sample_check(HDR + 0x1000, 0xBB, 4, 4, 64, "MRT B keeps independent HDR", .1875f, .5f, 1);
    } else {
        write_target(HDR, 12, 1, 1);
        sample_check(HDR, 0xBB, 1, 1, 16, "RGBA32F render/sample retains negative and HDR values", .125f, .3125f, .75f);
    }
    ldh_check("checked renderer remains ready", rsx_live_draw_checked_ready() && rsx_live_draw_flush_checked());
    rsx_live_draw_shutdown();
    return ldh_done(T);
}
