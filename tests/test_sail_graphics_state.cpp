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
 constexpr uint32_t INIT=0x1C983864,FINAL=0x76488BB1,GET=0x0247C69E;
 constexpr uint32_t ARG=0x80610701,STATE=0x80610702;
 // Reject borrowed callback tables before any guest clearing or registration.
 std::memset(memory+0x1000,0xA5,0x100);
 CHECK(call(INIT,0x1000,0xFFF0,0x11223344)==ARG);
 CHECK(fill_is(0x1000,0x100,0xA5));
 CHECK(call(GET,0x1000,0x2000)==STATE);
 CHECK(call(INIT,0x1000,0x2001)==ARG);
 CHECK(call(INIT,0x1000,0x2000)==ARG); // zero/nonexistent callback OPDs
 CHECK(call(INIT,0x1000)==0);CHECK(call(FINAL,0x1000)==0);
 constexpr uint32_t PREF=0x2E3CCB5E,FMT=0xFFD58AA4,BIND=0x18BCD21B,PLAYER=0x23654375;
 constexpr uint32_t NA=0x80610708;
 CHECK(ps3_hle_has(FMT));
 // A callback descriptor cannot survive clearing the adapter that contains it.
 std::memset(memory+0x1000,0xA5,0x100);
 for(unsigned i=0;i<5;++i){vm_write32(0x2000+i*4,0x3000+i*12);vm_write32(0x3000+i*12,0x4000+i*4);}
 vm_write32(0x2000,0x1080);
 CHECK(call(INIT,0x1000,0x2000)==ARG);
 CHECK(fill_is(0x1000,0x100,0xA5));
 // Snapshot a borrowed table before clearing an overlapping adapter object.
 for(unsigned i=0;i<5;++i){vm_write32(0x1000+i*4,0x3000+i*12);vm_write32(0x3000+i*12,0x4000+i*4);}
 CHECK(call(INIT,0x1000,0x1000,0x11223344)==0);
 CHECK(fill_is(0x1000,0x100,0));
 CHECK(call(PREF,0x1000,0xFFF0)==ARG);
 CHECK(call(PREF,0x1000,0x2001)==ARG);
 for(unsigned i=0;i<32;++i)memory[0x2000+i]=uint8_t(i+1);
 CHECK(call(PREF,0x1000,0x2000)==0);
 CHECK(call(PREF,0x1100,0x2000)==STATE);
 CHECK(call(PLAYER,0x5000)==0);
 CHECK(call(BIND,0x5000,1,0x1000)==NA);
 CHECK(call(BIND,0x5000,0,0x1100)==STATE);
 CHECK(call(BIND,0x5000,0,0x1000)==0);
 CHECK(call(BIND,0x5000,0,0x1000)==0); // idempotent association
 CHECK(call(PLAYER,0x6000)==0);
 CHECK(call(BIND,0x6000,0,0x1000)==STATE);
 std::memset(memory+0x2000,0xA5,32);
 SailGraphicsConfig config{};
 CHECK(sail_graphics_config_for_player(0x5000,&config)==0);
 CHECK(config.self_ea==0x1000&&config.player_ea==0x5000&&config.arg_ea==0x11223344);
 CHECK(config.has_preferred==1);
 for(unsigned i=0;i<5;++i)CHECK(config.callbacks[i]==0x3000+i*12);
 for(unsigned i=0;i<32;++i)CHECK(config.preferred[i]==i+1);
 CHECK(call(FMT,0x1000,0,0x2000)==NA);
 CHECK(fill_is(0x2000,32,0xA5)); // preferred bytes never masquerade as actual format
 CHECK(call(FMT,0x1000,0,0xFFF0)==ARG);
 CHECK(call(0x24F4E900,0x5000)==0); // PlayerDestroy
 CHECK(call(BIND,0x6000,0,0x1000)==0); // destruction releases association
 CHECK(call(FINAL,0x1000)==0);
 CHECK(sail_graphics_config_for_player(0x6000,&config)==NA);
 CHECK(call(FMT,0x1000,0,0x2000)==STATE);
 CHECK(call(INIT,0x1000)==0);
 CHECK(call(BIND,0x6000,0,0x1000)==0);
 if(ps3_hle_has(0x18B4629D)) {
   CHECK(call(0x18B4629D,0x6000)==0);
   CHECK(call(PLAYER,0x5000)==0);
   CHECK(call(BIND,0x5000,0,0x1000)==0);
 }
 CHECK(call(0xF220E64F,0)==0);
 CHECK(call(FMT,0x1000,0,0x2000)==STATE);
 std::printf("%u graphics callback and format checks PASS\n",checks);return 0;
}
