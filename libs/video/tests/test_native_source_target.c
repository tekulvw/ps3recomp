/* A read-only source snapshot must not allocate or pick an older target after
 * the decoded offset/format/size changed. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include <assert.h>
int main(void) {
    vm_base=calloc(1,GUEST_BYTES);assert(vm_base);ppu_vm_size=GUEST_BYTES;engine_up();
    m(M_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_0);
    uint32_t methods[3]={0x1834,M_DEPTH_BOUNDS_MIN,M_DEPTH_BOUNDS_MAX};
    uint32_t values[3]={0xdeadbeef,0xdeadbeef,0xdeadbeef};
    m(0x1834,0x901);m(M_DEPTH_BOUNDS_MIN,0x3e800000);m(M_DEPTH_BOUNDS_MAX,0x3f400000);
    assert(!rsx_draw_engine_native_registers(methods,values,3));
    assert(values[0]==0x901&&values[1]==0x3e800000&&values[2]==0x3f400000);
    methods[2]=0x2000;values[0]=0xdeadbeef;
    assert(rsx_draw_engine_native_registers(methods,values,3)==-1&&values[0]==0xdeadbeef);
    methods[2]=M_DEPTH_BOUNDS_MAX;

    rsx_native_source_target target;
    assert(!rsx_draw_engine_native_source_target(&target)&&!stub.n_color_create);
    m(M_CLEAR_BUFFERS,RSX_CLEAR_COLOR_R);
    assert(stub.n_color_create==1&&rsx_draw_engine_native_source_target(&target));
    assert(target.color_a_offset==0x40000&&target.color_a_dma==DMA_LOCAL);
    assert(target.clip_width==256&&target.clip_height==256&&target.native_width==256&&target.native_height==256);
    assert(target.native_surface);
    rsx_native_image_key key={.abi_version=1,.struct_size=sizeof(key),
        .kind=RSX_NATIVE_COLOR_IMAGE,.location=RSX_LOCATION_LOCAL,
        .offset=target.color_a_offset,.pitch=target.color_a_pitch,
        .format=target.surface_format&31u,.width=target.native_width,
        .height=target.native_height,.aa_grid=0x101};
    rsx_native_image_match match={.abi_version=1,.struct_size=sizeof(match)};
    assert(rsx_draw_engine_native_image(&key,&match)==1&&match.native_handle==target.native_surface);
    assert(match.queued_clear_planes==1);
    const uint32_t original_handle=match.native_handle;
    m(0x0a38,0x3f000000);m(0x0a28,0x3f000000);
    assert(rsx_draw_engine_native_source_target(&target));
    assert(target.viewport_z_scale_bits==0x3f000000&&target.viewport_z_offset_bits==0x3f000000);
    m(M_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);
    assert(!rsx_draw_engine_native_source_target(&target));
    assert(rsx_draw_engine_native_registers(methods,values,3)==-4);
    assert(rsx_draw_engine_native_image(&key,&match)==-4&&!match.native_handle);m(M_BEGIN_END,0);
    m(M_COLOR_A_OFFSET,0x80000);
    assert(!rsx_draw_engine_native_source_target(&target)&&!target.native_surface&&stub.n_color_create==1);
    /* An earlier source surface is still selected explicitly, even when the
     * dispatch registers point at another not-yet-created target. */
    assert(rsx_draw_engine_native_image(&key,&match)==1&&match.native_handle==original_handle);
    ++key.pitch;assert(!rsx_draw_engine_native_image(&key,&match)&&!match.native_handle);--key.pitch;
    ++key.width;assert(!rsx_draw_engine_native_image(&key,&match)&&!match.native_handle);--key.width;
    key.aa_grid=0x202;assert(!rsx_draw_engine_native_image(&key,&match));key.aa_grid=0x101;
    key.kind=RSX_NATIVE_DEPTH_IMAGE;key.aa_grid=0;
    assert(!rsx_draw_engine_native_image(&key,&match));
    assert(stub.n_color_create==1);
    m(M_COLOR_A_OFFSET,0x40000);m(M_SURFACE_CLIP_H,512u<<16);
    assert(!rsx_draw_engine_native_source_target(&target)&&stub.n_color_create==1);
    m(M_SURFACE_CLIP_H,256u<<16);
    m(M_CONTEXT_DMA_ZETA,DMA_LOCAL);
    m(M_SURFACE_FORMAT,8u|(2u<<5));
    m(M_ZETA_OFFSET,ZETA_OFFSET);m(M_ZETA_PITCH,256u*4u);
    m(M_COLOR_A_OFFSET,ZETA_OFFSET);m(M_COLOR_MASK,0);
    m(M_DEPTH_TEST,1);m(M_DEPTH_WRITE,1);
    m(M_CLEAR_BUFFERS,RSX_CLEAR_DEPTH);draw_triangle();
    key.offset=ZETA_OFFSET;key.pitch=256u*4u;key.format=2;
    /* The real decoder allocates depth to at least the 1280x720 canvas. */
    assert(!rsx_draw_engine_native_image(&key,&match));
    key.width=1280;key.height=720;
    assert(rsx_draw_engine_native_image(&key,&match)==1);
    assert(match.kind==RSX_NATIVE_DEPTH_IMAGE&&match.native_handle&&match.width==1280&&match.height==720);
    assert(match.queued_clear_planes==2); // A draw does not initialize uncleared stencil.
    assert(!rsx_draw_engine_native_image_clear_planes(match.native_handle,2));
    assert(rsx_draw_engine_native_image_clear_planes(match.native_handle,4)==-5);
    assert(rsx_draw_engine_native_image_clear_planes(match.native_handle,0)==-1);
    assert(rsx_draw_engine_native_image_clear_planes(UINT32_MAX,2)==-3);
    m(M_CLEAR_BUFFERS,RSX_CLEAR_DEPTH);
    assert(rsx_draw_engine_native_image(&key,&match)==1&&match.queued_clear_planes==2);
    m(M_CLEAR_BUFFERS,RSX_CLEAR_STENCIL);
    assert(rsx_draw_engine_native_image(&key,&match)==1&&match.queued_clear_planes==6);
    /* A new source depth allocation must not inherit another one's clears. */
    key.offset=ZETA_OFFSET+0x200000;
    m(M_ZETA_OFFSET,key.offset);m(M_CLEAR_BUFFERS,RSX_CLEAR_STENCIL);draw_triangle();
    assert(rsx_draw_engine_native_image(&key,&match)==1&&match.queued_clear_planes==4);
    key.reserved[0]=1;
    assert(rsx_draw_engine_native_image(&key,&match)==-1&&!match.native_handle);
    engine_down();free(vm_base);puts("PASS: actual dispatch target snapshot refuses missing/stale/partial targets without allocation");return 0;
}
