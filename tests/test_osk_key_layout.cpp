/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <initializer_list>
#include <thread>
#include "../libs/misc/cellOskDialog.h"

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


extern "C" unsigned fixture_osk_layout(void);
int main() {
    static uint8_t memory[0x10000]; vm_base=memory;
    memset(memory,0xA5,sizeof(memory));
    ppu_hle_register_all();
    if(!ps3_hle_has(0xB53C54FA)) {
        fprintf(stderr,"FAIL: original OSK key-layout NID B53C54FA is missing\n");return 1;
    }
    auto call=[](uint64_t mask) {
        ppu_context ctx{};ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;ctx.gpr[3]=mask;
        ctx.gpr[4]=~uint64_t(0);ctx.gpr[5]=0xFEF4B70;
        ps3_hle_call(0xB53C54FA,&ctx);
        if(ctx.gpr[2]!=0x12345678 || ctx.gpr[4]!=~uint64_t(0)) std::abort();
        return uint32_t(ctx.gpr[3]);
    };
    if(fixture_osk_layout()!=1) std::abort();
    for(uint32_t mask : {1u,2u,3u}) {
        if(call(mask)!=0 || fixture_osk_layout()!=mask) std::abort();
    }
    for(uint32_t mask : {0u,4u,0x80000000u,0xFFFFFFFFu})
        if(call(mask)!=0x8002B504u || fixture_osk_layout()!=3) std::abort();
    /* Single u32 ABI: only the low half of the register supplies the option. */
    if(call(0xFFFFFFFF00000003ull)!=0) std::abort();
    /* HLE's normal TOC guard owns the caller's stack slot +0x28. */
    if(vm_read64(0x8028)!=0x12345678) std::abort();
    for(size_t i=0;i<sizeof(memory);i++)
        if((i<0x8028 || i>=0x8030) && memory[i]!=0xA5) std::abort();
    puts("OSK key-layout option generated NID / real HLE ABI checks passed");
    return 0;
}
