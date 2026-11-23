/* SPURS policy-run observer: the hook table the opt-in policy-run recorder
 * (spurs_pm_record.inc, PS3RECOMP_SPURS_PM_RECORD / PS3RECOMP_SPURS_PM_STATS)
 * installs in ctx->pm_observer for the duration of one policy-module run.
 *
 * The pointer is NULL on every context unless one of those variables is set,
 * so each hook site costs one load and one untaken branch of a field of the
 * context it already works on. The hooks observe only: they never change a
 * transfer, a register, the reservation state or a scheduling decision. */
#ifndef SPURS_PM_OBSERVER_H
#define SPURS_PM_OBSERVER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct spu_context;

enum spurs_pm_xfer_kind {
    SPURS_PM_XFER_GET = 1,    /* bytes as they landed in local store       */
    SPURS_PM_XFER_PUT = 2,    /* bytes as they were sent to guest memory   */
    SPURS_PM_XFER_ZERO = 3    /* storage-control zeroing of a guest range  */
};
enum spurs_pm_kernel_kind {
    SPURS_PM_KERNEL_SELECT = 1,   /* select-workload answered; answer in r3 */
    SPURS_PM_KERNEL_EXIT = 2      /* exit-to-kernel taken                   */
};

struct spurs_pm_observer {
    /* One element of guest data movement (a list command reports each element). */
    void (*transfer)(struct spu_context*, uint32_t kind, uint32_t cmd, uint32_t lsa,
                     uint64_t ea, uint32_t size, const uint8_t* bytes);
    /* An MFC command as it reaches the DMA engine (atomics excluded). */
    void (*command)(struct spu_context*, uint32_t cmd, uint32_t lsa, uint64_t ea,
                    uint32_t size, uint32_t tag);
    /* A lock-line operation: GETLLAR with the line received, PUTLLC/PUTLLUC
     * with the line the program attempted to store and the atomic status. */
    void (*atomic)(struct spu_context*, uint32_t cmd, uint32_t lsa, uint32_t ea,
                   const uint8_t* line, uint32_t status);
    /* An indirect branch in policy mode, before it is resolved. Returns
     * nonzero when the observer has already performed the dispatch (and the
     * matching return) itself; the resolver then returns at once. */
    int (*branch)(struct spu_context*);
    /* A SPURS kernel service the policy module called. */
    void (*kernel)(struct spu_context*, uint32_t kind);
};

#ifdef __cplusplus
}
#endif
#endif
