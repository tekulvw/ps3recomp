/* Real GPU sampling of guest RGBA32F, including values outside UNORM. */
#include <windows.h>
#include "live_draw_test_harness.h"
#include "rsx_texture_layout.h"
#define T "test_live_draw_rgba32f"
#define TEX 0x30000u
#define FP 0x1000u
#define TARGET 0x10000u
static void bef(u8* p,float f){
    u32 v;memcpy(&v,&f,4);p[0]=(u8)(v>>24);p[1]=(u8)(v>>16);p[2]=(u8)(v>>8);p[3]=(u8)v;
}
static void texel(u8* p,float r,float g,float b,float a){
    bef(p,r);bef(p+4,g);bef(p+8,b);bef(p+12,a);
}
/* TEX r0,tc0; MAD r0,r0,inline(.25),inline(.25); END.
 * Both CONST operands refer to the same genuine inline payload. */
static void affine_program(void){
    rsx_test_fp_tex_r0_tc0(ldh_main+FP,0);
    rsx_test_fp_put_word(ldh_main+FP,RSX_TEST_FP_OPC(RSX_TEST_FP_OP_TEX)|
        RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC_TC0);
    rsx_test_fp_instr(ldh_main+FP+16,RSX_TEST_FP_OPC(0x04)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_END,
        RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(ldh_main+FP+24,2u|RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(ldh_main+FP+28,2u|RSX_TEST_FP_SWZ_IDENT);
    u32 v;float f=.25f;memcpy(&v,&f,4);
    for(unsigned i=0;i<4;i++)rsx_test_fp_put_word(ldh_main+FP+32+4*i,v);
}
static int near_byte(unsigned char got,float want){
    int delta=(int)got-(int)(want*255.f+.5f);return delta>=-2&&delta<=2;
}
static void draw_check(const char*name,float r,float g,float b){
    ldh_surface(0x8u|(1u<<8),TARGET,32,0x200000,32,8,8);
    ldh_draw(FP,0x40,0,0,0x207);
    u8 rgb[8*8*3];u32 w=0,h=0;
    int read=ldh_read_rgb(TARGET,"rgba32f.ppm",rgb,8,8,&w,&h)==0&&w==8&&h==8;
    ldh_check("read actual GPU target",read);
    if(read){
        u8* p=rgb+(4*8+4)*3;
        printf("[sample] %u %u %u expected %.3f %.3f %.3f\n",p[0],p[1],p[2],r,g,b);
        ldh_check(name,near_byte(p[0],r)&&near_byte(p[1],g)&&near_byte(p[2],b));
    }
}
int main(int argc,char**argv){
    const char*scenario=argc>1?argv[1]:"affine";
    if(ldh_init(T))return 2;
    ShowWindow((HWND)rsx_null_backend_get_hwnd(),SW_HIDE);
    affine_program();ldh_setup_quad(0,1,1,0,0);
    if(!strcmp(scenario,"ordinary")){
        const u8 p[4]={255,0,128,255};memcpy(ldh_local+TEX,p,4);
        ldh_texture0(TEX,0xA5,1,1,4);
        draw_check("ordinary texture and inline affine shader",.25f,.25f+.25f*128.f/255.f,.5f);
    }else if(!strcmp(scenario,"padded")||!strcmp(scenario,"swizzled")){
        const int linear=!strcmp(scenario,"padded");const u32 pitch=linear?80:64;
        memset(ldh_local+TEX,0xDE,320);
        for(u32 y=0;y<4;y++)for(u32 x=0;x<4;x++){
            u32 off=linear?y*pitch+x*16:rsx_swizzle_offset(x,y,2,2)*16;
            texel(ldh_local+TEX+off,-.5f+.125f*x,.25f+.25f*y,2,.75f);
        }
        ldh_texture0(TEX,linear?0xBB:0x9B,4,4,pitch);
        ldh_surface(0x8u|(1u<<8),TARGET,32,0x200000,32,8,8);ldh_draw(FP,0x40,0,0,0x207);
        u8 rgb[8*8*3];u32 w=0,h=0;int good=ldh_read_rgb(TARGET,"rgba32f.ppm",rgb,8,8,&w,&h)==0&&w==8&&h==8;
        for(u32 y=0;y<8&&good;y++)for(u32 x=0;x<8;x++){
            u8*p=rgb+(y*8+x)*3;
            good=near_byte(p[0],.125f+.03125f*(x/2))&&near_byte(p[1],.3125f+.0625f*(y/2))&&near_byte(p[2],.75f);
            if(!good)break;
        }
        ldh_check(linear?"padded linear float rows":"Morton float texels",good);
    }else if(!strcmp(scenario,"mips")){
        memset(ldh_local+TEX,0,336);
        texel(ldh_local+TEX+320,-.5f,.25f,2,.75f);
        ldh_texture0(TEX,0x9B,4,4,0);
        ldh_m(NV4097_SET_TEXTURE_FORMAT,1u|(2u<<4)|(0x9Bu<<8)|(3u<<16));
        ldh_m(NV4097_SET_TEXTURE_CONTROL0,0x80000000u|((2u*256u)<<19)|((2u*256u)<<7));
        draw_check("float mip chain keeps final level and float magnitude",.125f,.3125f,.75f);
    }else if(!strcmp(scenario,"cube")){
        for(u32 face=0;face<6;face++)texel(ldh_local+TEX+face*128,0,0,0,0);
        texel(ldh_local+TEX+4*128,-.5f,.25f,2,.75f);
        for(u32 v=0;v<4;v++){ldh_wf(v*48+32,0);ldh_wf(v*48+36,0);ldh_wf(v*48+40,1);}
        ldh_texture0(TEX,0xBB,1,1,16);
        ldh_m(NV4097_SET_TEXTURE_FORMAT,1u|4u|(2u<<4)|(0xBBu<<8)|(1u<<16));
        draw_check("float cube face uses aligned shared storage",.125f,.3125f,.75f);
    }else{
        u32 off=TEX,pitch=16;
        if(!strcmp(scenario,"truncated"))off=LDH_LOCAL_SIZE-8;
        else texel(ldh_local+TEX,-.5f,.25f,2,.75f);
        if(!strcmp(scenario,"short-pitch"))pitch=8;
        ldh_texture0(off,0xBB,1,1,pitch);
        if(!strcmp(scenario,"remap"))ldh_m(NV4097_SET_TEXTURE_CONTROL1,0x680Cu);
        if(!strcmp(scenario,"truncated")||!strcmp(scenario,"short-pitch"))
            draw_check("invalid source remains white fallback",.5f,.5f,.5f);
        else if(!strcmp(scenario,"remap")){
            draw_check("crossbar selects alpha and HDR with forced one once",.75f,.4375f,.5f);
            ldh_m(NV4097_SET_TEXTURE_CONTROL1,0x9240u);
            draw_check("crossbar forces zero and one without double remapping",.25f,.5f,.125f);
        }else{
            draw_check("negative HDR and BE32 components survive sample",.125f,.3125f,.75f);
            if(!strcmp(scenario,"refresh")){
                rsx_live_draw_present(0);texel(ldh_local+TEX,1,-.5f,.25f,.75f);
                draw_check("same address refresh preserves float values",.5f,.125f,.3125f);
            }
        }
    }
    ldh_check("checked completion remains valid",rsx_live_draw_checked_ready()&&rsx_live_draw_flush_checked());
    rsx_live_draw_shutdown();return ldh_done(T);
}
