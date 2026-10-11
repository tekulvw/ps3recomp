/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <initializer_list>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size=0x10000;
uint32_t g_ww_lo=0,g_ww_hi=0;
int g_resv_store_active=0;
ps3_guest_caller_fn g_ps3_guest_caller=nullptr;
void ppu_hle_register_all();
int ps3_hle_has(uint32_t);
void ps3_hle_call(uint32_t,ppu_context*);
int cellfs_translate_path(const char*path,char*out,size_t size) {
 const char*prefix="/dev_bdvd/";if(std::strncmp(path,prefix,10))return -1;
 int n=std::snprintf(out,size,"%s",path+10);return n>=0&&size_t(n)<size?0:-1;
}
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
static uint32_t next_alloc=0x5000,alloc_count=0,free_count=0,reentry_count=0;
uint64_t ppu_guest_call(uint32_t opd,uint64_t arg,uint64_t boundary,uint64_t size,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) {
 if(opd==0x3010) {if(arg!=0x11223344||boundary!=128||size<0x5000)std::abort();++free_count;return 0;}
 if(opd==0x3000) {
 ppu_context nested{};nested.gpr[3]=0;ps3_hle_call(0xC044FAB1,&nested);
 if(uint32_t(nested.gpr[3])!=0x80610702)std::abort();++reentry_count;
if(arg!=0x11223344||boundary!=128)std::abort();++alloc_count;uint32_t out=next_alloc;next_alloc+=uint32_t(size);return out;} std::abort();
}

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

static void unexpected_event(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) { std::abort(); }
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1; } } while(0)
static uint32_t call(uint32_t nid,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,uint64_t e=0) {
 ppu_context ctx{};ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;
 ctx.gpr[3]=a;ctx.gpr[4]=b;ctx.gpr[5]=c;ctx.gpr[6]=d;ctx.gpr[7]=e;
 ps3_hle_call(nid,&ctx);if(ctx.gpr[2]!=0x12345678)std::abort();return uint32_t(ctx.gpr[3]);
}
// Test-only host inspection: no production accessor or guest-memory side effect.
extern "C" int inspect_selection(uint32_t desc, uint32_t category, uint8_t* reader, uint8_t* metadata);
int main(int argc,char**argv) {
 static uint8_t memory[0x10000];vm_base=memory;g_ps3_guest_caller=unexpected_event;ppu_hle_register_all();
 constexpr uint32_t INIT=0x23654375,ALLOC=0x346EBBA3,CREATE=0xD7938B8D,OPEN=0xC044FAB1,DB=0x0ABB318B,DEL=0xFC839BD4;
 constexpr uint32_t SET=0x0D0C2F0C,DROP=0x28336E89,SELECT=0x28B4E2C1;
 constexpr uint32_t ARG=0x80610701,STATE=0x80610702,UNAVAILABLE=0x80610708;
 CHECK(argc==2);CHECK(ps3_hle_has(SET));CHECK(ps3_hle_has(DROP));
 vm_write32(0x3000,0x1234);vm_write32(0x3010,0x5678);
 vm_write32(0x2000,0x3000);vm_write32(0x2004,0x3010);vm_write32(0x2104,0x11223344);
 CHECK(call(ALLOC,0x2100,0x2000,0x11223344)==0);CHECK(call(INIT,0x1000,0x2100,0x3200,0x1234)==0);
 std::snprintf((char*)memory+0x4000,512,"x-cell-fs:///dev_bdvd/%s",argv[1]);
 CHECK(call(CREATE,0x1000,0,0,0x4000,0x4200)==0);uint32_t desc=vm_read32(0x4200);CHECK(desc==0);
 CHECK(call(SET,99,0,0,1,0x4300)==ARG);CHECK(call(SET,desc,0,0,1,0x4300)==STATE);
 CHECK(call(DROP,desc,0x4300)==STATE);
 CHECK(call(OPEN,desc)==0);CHECK(call(DB,desc,0x4300,128)==0);
 CHECK(call(SET,desc,2,0,1,0x4300)==UNAVAILABLE);CHECK(call(SET,desc,99,0,1,0x4300)==ARG);
 CHECK(call(SET,desc,0,1,1,0x4300)==UNAVAILABLE);CHECK(call(SET,desc,0,0,0,0x4300)==UNAVAILABLE);
 for(uint64_t bad:{0ull,0x4301ull,0xFFFCull,0x100004300ull}) {
  CHECK(call(SET,desc,0,0,1,bad)==ARG);CHECK(call(DROP,desc,bad)==ARG);
 }
 CHECK(call(SET,desc,0,0,1,0x4300)==ARG); // Reader initially selects video.
 CHECK(call(SELECT,0x4300,21,0)==1);
 uint8_t audio[128],video[128],snap[128],am[48],vm[48],meta[48];
 static uint8_t untouched[0x10000];std::memcpy(untouched,memory,sizeof(memory));
 std::memcpy(audio,memory+0x4300,128);std::memcpy(am,memory+vm_read32(0x4334),48);
 CHECK(call(SET,desc,0,0,1,0x4300)==0);CHECK(std::memcmp(memory,untouched,sizeof(memory))==0);CHECK(alloc_count==1);CHECK(free_count==0);CHECK(inspect_selection(desc,0,snap,meta)==1);
 CHECK(std::memcmp(snap,audio,128)==0);CHECK(std::memcmp(meta,am,48)==0);CHECK(inspect_selection(desc,1,snap,meta)==0);
 CHECK(call(SELECT,0x4300,20,0)==0);std::memcpy(video,memory+0x4300,128);std::memcpy(vm,memory+vm_read32(0x4334),48);
 CHECK(call(SET,desc,1,0,1,0x4300)==0);CHECK(call(SET,desc,0,0,1,0x4300)==ARG);
 CHECK(inspect_selection(desc,0,snap,meta)==1);CHECK(std::memcmp(snap,audio,128)==0);CHECK(std::memcmp(meta,am,48)==0);
 CHECK(inspect_selection(desc,1,snap,meta)==1);CHECK(std::memcmp(snap,video,128)==0);CHECK(std::memcmp(meta,vm,48)==0);
 // Forged selected pointer/index and another descriptor's valid reader must not commit.
 vm_write32(0x4320,99);CHECK(call(SET,desc,1,0,1,0x4300)==ARG);std::memcpy(memory+0x4300,video,128);
 vm_write32(0x4334,0xFFFF);CHECK(call(SET,desc,1,0,1,0x4300)==ARG);std::memcpy(memory+0x4300,video,128);
 CHECK(call(CREATE,0x1000,0,0,0x4000,0x4204)==0);uint32_t other=vm_read32(0x4204);
 CHECK(call(OPEN,other)==0);CHECK(call(DB,other,0x4400,128)==0);
 CHECK(call(SET,desc,1,0,1,0x4400)==ARG);CHECK(call(DROP,desc,0x4400)==ARG);
 CHECK(inspect_selection(other,0,snap,meta)==0);CHECK(inspect_selection(other,1,snap,meta)==0);
 CHECK(call(DROP,desc,0x4300)==0);CHECK(std::memcmp(memory+0x4300,video,128)==0);CHECK(free_count==0);
 std::memset(memory+0x4300,0xA5,128); // Caller reuses its stack after DestroyDatabase.
 CHECK(inspect_selection(desc,0,snap,meta)==1);CHECK(std::memcmp(snap,audio,128)==0);CHECK(std::memcmp(meta,am,48)==0);
 CHECK(inspect_selection(desc,1,snap,meta)==1);CHECK(std::memcmp(snap,video,128)==0);CHECK(std::memcmp(meta,vm,48)==0);
 CHECK(call(DROP,99,0x4400)==ARG);CHECK(call(DEL,0x1000,desc)==0);CHECK(free_count==1);
 CHECK(call(SET,desc,1,0,1,0x4400)==ARG);CHECK(call(CREATE,0x1000,0,0,0x4000,0x4200)==0);CHECK(vm_read32(0x4200)==desc);
 CHECK(inspect_selection(desc,0,snap,meta)==0);CHECK(inspect_selection(desc,1,snap,meta)==0);
 CHECK(call(DEL,0x1000,desc)==0);CHECK(call(DEL,0x1000,other)==0);CHECK(free_count==2);
 std::printf("%u descriptor stream selection checks PASS\n",checks);return 0;
}
