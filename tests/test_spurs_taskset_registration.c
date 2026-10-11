/* Exercise the actual HLE entry points against guest bytes; no game assets. */
#include "../libs/spurs/cellSpurs.c"
#include <assert.h>
uint8_t* vm_base;
int main(void) {
    vm_base=calloc(1,0x100000); assert(vm_base);
    s_inst[0].ea=0x1000; s_inst[1].ea=0x3000;
    assert(cellSpursCreateTaskset((CellSpurs*)0x1000,(CellSpursTaskset*)0x10000,42,NULL,1)==0);
    unsigned a=vm_read32(0x10000+CSTS_WID);
    assert(vm_read32(0x1000+SPURS_WKL_ENABLED)&(0x80000000u>>a));
    assert(vm_read64(0x1000+SPURS_WKL_INFO1+32*a+8)==0x10000);
    assert(vm_read64(0x10000+CSTS_ARGS)==42);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(CellSpursWorkloadId*)0x8000,(void*)0x9000,128,55,NULL,1,1)==0);
    unsigned b=vm_read32(0x8000); assert(a!=b);
    assert(cellSpursCreateTaskset((CellSpurs*)0x3000,(CellSpursTaskset*)0x20000,43,NULL,1)==0);
    unsigned c=vm_read32(0x20000+CSTS_WID); assert(c!=a && c!=b);
    assert(vm_read64(0x3000+SPURS_WKL_INFO1+32*c+8)==0x20000);
    assert(cellSpursDestroyTaskset((CellSpursTaskset*)0x10000)==0);
    assert(!(vm_read32(0x1000+SPURS_WKL_ENABLED)&(0x80000000u>>a)));
    assert(vm_read64(0x1000+SPURS_WKL_INFO1+32*a+8)==0);
    assert(vm_read64(0x3000+SPURS_WKL_INFO1+32*c+8)==0x20000);
    for(unsigned i=0;i<CELL_SPURS_MAX_WORKLOAD;i++) s_workloads[i].in_use=1;
    memset(vm_base+0x40000,0xA5,sizeof(CellSpursTaskset));
    assert(cellSpursCreateTaskset((CellSpurs*)0x1000,(CellSpursTaskset*)0x40000,0,NULL,1)!=0);
    assert(vm_base[0x40000]==0xA5);
    puts("SPURS taskset workload registration PASS"); free(vm_base);
}
/* This single-threaded fixture has no reservations or write-watch hooks. */
int g_resv_store_active; uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
int spu_coh_is_reserved(uint32_t ea) {(void)ea;return 0;}
void spu_lockline_lock(void) {} void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w) {(void)a;(void)v;(void)w;}
