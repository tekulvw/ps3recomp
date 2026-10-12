#define main original_engine_main
#include "test_rsx_draw_engine.c"
#undef main
#include <assert.h>

static u32 capture_calls;
static int stub_capture(void* user, u32 handle, const char* path)
{
    (void)user; assert(handle && stub.n_draw<4); ++capture_calls;
    FILE* f=fopen(path,"wb"); assert(f);
    assert(fwrite("test",1,4,f)==4); fclose(f); return 1;
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    vm_base = calloc(1, GUEST_BYTES); ppu_vm_size = GUEST_BYTES;
    g_stub_backend.capture_texture_rgba8=stub_capture;
    engine_up(); bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    m(M_TEX_UNIT + 0x04, 2u | (2u << 4) | (0xA5u << 8) | (1u << 16));
    m(0x1840, 48); m(M_TEX_UNIT + 0x18, (8u << 16) | 4u);
    for (u32 y=0; y<4; ++y) for (u32 x=0; x<8; ++x) {
        u8* p=vm_base+TEX_OFFSET+y*48+x*4;
        p[0]=255; p[1]=(u8)(x+1); p[2]=(u8)(y+2); p[3]=83;
    }
    if (!strcmp(argv[1], "compressed")) {
        m(M_TEX_UNIT + 0x04, 2u | (2u << 4) | (0xA6u << 8) | (1u << 16));
        m(M_TEX_UNIT + 0x18, (8u << 16) | 8u);
        memset(vm_base+TEX_OFFSET, 0x5A, 32);
    }
    draw_triangle(); rsx_draw_engine_present();
    /* Mutation in the same presented frame after a cached upload. The capture
     * must expose disagreement rather than calling the fresh decode an upload. */
    draw_triangle();
    if (!strcmp(argv[1], "stale")) vm_base[TEX_OFFSET+1]=98;
    if (!strcmp(argv[1], "trigger")) {
        FILE* f=fopen(getenv("PS3RECOMP_RSX_BINDING_CAPTURE_TRIGGER"),"wb");
        assert(f); fclose(f);
        rsx_draw_engine_present();
    }
    draw_triangle(); draw_triangle();
    assert(stub.n_draw == 4 && capture_calls<=1); engine_down(); free(vm_base); return 0;
}
