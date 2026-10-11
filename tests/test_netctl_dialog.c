/* Exercise the real HLE using guest addresses and big-endian result storage. */
#include "../libs/network/cellNetCtl.h"
#include "../libs/system/cellSysutil.h"
#include "../runtime/ppu/ppu_memory.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char memory[4096];
uint8_t* vm_base=memory;
static unsigned events;
void cellSysutilQueueEvent(int slot, u32 status, u32 param) {
    assert(slot>=0 && slot<CELL_SYSUTIL_MAX_CALLBACKS);
    assert(status==CELL_SYSUTIL_NET_CTL_NETSTART_UNLOADED && param==0);
    ++events;
}
const char* np_psnr_identity(void) { return NULL; }
int main(int argc, char** argv) {
    (void)argv;
    const uint32_t ea=0x100;
    CellNetCtlNetStartDialogResult* result=(void*)(uintptr_t)ea;
    assert(cellNetCtlInit()==CELL_OK);
    vm_write32(ea,8);vm_write32(ea+4,0xdeadbeef);vm_write32(ea+8,0x12345678);
    assert(cellNetCtlNetStartDialogUnloadAsync(result)==CELL_OK);
    assert(vm_read32(ea+4)==(argc>1?0:CELL_NET_CTL_ERROR_DIALOG_CANCELED));
    assert(vm_read32(ea)==8 && vm_read32(ea+8)==0x12345678);
    assert(events==CELL_SYSUTIL_MAX_CALLBACKS);
    puts("PASS: network dialog result matches configured connection state");
    return 0;
}
/* This single-threaded fixture has no reservations or write-watch hooks. */
int g_resv_store_active; uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
int spu_coh_is_reserved(uint32_t ea) {(void)ea;return 0;}
void spu_lockline_lock(void) {} void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w) {(void)a;(void)v;(void)w;}
