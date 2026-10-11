/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size=0x10000;
uint32_t g_ww_lo=0,g_ww_hi=0;
int g_resv_store_active=0;
ps3_guest_caller_fn g_ps3_guest_caller=nullptr;
void ppu_hle_register_all();
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
uint32_t g_ydkj_real_spurs_ea=0,g_ydkj_real_taskset_ea=0;

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1; } } while(0)
static uint32_t call(uint32_t nid,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,uint64_t e=0) {
 ppu_context ctx{};ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;
 ctx.gpr[3]=a;ctx.gpr[4]=b;ctx.gpr[5]=c;ctx.gpr[6]=d;ctx.gpr[7]=e;
 ps3_hle_call(nid,&ctx);if(ctx.gpr[2]!=0x12345678)std::abort();return uint32_t(ctx.gpr[3]);
}
int main() {
 static uint8_t memory[0x10000];vm_base=memory;ppu_hle_register_all();
 constexpr uint32_t INIT=0x23654375,COUNT=0x752F8585,CREATE=0xD7938B8D,ADD=0x7C8DFF3B,DEL=0xFC839BD4,REMOVE=0x9897FBD1,DESTROY=0x24F4E900;
 constexpr uint32_t ARG=0x80610701,STATE=0x80610702,UNAVAILABLE=0x80610708,MEMORY=0x806107F0;
 CHECK(ps3_hle_has(COUNT));CHECK(call(COUNT,0x1000)==ARG);
 CHECK(call(INIT,0x1000)==0);CHECK(call(INIT,0x2000)==0);CHECK(call(COUNT,0x1000)==0);
 vm_write32(0x4000,0xA5A5A5A5);CHECK(call(CREATE,0x9999,0,0,0,0x4000)==ARG);CHECK(vm_read32(0x4000)==0xA5A5A5A5);
 CHECK(call(CREATE,0x1000,0,0,0,0)==ARG);CHECK(call(CREATE,0x1000,0,0,0,0x4001)==ARG);
 CHECK(call(CREATE,0x1000,0,0,0,0xFFFF)==ARG);CHECK(call(CREATE,0x1000,0,0,0,0xFFFFFFFC)==ARG);
 CHECK(call(CREATE,0x1000,0,0,0x10000,0x4000)==ARG);CHECK(vm_read32(0x4000)==0xA5A5A5A5);
 CHECK(call(CREATE,0x1000,0,0,0,0x4000)==0);CHECK(vm_read32(0x4000)==0); // Zero-index handle is valid in current HLE.
 CHECK(call(COUNT,0x1000)==0);CHECK(call(ADD,0x2000,0)==ARG);CHECK(call(DEL,0x2000,0)==ARG);
 CHECK(call(ADD,0x1000,0)==0);CHECK(call(COUNT,0x1000)==1);CHECK(call(COUNT,0x2000)==0);
 CHECK(call(ADD,0x1000,0)==STATE);CHECK(call(COUNT,0x1000)==1);CHECK(call(DEL,0x1000,0)==STATE);
 CHECK(call(REMOVE,0x1000,0x4000)==UNAVAILABLE);CHECK(vm_read32(0x4000)==0);CHECK(call(COUNT,0x1000)==1);
 CHECK(call(DESTROY,0x1000)==STATE);CHECK(call(COUNT,0x1000)==1);
 CHECK(call(CREATE,0x2000,1,0,0,0x4004)==0);uint32_t second=vm_read32(0x4004);CHECK(second==1);
 CHECK(call(COUNT,0x2000)==0);CHECK(call(DEL,0x2000,second)==0);CHECK(call(DEL,0x2000,second)==ARG);
 CHECK(call(CREATE,0x2000,1,0,0,0x4004)==0);CHECK(call(DESTROY,0x2000)==0);
 CHECK(call(COUNT,0x2000)==ARG);CHECK(call(INIT,0x2000)==0);CHECK(call(COUNT,0x2000)==0);
 CHECK(call(ADD,0x2000,second)==ARG); // Player destruction released unregistered records.
 for(unsigned i=1;i<16;++i) {CHECK(call(CREATE,0x2000,0,0,0,0x4004)==0);CHECK(vm_read32(0x4004)==i);}
 vm_write32(0x4004,0xDEADBEEF);CHECK(call(CREATE,0x2000,0,0,0,0x4004)==MEMORY);CHECK(vm_read32(0x4004)==0xDEADBEEF);
 CHECK(call(COUNT,0x2000)==0);CHECK(call(ADD,0x2000,15)==0);CHECK(call(COUNT,0x2000)==1);
 CHECK(call(ADD,0x2000,16)==ARG);CHECK(call(REMOVE,0x9999,0)==ARG);
 std::printf("%u checks PASS\n",checks);return 0;
}
