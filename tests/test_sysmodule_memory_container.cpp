/* Real container lifecycle plus generated NID registration/PPU HLE dispatch. */
#include "ppu_context.h"
#include "sys_memory.h"
#include "cellSysmodule.h"
#include "ps3emu/guest_call.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <thread>
#include <atomic>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size=0x10000;
uint32_t ppu_hle_inject_base=0x20000000;
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
static uint32_t create_container(uint32_t size) {
    ppu_context ctx{};ctx.gpr[3]=0x100;ctx.gpr[4]=size;
    if(sys_memory_container_create(&ctx)!=0)std::abort();
    return vm_read32(0x100);
}
static void destroy_container(uint32_t id) {
    ppu_context ctx{};ctx.gpr[3]=id;
    if(sys_memory_container_destroy(&ctx)!=0)std::abort();
}
int main() {
    static uint8_t memory[0x10000];vm_base=memory;
    ppu_hle_register_all();
    constexpr uint32_t SET=0xA193143C,INVALID=0x80012004,DISABLED=0xFFFFFFFF;
    CHECK(ps3_hle_has(SET));
    CHECK(ps3_sysmodule_memcontainer()==DISABLED);
    CHECK(call(SET,0)==INVALID);
    CHECK(call(SET,0xFFFFFFFE)==INVALID);
    CHECK(call(SET,DISABLED)==0);
    const uint32_t small=create_container(0x5FFFF);
    const uint32_t exact=create_container(0x60000);
    CHECK(call(SET,small)==INVALID);
    CHECK(ps3_sysmodule_memcontainer()==DISABLED);
    CHECK(call(SET,exact)==0);
    CHECK(ps3_sysmodule_memcontainer()==exact);
    CHECK(g_sys_mem_containers[exact-1].used_size==0);
    CHECK(cellSysmoduleLoadModule(CELL_SYSMODULE_PAMF)==0);
    CHECK(g_sys_mem_containers[exact-1].used_size==0); // HLE modules need no PRX allocation
    CHECK(ps3_sysmodule_memcontainer()==exact);
    CHECK(cellSysmoduleUnloadModule(CELL_SYSMODULE_PAMF)==0);
    CHECK(cellSysmoduleInitialize()==0);
    CHECK(cellSysmoduleFinalize()==0);
    CHECK(ps3_sysmodule_memcontainer()==exact); // official initialization/finalization are no-ops
    uint32_t available=0xDEADBEEF;
    CHECK(!sys_memory_container_available(0,&available));
    CHECK(available==0xDEADBEEF);
    CHECK(!sys_memory_container_available(exact,nullptr));
    CHECK(sys_memory_container_available(exact,&available)&&available==0x60000);
    g_sys_mem_containers[exact-1].used_size=1;
    CHECK(call(SET,exact)==INVALID); // available bytes, not total bytes
    CHECK(ps3_sysmodule_memcontainer()==DISABLED);
    g_sys_mem_containers[exact-1].used_size=0x60001;
    CHECK(call(SET,exact)==INVALID); // no unsigned-underflow admission
    g_sys_mem_containers[exact-1].used_size=0;
    CHECK(call(SET,exact)==0);
    destroy_container(exact);
    CHECK(ps3_sysmodule_memcontainer()==exact); // retained request, not a lifetime pin
    CHECK(call(SET,exact)==INVALID);
    CHECK(ps3_sysmodule_memcontainer()==DISABLED);
    CHECK(call(SET,DISABLED)==0);
    const uint32_t reused=create_container(0x70000);
    CHECK(call(SET,reused)==0);
    CHECK(call(SET,SYS_MEMORY_CONTAINER_MAX+1)==INVALID);
    CHECK(ps3_sysmodule_memcontainer()==DISABLED);
    CHECK(call(SET,reused)==0);
    CHECK(call(SET,DISABLED)==0);
    CHECK(ps3_sysmodule_memcontainer()==DISABLED);
    CHECK(g_sys_mem_containers[reused-1].active);
    CHECK(g_sys_mem_containers[reused-1].total_size==0x70000);
    CHECK(g_sys_mem_containers[reused-1].used_size==0);
    // Concurrent real create/destroy and availability snapshots must never
    // observe torn container state. Selection itself does not reserve memory.
    std::atomic<unsigned> running{2};std::atomic<bool> bad{false};
    auto writer=[&](unsigned index) {
        for(unsigned i=0;i<2000;++i) {
            ppu_context ctx{};ctx.gpr[3]=0x200+index*4;
            ctx.gpr[4]=index ? 0xB0000 : 0x90000;
            if(sys_memory_container_create(&ctx)!=0) { bad=true;break; }
            uint32_t id=vm_read32(ctx.gpr[3]);ctx.gpr[3]=id;
            uint32_t size=0;
            if(!sys_memory_container_available(id,&size)||size!=(index ? 0xB0000u : 0x90000u))bad=true;
            if(sys_memory_container_destroy(&ctx)!=0)bad=true;
        }
        --running;
    };
    std::thread a(writer,0),b(writer,1);
    do {
        for(uint32_t id=1;id<=SYS_MEMORY_CONTAINER_MAX;++id) {
            uint32_t size=0;
            if(sys_memory_container_available(id,&size) &&
               size!=0x5FFFF&&size!=0x70000&&size!=0x90000&&size!=0xB0000)bad=true;
        }
    } while(running.load());
    a.join();b.join();CHECK(!bad.load());
    std::printf("PASS: Sysmodule container selection, real lifecycle/availability, no allocation, real HLE dispatch (%u checks)\n",checks);
}
