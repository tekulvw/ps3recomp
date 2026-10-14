#include <assert.h>
/* Neutral programs through the actual draw observer and engine entry. */
#define main retention_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"
#include "rsx_test_programs.h"
static unsigned fp_reads, reader_calls;
static const u8* observed_reader(void* u,u32 location,u32 offset,u32 bytes)
{
    (void)u;(void)location;reader_calls++;
    if(offset==0x60000)fp_reads++;
    if((u64)offset+bytes>GUEST_BYTES)return NULL;
    return vm_base+offset;
}
static void vertex_program(u32 swizzle)
{
    u8 vp[16];rsx_test_vp_mov_out(vp,0,swizzle,0,1);m(0x1E9C,0);
    for(unsigned i=0;i<4;i++){u32 word;memcpy(&word,vp+4*i,4);m(0xB80+4*i,word);}
    m(0x1EA0,0);
}
int main(int argc,char**argv)
{
    if(argc!=2)return 2;
    vm_base=calloc(1,GUEST_BYTES);ppu_vm_size=GUEST_BYTES;engine_up();
    rsx_draw_engine_set_guest_memory(observed_reader,NULL);
    vertex_program(RSX_TEST_VP_SWZ_IDENT);
    rsx_test_fp_mov_r0_col0(vm_base+0x60000,RSX_TEST_FP_SWZ_IDENT);m(0x8E4,0x60002);m(0x1D60,0x40);
    if(!strcmp(argv[1],"inactive-poison")){
#ifdef DRAW_OBSERVER_ID_TEST
        draw_observer_programs((const u8*)1,RSX_DSP_VP_INSTR,(const u8*)1,ENG_FP_MAX_BYTES);
#endif
        assert(!reader_calls);
    }else if(!strcmp(argv[1],"early")){
        m(M_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);m(M_BEGIN_END,0);assert(!fp_reads);
    }else if(!strcmp(argv[1],"prefetch")){
        m(0x8E4,(GUEST_BYTES-16)|2u);dc_reset();dc.n_packets=1;dc.fetch_ok=0;sink_end(NULL,&g.rsx);assert(!reader_calls);
    }else{
        if(!strcmp(argv[1],"invalid-vp"))memset(g.rsx.vp,0,sizeof g.rsx.vp);
        if(!strcmp(argv[1],"invalid-fp"))m(0x8E4,0);
        if(!strcmp(argv[1],"inline-constant")){rsx_test_fp_instr(vm_base+0x60000,RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_END,2u|RSX_TEST_FP_SWZ_IDENT);memset(vm_base+0x60010,0,16);}
        if(!strcmp(argv[1],"max-vp")){memset(g.rsx.vp,0,sizeof g.rsx.vp);g.rsx.vp[(RSX_DSP_VP_INSTR-1)*4+3]=1;}
        if(!strcmp(argv[1],"max-fp")){for(u32 i=0;i<ENG_FP_MAX_BYTES;i+=16)rsx_test_fp_instr(vm_base+0x60000+i,RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV)|RSX_TEST_FP_MASK_XYZW|(i+16==ENG_FP_MAX_BYTES?RSX_TEST_FP_END:0),RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);}
        draw_triangle();
        if(!strcmp(argv[1],"recycle-fp")){rsx_test_fp_mov_r0_col0(vm_base+0x60000,RSX_TEST_FP_SWZ(2,1,0,3));draw_triangle();}
        if(!strcmp(argv[1],"recycle-vp")){vertex_program(RSX_TEST_VP_SWZ(2,1,0,3));draw_triangle();}
        if(!strcmp(argv[1],"inline-constant")){memset(vm_base+0x60010,0x31,16);draw_triangle();}
        if(!strcmp(argv[1],"valid-then-early")){m(M_BEGIN_END,RSX_PRIMITIVE_TRIANGLES);m(M_BEGIN_END,0);}
    }
    printf("draws=%d fp_reads=%u reader_calls=%u\n",stub.n_draw,fp_reads,reader_calls);
    rsx_draw_engine_set_guest_memory(NULL,NULL);engine_down();free(vm_base);return 0;
}
