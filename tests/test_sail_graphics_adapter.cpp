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
int cellfs_translate_path(const char*,char*,size_t) { return -1; }
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
    static uint8_t memory[0x10000];vm_base=memory;
    ppu_hle_register_all();
    constexpr uint32_t INIT=0x1C983864,GET=0x0247C69E,FINAL=0x76488BB1,SYNC=0x44A20E79;
    constexpr uint32_t INVALID_ARG=0x80610701,INVALID_STATE=0x80610702,NOT_AVAILABLE=0x80610708;
    CHECK(ps3_hle_has(GET));CHECK(ps3_hle_has(FINAL));CHECK(ps3_hle_has(SYNC));
    std::memset(vm_base+0x1FFC,0xA5,32);
    CHECK(call(GET,0x1000,0x2000)==INVALID_STATE);
    CHECK(fill_is(0x1FFC,32,0xA5));
    CHECK(call(INIT,0x1000)==0);
    CHECK(call(INIT,0x1000)==INVALID_STATE);
    CHECK(call(GET,0x1000,0x2000)==0); // CELL_SAIL_SYNC_NO_FRAME, not decoded success
    CHECK(fill_is(0x2000,24,0));
    CHECK(fill_is(0x1FFC,4,0xA5)&&fill_is(0x2018,4,0xA5));
    CHECK(call(SYNC,0x1000,0,0x123456789abcdef0ULL)==NOT_AVAILABLE);
    CHECK(call(GET,0x1000,0)==INVALID_ARG);
    CHECK(call(GET,0,0x2000)==INVALID_ARG);
    CHECK(call(GET,0xFFFFFF80,0x2000)==INVALID_ARG);
    CHECK(call(GET,0x1000,0xFFF0)==INVALID_ARG);
    CHECK(call(GET,0x1000,0xFFFFFFF0)==INVALID_ARG);
    CHECK(call(INIT,0xFF80)==INVALID_ARG);
    CHECK(call(INIT,0)==INVALID_ARG);
    std::memset(vm_base+0x1FFC,0x5A,32);
    CHECK(call(FINAL,0x1000)==0);
    CHECK(call(GET,0x1000,0x2000)==INVALID_STATE);
    CHECK(fill_is(0x1FFC,32,0x5A));
    CHECK(call(FINAL,0x1000)==INVALID_STATE);
    CHECK(call(SYNC,0x1000,0,99)==INVALID_STATE);
    CHECK(call(INIT,0x1000)==0);
    CHECK(call(INIT,0x3000)==0);
    CHECK(call(GET,0x3000,0x2000)==0);
    CHECK(call(FINAL,0x1000)==0);
    CHECK(call(GET,0x3000,0x2000)==0);
    CHECK(call(0xF220E64F,0)==0); // cellSailTerm
    CHECK(call(GET,0x3000,0x2000)==INVALID_STATE);
    CHECK(call(INIT,0x3000)==0);
    CHECK(call(GET,0x3000,0xFFE8)==0); // last complete record inside the VM
    CHECK(fill_is(0xFFE8,24,0));
    std::printf("PASS: Sail graphics no-frame lifecycle, guest bounds/canaries, real HLE dispatch (%u checks)\n",checks);
}
