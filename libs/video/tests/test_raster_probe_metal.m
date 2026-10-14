/* Neutral full-engine cull diagnostic and retained depth/stencil, no title data. */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"
#include <math.h>
u8* vm_base;u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local,u32 offset){(void)local;return offset;}
u32 cellGcmResolveIO(u32 offset){return offset<ppu_vm_size?offset:0;}
void cellGcm_replay_display_buffers_to_engine(void){}
static unsigned checks,failures;
#define T(x,s) do{checks++;if(!(x)){failures++;if(failures<12)fprintf(stderr,"FAIL %s\n",s);}}while(0)
static void method(u32 a,u32 v){rsx_draw_engine_method(a,v);}
static void bf(u8*p,float f){u32 v;memcpy(&v,&f,4);p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void triangle(float z,int green){const float v[24]={-1,-1,z,1,green?0:1,green?1:0,0,1,3,-1,z,1,green?0:1,green?1:0,0,1,-1,3,z,1,green?0:1,green?1:0,0,1};for(u32 i=0;i<24;i++)bf(vm_base+0x10000+i*4,v[i]);method(0x1808,RSX_PRIMITIVE_TRIANGLES);method(0x1814,2u<<24);method(0x1808,0);}
int main(int argc,char**argv){if(argc!=3&&argc!=4)return 2;const int expected=atoi(argv[1]),aa=atoi(argv[2]),reference=argc==4?atoi(argv[3]):7;@autoreleasepool{
 s_dev=MTLCreateSystemDefaultDevice();if(!s_dev)return 2;s_queue=[s_dev newCommandQueue];s_headless=1;s_guest_shaders=1;vm_base=calloc(1,1u<<20);ppu_vm_size=1u<<20;rsx_draw_engine_set_backend(&s_engine_backend);T(!rsx_draw_engine_init(64,64),"engine init");
 const u32 sx=aa?2:1;method(0x194,0xFEED0000);method(0x210,0x40000);method(0x20c,64*sx*4);method(0x200,64u<<16);method(0x204,64u<<16);method(0x208,0x148|((u32)aa<<12));
 method(0x1740,2|(4<<4)|(32<<8));method(0x1680,0x80010000);method(0x1744,2|(4<<4)|(32<<8));method(0x1684,0x80010010);
 u8 vp[32];rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,0);rsx_test_vp_mov_out(vp+16,1,RSX_TEST_VP_SWZ_IDENT,1,1);method(0x1e9c,0);for(u32 i=0;i<8;i++){u32 w;memcpy(&w,vp+i*4,4);method(0xb80+i*4,w);}method(0x1ea0,0);rsx_test_fp_mov_r0_col0(vm_base+0x60000,RSX_TEST_FP_SWZ_IDENT);method(0x8e4,0x60002);method(0x1d60,64);
 method(0x183c,1);method(0x1830,0x405);method(0x1834,0x900);method(0xa6c,0x203);method(0xa70,1);method(0xa74,1);
 method(0x328,1);method(0x330,0x202);method(0x334,(u32)reference);method(0x338,255);method(0x33c,0x1e00);method(0x340,0x1e00);method(0x344,0x1e00);method(0x1d90,0);method(0x1d8c,0xffffff07);method(0x1d94,0xf3);
 triangle(.2f,0);triangle(.75f,1);T(s_engine_backend.submit_and_wait_checked(NULL,0),"checked GPU completion");
 u8 rgba[128*64*4];float depth[128*64];s_engine_backend.readback(NULL,g.surfaces[0].handle,0,0,64*sx,64,rgba,64*sx*4);T(!s_engine_backend.depth_readback(NULL,g.zdepths[0].handle,0,0,64*sx,64,depth,64*sx*4),"depth readback");
 for(u32 i=0;i<64*sx*64;i++){T(rgba[4*i]==(expected?255:0)&&rgba[4*i+1]==0&&rgba[4*i+2]==0,"cull-off exposes red; later green still fails depth");T(fabsf(depth[i]-(expected?.2f:1.f))<1e-6,"depth write/test unchanged");}
 T(!atomic_load(&s_eng_checked_failed),"no checked backend failure");rsx_draw_engine_shutdown();rsx_draw_engine_set_backend(NULL);free(vm_base);printf("GPU raster probe checks=%u failures=%u\n",checks,failures);return failures?1:0;}}
