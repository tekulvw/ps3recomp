/*
 * ps3recomp - lock-line coherence by per-line version counters (opt-in)
 *
 * Compiled as part of spu_coherency.c, which includes this file.
 *
 * See spu_coh_version.h for the interface. This file holds the version table,
 * the line ownership protocol, the lock-free line snapshot and the wait
 * buckets. It knows nothing about SPU or PPU contexts.
 *
 * Ordering, in the C11 model (and why it holds on arm64 as well as x86):
 *
 * Writer. A writer takes the line with a compare-and-swap that sets WRITING
 * (acquire, so it sees the previous writer's bytes), then issues a release
 * fence before it stores any data, so the WRITING mark is ordered before the
 * data. It releases with a fetch-add (release) that clears WRITING and
 * advances the count in one step, so the data is ordered before the new count.
 *
 * Snapshot reader. It loads the word (acquire), copies the bytes, issues an
 * acquire fence, and loads the word again. If it read any byte a writer stored
 * after that writer's release fence, the fences synchronize and the second
 * load sees the writer's WRITING mark or a later value, so the reader retries.
 * If the first load saw a completed count, every byte of that write is
 * visible. A snapshot that returns therefore holds exactly the line as of the
 * returned count. On x86 (TSO) both fences are compiler barriers; on arm64 the
 * release fence is a dmb ish and the acquire fence a dmb ishld. The data is
 * copied with relaxed 8-byte atomics, which are plain loads and stores on
 * both, so that the model (and ThreadSanitizer) sees no data race.
 *
 * Reservation check. A holder compares the count it reserved under with an
 * acquire load of the word. Anything the writer did before its release is
 * visible to a holder that sees the new count, so a holder that learns of a
 * write through any other release/acquire chain (a mailbox, an event flag)
 * sees the count moved when it next checks. That is the guest-visible
 * guarantee lazy delivery has to keep: a lost reservation is observed no later
 * than anything the writer published after the write.
 *
 * Waiters. Both the waiter's fetch-or of WAITERS and the writer's releasing
 * fetch-add are read-modify-writes of the same word, so they are totally
 * ordered: either the writer's comes later and returns WAITERS set, or the
 * waiter's comes later and returns the advanced count. The waiter links itself
 * into its bucket before the fetch-or, under the bucket lock, and the writer
 * takes that lock before walking the bucket, so a writer that sees the bit
 * also sees the waiter.
 */

#include "spu_coh_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sched.h>
#include <sys/mman.h>
#endif

unsigned long long g_spu_cohv_wakes;

/* Measurement hook (PS3RECOMP_COH_STATS in spu_coherency.c); a no-op standalone. */
#ifndef COHV_STAT_HOOK
#define COHV_STAT_HOOK(which) ((void)0)
#endif
#define COHV_STAT_LOCK_WAIT     0   /* a writer found the line owned */
#define COHV_STAT_READ_FALLBACK 1   /* a snapshot gave up retrying and took the line */

/* ---------------------------------------------------------------------------
 * The switch
 * -------------------------------------------------------------------------*/
static int s_cohv_on = -1;

int spu_cohv_enabled(void)
{
    int on = __atomic_load_n(&s_cohv_on, __ATOMIC_RELAXED);
    if (__builtin_expect(on < 0, 0)) {
        const char* e = getenv("PS3RECOMP_COH_VERSIONS");
        on = e && !strcmp(e, "1");
        int expected = -1;
        if (!__atomic_compare_exchange_n(&s_cohv_on, &expected, on, 0,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return expected;
        if (on) { fprintf(stderr, "[spu-coh] per-line version counters on\n"); fflush(stderr); }
    }
    return on;
}

void spu_cohv_force(int on) { __atomic_store_n(&s_cohv_on, on ? 1 : 0, __ATOMIC_RELAXED); }

/* ---------------------------------------------------------------------------
 * The table
 *
 * One 32-bit word per 128-byte line of the 32-bit guest address space: 2^25
 * words, 128 MiB of address space. It is reserved once and committed by the
 * host on first touch, and only lines something reserved or watched are ever
 * touched, so the resident part is a few pages around the lines in use.
 *
 * The index is transposed inside each block of 1024 lines (4 KiB of words):
 * guest lines 0..31 of a block go to words 0, 32, 64, ..., so 32 neighbouring
 * guest lines -- a 4 KiB structure of hot lock words -- land in 32 different
 * 128-byte host cache lines (and different 64-byte ones), instead of sharing
 * one and ping-ponging it between the cores that use them.
 * -------------------------------------------------------------------------*/
#define COHV_LINES      (1u << 25)
#define COHV_TABLE_SIZE ((size_t)COHV_LINES * sizeof(uint32_t))

static uint32_t* s_table;

static uint32_t* cohv_table_create(void)
{
#if defined(_WIN32)
    /* Committed up front, physically backed on first touch. */
    void* p = VirtualAlloc(NULL, COHV_TABLE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) { fprintf(stderr, "[spu-coh] cannot reserve the version table\n"); abort(); }
    return (uint32_t*)p;
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void* p = mmap(NULL, COHV_TABLE_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) { fprintf(stderr, "[spu-coh] cannot reserve the version table\n"); abort(); }
    return (uint32_t*)p;
#endif
}

static void cohv_table_destroy(uint32_t* t)
{
#if defined(_WIN32)
    VirtualFree(t, 0, MEM_RELEASE);
#else
    munmap(t, COHV_TABLE_SIZE);
#endif
}

static __attribute__((noinline)) uint32_t* cohv_table_slow(void)
{
    uint32_t* t = cohv_table_create();
    uint32_t* expected = NULL;
    if (!__atomic_compare_exchange_n(&s_table, &expected, t, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        cohv_table_destroy(t);
        return expected;
    }
    return t;
}

static inline uint32_t* cohv_table(void)
{
    uint32_t* t = __atomic_load_n(&s_table, __ATOMIC_ACQUIRE);
    return __builtin_expect(t != NULL, 1) ? t : cohv_table_slow();
}

static inline uint32_t cohv_slot(uint32_t ea)
{
    const uint32_t l = ea >> 7;
    return (l & ~0x3FFu) | ((l & 0x1Fu) << 5) | ((l >> 5) & 0x1Fu);
}

uint32_t* spu_cohv_word(uint32_t ea) { return cohv_table() + cohv_slot(ea); }

void spu_cohv_prepare(uint32_t ea) { (void)spu_cohv_word(ea); }

/* ---------------------------------------------------------------------------
 * Spinning
 * -------------------------------------------------------------------------*/
static inline void cohv_relax(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(_M_ARM64)
    __yield();
#elif defined(_M_X64)
    _mm_pause();
#endif
}

static inline void cohv_yield(void)
{
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}

/* Writers hold a line for one copy of at most 128 bytes. Spin a little, then
 * give the core away: a writer preempted while it owns the line must not have
 * its waiters burning the core it needs to finish. */
static inline void cohv_backoff(unsigned* spins)
{
    if (++*spins < 128u) cohv_relax();
    else { *spins = 0; cohv_yield(); }
}

/* ---------------------------------------------------------------------------
 * Copies
 * -------------------------------------------------------------------------*/
void spu_cohv_copy_line(void* dst, const void* src)
{
    if ((((uintptr_t)dst | (uintptr_t)src) & 7u) == 0) {
        uint64_t* d = (uint64_t*)dst;
        const uint64_t* s = (const uint64_t*)src;
        for (unsigned i = 0; i < 16; i++)
            __atomic_store_n(&d[i], __atomic_load_n(&s[i], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    } else {
        unsigned char* d = (unsigned char*)dst;
        const unsigned char* s = (const unsigned char*)src;
        for (unsigned i = 0; i < 128; i++)
            __atomic_store_n(&d[i], __atomic_load_n(&s[i], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    }
}

int spu_cohv_line_differs(const void* a, const void* b)
{
    if ((((uintptr_t)a | (uintptr_t)b) & 7u) == 0) {
        const uint64_t* x = (const uint64_t*)a;
        const uint64_t* y = (const uint64_t*)b;
        uint64_t diff = 0;
        for (unsigned i = 0; i < 16; i++)
            diff |= __atomic_load_n(&x[i], __ATOMIC_RELAXED) ^ __atomic_load_n(&y[i], __ATOMIC_RELAXED);
        return diff != 0;
    }
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (unsigned i = 0; i < 128; i++)
        if (__atomic_load_n(&x[i], __ATOMIC_RELAXED) != __atomic_load_n(&y[i], __ATOMIC_RELAXED))
            return 1;
    return 0;
}

void spu_cohv_store_bytes(void* dst, const void* src, unsigned n)
{
    const uintptr_t a = (uintptr_t)dst;
    switch (n) {
    case 1: { uint8_t v; memcpy(&v, src, 1); __atomic_store_n((uint8_t*)dst, v, __ATOMIC_RELAXED); return; }
    case 2: if (!(a & 1u)) { uint16_t v; memcpy(&v, src, 2); __atomic_store_n((uint16_t*)dst, v, __ATOMIC_RELAXED); return; } break;
    case 4: if (!(a & 3u)) { uint32_t v; memcpy(&v, src, 4); __atomic_store_n((uint32_t*)dst, v, __ATOMIC_RELAXED); return; } break;
    case 8: if (!(a & 7u)) { uint64_t v; memcpy(&v, src, 8); __atomic_store_n((uint64_t*)dst, v, __ATOMIC_RELAXED); return; } break;
    default: break;
    }
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned i = 0; i < n; i++) __atomic_store_n(&d[i], s[i], __ATOMIC_RELAXED);
}

static inline uint64_t cohv_load_value(const void* p, unsigned size)
{
    return size == 8 ? __atomic_load_n((const uint64_t*)p, __ATOMIC_RELAXED)
                     : __atomic_load_n((const uint32_t*)p, __ATOMIC_RELAXED);
}

/* ---------------------------------------------------------------------------
 * Snapshots
 * -------------------------------------------------------------------------*/
/* A line written back to back can keep a lock-free reader retrying forever:
 * every attempt overlaps some write. After COHV_READ_TRIES attempts the reader
 * takes the line like a writer -- a turn in the same compare-and-swap the
 * writers race for -- copies it, and releases it unchanged. */
#define COHV_READ_TRIES 512u

uint32_t spu_cohv_read_line(uint32_t ea, const void* line, void* dst)
{
    uint32_t* w = spu_cohv_word(ea);
    unsigned spins = 0;
    for (unsigned tries = 0; tries < COHV_READ_TRIES; tries++, cohv_backoff(&spins)) {
        const uint32_t v1 = __atomic_load_n(w, __ATOMIC_ACQUIRE);
        if (v1 & SPU_COHV_WRITING) continue;
        spu_cohv_copy_line(dst, line);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t v2 = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (((v1 ^ v2) & ~SPU_COHV_WAITERS) == 0) return v1 & ~SPU_COHV_FLAGS;
    }
    COHV_STAT_HOOK(COHV_STAT_READ_FALLBACK);
    const uint32_t count = spu_cohv_lock(ea);
    spu_cohv_copy_line(dst, line);
    spu_cohv_unlock_unchanged(ea);
    return count;
}

uint32_t spu_cohv_read_value(uint32_t ea, const void* p, unsigned size, uint64_t* value)
{
    uint32_t* w = spu_cohv_word(ea);
    unsigned spins = 0;
    for (unsigned tries = 0; tries < COHV_READ_TRIES; tries++, cohv_backoff(&spins)) {
        const uint32_t v1 = __atomic_load_n(w, __ATOMIC_ACQUIRE);
        if (v1 & SPU_COHV_WRITING) continue;
        const uint64_t v = cohv_load_value(p, size);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t v2 = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (((v1 ^ v2) & ~SPU_COHV_WAITERS) == 0) { *value = v; return v1 & ~SPU_COHV_FLAGS; }
    }
    COHV_STAT_HOOK(COHV_STAT_READ_FALLBACK);
    const uint32_t count = spu_cohv_lock(ea);
    *value = cohv_load_value(p, size);
    spu_cohv_unlock_unchanged(ea);
    return count;
}

/* ---------------------------------------------------------------------------
 * Ownership
 * -------------------------------------------------------------------------*/
uint32_t spu_cohv_lock(uint32_t ea)
{
    uint32_t* w = spu_cohv_word(ea);
    uint32_t cur = __atomic_load_n(w, __ATOMIC_RELAXED);
    for (unsigned spins = 0, waited = 0;;) {
        if (!(cur & SPU_COHV_WRITING)) {
            if (__atomic_compare_exchange_n(w, &cur, cur | SPU_COHV_WRITING, 1,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                break;
            continue;
        }
        if (!waited) { waited = 1; COHV_STAT_HOOK(COHV_STAT_LOCK_WAIT); }
        cohv_backoff(&spins);
        cur = __atomic_load_n(w, __ATOMIC_RELAXED);
    }
    /* The WRITING mark before any data store (see the ordering note). */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return cur & ~SPU_COHV_FLAGS;
}

int spu_cohv_lock_if(uint32_t ea, uint32_t count)
{
    uint32_t* w = spu_cohv_word(ea);
    uint32_t cur = __atomic_load_n(w, __ATOMIC_RELAXED);
    for (;;) {
        /* count never has WRITING set, so a line being written fails here. */
        if ((cur & ~SPU_COHV_WAITERS) != count) return 0;
        if (__atomic_compare_exchange_n(w, &cur, cur | SPU_COHV_WRITING, 1,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return 1;
}

static void cohv_wake_line(uint32_t ea);

uint32_t spu_cohv_unlock_written(uint32_t ea)
{
    /* WRITING is set, so adding STEP - WRITING clears it and advances the
     * count in one operation, and leaves WAITERS as it is. (Releasing with a
     * plain store instead measured the same on the hot-line benchmark, and
     * needs waiters to stay off owned words; not worth it.) */
    const uint32_t old = __atomic_fetch_add(spu_cohv_word(ea), SPU_COHV_STEP - SPU_COHV_WRITING,
                                            __ATOMIC_RELEASE);
    if (__builtin_expect((old & SPU_COHV_WAITERS) != 0, 0)) cohv_wake_line(ea);
    return (old + SPU_COHV_STEP - SPU_COHV_WRITING) & ~SPU_COHV_FLAGS;
}

void spu_cohv_unlock_unchanged(uint32_t ea)
{
    __atomic_fetch_and(spu_cohv_word(ea), ~SPU_COHV_WRITING, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------------------
 * Operations
 * -------------------------------------------------------------------------*/
int spu_cohv_putllc(uint32_t ea, void* line, uint32_t count,
                    const void* snapshot, const void* data)
{
    /* A moved count fails without touching the word for writing: contended
     * claims fail often, and a failing CAS still takes the cache line. */
    if (spu_cohv_moved(ea, count)) return 0;
    if (!spu_cohv_lock_if(ea, count)) return 0;
    /* The count proves no versioned writer touched the line. The bytes are
     * compared too, for writers outside the protocol (a host copy that does
     * not take the line, or a store that raced the line's first reservation). */
    if (spu_cohv_line_differs(line, snapshot)) { spu_cohv_unlock_unchanged(ea); return 0; }
    spu_cohv_copy_line(line, data);
    spu_cohv_unlock_written(ea);
    return 1;
}

uint32_t spu_cohv_putlluc(uint32_t ea, void* line, const void* data)
{
    spu_cohv_lock(ea);
    spu_cohv_copy_line(line, data);
    return spu_cohv_unlock_written(ea);
}

uint32_t spu_cohv_store(uint32_t ea, void* p, const void* src, unsigned n)
{
    spu_cohv_lock(ea);
    spu_cohv_store_bytes(p, src, n);
    return spu_cohv_unlock_written(ea);
}

int spu_cohv_store_conditional(uint32_t ea, void* p, unsigned size, uint32_t count,
                               uint64_t expected, uint64_t value)
{
    if (spu_cohv_moved(ea, count)) return 0;
    if (!spu_cohv_lock_if(ea, count)) return 0;
    if (cohv_load_value(p, size) != expected) { spu_cohv_unlock_unchanged(ea); return 0; }
    if (size == 8) __atomic_store_n((uint64_t*)p, value, __ATOMIC_RELAXED);
    else           __atomic_store_n((uint32_t*)p, (uint32_t)value, __ATOMIC_RELAXED);
    spu_cohv_unlock_written(ea);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Wait buckets
 *
 * Touched only when someone waits: a writer reaches them only through a word
 * whose WAITERS bit is set. Each bucket is a short list under a spin lock held
 * for a list walk; the wake callbacks it runs are a mutex and a signal.
 * -------------------------------------------------------------------------*/
#define COHV_BUCKETS 64u
static struct {
    uint32_t lock;
    spu_cohv_waiter* head;
    char pad[64 - sizeof(uint32_t) - sizeof(void*)];
} s_buckets[COHV_BUCKETS];

static inline unsigned cohv_bucket(uint32_t line) { return (line >> 7) * 0x9E3779B1u >> 26; }

static void cohv_bucket_lock(unsigned b)
{
    for (unsigned spins = 0;; cohv_backoff(&spins)) {
        uint32_t z = 0;
        if (!__atomic_load_n(&s_buckets[b].lock, __ATOMIC_RELAXED) &&
            __atomic_compare_exchange_n(&s_buckets[b].lock, &z, 1u, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
    }
}
static void cohv_bucket_unlock(unsigned b) { __atomic_store_n(&s_buckets[b].lock, 0u, __ATOMIC_RELEASE); }

uint32_t spu_cohv_wait_register(spu_cohv_waiter* w, uint32_t ea,
                                void (*wake)(void*), void* opaque)
{
    const uint32_t line = ea & ~127u;
    const unsigned b = cohv_bucket(line);
    w->line = line; w->wake = wake; w->opaque = opaque;
    cohv_bucket_lock(b);
    w->next = s_buckets[b].head;
    s_buckets[b].head = w;
    const uint32_t v = __atomic_fetch_or(spu_cohv_word(line), SPU_COHV_WAITERS, __ATOMIC_ACQ_REL);
    cohv_bucket_unlock(b);
    return v & ~SPU_COHV_FLAGS;
}

void spu_cohv_wait_unregister(spu_cohv_waiter* w)
{
    const uint32_t line = w->line;
    const unsigned b = cohv_bucket(line);
    cohv_bucket_lock(b);
    int others = 0;
    for (spu_cohv_waiter** link = &s_buckets[b].head; *link;) {
        if (*link == w) { *link = w->next; continue; }
        others |= (*link)->line == line;
        link = &(*link)->next;
    }
    /* The last waiter on the line takes the bit down; a writer that read it
     * set just before finds nobody to wake, which costs it a bucket walk. */
    if (!others) __atomic_fetch_and(spu_cohv_word(line), ~SPU_COHV_WAITERS, __ATOMIC_RELAXED);
    cohv_bucket_unlock(b);
    w->next = NULL;
}

static void cohv_wake_line(uint32_t ea)
{
    const uint32_t line = ea & ~127u;
    const unsigned b = cohv_bucket(line);
    __atomic_fetch_add(&g_spu_cohv_wakes, 1ull, __ATOMIC_RELAXED);
    cohv_bucket_lock(b);
    for (spu_cohv_waiter* w = s_buckets[b].head; w; w = w->next)
        if (w->line == line) w->wake(w->opaque);
    cohv_bucket_unlock(b);
}
