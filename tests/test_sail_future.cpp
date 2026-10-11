/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <thread>
#include "../libs/codec/cellSail.h"

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
constexpr uint32_t INIT=0x4CC54F8E, SET=0xA37FED15, GET=0x3A2D806C, FINAL=0x9553AF65;
constexpr uint32_t BAD_ARG=0x80610701, BAD_STATE=0x80610702, UNAVAILABLE=0x80610708;
static uint32_t callback_result;
static void callback(uint32_t opd,uint64_t object,uint64_t event,uint64_t result,uint64_t future,uint64_t,uint64_t,uint64_t,uint64_t) {
    if(opd!=0x1234 || object!=0x5678 || event!=0x200000001ull) std::abort();
    callback_result=call(SET,future,result);
}
int main() {
    static uint8_t memory[0x10000];vm_base=memory;
    ppu_hle_register_all();
    CHECK(ps3_hle_has(INIT));CHECK(ps3_hle_has(SET));CHECK(ps3_hle_has(GET));CHECK(ps3_hle_has(FINAL));
    std::memset(vm_base+0xFF8,0xA5,40);
    std::memset(vm_base+0x1FFC,0xA5,12);
    CHECK(call(GET,0x1000,0,0x2000)==BAD_STATE);
    CHECK(call(SET,0x1000,7)==BAD_STATE);
    CHECK(call(FINAL,0x1000)==BAD_STATE);
    CHECK(call(INIT,0x1000)==0);
    CHECK(fill_is(0x1000,24,0));
    CHECK(fill_is(0xFF8,8,0xA5)&&fill_is(0x1018,8,0xA5));
    CHECK(call(INIT,0x1000)==BAD_STATE);
    CHECK(call(GET,0x1000,0,0x2000)==UNAVAILABLE);
    CHECK(call(GET,0x1000,0xffffffffffffffffull,0x2000)==UNAVAILABLE);
    CHECK(fill_is(0x1FFC,12,0xA5));
    g_ps3_guest_caller=callback;
    g_ps3_guest_caller(0x1234,0x5678,0x200000001ull,0xffffffff81234567ull,0x1000,0,0,0,0);
    CHECK(callback_result==0);
    CHECK(vm_read32(0x100C)==0x81234567); // signed result preserved exactly
    CHECK(fill_is(0x1000,12,0)&&fill_is(0x1010,8,0)); // no invented mutex/flags
    CHECK(call(GET,0x1000,0,0x2000)==0);
    CHECK(vm_read32(0x2000)==0x81234567);
    CHECK(fill_is(0x1FFC,4,0xA5)&&fill_is(0x2004,4,0xA5));
    CHECK(call(GET,0x1000,99,0x2000)==0);
    CHECK(call(SET,0x1000,0)==BAD_STATE);
    CHECK(vm_read32(0x100C)==0x81234567);
    CHECK(call(GET,0x1000,0,0)==BAD_ARG);
    CHECK(call(GET,0x1000,0,0xFFFE)==BAD_ARG);
    CHECK(call(GET,0x1000,0,0xFFFFFFFC)==BAD_ARG);
    CHECK(call(INIT,0)==BAD_ARG);
    CHECK(call(INIT,0xFFF0)==BAD_ARG);
    CHECK(call(INIT,0xFFFFFFF8)==BAD_ARG);
    CHECK(call(INIT,0x1001)==BAD_ARG);
    CHECK(call(FINAL,0x1000)==0);
    CHECK(call(SET,0x1000,0)==BAD_STATE);
    std::memset(vm_base+0x2000,0x5A,4);
    CHECK(call(GET,0x1000,0,0x2000)==BAD_STATE);
    CHECK(fill_is(0x2000,4,0x5A));
    CHECK(call(FINAL,0x1000)==BAD_STATE);
    CHECK(call(INIT,0x1000)==0);
    CHECK(call(GET,0x1000,0,0x2000)==UNAVAILABLE);
    CHECK(call(SET,0x1000,0)==0);
    CHECK(call(GET,0x1000,0,0x2000)==0 && vm_read32(0x2000)==0);
    CHECK(call(INIT,0xFFE8)==0); // last complete aligned 24-byte record
    CHECK(call(SET,0xFFE8,42)==0);
    CHECK(call(GET,0xFFE8,0,0xFFFC)==0 && vm_read32(0xFFFC)==42);
    CHECK(call(0xF220E64F,0)==0); // cellSailTerm invalidates host lifecycle
    CHECK(call(GET,0x1000,0,0x2000)==BAD_STATE);
    CHECK(call(GET,0xFFE8,0,0x2000)==BAD_STATE);
    ppu_vm_size=0;
    CHECK(call(INIT,0xFFFFFFF8)==BAD_ARG); // overflow with full 4-GiB sentinel
    CHECK(call(INIT,0x1000)==0);
    CHECK(call(FINAL,0x1000)==0);
    ppu_vm_size=sizeof memory;
    for(unsigned i=0;i<64;i++) CHECK(call(INIT,0x3000+i*32)==0);
    std::memset(vm_base+0x3800,0x5A,24);
    CHECK(call(INIT,0x3800)==0x806107F0);
    CHECK(fill_is(0x3800,24,0x5A));
    CHECK(call(FINAL,0x3000)==0);
    CHECK(call(INIT,0x3800)==0);
    CHECK(call(0xF220E64F,0)==0);
    std::atomic<bool> ok{true};
    std::thread workers[4];
    for(unsigned i=0;i<4;i++) workers[i]=std::thread([&,i] {
        const uint32_t ea=0x4000+32*i,out=0x5000+4*i;
        for(unsigned j=0;j<500;j++) {
            int32_t result=int32_t(0x80000000u | (i<<16) | j);
            if(cellSailFutureInitialize(ea)!=0 || cellSailFutureSet(ea,result)!=0 ||
               cellSailFutureGet(ea,0,out)!=0 || vm_read32(out)!=uint32_t(result) ||
               cellSailFutureFinalize(ea)!=0) ok=false;
        }
    });
    for(auto& thread:workers) thread.join();
    CHECK(ok.load());
    std::printf("PASS: callback-completed Sail futures, pending behavior, lifecycle, ABI, bounds/canaries (%u checks)\n",checks);
}
