/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "../libs/codec/sail_graphics_state.h"
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
int cellfs_translate_path(const char*,char*,size_t) { std::abort(); }
int32_t cellPamfReaderInitialize(void*,void*,uint64_t,uint32_t) { std::abort(); }
int32_t cellPamfReaderGetStreamIndex(void*) { std::abort(); }
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

#ifndef SAIL_GRAPHICS_HAS_REPLACEMENT
extern "C" s32 sail_graphics_replace_frame(u32,u32,const u8[32],u64,u64,u32){return (s32)0x80610708u;}
#endif
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1; } } while(0)
static uint32_t call(uint32_t nid,uint64_t a,uint64_t b=0,uint64_t c=0) {
    ppu_context ctx{};ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;
    ctx.gpr[3]=a;ctx.gpr[4]=b;ctx.gpr[5]=c;
    ps3_hle_call(nid,&ctx);
    if(ctx.gpr[2]!=0x12345678)std::abort();
    return uint32_t(ctx.gpr[3]);
}
static bool fill_is(uint32_t a,size_t n,uint8_t value) {
    for(size_t i=0;i<n;++i)if(vm_base[a+i]!=value)return false;
    return true;
}
int main() {
 static uint8_t memory[0x10000];vm_base=memory;ppu_hle_register_all();
 constexpr uint32_t INIT=0x1C983864,GET=0x0247C69E,FMT=0xFFD58AA4,PTS=0x1872331B;
 constexpr uint32_t PLAYER=0x23654375,BIND=0x18BCD21B,FINAL=0x76488BB1,STATE=0x80610702,ARG=0x80610701,NA=0x80610708;
 CHECK(call(INIT,0x1000)==0);CHECK(call(PLAYER,0x5000)==0);CHECK(call(BIND,0x5000,0,0x1000)==0);
 u8 format[32]={1,0,8,3,0,64,0,48,0,0,1,0,0,0,0,255,1,1,0};
 SailGraphicsConfig pinned{};u32 session=0;
 CHECK(sail_graphics_begin_session(0x5000,&pinned,&session)==0);CHECK(session!=0&&session!=UINT32_MAX);
 CHECK(call(FINAL,0x1000)==STATE);CHECK(call(0x24F4E900,0x5000)==STATE);
 CHECK(call(0xF220E64F,0)==STATE);
 CHECK(sail_graphics_begin_session(0x5000,&pinned,&session)==STATE);
 CHECK(sail_graphics_publish_frame(0x5000,0x6000,format,177087,90000,0x12345678)==0);
 CHECK(call(GET,0x1000,0x2000)==0x200);
 CHECK(vm_read32(0x2000)==0x6000&&vm_read32(0x2008)==0x12345678);
 CHECK(vm_read64(0x2010)==177087);CHECK(vm_read32(0x2004)==session);CHECK(vm_read32(0x200C)==0);
 CHECK(call(GET,0x1000,0x2000)==0x100);
 CHECK(call(FMT,0x1000,0x3000)==0);CHECK(!std::memcmp(memory+0x3000,format,32));
 CHECK(call(PTS,0x1000,177087,0x4000)==0);CHECK(vm_read64(0x4000)==967633);
 CHECK(call(PTS,0x1000,90000,0x4000)==0);CHECK(vm_read64(0x4000)==0);
 CHECK(call(PTS,0x1000,90001,0x4000)==0);CHECK(vm_read64(0x4000)==11);
 CHECK(call(PTS,0x1000,90090,0x4000)==0);CHECK(vm_read64(0x4000)==1000);
 CHECK(call(PTS,0x1000,89999,0x4000)==0);CHECK(vm_read64(0x4000)==uint64_t(-11));
 CHECK(call(PTS,0x1000,UINT64_MAX,0x4000)==0);CHECK(vm_read64(0x4000)==uint64_t(-1000011));
 CHECK(call(PTS,0x1000,177087,0xFFFC)==ARG);
 CHECK(call(FINAL,0x1000)==STATE);CHECK(call(0x24F4E900,0x5000)==STATE);CHECK(call(0xF220E64F,0)==STATE);
 CHECK(sail_graphics_unpublish_frame(0x5000)==0);
 CHECK(call(GET,0x1000,0x2000)==0);CHECK(fill_is(0x2000,24,0));
 CHECK(call(FINAL,0x1000)==STATE);
 format[5]=32;CHECK(sail_graphics_publish_frame(0x5000,0x6000,format,177087,90000,0)==STATE);format[5]=64;
 CHECK(sail_graphics_withdraw_frame(0x5000)==0);
 std::memset(memory+0x2000,0xA5,24);CHECK(call(GET,0x1000,0x2000)==0);CHECK(fill_is(0x2000,24,0));

 CHECK(sail_graphics_replace_frame(0x5000,0x6800,format,180090,90000,1)==STATE);
 CHECK(sail_graphics_begin_session(0x5000,&pinned,&session)==0);
 CHECK(sail_graphics_unpublish_frame(0x5000)==0);
 CHECK(sail_graphics_replace_frame(0x5000,0x6000,format,90000,90000,1)==STATE);
 CHECK(call(GET,0x1000,0x2000)==0);CHECK(fill_is(0x2000,24,0));
 CHECK(sail_graphics_withdraw_frame(0x5000)==0);
 CHECK(sail_graphics_begin_session(0x5000,&pinned,&session)==0);
 CHECK(sail_graphics_replace_frame(0x5000,0x6000,format,90000,90000,1)==0);
 CHECK(call(GET,0x1000,0x2000)==0x200);CHECK(vm_read32(0x2008)==1 && vm_read32(0x2004)==session);
 CHECK(call(GET,0x1000,0x2000)==0x100);CHECK(vm_read32(0x2008)==2);
 CHECK(call(GET,0x1000,0x2000)==0x100);CHECK(vm_read32(0x2008)==3);
 CHECK(sail_graphics_replace_frame(0x5000,0x6800,format,93003,90000,2)==0);
 CHECK(call(GET,0x1000,0x2000)==0x200);CHECK(vm_read32(0x2008)==4 && vm_read32(0x2000)==0x6800 && vm_read32(0x2004)==session);
 CHECK(vm_read64(0x2010)==93003);
 CHECK(sail_graphics_replace_frame(0x5000,0x6000,format,90000,90000,1)==STATE);
 CHECK(sail_graphics_replace_frame(0x5000,0x6000,format,90000,90000,2)==STATE);
 format[5]=32;CHECK(sail_graphics_replace_frame(0x5000,0x6000,format,96006,90000,3)==STATE);format[5]=64;
 CHECK(sail_graphics_replace_frame(0x5000,0xFFFF,format,96006,90000,3)==ARG);
 CHECK(call(GET,0x1000,0x2000)==0x100);CHECK(vm_read32(0x2000)==0x6800 && vm_read64(0x2010)==93003);
 CHECK(call(FINAL,0x1000)==STATE);CHECK(call(0x24F4E900,0x5000)==STATE);
 CHECK(sail_graphics_unpublish_frame(0x5000)==0);
 CHECK(sail_graphics_replace_frame(0x5000,0x6000,format,96006,90000,3)==STATE);
 CHECK(sail_graphics_withdraw_frame(0x5000)==0);
 CHECK(call(FMT,0x1000,0x3000)==NA);
 std::memset(memory+0x4000,0xA5,8);CHECK(call(PTS,0x1000,177087,0x4000)==STATE);CHECK(fill_is(0x4000,8,0xA5));
 CHECK(sail_graphics_publish_frame(0x5000,0xFFF0,format,177087,90000,0)==ARG);
 CHECK(sail_graphics_publish_frame(0x5000,0x6000,format,177087,UINT64_MAX,0)==0);
 CHECK(call(GET,0x1000,0x2000)==0x200);CHECK(call(PTS,0x1000,177087,0x4000)==STATE);CHECK(fill_is(0x4000,8,0xA5));
 CHECK(sail_graphics_withdraw_frame(0x5000)==0);CHECK(call(FINAL,0x1000)==0);
 std::printf("%u real frame/format/PTS HLE checks PASS\n",checks);return 0;
}
