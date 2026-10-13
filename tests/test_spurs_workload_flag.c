/* Use the real initializer and guest-memory accessors. Only thread creation is
 * suppressed: the layout/getter checks must not race a kernel poll thread. */
#include "../runtime/platform/win32_compat.h"
/* Layout/poll fixture: acknowledge thread creation without starting host
 * workers. The separate scheduler fixture verifies actual creation and joins. */
#define CreateThread(sa, stack, fn, arg, flags, id) ((HANDLE)1)
#define WaitForSingleObject(h, ms) (WAIT_OBJECT_0)
#define CloseHandle(h) (TRUE)
#include "../libs/spurs/cellSpurs.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }

int main(void)
{
    vm_base = malloc(ppu_vm_size);
    assert(vm_base);
    memset(vm_base, 0xA5, ppu_vm_size);
    assert(cellSpursInitialize((CellSpurs*)0x1000, 3, 0, 0, 0) == 0);
    /* The 16-byte object starts at +0x60; its atomic BE32 flag is +0x6C. */
    assert(vm_read32(0x106C) == 0xFFFFFFFFu);
    assert(vm_base[0x1077] == 0xFF); /* no workload selected as receiver */
    for (unsigned i = 0; i < 12; ++i) assert(vm_base[0x1060 + i] == 0);
    assert(vm_base[0x1076] == 3);
    assert(vm_read16(0x1070) == 0);
    assert(vm_base[0x0FFF] == 0xA5 && vm_base[0x2000] == 0xA5);
    /* Return the object, not its scalar at +0x0C or a host-side copy. */
    unsigned char saved[16];
    memset(vm_base + 0x1060, 0x37, 16);
    memcpy(saved, vm_base + 0x1060, 16);
    assert(cellSpursGetWorkloadFlag((CellSpurs*)0x1000,
                                  (CellSpursWorkloadFlag**)0x8000) == 0);
    assert(vm_read32(0x8000) == 0x1060);
    assert(vm_base[0x8000] == 0 && vm_base[0x8001] == 0 &&
           vm_base[0x8002] == 0x10 && vm_base[0x8003] == 0x60);
    assert(vm_base[0x7FFF] == 0xA5 && vm_base[0x8004] == 0xA5);
    assert(memcmp(saved, vm_base + 0x1060, 16) == 0);
    vm_write32(vm_read32(0x8000) + 12, 0x12345678);
    assert(vm_read32(0x106C) == 0x12345678);
    /* Like firmware, the getter does not require an HLE registry entry, and
     * does not reset a caller's flag or enforce output pointer alignment. */
    assert(cellSpursGetWorkloadFlag((CellSpurs*)0x3000,
                                  (CellSpursWorkloadFlag**)0x8001) == 0);
    assert(vm_read32(0x8001) == 0x3060 && vm_base[0x8005] == 0xA5);
    assert(vm_read32(0x306C) == 0xA5A5A5A5);

    vm_write32(0x8000, 0xDEADBEEF);
    assert((u32)cellSpursGetWorkloadFlag(NULL, (CellSpursWorkloadFlag**)0x8000) == 0x80410811);
    assert((u32)cellSpursGetWorkloadFlag((CellSpurs*)0x1000, NULL) == 0x80410811);
    assert((u32)cellSpursGetWorkloadFlag((CellSpurs*)0x1001, NULL) == 0x80410811);
    assert((u32)cellSpursGetWorkloadFlag((CellSpurs*)0x1001,
                                      (CellSpursWorkloadFlag**)0x8000) == 0x80410810);
    assert((u32)cellSpursGetWorkloadFlag((CellSpurs*)0x10000,
                                      (CellSpursWorkloadFlag**)0x8000) == 0x8041080D);
    assert((u32)cellSpursGetWorkloadFlag((CellSpurs*)0x1000,
                                      (CellSpursWorkloadFlag**)0xFFFF) == 0x8041080D);
    assert(vm_read32(0x8000) == 0xDEADBEEF);
    assert(vm_base[0xFFFF] == 0xA5);
    if (sizeof(uintptr_t) > 4) {
        assert((u32)cellSpursGetWorkloadFlag((CellSpurs*)(uintptr_t)0x100001000ull,
                                          (CellSpursWorkloadFlag**)0x8000) == 0x8041080D);
        assert((u32)cellSpursGetWorkloadFlag((CellSpurs*)0x1000,
                     (CellSpursWorkloadFlag**)(uintptr_t)0x100008000ull) == 0x8041080D);
        assert(vm_read32(0x8000) == 0xDEADBEEF);
    }
    /* Bound only the returned 16-byte object, not unrelated instance fields. */
    ppu_vm_size = 0x1070;
    assert(cellSpursGetWorkloadFlag((CellSpurs*)0x1000,
                                  (CellSpursWorkloadFlag**)0x800) == 0);
    ppu_vm_size = 0x106F;
    vm_write32(0x800, 0xDEADBEEF);
    assert((u32)cellSpursGetWorkloadFlag((CellSpurs*)0x1000,
                                      (CellSpursWorkloadFlag**)0x800) == 0x8041080D);
    assert(vm_read32(0x800) == 0xDEADBEEF);
    ppu_vm_size = 0x10000;
    assert(cellSpursGetWorkloadFlag((CellSpurs*)0x1000,
                                  (CellSpursWorkloadFlag**)0xFFFC) == 0);
    assert(vm_read32(0xFFFC) == 0x1060);
    ppu_vm_size = 0; /* runtime convention: fully backed 32-bit address space */
    assert(cellSpursGetWorkloadFlag((CellSpurs*)0x1000,
                                  (CellSpursWorkloadFlag**)0x8000) == 0);
    assert((u32)cellSpursGetWorkloadFlag((CellSpurs*)0x1000,
                      (CellSpursWorkloadFlag**)(uintptr_t)0xFFFFFFFEu) == 0x8041080D);
    uint8_t* saved_base = vm_base;
    vm_base = NULL;
    assert((u32)cellSpursGetWorkloadFlag((CellSpurs*)0x1000,
                                      (CellSpursWorkloadFlag**)0x8000) == 0x8041080D);
    vm_base = saved_base;
    free(vm_base);
    puts("SPURS workload flag guest ABI/layout/bounds PASS");
}
