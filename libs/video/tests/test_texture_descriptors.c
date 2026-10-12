#define main original_engine_main
#include "test_rsx_draw_engine.c"
#undef main
#include <assert.h>

int main(int argc, char** argv)
{
    assert(argc == 2);
    vm_base = calloc(1, GUEST_BYTES);
    ppu_vm_size = GUEST_BYTES;
    engine_up();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    m(M_TEX_UNIT + 0x04, 2u | (2u << 4) | (0xA5u << 8) | (3u << 16));
    m(0x1840, 48);
    m(M_TEX_UNIT + 0x18, (8u << 16) | 4u);
    m(M_TEX_UNIT + 0x14, 0x02060000);
    if (!strcmp(argv[1], "compressed")) {
        m(M_TEX_UNIT + 0x04, 2u | (2u << 4) | (0xA6u << 8) | (3u << 16));
        m(M_TEX_UNIT + 0x18, (8u << 16) | 8u);
    }
    draw_triangle();
    rsx_draw_engine_present();
    if (!strcmp(argv[1], "surface")) {
        m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
        m(M_COLOR_A_OFFSET, TEX_OFFSET);
        m(M_CLEAR_BUFFERS, 0xF0u);
        m(M_COLOR_A_OFFSET, 0x90000u);
    }
    draw_triangle();
    draw_triangle();                 /* duplicate descriptor */
    m(M_TEX_UNIT + 0x14, 0x02050000);
    draw_triangle();
    m(M_TEX_UNIT + 0x14, 0x02040000);
    draw_triangle();                 /* exceeds configured two-record budget */
    assert(stub.n_draw == 5);
    engine_down();
    free(vm_base);
    return 0;
}
