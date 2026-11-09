/*
 * ps3recomp - host read-modify-write sections on guest lock lines
 *
 * Host code (the SPURS, Sync and GCM HLE, the taskset bookkeeping) updates
 * guest lines that SPU code updates with GETLLAR/PUTLLC and PPU code with
 * stores and store-conditionals. Such an update reads the line, computes, and
 * writes it back, and must not interleave with any of those writers.
 *
 *   spu_coh_write_begin(ea, len);   ... read and write [ea, ea + len) ...
 *   spu_coh_notify_write(line);     for each line written, as before
 *   spu_coh_write_end();
 *
 * [ea, ea + len) names the lines the section may WRITE. It may read others,
 * but only one value at a time: they are not held.
 *
 * With the default coherence scheme this is exactly the lock-line lock, which
 * every one of those writers takes. With per-line version counters
 * (PS3RECOMP_COH_VERSIONS=1), SPU and PPU writers no longer take that lock, so
 * the section also owns each line of the range for its duration; a line it
 * notified is released as written, the others unchanged. Sections still hold
 * the lock-line lock, so they exclude each other as before.
 *
 * Sections do not nest, and a section must not store through vm_write* to a
 * line it holds (the default scheme would deadlock on the lock-line lock).
 *
 * The version scheme is reached through g_spu_coh_ops (spu_coh_ops.h), which is
 * NULL unless it is on, so targets that stub the lock-line lock and never link
 * spu_coherency.c keep the plain lock.
 */

#ifndef SPU_COH_SECTION_H
#define SPU_COH_SECTION_H

#include <stdint.h>

#include "spu_coh_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

void spu_lockline_lock(void);
void spu_lockline_unlock(void);

static inline void spu_coh_write_begin(uint32_t ea, uint32_t len)
{
    spu_lockline_lock();
    const spu_coh_ops* ops = g_spu_coh_ops;
    if (ops) ops->section_begin(ea, len);
}

static inline void spu_coh_write_end(void)
{
    const spu_coh_ops* ops = g_spu_coh_ops;
    if (ops) ops->section_end();
    spu_lockline_unlock();
}

#ifdef __cplusplus
}
#endif

#endif /* SPU_COH_SECTION_H */
