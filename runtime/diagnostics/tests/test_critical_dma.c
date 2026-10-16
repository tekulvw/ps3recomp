#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#define main disposition_fixture_main
#include "../../spu/tests/test_spu_dma_disposition.c"
#undef main
#include "critical_trace.h"
#include "critical_write_trace.h"
uint32_t spurs_policy_service_identity(const void *ctx){(void)ctx;assert(locks==0);return 42;}
int main(int argc,char**argv){assert(argc==2);setenv("PS3RECOMP_CRITICAL_WATCH_CONTAINER","1234",1);setenv("PS3RECOMP_CRITICAL_WATCH_ARRAY","3",1);memset(g_vm_page_bitmap,255,sizeof(g_vm_page_bitmap));static spu_context s;static uint8_t ls[SPU_LS_SIZE];s.ls=ls;s.image_id=105;s.pc=0x1234;s.gpr[0]._u32[0]=0x5678;assert(!ps3_critical_begin(33,1000000000));ps3_critical_watch_bind(33,1,0x1234,3,0x12070);reserved=1;ls[0x173]=17;assert(!mfc_do_transfer(&s,0x100,0x12000,128,0x20));assert(memory[0x12073]==17&&locks==0&&notifications==1);reserved=0;ls[0x173]=33;assert(!mfc_do_transfer(&s,0x100,0x12000,128,0x20));assert(memory[0x12073]==33&&locks==0);assert(!ps3_critical_finish(argv[1]));return 0;}
