/* Test the real queue boundary without manufacturing a completed GPU fence. */
#include "../rsx_live_draw.c"
#include "live_draw_test_harness.h"
int main(void)
{
    g.ready=1;g.fence_value=UINT64_MAX-1;
    g.n_retired_textures=MAX_RETIRED_TEXTURES;
    ID3D12Resource* resource=(ID3D12Resource*)(uintptr_t)0x12345678u;
    retire_texture(resource);
    if(g.ready || g.n_retired_textures!=MAX_RETIRED_TEXTURES){
        fprintf(stderr,"FAIL: failed flush must not append beyond retirement capacity\n");return 41;
    }
    for(u32 i=0;i<MAX_RETIRED_TEXTURES;i++)if(g.retired_textures[i]){
        fprintf(stderr,"FAIL: failed completion must not reclaim queued resources\n");return 42;
    }
    const u8 pixels[4]={255,0,0,255};
    const u64 token=rsx_picture_create(0x1000,0,0x1000,4,pixels);
    if(!token)return 43;
    s_picture_bound[0]=token;
    g.n_textures=1;g.textures[0].picture_token=token;g.textures[0].tex=resource;
    g.ready=1;ld_picture_unbind(0);
    if(g.ready || s_picture_bound[0]!=token || g.textures[0].tex!=resource || g.n_retired_textures!=MAX_RETIRED_TEXTURES){
        fprintf(stderr,"FAIL: failed unbind must retain its CPU and GPU owners\n");return 44;
    }
    rsx_live_draw_shutdown();
    if(rsx_picture_live_count())return 45;
    puts("PASS: failed retirement flush preserves bounded quarantine and revokes readiness");return 0;
}
