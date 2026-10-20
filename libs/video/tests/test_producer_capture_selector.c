/* Actual producer caller, neutral programs/memory/backend. No GPU/title bytes. */
#define main retained_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"
#include "rsx_test_programs.h"
#include <assert.h>
struct spu_context;uint32_t spurs_policy_service_identity(const struct spu_context*p){(void)p;return 0;}
int main(int argc,char**argv)
{
 assert(argc==3);setenv("RSX_ENGINE_PRODUCER_CAPTURE_DIR",argv[1],1);
 setenv("RSX_ENGINE_PRODUCER_CAPTURE_TARGET",!strcmp(argv[2],"wrong-target")?"0:00050000":"0:00040000",1);
 setenv("RSX_ENGINE_PRODUCER_CAPTURE_LIMIT",!strcmp(argv[2],"oversized-limit")?"257":"2",1);
 vm_base=calloc(1,GUEST_BYTES);assert(vm_base);ppu_vm_size=GUEST_BYTES;engine_up();
 u8 vp[16];rsx_test_vp_mov_out(vp,0,RSX_TEST_VP_SWZ_IDENT,0,1);m(0x1E9C,0);
 for(u32 i=0;i<4;i++){u32 word;memcpy(&word,vp+4*i,4);m(0xB80+4*i,word);}m(0x1EA0,0);
 rsx_test_fp_mov_r0_col0(vm_base+0x60000,RSX_TEST_FP_SWZ_IDENT);m(0x8E4,0x60002);m(0x1D60,0x40);
 char exact[64];snprintf(exact,sizeof exact,"%016llX:%08X",(unsigned long long)rsx_fp_structural_hash(vm_base+0x60000,16,1469598103934665603ull),!strcmp(argv[2],"wrong-control")?0:0x40);
 if(!strcmp(argv[2],"unset"))unsetenv("RSX_ENGINE_PRODUCER_CAPTURE_SELECTOR");
 else setenv("RSX_ENGINE_PRODUCER_CAPTURE_SELECTOR",!strcmp(argv[2],"empty")?"":!strcmp(argv[2],"malformed")?"*x":!strcmp(argv[2],"multiple")?"*,*":(!strcmp(argv[2],"exact")||!strcmp(argv[2],"wrong-control"))?exact:"*",1);
 draw_triangle();rsx_draw_engine_present();draw_triangle();draw_triangle();draw_triangle();
 rsx_draw_engine_present();draw_triangle();assert(stub.n_draw==5);
 engine_down();free(vm_base);return 0;
}
