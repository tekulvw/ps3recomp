/* Immutable synthetic pictures through the actual Windows GPU renderer. */
#include <windows.h>
#include "live_draw_test_harness.h"
#include "rsx_picture_identity.h"
#define T "test_live_draw_pictures"
#define TEX 0x30000u
#define DESC 0x2000u
#define CMD_A 0x4004u
#define CMD_B 0x5004u
static u32 subchannel;
static void method_at(u32 method,u32 value,u32 ea) {
#ifdef RSX_LIVE_HAS_PICTURE_IDENTITIES
    rsx_live_draw_method_at(method|subchannel,value,ea);
#else
    (void)ea;rsx_live_draw_method(method,value);
#endif
}
static void texture(u32 ea) {
    method_at(NV4097_SET_TEXTURE_OFFSET,TEX,ea);
    method_at(NV4097_SET_TEXTURE_FORMAT,1u|(2u<<4)|(0xA5u<<8)|(1u<<16),ea+4);
    ldh_m(NV4097_SET_TEXTURE_ADDRESS,3u|(3u<<8)|(3u<<16));
    ldh_m(NV4097_SET_TEXTURE_CONTROL0,0x80000000u|((12u<<8)<<7));
    ldh_m(NV4097_SET_TEXTURE_CONTROL1,0xAAE4u);
    ldh_m(NV4097_SET_TEXTURE_CONTROL3,8);
    ldh_m(NV4097_SET_TEXTURE_FILTER,(1u<<16)|(1u<<24)|(1u<<13));
    ldh_m(NV4097_SET_TEXTURE_IMAGE_RECT,(2u<<16)|2u);
}
static int pixel(u32 target,unsigned channel) {
    u8 rgb[8*8*3];u32 w=0,h=0;
    if(ldh_read_rgb(target,"pictures.ppm",rgb,8,8,&w,&h)||w!=8||h!=8)return -1;
    return rgb[(4*8+4)*3+channel];
}
int main(int argc,char**argv) {
    const char* mode=argc>1?argv[1]:"reuse";
    subchannel=!strcmp(mode,"subchannel")?3u<<13:!strcmp(mode,"subchannel7")?7u<<13:0;
    if(ldh_init(T))return 2;
    ShowWindow((HWND)rsx_null_backend_get_hwnd(),SW_HIDE);
    rsx_test_fp_tex_r0_tc0(ldh_main+0x1000,0);ldh_setup_quad(0,1,1,0,0);
    if(!strcmp(mode,"ordinary")){
        for(unsigned i=0;i<4;i++){ldh_local[TEX+4*i]=255;ldh_local[TEX+4*i+1]=255;ldh_local[TEX+4*i+2]=255;ldh_local[TEX+4*i+3]=0;}
        ldh_surface(0x8u|(1u<<8),0x10000,32,0x200000,32,8,8);
        ldh_texture0(TEX,0xA5,2,2,8);ldh_draw(0x1000,0x40,0,0,0x207);
        ldh_check("ordinary legacy texture remains readable",pixel(0x10000,0)>240 && pixel(0x10000,1)>240 && pixel(0x10000,2)<10);
        ldh_check("ordinary consumer remains valid",rsx_live_draw_flush_checked()!=0);
        rsx_live_draw_shutdown();return ldh_done(T);
    }
    const u8 red[16]={255,255,0,0,255,255,0,0,255,255,0,0,255,255,0,0};
    const u8 green[16]={255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,0};
    const u64 a=rsx_picture_create(0x10000000u+TEX,0,TEX,16,red);
    ldh_check("old immutable picture created",a!=0);
    ldh_check("old descriptor retained",rsx_picture_bind_descriptor(DESC,a)==1);
    ldh_check("old command retained",rsx_picture_capture_command(CMD_A,DESC,0,TEX)==1);
    rsx_picture_release(a);
    const u64 b=rsx_picture_create(0x10000000u+TEX,0,TEX,16,green);
    ldh_check("reused guest allocation has new identity",b && b!=a);
    ldh_check("new descriptor retained",rsx_picture_bind_descriptor(DESC,b)==1);
    ldh_check("new command retained",rsx_picture_capture_command(CMD_B,DESC,0,TEX)==1);
    rsx_picture_release(b);rsx_picture_bind_descriptor(DESC,0);
    /* Both commands exist before either consumer bind. Mutable memory is blue. */
    for(unsigned i=0;i<4;i++){ldh_local[TEX+4*i]=255;ldh_local[TEX+4*i+1]=0;ldh_local[TEX+4*i+2]=0;ldh_local[TEX+4*i+3]=255;}
    ldh_surface(0x8u|(1u<<8),0x10000,32,0x200000,32,8,8);
    if(!strcmp(mode,"missing") || !strcmp(mode,"mismatch") || !strcmp(mode,"superseded") || !strcmp(mode,"stale") || !strcmp(mode,"partial") || !strcmp(mode,"partial-format") || !strcmp(mode,"oversize")){
        if(!strcmp(mode,"missing"))texture(0x6004);
        if(!strcmp(mode,"mismatch")){
            method_at(NV4097_SET_TEXTURE_OFFSET,TEX+4,CMD_A);
            method_at(NV4097_SET_TEXTURE_FORMAT,1u|(2u<<4)|(0xA5u<<8)|(1u<<16),CMD_A+4);
        }
        if(!strcmp(mode,"superseded")){
            method_at(NV4097_SET_TEXTURE_OFFSET,TEX,CMD_A);
            method_at(NV4097_SET_TEXTURE_OFFSET,TEX,CMD_B);
        }
        if(!strcmp(mode,"stale")){texture(CMD_A);texture(CMD_A);}
        if(!strcmp(mode,"partial")){
            texture(CMD_A);ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_QUADS);
            ldh_m(NV4097_DRAW_ARRAYS,3u<<24);texture(CMD_B);ldh_m(NV4097_SET_BEGIN_END,0);
        }
        if(!strcmp(mode,"partial-format")){
            texture(CMD_A);ldh_m(NV4097_SET_BEGIN_END,RSX_PRIMITIVE_QUADS);
            ldh_m(NV4097_DRAW_ARRAYS,3u<<24);
            ldh_m(NV4097_SET_TEXTURE_FORMAT,1u|(2u<<4)|(0xA4u<<8)|(1u<<16));
            ldh_m(NV4097_SET_BEGIN_END,0);
        }
        if(!strcmp(mode,"oversize")){
            texture(CMD_A);ldh_m(NV4097_SET_TEXTURE_IMAGE_RECT,(4u<<16)|4u);
            ldh_draw(0x1000,0x40,0,0,0x207);
        }
        ldh_check("invalid picture binding refuses checked completion",!rsx_live_draw_checked_ready() && !rsx_live_draw_flush_checked());
        ldh_check("invalid picture binding latches registry failure",!rsx_picture_ready());
        rsx_live_draw_shutdown();return ldh_done(T);
    }
    if(!strcmp(mode,"alias")){
        ldh_surface(0x8u|(1u<<8),TEX,8,0x200000,32,2,2);
        ldh_m(NV4097_SET_COLOR_CLEAR_VALUE,0xFF0000FF);ldh_m(NV4097_CLEAR_SURFACE,0xF0);
        ldh_surface(0x8u|(1u<<8),0x10000,32,0x200000,32,8,8);
    }
    texture(CMD_A);ldh_draw(0x1000,0x40,0,0,0x207);
    ldh_surface(0x8u|(1u<<8),0x11000,32,0x200000,32,8,8);
    texture(CMD_B);ldh_draw(0x1000,0x40,0,0,0x207);
    ldh_check("queued old picture survives address reuse",pixel(0x10000,0)>240 && pixel(0x10000,1)<10 && pixel(0x10000,2)<10);
    ldh_check("new picture has separate same-frame texture cache entry",pixel(0x11000,1)>240 && pixel(0x11000,0)<10 && pixel(0x11000,2)<10);
    ldh_check("consumer remains valid",rsx_live_draw_flush_checked()!=0 && rsx_picture_ready());
    rsx_live_draw_shutdown();
    ldh_check("shutdown releases bound picture reference",rsx_picture_live_count()==0);
    return ldh_done(T);
}
