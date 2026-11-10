/* CPU-only regression for derived texture state at native publication. */
#include "../rsx_draw_engine.c"
#define main original_owner_test_main
#include "test_original_owner_ingress.c"
#undef main
int main(void) {
    assert(original_owner_test_main()==0);
    static rsx_draw_backend backend;
    backend.init=backend_init;backend.shutdown=backend_shutdown;backend.submit_and_wait_checked=checked;
    rsx_draw_engine_set_backend(&backend);assert(!rsx_draw_engine_init(8,8));
    rsx_draw_engine_method_at(0x1840,0x00100100u,0x5004);
    rsx_dsp_texture cached;eng_get_texture(0,&cached);assert(cached.pitch==0x100);
    rsx_native_register_write word={0x1840,0x00100200u};
    rsx_native_external_pass pass={.abi_version=1,.struct_size=sizeof(pass),
        .source_observation=123,.native_generation=19,.native_ticket=41,
        .register_count=1,.registers=&word};
    assert(!rsx_draw_engine_native_publish_external(&pass));
    eng_get_texture(0,&cached);
    if(cached.pitch!=0x200u) {
        fprintf(stderr,"FAIL native SIZE1 publication retained stale cached pitch: %x\n",cached.pitch);return 1;
    }
    const u32 valid=g.decoded_textures_valid;
    assert(!rsx_draw_engine_native_publish_external(&pass));assert(g.decoded_textures_valid==valid);
    word=(rsx_native_register_write){0x0324,0x12345678};
    assert(!rsx_draw_engine_native_publish_external(&pass));assert(g.decoded_textures_valid==valid);
    word=(rsx_native_register_write){0x1814,0};
    assert(rsx_draw_engine_native_publish_external(&pass)==-1);assert(g.decoded_textures_valid==valid);
    for(u32 unit=0;unit<16;unit++) {
        rsx_dsp_texture expected;eng_get_texture(unit,&cached);
        word=(rsx_native_register_write){0x1a00+unit*32,0x1000+unit*128};
        assert(!rsx_draw_engine_native_publish_external(&pass));
        eng_get_texture(unit,&cached);rsx_dsp_get_texture(&g.rsx,unit,&expected);
        assert(cached.offset==expected.offset);
        word=(rsx_native_register_write){0x1a04+unit*32,0x00008502};
        assert(!rsx_draw_engine_native_publish_external(&pass));
        eng_get_texture(unit,&cached);rsx_dsp_get_texture(&g.rsx,unit,&expected);
        assert(cached.format==expected.format);
    }
    rsx_draw_engine_shutdown();
    puts("PASS native texture/SIZE1 publication invalidates before direct writes; equal/unrelated/rejected writes preserve reuse");
    return 0;
}
