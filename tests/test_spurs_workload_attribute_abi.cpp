/* Real PPU dispatcher and real SPURS initializer; synthetic guest stack only. */
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <initializer_list>
extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size=0x10000;
uint32_t ppu_hle_inject_base=0x20000000;
uint32_t g_ww_lo=0,g_ww_hi=0;
int g_resv_store_active=0;
ps3_guest_caller_fn g_ps3_guest_caller=nullptr;
void ps3_hle_register(uint32_t,const char*,void*);
int32_t _cellSpursWorkloadAttributeInitialize(uint64_t,uint32_t,uint32_t,uint64_t,uint32_t,uint64_t,uint64_t,uint32_t,uint32_t);
int ps3_hle_has(uint32_t);
void ps3_hle_call(uint32_t,ppu_context*);
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
uint64_t ppu_guest_call(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) { std::abort(); }
void ppu_prof_stamp(void*,unsigned) {}
void ppu_gcm_pump() {}
uint32_t ppu_prof_resolve_host(void*) { return 0; }
void ps3_msf(const char*,...) {}
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) {}
void spu_lockline_lock() {}
void spu_lockline_unlock() {}
void ppu_resv_break_store(uint64_t) {}
void ps3_ww_report_inline(uint32_t,uint64_t,int) {}
uint32_t vm_read32(uint64_t a) { uint32_t x;std::memcpy(&x,vm_base+a,4);return __builtin_bswap32(x); }
void vm_write32(uint64_t a,uint32_t x) { x=__builtin_bswap32(x);std::memcpy(vm_base+a,&x,4); }
}
extern "C" uint64_t vm_read64(uint64_t a) { uint64_t x;std::memcpy(&x,vm_base+a,8);return __builtin_bswap64(x); }
void vm_write64(uint64_t a,uint64_t x) { x=__builtin_bswap64(x);std::memcpy(vm_base+a,&x,8); }
extern "C" void ppu_dump_guest_stack(ppu_context*,const char*) {}
extern "C" void ppu_dump_bctrl_ring(uint32_t,const char*) {}
extern "C" void ppu_guest_callstack(const char*) {}
extern uint32_t g_ydkj_real_spurs_ea,g_ydkj_real_taskset_ea;


int main(int argc,char **argv){
 assert(argc==2);bool enabled=strcmp(argv[1],"off");if(enabled)setenv("PS3RECOMP_SPURS_WORKLOAD_STACK_ARG9","1",1);else unsetenv("PS3RECOMP_SPURS_WORKLOAD_STACK_ARG9");
 static uint8_t memory[0x10000];vm_base=memory;
 ps3_hle_register(0xEFEB2679u,"_cellSpursWorkloadAttributeInitialize",(void*)_cellSpursWorkloadAttributeInitialize);
 for(uint64_t maximum:{uint64_t(1),uint64_t(5),uint64_t(8),UINT64_C(0x1122334400000005)}){
  memset(memory,0xA5,sizeof memory);ppu_context c{};c.gpr[1]=0x8000;c.gpr[2]=0x12345678;c.gpr[3]=0x100;c.gpr[4]=2;c.gpr[5]=0x360001;c.gpr[6]=0x3000;c.gpr[7]=12032;c.gpr[8]=0x155ED500;c.gpr[9]=0x400;c.gpr[10]=1;vm_write64(0x8070,maximum);
  ps3_hle_call(0xEFEB2679,&c);assert((uint32_t)c.gpr[3]==0);assert(c.gpr[2]==0x12345678);assert(vm_read32(0x100)==2&&vm_read32(0x104)==0x360001);assert(vm_read32(0x124)==(enabled?(uint32_t)maximum:1));assert(vm_read64(0x110)==0x155ED500);assert(vm_read64(0x8070)==maximum);assert(memory[0xff]==0xA5&&memory[0x140]==0xA5);
 }
 if(enabled){for(uint64_t sp:{uint64_t(0xFFA0),UINT64_C(0xFFFFFFFFFFFFFFF0)}){
  memset(memory,0xA5,sizeof memory);ppu_context c{};c.gpr[1]=sp;c.gpr[3]=0x100;ps3_hle_call(0xEFEB2679,&c);assert((uint32_t)c.gpr[3]==0x80410702u);for(unsigned i=0;i<sizeof memory;i++)assert(memory[i]==0xA5);
 }}
 puts("PASS real ps3_hle_call: stack BE64/low32, 1/5/8, revision fields, default-off1, invalid-stack immutability");
}
