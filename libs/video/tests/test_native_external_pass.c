/* Real decoder and CPU backend; no GPU device or guest renderer execution. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_native_handoff.h"
#include <assert.h>
static rsx_native_external_pass pass(void) {
    rsx_native_external_pass p={.abi_version=1,.struct_size=sizeof(p),
        .source_observation=123,.native_generation=19,.native_ticket=41};
    return p;
}
static void upload_programs(int words) {
    const u32 src=0x1b02;
    const u32 vp[4]={(1u<<30)|(63u<<15),(1u<<22)|(src>>9),(src&511u)<<23,(15u<<13)|1};
    m(0x1E9C,0);
    for(int i=0;i<words;++i)m(0x0B80+4u*(u32)i,vp[i]);
    m(0x1EA0,0);
    /* END MOV r0, f[COL0], encoded in guest FP halfword byte order. */
    const u32 fp[4]={(0x17u<<24)|(15u<<9)|1u,(7u<<18),0,0};
    for(u32 i=0;i<4;++i){u32 v=(fp[i]<<16)|(fp[i]>>16);u8* b=vm_base+0x70000+4*i;
        b[0]=(u8)(v>>24);b[1]=(u8)(v>>16);b[2]=(u8)(v>>8);b[3]=(u8)v;}
    m(0x08E4,0x70000|2);m(0x1D60,0);
}
static void fresh_arrays(void){m(M_VTXFMT,2u|(4u<<4)|(16u<<8));m(M_VTXBUF_OFFSET,0x80000000u|VTX_OFFSET);m(0x1738,0);m(0x173C,0);}
static rsx_native_external_status status(void){rsx_native_external_status s={.abi_version=1,.struct_size=sizeof(s)};assert(!rsx_draw_engine_native_external_status(&s));return s;}
int main(void){
    vm_base=calloc(1,GUEST_BYTES);assert(vm_base);ppu_vm_size=GUEST_BYTES;
    engine_up();m(M_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_0);m(M_CLEAR_BUFFERS,RSX_CLEAR_COLOR_R);
    rsx_native_external_pass p=pass();
    rsx_native_register_write r[2]={{0x0324,0x12345678},{0x1814,0}};
    p.registers=r;p.register_count=2;
    assert(rsx_draw_engine_native_publish_external(&p)==-1);
    u32 method=0x0324,value=0;assert(!rsx_draw_engine_native_registers(&method,&value,1)&&value!=r[0].value);
    assert(!status().active); // Full prevalidation: no partial guard/state mutation.
    p.register_count=1;assert(!rsx_draw_engine_native_publish_external(&p));
    assert(!rsx_draw_engine_native_registers(&method,&value,1)&&value==r[0].value);
    assert(status().active&&!status().failure);
    draw_triangle();assert(!stub.n_draw&&status().failure==1&&status().source_observation==123);
    engine_down();engine_up();p=pass();assert(!rsx_draw_engine_native_publish_external(&p));
    upload_programs(3);fresh_arrays();draw_triangle();assert(!stub.n_draw&&status().failure==2&&status().detail==3);
    engine_down();engine_up();p=pass();assert(!rsx_draw_engine_native_publish_external(&p));
    upload_programs(4);draw_triangle();assert(!stub.n_draw&&status().failure==5);
    engine_down();engine_up();p=pass();assert(!rsx_draw_engine_native_publish_external(&p));
    upload_programs(4);fresh_arrays();draw_triangle();assert(stub.n_draw==1&&!status().failure);
    engine_down();engine_up();
    m(M_CONTEXT_DMA_ZETA,DMA_LOCAL);m(M_SURFACE_FORMAT,8u|(2u<<5));
    m(M_ZETA_OFFSET,ZETA_OFFSET);m(M_ZETA_PITCH,256u*4);m(M_COLOR_A_OFFSET,0x90000);
    m(M_COLOR_MASK,0);m(M_DEPTH_TEST,1);m(M_DEPTH_WRITE,1);
    m(M_CLEAR_BUFFERS,RSX_CLEAR_DEPTH);draw_triangle();
    rsx_native_image_key key={.abi_version=1,.struct_size=sizeof(key),.kind=RSX_NATIVE_DEPTH_IMAGE,
        .location=RSX_LOCATION_LOCAL,.offset=ZETA_OFFSET,.pitch=256u*4,.format=2,.width=1280,.height=720};
    rsx_native_image_match found={.abi_version=1,.struct_size=sizeof(found)};
    assert(rsx_draw_engine_native_image(&key,&found)==1&&found.queued_clear_planes==2);
    m(M_ZETA_OFFSET,ZETA_OFFSET+0x100000);m(M_DEPTH_TEST,0);m(M_DEPTH_WRITE,0);m(M_COLOR_MASK,0x01010101);
    bind_texture_unit(RSX_LOCATION_LOCAL,ZETA_OFFSET);
    m(M_TEX_UNIT+4,1u|(2u<<4)|((RSX_TEX_FMT_DEPTH24_D8|0x20u)<<8)|(1u<<16));
    draw_triangle();assert(stub.n_depth_snapshot==1);
    draw_triangle();assert(stub.n_depth_snapshot==1);
    rsx_native_external_image_write writes[2]={{.key=key,.native_handle=found.native_handle,
        .planes=2,.source_clip_width=256,.source_clip_height=256}};
    writes[1]=writes[0];writes[1].native_handle++;
    p=pass();p.writes=writes;p.write_count=2;
    assert(rsx_draw_engine_native_publish_external(&p)==-3&&!status().active);
    draw_triangle();assert(stub.n_depth_snapshot==1); // Invalid last row did not dirty first row.
    writes[1]=writes[0];writes[1].source_clip_width=128;writes[1].source_clip_height=160;
    assert(!rsx_draw_engine_native_publish_external(&p));
    assert(!rsx_draw_engine_native_image_clear_planes(found.native_handle,2));
    assert(rsx_draw_engine_native_image_clear_planes(found.native_handle,4)==-5);
    upload_programs(4);fresh_arrays();draw_triangle();
    assert(!status().failure&&stub.n_depth_snapshot==2);
    engine_down();engine_up();p=pass();assert(!rsx_draw_engine_native_publish_external(&p));
    upload_programs(4);fresh_arrays();assert(!rsx_draw_engine_native_abort_external(123,19,41,-7));
    draw_triangle();assert(!stub.n_draw&&status().failure==10&&status().native_ticket==41);
    engine_down();engine_up();
    m(M_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_0);m(M_CLEAR_BUFFERS,RSX_CLEAR_COLOR_R);
    rsx_native_image_key output={.abi_version=1,.struct_size=sizeof(output),.kind=0,
        .location=0,.offset=0x900000,.pitch=10240,.format=8,.width=2560,.height=720,.aa_grid=0x101};
    rsx_native_image_key backing={0}; uint64_t scope=0;
    assert(!rsx_draw_engine_native_images_begin(77,&scope)&&scope);
    uint64_t overlapping=0;
    assert(rsx_draw_engine_native_images_begin(78,&overlapping)==-4&&!overlapping);
    assert(rsx_draw_engine_native_images_end(scope+1)==-3);
    found=(rsx_native_image_match){.abi_version=1,.struct_size=sizeof(found)};
    int clears=stub.n_clear_color;
    assert(rsx_draw_engine_native_prepare_image(scope,&output,1,1,1,&backing,&found)==-1);
    assert(stub.n_clear_color==clears); // Combined input/output cannot fabricate prior contents.
    assert(rsx_draw_engine_native_prepare_image(scope,&output,0,1,0,&backing,&found)==-3);
    assert(rsx_draw_engine_native_prepare_image(scope,&output,0,1,1,&backing,&found)==1);
    assert(found.native_handle&&found.queued_clear_planes==1&&stub.n_clear_color==clears+1);
    u32 staged=found.native_handle;
    assert(!rsx_draw_engine_native_image_clear_planes(staged,1));
    rsx_native_image_match absent={.abi_version=1,.struct_size=sizeof(absent)};
    assert(!rsx_draw_engine_native_image(&output,&absent)); // Staging does not change current cache.
    assert(rsx_draw_engine_native_prepare_image(scope,&output,0,1,1,&backing,&found)==1&&found.native_handle==staged);
    assert(stub.n_clear_color==clears+1); // Exact repeated lookup uses one real allocation/clear.
    writes[0]=(rsx_native_external_image_write){.key=backing,.native_handle=staged,.planes=1,
        .source_clip_width=2560,.source_clip_height=720};
    p=pass();p.source_observation=77;p.writes=writes;p.write_count=1;
    assert(!rsx_draw_engine_native_publish_external(&p));
    assert(!rsx_draw_engine_native_images_end(scope));
    absent=(rsx_native_image_match){.abi_version=1,.struct_size=sizeof(absent)};
    assert(rsx_draw_engine_native_image(&output,&absent)==1&&absent.native_handle==staged);
    assert(!rsx_draw_engine_native_images_begin(78,&scope));
    rsx_native_image_key small=output;small.width=1280;
    assert(rsx_draw_engine_native_prepare_image(scope,&small,0,1,0,&backing,&found)==1);
    assert(backing.width==2560&&found.native_handle==staged); // Same-pitch/grid larger reuse.
    small.aa_grid=0x102;
    assert(rsx_draw_engine_native_prepare_image(scope,&small,0,1,0,&backing,&found)==-3);
    assert(rsx_draw_engine_native_prepare_image(scope,&small,0,1,1,&backing,&found)==1&&found.native_handle!=staged);
    u32 alternate=found.native_handle;
    writes[0]=(rsx_native_external_image_write){.key=backing,.native_handle=alternate,.planes=1,
        .source_clip_width=1280,.source_clip_height=720};
    writes[1]=(rsx_native_external_image_write){.key=output,.native_handle=staged,.planes=1,
        .source_clip_width=2560,.source_clip_height=720};
    p=pass();p.source_observation=78;p.writes=writes;p.write_count=2;
    assert(!rsx_draw_engine_native_publish_external(&p)); // Last write restores old current layout.
    assert(!rsx_draw_engine_native_images_end(scope)); // Unwritten output cancelled, old cache remains.
    assert(rsx_draw_engine_native_images_end(scope)==-3);
    assert(!rsx_draw_engine_native_images_begin(79,&scope));
    assert(rsx_draw_engine_native_prepare_image(scope,&small,0,1,1,&backing,&found)==1);
    writes[0]=(rsx_native_external_image_write){.key=output,.native_handle=staged,.planes=1,
        .source_clip_width=2560,.source_clip_height=720};
    writes[1]=(rsx_native_external_image_write){.key=backing,.native_handle=found.native_handle,.planes=1,
        .source_clip_width=1280,.source_clip_height=720};
    p.source_observation=79;
    assert(!rsx_draw_engine_native_publish_external(&p)); // Reverse order promotes the new layout.
    assert(!rsx_draw_engine_native_images_end(scope));
    assert(rsx_draw_engine_native_image(&small,&absent)==1&&absent.native_handle==found.native_handle);
    // Pure cancellation does not publish a third layout.
    assert(!rsx_draw_engine_native_images_begin(80,&scope));
    assert(rsx_draw_engine_native_prepare_image(scope,&output,0,1,1,&backing,&found)==1);
    assert(!rsx_draw_engine_native_images_end(scope));
    assert(!rsx_draw_engine_native_image(&output,&absent));
    engine_down();free(vm_base);puts("PASS: external native publication validates atomically and requires fresh actual programs/arrays");return 0;
}
