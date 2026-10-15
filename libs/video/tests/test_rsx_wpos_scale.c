/* Synthetic WPOS programs exercise the real engine and its pipeline cache. */
#define main existing_engine_test_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_fp_decompiler.h"
static char captured[65536];
static u32 capture_pipeline(void* u,const char* vs,const char* ps,const rsx_be_render_state* rs,const rsx_vertex_layout_plan* l,u32 stride,rsx_be_format f,u32 n)
{ snprintf(captured,sizeof captured,"%s",ps);return stub_pipeline_create(u,vs,ps,rs,l,stride,f,n); }
int main(void)
{
 vm_base=calloc(1,GUEST_BYTES);ppu_vm_size=GUEST_BYTES;engine_up();
 rsx_draw_backend be=g_stub_backend;be.pipeline_create=capture_pipeline;rsx_draw_engine_set_backend(&be);
 u8 vp[16];rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,1);m(0x1E9C,0);
 for(u32 i=0;i<4;i++){u32 x;memcpy(&x,vp+i*4,4);m(0xB80+i*4,x);}m(0x1EA0,0);
 rsx_test_fp_instr(vm_base+0x60000,RSX_TEST_FP_OPC(1)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_END,RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);
 m(0x8E4,0x60002);m(0x1D60,0x40);m(M_SURFACE_FORMAT,8);draw_triangle();
 const u32 normal=stub.bound_pipeline;char original[65536];strcpy(original,captured);
 m(M_SURFACE_FORMAT,8|(3u<<12));draw_triangle();const u32 aa2=stub.bound_pipeline;
 CHECK(aa2!=normal,"WPOS 2x1 chooses a different pipeline from 1x1");
 CHECK(strstr(captured,"input.position.xy * float2(0.5, 1.0)")!=NULL,"2x1 WPOS reads logical coordinates");
 m(M_SURFACE_FORMAT,8|(4u<<12));draw_triangle();const u32 aa4=stub.bound_pipeline;
 CHECK(aa4!=aa2&&aa4!=normal,"WPOS 2x2 has its own pipeline");
 CHECK(strstr(captured,"input.position.xy * float2(0.5, 0.5)")!=NULL,"2x2 scales both coordinates");
 const int count=stub.n_pipeline_create;m(M_SURFACE_FORMAT,8);draw_triangle();
 CHECK(stub.bound_pipeline==normal&&stub.n_pipeline_create==count,"1x1 switchback reuses original pipeline");
 rsx_test_fp_instr(vm_base+0x60000,RSX_TEST_FP_OPC(1)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC_TC0|RSX_TEST_FP_END,RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);
 draw_triangle();const u32 tc=stub.bound_pipeline;const int tc_count=stub.n_pipeline_create;
 m(M_SURFACE_FORMAT,8|(3u<<12));draw_triangle();CHECK(stub.bound_pipeline==tc&&stub.n_pipeline_create==tc_count,"non-WPOS programs keep AA-independent pipeline identity");
 engine_down();free(vm_base);printf("WPOS scale: %d failures\n",g_failures);return g_failures?1:0;
}
