/* Synthetic WPOS programs exercise the real engine and its pipeline cache. */
#define main existing_engine_test_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_fp_decompiler.h"
static char captured[65536];
static u32 capture_pipeline(void* u,const char* vs,const char* ps,const rsx_be_render_state* rs,const rsx_vertex_layout_plan* l,u32 stride,rsx_be_format f,u32 n)
{ snprintf(captured,sizeof captured,"%s",ps);return stub_pipeline_create(u,vs,ps,rs,l,stride,f,n); }
int main(int argc, char** argv)
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
 char source[65536],copy[65536];u8 fp[48]={0};u32 constants=0;
 rsx_test_fp_instr(fp,RSX_TEST_FP_OPC(1)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_END,RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);
 CHECK(rsx_fp_uses_wpos(fp,16)==1,"WPOS detector sees actual input");
 CHECK(!rsx_fp_uses_wpos(fp,15)&&!rsx_fp_uses_wpos(NULL,16),"truncated/null programs do not qualify");
 for(u32 sx=1;sx<=2;sx++)for(u32 sy=1;sy<=2;sy++){
 CHECK(rsx_fp_decompile_buffered_ex(fp,16,0x40,0,source,sizeof source,&constants)>0,"synthetic WPOS decompiles");strcpy(copy,source);
 CHECK(rsx_fp_apply_wpos_scale(source,sizeof source,sx,sy)==1,"all four sample grids transform");
 if(sx==1&&sy==1)CHECK(!strcmp(copy,source),"1x1 source byte identity");
 else CHECK(strstr(source,", input.position.zw)")!=NULL,"Z/W preserved directly");
 if(argc==2){char path[1024];snprintf(path,sizeof path,"%s/wpos-%u-%u.hlsl",argv[1],sx,sy);FILE* f=fopen(path,"wb");if(!f)return 2;fputs(source,f);fclose(f);}
 }
 strcpy(source,copy);CHECK(rsx_fp_apply_wpos_scale(source,(u32)strlen(source)+1,2,1)<0&&!strcmp(source,copy),"insufficient capacity leaves source unchanged");
 CHECK(rsx_fp_apply_wpos_scale(source,sizeof source,0,1)<0&&!strcmp(source,copy),"invalid scale leaves source unchanged");
 char unterminated[4]={'a','b','c','d'};CHECK(rsx_fp_apply_wpos_scale(unterminated,4,2,1)<0,"unterminated source rejected");
 strcpy(source,"float4 main() { return 0; }");strcpy(copy,source);CHECK(rsx_fp_apply_wpos_scale(source,sizeof source,2,2)==1&&!strcmp(source,copy),"non-WPOS source byte identity");
 /* An inline constant payload containing input-like bits must be skipped. */
 rsx_test_fp_instr(fp,RSX_TEST_FP_OPC(1)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_END,2u|RSX_TEST_FP_SWZ_IDENT);
 rsx_test_fp_instr(fp+16,RSX_TEST_FP_OPC(1)|RSX_TEST_FP_END,RSX_TEST_FP_SRC_INPUT);
 CHECK(!rsx_fp_uses_wpos(fp,32)&&!rsx_fp_uses_wpos(fp,16),"inline payload and truncated constants cannot manufacture WPOS");
 engine_down();free(vm_base);printf("WPOS scale: %d failures\n",g_failures);return g_failures?1:0;
}
