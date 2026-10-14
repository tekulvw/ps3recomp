/* Synthetic diagnostic consumer selection through the real draw engine. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_fp_decompiler.h"
static int captures;
static int capture_texture(void* u,u32 h,const char* path){(void)u;(void)h;captures++;FILE*f=fopen(path,"wb");if(!f)return 0;fwrite("GPU",1,3,f);return fclose(f)==0;}
int main(int argc,char**argv){if(argc!=3)return 2;
 setenv("PS3RECOMP_RSX_BINDING_CAPTURE_DIR",argv[1],1);setenv("PS3RECOMP_RSX_BINDING_CAPTURE_OFFSETS","20000",1);
 setenv("PS3RECOMP_RSX_BINDING_CAPTURE_LOCATION","1",1);
 vm_base=calloc(1,GUEST_BYTES);if(!vm_base)return 2;ppu_vm_size=GUEST_BYTES;engine_up();rsx_draw_backend be=g_stub_backend;be.capture_texture_rgba8=capture_texture;rsx_draw_engine_set_backend(&be);
 u8 vp[16];rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,1);m(0x1E9C,0);for(u32 i=0;i<4;i++){u32 w;memcpy(&w,vp+i*4,4);m(0xB80+i*4,w);}m(0x1EA0,0);
 u32 base=0x60000;u32 ins=RSX_TEST_FP_OPC(0x17)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC_TC0|RSX_TEST_FP_END;
 rsx_test_fp_instr(vm_base+base,ins,RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);
 rsx_test_fp_instr(vm_base+base+256,ins|(1u<<22),RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);
 u64 hash=rsx_fp_structural_hash(vm_base+base+256,16,1469598103934665603ull);char filter[64];snprintf(filter,sizeof filter,"%016llX:00000040",(unsigned long long)hash);
 int selected=strcmp(argv[2],"selected")==0,invalid=strcmp(argv[2],"invalid")==0;
 if(selected)setenv("PS3RECOMP_RSX_BINDING_CAPTURE_CONSUMERS",filter,1);
 else if(invalid)setenv("PS3RECOMP_RSX_BINDING_CAPTURE_CONSUMERS","1:40,2:40",1);
 else if(strcmp(argv[2],"wildcard")==0)setenv("PS3RECOMP_RSX_BINDING_CAPTURE_CONSUMERS","*",1);
 else unsetenv("PS3RECOMP_RSX_BINDING_CAPTURE_CONSUMERS");
 bind_texture_unit(RSX_LOCATION_MAIN,TEX_OFFSET);memset(vm_base+TEX_OFFSET,0x52,64);
 m(0x8E4,base|2);m(0x1D60,64);draw_triangle();CHECK(captures==(selected||invalid?0:1),"early different program cannot consume selected request");
 m(0x8E4,(base+256)|2);m(0x1D60,0);draw_triangle();CHECK(captures==(selected||invalid?0:1),"raw shader control is an exact part of selector");
 m(0x1D60,64);draw_triangle();CHECK(captures==(invalid?0:1),"matching late consumer gets one capture");
 draw_triangle();CHECK(captures==(invalid?0:1),"one capture attempt per selected offset");CHECK(stub.n_draw==4,"capture filtering never changes draw emission");
 engine_down();free(vm_base);printf("binding consumer failures: %d\n",g_failures);return g_failures?1:0;}
