/* A read-only source snapshot must not allocate or pick an older target after
 * the decoded offset/format/size changed. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include <assert.h>
int main(void) {
    vm_base=calloc(1,GUEST_BYTES);assert(vm_base);ppu_vm_size=GUEST_BYTES;engine_up();
    m(M_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_0);
    rsx_native_source_target target;
    assert(!rsx_draw_engine_native_source_target(&target)&&!stub.n_color_create);
    m(M_CLEAR_BUFFERS,RSX_CLEAR_COLOR_R);
    assert(stub.n_color_create==1&&rsx_draw_engine_native_source_target(&target));
    assert(target.color_a_offset==0x40000&&target.color_a_dma==DMA_LOCAL);
    assert(target.clip_width==256&&target.clip_height==256&&target.native_width==256&&target.native_height==256);
    assert(target.native_surface);
    m(0x0a38,0x3f000000);m(0x0a28,0x3f000000);
    assert(rsx_draw_engine_native_source_target(&target));
    assert(target.viewport_z_scale_bits==0x3f000000&&target.viewport_z_offset_bits==0x3f000000);
    m(M_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);
    assert(!rsx_draw_engine_native_source_target(&target));m(M_BEGIN_END,0);
    m(M_COLOR_A_OFFSET,0x80000);
    assert(!rsx_draw_engine_native_source_target(&target)&&!target.native_surface&&stub.n_color_create==1);
    m(M_COLOR_A_OFFSET,0x40000);m(M_SURFACE_CLIP_H,512u<<16);
    assert(!rsx_draw_engine_native_source_target(&target)&&stub.n_color_create==1);
    engine_down();free(vm_base);puts("PASS: actual dispatch target snapshot refuses missing/stale/partial targets without allocation");return 0;
}
