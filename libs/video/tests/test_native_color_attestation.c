/* CPU-only: actual publication/sampler diagnostic code, no driver or GPU. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "../rsx_draw_engine.c"
#include <assert.h>
int main(void) {
    rsx_native_external_pass p={.abi_version=1,.struct_size=sizeof(p),
        .source_observation=123,.native_generation=19,.native_ticket=41};
    rsx_native_external_image_write writes[3]={0};
    const u32 offsets[2]={0x30000,0x7c0000};
    for(u32 i=0;i<2;++i) {
        writes[i].key.kind=RSX_NATIVE_COLOR_IMAGE;writes[i].key.location=RSX_LOCATION_LOCAL;
        writes[i].key.offset=offsets[i];writes[i].native_handle=71+i;writes[i].planes=1;
        g.surfaces[i]=(eng_surface){.location=RSX_LOCATION_LOCAL,.offset=offsets[i],.w=2560,
            .h=720,.pitch=10240,.aa_grid=0x102,.handle=71+i,.fmt=RSX_BE_FMT_R8G8B8A8,.color_serial=100+i};
    }
    g.n_surfaces=2;p.writes=writes;p.write_count=2;
    unsetenv("PS3_RENDER_SHADER_INVENTORY_DIR");
    eng_native_color_attest_publish(&p);assert(!s_native_color_attest_source);
    setenv("PS3_RENDER_SHADER_INVENTORY_DIR","relative-not-owned",1);
    eng_native_color_attest_publish(&p);assert(!s_native_color_attest_source);
    setenv("PS3_RENDER_SHADER_INVENTORY_DIR","/private/tmp/owned-inventory",1);
    /* Last native write wins in expected identity; actual cache stays separately observed. */
    writes[2]=writes[1];writes[2].native_handle=99;p.write_count=3;
    eng_native_color_attest_publish(&p);
    assert(s_native_color_attest_source==123&&s_native_color_attest[1].expected_handle==99);
    assert(s_native_color_attest[1].published_handle==72&&s_native_color_attest[1].serial==101);
    p.source_observation=124;writes[0].native_handle=888;eng_native_color_attest_publish(&p);
    assert(s_native_color_attest[0].source==123&&s_native_color_attest[0].expected_handle==71);
    rsx_dsp_texture t={.location=RSX_LOCATION_LOCAL,.offset=0x7c0000,.width=1280,.height=720,
        .pitch=10240,.format=0xa5,.remap=0xaae4};
    eng_native_color_attest_sample(0,&t,72,"surface",NULL);
    assert(!s_native_color_attest[1].sampled);
    /* Later cache allocation/layout replacement must be reported, not accepted as identical. */
    g.surfaces[1].handle=105;g.surfaces[1].w=1280;g.surfaces[1].aa_grid=0x101;
    eng_native_color_attest_sample(0,&t,106,"surface",&g.surfaces[1]);
    assert(s_native_color_attest[1].sampled==1);
    eng_native_color_attest_sample(0,&t,107,"surface",&g.surfaces[1]);
    assert(s_native_color_attest[1].sampled==1&&!s_native_color_attest[0].sampled);
    puts("PASS: inventory-only native/retained color identity attestation; CPU-only, no GPU");
    return 0;
}
