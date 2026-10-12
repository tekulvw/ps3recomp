#define main original_engine_main
#include "test_rsx_draw_engine.c"
#undef main
#include <assert.h>
/* Baseline link compatibility: actual production definitions override these. */
__attribute__((weak)) int rsx_draw_engine_flush_checked(void) {return 0;}
__attribute__((weak)) int rsx_draw_engine_checked_ready(void) {return 0;}
__attribute__((weak)) void rsx_draw_engine_mark_guest_dirty(u32 ea,u32 bytes) {(void)ea;(void)bytes;}
static int completion=1,calls;
static int checked_submit(void* u,u32 why) {(void)u;assert(why==RSX_BE_FLUSH_GUEST_REFERENCE);calls++;return completion;}
static u32 failed_color(void*u,rsx_be_format f,u32 w,u32 h,const void*p,u32 pitch) {(void)u;(void)f;(void)w;(void)h;(void)p;(void)pitch;return 0;}
static u32 failed_depth(void*u,u32 w,u32 h){(void)u;(void)w;(void)h;return 0;}
static u32 failed_second_color(void*u,rsx_be_format f,u32 w,u32 h,const void*p,u32 pitch){if(stub.n_color_create)return 0;return stub_color_create(u,f,w,h,p,pitch);}
int main(int argc,char**argv) {
 assert(argc==2);vm_base=calloc(1,GUEST_BYTES);ppu_vm_size=GUEST_BYTES;
 if(!strcmp(argv[1],"clear_failure") || !strcmp(argv[1],"depth_failure") || !strcmp(argv[1],"mrt_failure") || !strcmp(argv[1],"batch_overflow")) {
  g_stub_backend.submit_and_wait_checked=checked_submit;
  if(!strcmp(argv[1],"clear_failure"))g_stub_backend.color_target_create=failed_color;
  if(!strcmp(argv[1],"depth_failure"))g_stub_backend.depth_target_create=failed_depth;
  if(!strcmp(argv[1],"mrt_failure"))g_stub_backend.color_target_create=failed_second_color;
  engine_up();
  if(!strcmp(argv[1],"depth_failure"))m(M_CLEAR_BUFFERS,RSX_CLEAR_DEPTH);
  else if(!strcmp(argv[1],"mrt_failure")){m(M_CONTEXT_DMA_COLOR_B,DMA_LOCAL);m(M_COLOR_B_OFFSET,0x50000);m(M_COLOR_TARGET,CELL_GCM_SURFACE_TARGET_MRT1);m(M_CLEAR_BUFFERS,RSX_CLEAR_COLOR_R);}
  else if(!strcmp(argv[1],"clear_failure")){m(M_CLEAR_BUFFERS,RSX_CLEAR_COLOR_R);assert(!stub.n_clear_color);}
  else{m(M_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);for(unsigned i=0;i<257;i++)m(M_DRAW_ARRAYS,2u<<24);m(M_BEGIN_END,0);}
  assert(!rsx_draw_engine_flush_checked());assert(!calls);engine_down();
 }
 else if(!strcmp(argv[1],"dirty")) {
  engine_up();bind_texture_unit(RSX_LOCATION_MAIN,TEX_OFFSET);memset(vm_base+TEX_OFFSET,0x11,64);draw_triangle();assert(stub.n_texture_create==1);
  memset(vm_base+TEX_OFFSET,0x22,64);rsx_draw_engine_mark_guest_dirty(TEX_OFFSET,64);draw_triangle();assert(stub.n_texture_create==2);engine_down();
 } else {
  rsx_draw_backend be=g_stub_backend;if(strcmp(argv[1],"missing"))be.submit_and_wait_checked=checked_submit;
  rsx_draw_engine_set_backend(&be);assert(!rsx_draw_engine_init(1280,720));
  if(!strcmp(argv[1],"success")){assert(rsx_draw_engine_checked_ready());assert(rsx_draw_engine_flush_checked()==1);assert(calls==1);}
  else if(!strcmp(argv[1],"partial")){m(M_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);assert(!rsx_draw_engine_flush_checked());assert(!calls);m(M_BEGIN_END,0);assert(!rsx_draw_engine_flush_checked());}
  else if(!strcmp(argv[1],"failure")){completion=0;assert(!rsx_draw_engine_flush_checked());assert(calls==1);completion=1;assert(!rsx_draw_engine_flush_checked());assert(calls==1);}
  else{assert(!strcmp(argv[1],"missing"));assert(!rsx_draw_engine_checked_ready());assert(!rsx_draw_engine_flush_checked());assert(!calls);}
  rsx_draw_engine_shutdown();
 }
 free(vm_base);puts("checked engine scenario passed");
}
