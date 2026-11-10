/*
 * ps3recomp - lock-line coherence by per-line version counters (opt-in)
 *
 * PS3RECOMP_COH_VERSIONS=1 selects this scheme instead of the lock-line lock,
 * holder walk and notify of spu_coherency.c. The ordering argument is at the
 * top of spu_coh_version.inc.
 *
 * Every 128-byte guest line has one 32-bit word:
 *
 *   bit 0       WRITING  a writer owns the line (taken with a compare-and-swap)
 *   bit 1       WAITERS  a host thread is parked until the line changes
 *   bits 2..31  count    advanced by every completed write to the line
 *
 * A reservation (an SPU GETLLAR or a PPU load-reserve) records the count it
 * read the line under. It is lost exactly when the count moves, so nobody has
 * to find the holders of a line and tell them: a holder compares when it next
 * looks (its store-conditional, or a read of its event status), and a holder
 * that is parked waiting for the change is the only one a writer wakes, through
 * the WAITERS bit.
 *
 * Only lines that something has reserved or watched are written this way. The
 * caller keeps that set (the interest bitmap in spu_coherency.c) and calls
 * spu_cohv_prepare before the first reservation of a line.
 */

#ifndef SPU_COH_VERSION_H
#define SPU_COH_VERSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_COHV_WRITING  1u
#define SPU_COHV_WAITERS  2u
#define SPU_COHV_STEP     4u
#define SPU_COHV_FLAGS    (SPU_COHV_WRITING | SPU_COHV_WAITERS)

/* PS3RECOMP_COH_VERSIONS=1, read once. The scheme is chosen for the whole
 * process before the first reservation; the two schemes never run together. */
int spu_cohv_enabled(void);
/* Tests only: choose the scheme without the environment. Call before any
 * reservation. */
void spu_cohv_force(int on);

/* The version word of ea's line. The table is reserved once, lazily committed
 * by the host, and laid out so that the words of neighbouring guest lines sit
 * in different host cache lines. */
uint32_t* spu_cohv_word(uint32_t ea);
/* Before the first reservation, watch or versioned write of ea's line. */
void spu_cohv_prepare(uint32_t ea);

/* The line's count, without the flag bits. Acquire: a caller that sees a count
 * also sees every write that produced it. */
static inline uint32_t spu_cohv_count(uint32_t ea)
{
    return __atomic_load_n(spu_cohv_word(ea), __ATOMIC_ACQUIRE) & ~SPU_COHV_FLAGS;
}
/* Has the count moved away from a reservation's? A write still in progress is
 * not a loss yet; it will be one when it completes. */
static inline int spu_cohv_moved(uint32_t ea, uint32_t count)
{
    return spu_cohv_count(ea) != count;
}

/* Lazy lost-reservation delivery. A holder calls this wherever it could observe
 * the loss: reading its event status, waiting on it, and before it ends or
 * replaces the reservation itself. If the reservation is valid and its count
 * moved, it is ended and SPU_EVENT_LR (0x400) is raised with an atomic OR, as
 * every other event producer does. Returns 1 when it raised LR. */
static inline int spu_cohv_settle(int* valid, uint32_t ea, uint32_t count,
                                  uint32_t* event_status)
{
    if (!__atomic_load_n(valid, __ATOMIC_RELAXED) || !spu_cohv_moved(ea, count)) return 0;
    __atomic_store_n(valid, 0, __ATOMIC_RELAXED);
    __atomic_fetch_or(event_status, 0x400u, __ATOMIC_SEQ_CST);
    return 1;
}

/* Copy ea's 128-byte line from `line` (host address of the line) to `dst` as
 * one consistent snapshot, and return the count it is consistent with. Waits
 * out a writer in progress; never blocks one. */
uint32_t spu_cohv_read_line(uint32_t ea, const void* line, void* dst);
/* The same for one naturally aligned 4- or 8-byte value (PPU load-reserve).
 * Returns the count; *value receives the raw (guest byte order) bits. */
uint32_t spu_cohv_read_value(uint32_t ea, const void* p, unsigned size, uint64_t* value);

/* Own the line for a write. Waits for a writer in progress. Returns the count
 * the line had. */
uint32_t spu_cohv_lock(uint32_t ea);
/* Own the line only if its count is still `count` (a store-conditional).
 * Returns 0, without waiting, when the count moved or another writer owns it:
 * either way the reservation is lost or about to be. */
int spu_cohv_lock_if(uint32_t ea, uint32_t count);
/* Release after writing: advance the count by one step, and wake the line's
 * waiters if any. Returns the new count. */
uint32_t spu_cohv_unlock_written(uint32_t ea);
/* Release without having written (a store-conditional that failed its data
 * compare): the count stays. */
void spu_cohv_unlock_unchanged(uint32_t ea);

/* Copies within the protocol. Guest memory is shared with readers that take
 * snapshots without a lock, so these copy in naturally aligned 8-byte relaxed
 * atomic accesses, which compile to plain loads and stores. */
void spu_cohv_copy_line(void* dst, const void* src);
int  spu_cohv_line_differs(const void* a, const void* b);
void spu_cohv_store_bytes(void* dst, const void* src, unsigned n);

/* The common operations, in terms of the primitives above. `line` is the host
 * address of ea's 128-byte line. */
/* PUTLLC: commit `data` if the count is still `count` and the line still holds
 * `snapshot`. 1 on commit. */
int spu_cohv_putllc(uint32_t ea, void* line, uint32_t count,
                    const void* snapshot, const void* data);
/* PUTLLUC / PUTQLLUC: unconditional line write. Returns the new count. */
uint32_t spu_cohv_putlluc(uint32_t ea, void* line, const void* data);
/* A plain store of n bytes at host address p inside ea's line (n <= 128, the
 * store does not cross the line). Returns the new count. */
uint32_t spu_cohv_store(uint32_t ea, void* p, const void* src, unsigned n);
/* A PPU store-conditional of a naturally aligned 4- or 8-byte value: store
 * `value` (raw guest byte order) at p if the count is still `count` and p still
 * holds `expected`. 1 on store. */
int spu_cohv_store_conditional(uint32_t ea, void* p, unsigned size, uint32_t count,
                               uint64_t expected, uint64_t value);

/* Waiting for a line to change.
 *
 * A waiter is registered on one line. Registration sets the line's WAITERS
 * bit and returns the count at that instant, read by the same atomic
 * operation. A writer that completes after that sees the bit and calls every
 * registered wake for the line; a writer that completed before it shows up in
 * the returned count. So a caller that compares the returned count, and parks
 * only while it is unchanged, cannot miss a write.
 *
 * wake runs on the writer's thread with the line's wait bucket locked. It
 * must not re-enter this module, and must not take a lock the waiter holds
 * while registering or unregistering. The usual shape: the waiter registers,
 * then takes its own mutex to check and sleep; wake takes that mutex,
 * signals, and releases it. */
typedef struct spu_cohv_waiter {
    uint32_t line;
    void (*wake)(void* opaque);
    void* opaque;
    struct spu_cohv_waiter* next;
} spu_cohv_waiter;
uint32_t spu_cohv_wait_register(spu_cohv_waiter* w, uint32_t ea,
                                void (*wake)(void*), void* opaque);
void spu_cohv_wait_unregister(spu_cohv_waiter* w);

/* Writers that found WAITERS set (relaxed; touched only on that path). */
extern unsigned long long g_spu_cohv_wakes;

#ifdef __cplusplus
}
#endif

#endif /* SPU_COH_VERSION_H */
