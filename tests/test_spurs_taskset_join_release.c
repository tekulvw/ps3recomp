/* Shutdown + Join of a legacy taskset gives its workload slot back
 * (docs/SPURS_JOIN_TASKSET_RELEASE.md). Synthetic guest bytes only. */
#include "../libs/spurs/cellSpurs.c"
#include <assert.h>
#include <time.h>
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x200000;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi, g_barrier_sync_watch;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w) {(void)a;(void)v;(void)w;}
void ydkj_wake_all_event_flags(void) { abort(); }
unsigned long long ps3_ms_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (unsigned long long)t.tv_sec*1000+t.tv_nsec/1000000;
}

#define SPURS 0x1000u
#define ATTR  0x8000u
#define WID_OF(ts) vm_read32((ts) + CSTS_WID)

static int slot_live(u32 wid) {
    return s_workloads[wid].in_use &&
           (vm_read32(SPURS + SPURS_WKL_ENABLED) & (0x80000000u >> wid));
}
static unsigned live_slots(void) {
    unsigned n = 0;
    for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) n += s_workloads[i].in_use != 0;
    return n;
}
static u32 add_job_workload(u32 data) {
    assert(cellSpursAddWorkload((CellSpurs*)(uintptr_t)SPURS, (CellSpursWorkloadId*)0x8400,
                                (void*)0x9000, 128, data, NULL, 1, 1) == 0);
    return vm_read32(0x8400);
}
/* The two legacy creation entry points a title uses. */
static s32 create_legacy(u32 ts, int with_attribute) {
    if (!with_attribute)
        return cellSpursCreateTaskset((CellSpurs*)(uintptr_t)SPURS, (CellSpursTaskset*)(uintptr_t)ts,
                                      0, NULL, 8);
    assert(cellSpursTasksetAttributeInitialize((CellSpursTasksetAttribute*)(uintptr_t)ATTR) == 0);
    ((CellSpursTasksetAttribute*)(vm_base + ATTR))->tasksetSize = 0x2900;
    return cellSpursCreateTasksetWithAttribute((CellSpurs*)(uintptr_t)SPURS,
        (CellSpursTaskset*)(uintptr_t)ts, (const CellSpursTasksetAttribute*)(uintptr_t)ATTR);
}
static void retire(u32 ts) {
    assert(cellSpursShutdownTaskset((CellSpursTaskset*)(uintptr_t)ts) == 0);
    assert(cellSpursJoinTaskset((CellSpursTaskset*)(uintptr_t)ts) == 0);
}

static void many_cycles(void) {
    /* Twelve slots stay taken all session, as in MAG. */
    for (u32 i = 0; i < 12; i++) add_job_workload(0x20000 + i * 0x100);
    assert(live_slots() == 12);
    static const u32 addr[4] = {0x40000, 0x48000, 0x40000, 0x50000};
    for (int cycle = 0; cycle < 40; cycle++) {
        u32 ts = addr[cycle % 4] + (cycle >= 20 ? 0x80000u : 0);
        s32 rc = create_legacy(ts, cycle & 1);
        if (rc) fprintf(stderr, "cycle %d: create 0x%08X rc=0x%08X\n", cycle, ts, (u32)rc);
        assert(rc == 0);
        u32 wid = WID_OF(ts);
        assert(wid < CELL_SPURS_MAX_WORKLOAD && slot_live(wid));
        assert(vm_read64(SPURS + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ + 8) == ts);
        assert(live_slots() == 13);
        retire(ts);
        assert(!s_workloads[wid].in_use);
        assert(!(vm_read32(SPURS + SPURS_WKL_ENABLED) & (0x80000000u >> wid)));
        assert(vm_base[SPURS + SPURS_WKL_STATE1 + wid] == 0);
        assert(vm_read64(SPURS + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ + 8) == 0);
        assert(WID_OF(ts) == 32);
        assert(live_slots() == 12);
    }
}

static void dead_taskset_cannot_touch_new_owner(void) {
    u32 ts = 0x60000;
    assert(create_legacy(ts, 1) == 0);
    u32 wid = WID_OF(ts);
    retire(ts);
    /* The released slot goes to the next registration. */
    u32 other = add_job_workload(0x7777);
    assert(other == wid && slot_live(other));
    /* Neither a stray Destroy nor a second Join of the dead taskset may free it. */
    assert(cellSpursDestroyTaskset((CellSpursTaskset*)(uintptr_t)ts) == 0);
    assert(cellSpursJoinTaskset((CellSpursTaskset*)(uintptr_t)ts) == 0);
    assert(slot_live(other));
    assert(vm_read64(SPURS + SPURS_WKL_INFO1 + other * SPURS_WKL_INFO_SZ + 8) == 0x7777);
    assert(cellSpursRemoveWorkload((CellSpurs*)(uintptr_t)SPURS, other) == 0);
}

static void busy_join_keeps_slot(void) {
    u32 ts = 0x68000;
    assert(create_legacy(ts, 0) == 0);
    u32 wid = WID_OF(ts);
    /* A task that has not exited keeps its enabled bit. */
    spurs_bitset_set(ts + CSTS_ENABLED, 3);
    s_taskset_join_timeout_ms = 20;
    assert((u32)cellSpursJoinTaskset((CellSpursTaskset*)(uintptr_t)ts) == 0x8041090Au);
    s_taskset_join_timeout_ms = 10000;
    assert(slot_live(wid) && WID_OF(ts) == wid);
    spurs_bitset_clear(ts + CSTS_ENABLED, 3);
    retire(ts);
    assert(!s_workloads[wid].in_use);
}

static void taskset2_still_destroyed_by_destroy2(void) {
    u32 ts = 0x70000;
    assert(cellSpursCreateTaskset2((CellSpurs*)(uintptr_t)SPURS, (CellSpursTaskset2*)(uintptr_t)ts, NULL) == 0);
    u32 wid = WID_OF(ts);
    retire(ts);
    assert(slot_live(wid) && WID_OF(ts) == wid);
    assert(cellSpursDestroyTaskset2((CellSpursTaskset2*)(uintptr_t)ts) == 0);
    assert(!s_workloads[wid].in_use);
}

static void full_table_still_refuses(void) {
    u32 held[CELL_SPURS_MAX_WORKLOAD]; unsigned n = 0;
    while (live_slots() < CELL_SPURS_MAX_WORKLOAD) held[n++] = add_job_workload(0x30000 + n);
    memset(vm_base + 0x78000, 0xA5, 0x1900);
    assert((u32)create_legacy(0x78000, 0) == (u32)CELL_SPURS_CORE_ERROR_NOMEM);
    assert(vm_base[0x78000] == 0xA5);
    for (unsigned i = 0; i < n; i++)
        assert(cellSpursRemoveWorkload((CellSpurs*)(uintptr_t)SPURS, held[i]) == 0);
}

int main(void) {
    vm_base = calloc(1, ppu_vm_size); assert(vm_base);
    s_inst[0].ea = SPURS; s_inst[0].kernel_live = 1;
    many_cycles();
    dead_taskset_cannot_touch_new_owner();
    busy_join_keeps_slot();
    taskset2_still_destroyed_by_destroy2();
    full_table_still_refuses();
    puts("PASS: legacy taskset Join releases its workload slot");
    return 0;
}
