/* Synthetic zero-sampler producer through the actual public engine entry. */
#define main retention_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"
#include "rsx_test_programs.h"
#include <unistd.h>
static unsigned calls, draw_extra;
static void render(void){unsigned before=calls;draw_triangle();draw_extra+=calls-before;}
static void no_submit(void*u,u32 reason){(void)u;(void)reason;calls++;}
static void no_read(void*u,u32 s,u32 x,u32 y,u32 w,u32 h,void*d,u32 p){(void)u;(void)s;(void)x;(void)y;(void)w;(void)h;(void)d;(void)p;calls++;}
int main(int argc,char**argv){if(argc!=3)return 2;const char*kind=argv[1];vm_base=calloc(1,GUEST_BYTES);ppu_vm_size=GUEST_BYTES;engine_up();g_stub_backend.submit_and_wait=no_submit;g_stub_backend.readback=no_read;
 u8 vp[16];rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,1);m(0x1E9C,0);for(unsigned i=0;i<4;i++){u32 w;memcpy(&w,vp+4*i,4);m(0xB80+4*i,w);}m(0x1EA0,0);rsx_test_fp_mov_r0_col0(vm_base+0x60000,RSX_TEST_FP_SWZ_IDENT);m(0x8E4,0x60002);m(0x1D60,0x40);
 char selector[80];snprintf(selector,sizeof selector,"%016llX:40",(unsigned long long)rsx_fp_structural_hash(vm_base+0x60000,16,1469598103934665603ull));if(!getenv("RSX_ENGINE_PRODUCER_CAPTURE_SELECTOR"))setenv("RSX_ENGINE_PRODUCER_CAPTURE_SELECTOR",selector,1);
 if(!strcmp(kind,"b-only")){m(M_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_1);m(M_COLOR_A_OFFSET,0x50000);m(M_CONTEXT_DMA_COLOR_B,DMA_LOCAL);m(M_COLOR_B_OFFSET,0x40000);}
 if(!strncmp(kind,"mrt",3)){m(M_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_MRT1);m(M_CONTEXT_DMA_COLOR_B,DMA_LOCAL);m(M_COLOR_B_OFFSET,0x50000);}
 for(unsigned i=0;i<12;i++){float f=(float)i+.25f;u32 w;memcpy(&w,&f,4);vm_base[VTX_OFFSET+4*i]=w>>24;vm_base[VTX_OFFSET+4*i+1]=w>>16;vm_base[VTX_OFFSET+4*i+2]=w>>8;vm_base[VTX_OFFSET+4*i+3]=w;}
 render();FILE*f=fopen(argv[2],"w");if(!f)return 3;fclose(f);
 render();rsx_draw_engine_present();
 if(!strcmp(kind,"miss-then-match")){m(0x1D60,0x41);render();m(0x1D60,0x40);}
 if(!strcmp(kind,"indexed")){const u8 ix[6]={0,0,0,1,0,2};memcpy(vm_base+0x30000,ix,6);m(0x181C,0x30000);m(0x1820,0x11);unsigned before=calls;m(M_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);m(0x1824,2u<<24);m(M_BEGIN_END,0);draw_extra+=calls-before;}else render();
 if(!strcmp(kind,"census")){for(unsigned i=1;i<300;i++)render();}else{render();render();}
 rsx_draw_engine_present();render();
 printf("draws=%d texture_mask=%u GPU_sync_or_read=%u\n",stub.n_draw,stub.bound_mask,draw_extra);
 const char*dir=getenv("RSX_ENGINE_PRODUCER_CAPTURE_DIR");if(dir&&*dir){char path[2048];
 #define EXPECTED(suffix,ptr,bytes) do{snprintf(path,sizeof path,"%s/expected%s",dir,suffix);f=fopen(path,"wb");if(f){fwrite(ptr,1,bytes,f);fclose(f);}}while(0)
 EXPECTED(".vertices.bin",dc.verts,dc.n_verts*stub.draw_stride);EXPECTED(".vp-program.bin",vp,sizeof vp);EXPECTED(".fp-program.bin",vm_base+0x60000,16);EXPECTED(".vp-constants.bin",g.vp_cb,ENG_VP_CB_BYTES);EXPECTED(".fp-constants.bin",g.fp_cb,32);rsx_be_render_state rs;rsx_draw_engine_decode_render_state(&g.rsx,&rs);EXPECTED(".render-state.bin",&rs,sizeof rs);
 #undef EXPECTED
 }
 engine_down();free(vm_base);return draw_extra?1:0;}
