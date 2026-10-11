/* Guest-backed SPURS1 receiver flag transactions. No host shadow flag. */
#ifndef SPURS_WORKLOAD_FLAG_H
#define SPURS_WORKLOAD_FLAG_H
#include "spu_coherency.h"
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
void ppu_resv_break_store(uint64_t ea);
#ifdef __cplusplus
}
#endif
static inline int spurs_flag_range(uint64_t ea)
{
    uint64_t limit = ppu_vm_size ? ppu_vm_size : UINT64_C(0x100000000);
    return vm_base && ea && !(ea & 127) && ea <= UINT32_MAX && ea + 0xD70 <= limit;
}
static inline uint32_t spurs_flag_be32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
/* Caller holds the lock-line lock. Reset/ownership changes invalidate both
 * SPU reservations and PPU reservations covering the modified words. */
static inline void spurs_flag_notify(uint32_t ea, int receiver_changed)
{
    spu_coh_notify_write(ea);
    ppu_resv_break_store(ea + 0x6C);
    if (receiver_changed) ppu_resv_break_store(ea + 0x74);
}
/* Observe a real pending zero without consuming it (idle-backoff decision),
 * or deliver it to the selected receiver and reset the scalar atomically.
 * Only SPURS1 runnable workloads are served by the native policy scheduler. */
static inline uint32_t spurs_flag_status(uint32_t ea, uint32_t wid, int consume)
{
    if (wid >= 16 || !spurs_flag_range(ea)) return 0;
    uint32_t status = 0;
    spu_lockline_lock();
    if (!(vm_base[ea + 0x74] & 0x40) &&
        (spurs_flag_be32(ea + 0xB0) & (0x80000000u >> wid)) &&
        vm_base[ea + 0x80 + wid] == 2 && !spurs_flag_be32(ea + 0xD6C) &&
        vm_base[ea + 0x77] == wid) {
        status = spurs_flag_be32(ea + 0x6C) == 0 ? 4u : 0u;
        if (consume) {
            memset(vm_base + ea + 0x6C, 0xFF, 4);
            spurs_flag_notify(ea, 0);
        }
    }
    spu_lockline_unlock();
    return status;
}
#endif
