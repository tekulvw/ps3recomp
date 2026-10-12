#define main original_engine_main
#include "test_rsx_draw_engine.c"
#undef main
#include "../rsx_picture_identity.h"
#include <assert.h>
#ifndef RSX_ENGINE_HAS_PICTURE_IDENTITIES
#include "../rsx_picture_identity.inc"
void rsx_draw_engine_method_at(u32 method,u32 arg,u32 ea){(void)ea;rsx_draw_engine_method(method,arg);}
#endif
static u8 uploaded[64];
static void capture_upload(void*u,u32 t,u32 face,u32 mip,u32 w,u32 h,const void*p,u32 pitch,u32 rows){
    assert(pitch*rows==64);memcpy(uploaded,p,64);stub_texture_upload(u,t,face,mip,w,h,p,pitch,rows);
}
int main(int argc,char**argv){
    assert(argc==2);
    vm_base=calloc(1,GUEST_BYTES);ppu_vm_size=GUEST_BYTES;
    g_stub_backend.texture_upload=capture_upload;engine_up();
    m(M_CONTEXT_DMA_COLOR_A,DMA_MAIN);m(M_COLOR_A_OFFSET,TEX_OFFSET);m(M_CLEAR_BUFFERS,0xF0);
    m(M_CONTEXT_DMA_COLOR_A,DMA_LOCAL);m(M_COLOR_A_OFFSET,0x40000);
    u8 old[64],fresh[64];memset(old,0x17,64);memset(fresh,0x89,64);
    if(!strcmp(argv[1],"superseded")){
        u64 pending=rsx_picture_create(TEX_OFFSET,RSX_LOCATION_MAIN,TEX_OFFSET,64,old);assert(pending);
        assert(rsx_picture_bind_descriptor(0x3000,pending)==1);
        assert(rsx_picture_capture_command(0x4004,0x3000,RSX_LOCATION_MAIN,TEX_OFFSET)==1);
        rsx_draw_engine_method_at(M_TEX_UNIT,TEX_OFFSET,0x4004);
        rsx_draw_engine_method_at(M_TEX_UNIT,TEX_OFFSET+64,0x5004);
        assert(!rsx_picture_ready());assert(!rsx_draw_engine_checked_ready());
        puts("PASS superseded unresolved picture command fails rather than leaking while certifying");return 0;
    }
    u64 a=rsx_picture_create(TEX_OFFSET,RSX_LOCATION_MAIN,TEX_OFFSET,64,old);assert(a);
    assert(rsx_picture_bind_descriptor(0x3000,a)==1);
    assert(rsx_picture_capture_command(0x4004,0x3000,RSX_LOCATION_MAIN,TEX_OFFSET)==1);
    rsx_picture_release(a);
    u64 b=rsx_picture_create(TEX_OFFSET,RSX_LOCATION_MAIN,TEX_OFFSET,64,fresh);assert(b);
    assert(rsx_picture_bind_descriptor(0x3000,b)==1);
    assert(rsx_picture_capture_command(0x5004,0x3000,RSX_LOCATION_MAIN,TEX_OFFSET)==1);
    rsx_picture_release(b);memcpy(vm_base+TEX_OFFSET,fresh,64);
    rsx_draw_engine_method_at(M_TEX_UNIT,TEX_OFFSET,0x4004);
    m(M_TEX_UNIT+4,2u|(2u<<4)|(0xA5u<<8)|(1u<<16));
    m(M_TEX_UNIT+0x0C,0x80000000);m(M_TEX_UNIT+0x10,0xAAE4);m(M_TEX_UNIT+0x18,(4u<<16)|4u);
    draw_triangle();assert(!memcmp(uploaded,old,64));u32 old_handle=stub.bound_tex[0];
    if(!strcmp(argv[1],"partial")){
        int prior=stub.n_draw;m(M_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);m(M_DRAW_ARRAYS,2u<<24);
        rsx_draw_engine_method_at(M_TEX_UNIT,TEX_OFFSET,0x5004);
        assert(!rsx_picture_ready());m(M_BEGIN_END,0);assert(stub.n_draw==prior);
        puts("PASS picture identity replacement during a pending draw is rejected");return 0;
    }
    rsx_draw_engine_method_at(M_TEX_UNIT,TEX_OFFSET,0x5004);
    m(M_TEX_UNIT+4,2u|(2u<<4)|(0xA5u<<8)|(1u<<16));
    draw_triangle();assert(!memcmp(uploaded,fresh,64));assert(stub.bound_tex[0]!=old_handle);
    assert(stub.alive[old_handle]); /* Separate immutable cache entries. */
    assert(rsx_picture_bind_descriptor(0x3000,0)==1);engine_down();assert(rsx_picture_live_count()==0);
    u64 missing=rsx_picture_create(TEX_OFFSET,RSX_LOCATION_MAIN,TEX_OFFSET,64,old);assert(missing);
    engine_up();bind_texture_unit(RSX_LOCATION_MAIN,TEX_OFFSET);memcpy(vm_base+TEX_OFFSET,fresh,64);
    draw_triangle();assert(stub.n_texture_upload==0 && stub.n_draw==0);assert(!rsx_draw_engine_checked_ready());
    rsx_picture_release(missing);engine_down();
    free(vm_base);puts("PASS actual engine uploads old/new immutable command identities at the same EA in one display epoch");
}
