/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <vector>

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
 if(opd==0x3010) {if(arg!=0x11223344||boundary!=128||size<0x5000)std::abort();ppu_context nested{};nested.gpr[3]=0x1800;ps3_hle_call(0x18B4629D,&nested);if(uint32_t(nested.gpr[3])!=0x80610702)std::abort();++free_count;return 0;}
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

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1; } } while(0)
static uint32_t call(uint32_t nid,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,uint64_t e=0) {
 ppu_context ctx{};ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;
 ctx.gpr[3]=a;ctx.gpr[4]=b;ctx.gpr[5]=c;ctx.gpr[6]=d;ctx.gpr[7]=e;
 ps3_hle_call(nid,&ctx);if(ctx.gpr[2]!=0x12345678)std::abort();return uint32_t(ctx.gpr[3]);
}

struct Event { uint32_t major,minor;uint64_t arg0,arg1; };
static std::vector<Event> events;
static bool destroy_on_close=false, callback_ok=true;
static uint32_t active_desc;
static void event_callback(uint32_t opd,uint64_t arg,uint64_t ev,uint64_t a,uint64_t b,uint64_t,uint64_t,uint64_t,uint64_t) {
 if(opd!=0x3200||arg!=0x99887766)callback_ok=false;
 events.push_back({uint32_t(ev>>32),uint32_t(ev),a,b});
 // Each callback may reenter the HLE bridge. Protect the in-flight player
 // without holding the descriptor allocator lock needed by close consumers.
 for(uint32_t nid : {0x34ECC1B9u,0x85BEFFCCu,0x18B4629Du,0x23654375u,0xF220E64Fu})
  if(call(nid,0x1000)!=0x80610702)callback_ok=false;
 if((ev>>32)==3 && a==8 && call(0xFC839BD4,0x1000,active_desc)!=0x80610702)callback_ok=false;
 if((ev>>32)==4) {
  if(a!=active_desc||call(0x0ABB318B,a,0x4400,128,0)!=0||call(0x4C191088,a)==0)callback_ok=false;
 }
 if((ev>>32)==5) {
  if(a!=active_desc||call(0x28336E89,a,0x4400)!=0||call(0x4C191088,a)==0||call(0x752F8585,0x1000)!=0)callback_ok=false;
  // Real callbacks may consume synchronously, or merely queue this event.
  if(destroy_on_close && call(0xFC839BD4,0x1000,a)!=0)callback_ok=false;
 }
}
int main(int argc,char**argv) {
 static uint8_t memory[0x10000];vm_base=memory;ppu_hle_register_all();
 constexpr uint32_t INIT=0x23654375,ALLOC=0x346EBBA3,CREATE=0xD7938B8D,OPEN=0xC044FAB1,ADD=0x7C8DFF3B,DEL=0xFC839BD4,FINAL=0x18B4629D,COUNT=0x752F8585;
 constexpr uint32_t STREAM_OPEN=0x34ECC1B9,STREAM_CLOSE=0x85BEFFCC,ARG=0x80610701,STATE=0x80610702;
 CHECK(argc==2);
 vm_write32(0x3000,0x1234);vm_write32(0x3010,0x5678);vm_write32(0x3200,0x9876);
 vm_write32(0x2000,0x3000);vm_write32(0x2004,0x3010);
 CHECK(call(ALLOC,0x2100,0x2000,0x11223344)==0);
 CHECK(call(INIT,0x1000,0x2100,0x3200,0x99887766)==0);
 g_ps3_guest_caller=event_callback;
 CHECK(call(STREAM_OPEN,0x1000)==STATE);CHECK(events.empty());
 CHECK(call(STREAM_CLOSE,0x1000)==STATE);CHECK(events.empty());
 std::snprintf((char*)memory+0x4000,512,"x-cell-fs:///dev_bdvd/%s",argv[1]);
 for(unsigned pass=0;pass<2;++pass) {
  CHECK(call(CREATE,0x1000,0,0,0x4000,0x4200)==0);active_desc=vm_read32(0x4200);
  CHECK(call(ADD,0x1000,active_desc)==0);
  CHECK(call(STREAM_OPEN,0x1000)==STATE);CHECK(events.empty());
  CHECK(call(OPEN,active_desc)==0);CHECK(call(STREAM_OPEN,0x1000,0x1122334455667788ull)==0);
  CHECK(callback_ok);CHECK(events.size()==4);CHECK(events[0].major==3&&events[0].arg0==3);
  CHECK(events[1].major==4&&events[1].arg0==active_desc);
  CHECK(events[2].major==2&&events[2].minor==2&&events[2].arg1==0x1122334455667788ull);CHECK(events[3].major==3&&events[3].arg0==4);
  CHECK(call(STREAM_OPEN,0x1000)==STATE);CHECK(call(FINAL,0x1000)==STATE);
  destroy_on_close=(pass==1);events.clear();CHECK(call(STREAM_CLOSE,0x1000,0x8877665544332211ull)==0);
  CHECK(callback_ok);CHECK(events.size()==4);CHECK(events[0].major==3&&events[0].arg0==8);
  CHECK(events[1].major==5&&events[1].arg0==active_desc);
  CHECK(events[2].major==2&&events[2].minor==3&&events[2].arg1==0x8877665544332211ull);CHECK(events[3].major==3&&events[3].arg0==2);
  CHECK(call(COUNT,0x1000)==0);CHECK(call(STREAM_CLOSE,0x1000)==STATE);
  if(!destroy_on_close) {
   CHECK(free_count==0);CHECK(call(FINAL,0x1000)==STATE);CHECK(call(0xF220E64F)==STATE);CHECK(call(0x4C191088,active_desc)!=0);
   CHECK(call(ADD,0x1000,active_desc)==STATE);CHECK(call(OPEN,active_desc)==STATE);
   CHECK(call(DEL,0x1000,active_desc)==0);
  }
  CHECK(free_count==pass+1);CHECK(call(DEL,0x1000,active_desc)==ARG);events.clear();
 }
 CHECK(call(FINAL,0x1000)==0);CHECK(free_count==2);CHECK(callback_ok);
 std::printf("%u descriptor event checks PASS\n",checks);return 0;
}
