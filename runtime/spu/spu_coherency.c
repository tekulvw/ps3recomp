/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line lock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store.
 */

#include <stdlib.h>
#include <string.h>
#include "spu_coherency.h"

#include <stdint.h>
#include <stdio.h>
#include "ps3emu/log_first.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * exclusion semantics, now shared. MSVC uses a bounded polite spin loop;
 * POSIX uses a mutex so a descheduled owner does not make every waiter spin.
 * ===========================================================================*/
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
static volatile long g_lockline = 0;
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
void spu_lockline_lock(void)
{
    unsigned spins = 0;
    while (_InterlockedExchange(&g_lockline, 1)) {
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (g_lockline);
    }
}
void spu_lockline_unlock(void) { _InterlockedExchange(&g_lockline, 0); }
#elif defined(_WIN32)
/* Retain the existing non-MSVC Windows path without a pthread dependency. */
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
#else
#include <pthread.h>
#include <time.h>
/* The owner may be descheduled or wake a host channel waiter while holding
 * this global lock. An unbounded exchange loop then consumes whole cores and
 * competes with the owner and the remaining SPU workers. Let the host park
 * contended waiters; the mutex preserves the same non-recursive exclusion and
 * acquire/release publication for every reservation and coherent store. */
static pthread_mutex_t g_lockline = PTHREAD_MUTEX_INITIALIZER;
/* Hold times are a line copy and a short list walk, far shorter than a
 * kernel park and wake, and every SPU's policy module polls through here.
 * Retry before parking; the trylocks are spaced out so the waiters do not
 * keep stealing the lock's cache line from its owner. */
static inline uint64_t spu_lockline_now_ns(void)
{
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}
static inline void spu_lockline_relax(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}
/* SPU_LOCKLINE_PROFILE=1: wait and hold time per acquiring call site (return
 * address), printed every two seconds with the image slide so the sites can
 * be symbolized offline (atos -l). Off, the lock pays one predictable branch. */
#include <stdatomic.h>
#include <time.h>
#include <mach-o/dyld.h>
typedef struct { _Atomic(uintptr_t) site; atomic_ullong n, wait_ns, hold_ns; } lockline_prof;
static lockline_prof s_llp[512];
static int s_llp_on = -1;
static __thread struct { uintptr_t site; uint64_t t; } t_llp;
static uint64_t llp_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static lockline_prof* llp_slot(uintptr_t site)
{
    for (unsigned i = (unsigned)(site >> 2) & 511u, k = 0; k < 512; ++k, i = (i + 1) & 511u) {
        uintptr_t cur = atomic_load_explicit(&s_llp[i].site, memory_order_relaxed);
        if (cur == site) return &s_llp[i];
        if (!cur && atomic_compare_exchange_strong(&s_llp[i].site, &cur, site)) return &s_llp[i];
        if (cur == site) return &s_llp[i];
    }
    return 0;
}
static void llp_report(void)
{
    static atomic_ullong next;
    uint64_t now = llp_now(), due = atomic_load(&next);
    if (now < due || !atomic_compare_exchange_strong(&next, &due, now + 2000000000ull)) return;
    fprintf(stderr, "[lockline-prof] slide=0x%lx\n", (unsigned long)_dyld_get_image_vmaddr_slide(0));
    for (unsigned i = 0; i < 512; ++i) {
        uintptr_t s = atomic_load(&s_llp[i].site);
        if (s) fprintf(stderr, "[lockline-prof] site=0x%lx n=%llu wait_us=%llu hold_us=%llu\n", (unsigned long)s,
                       atomic_load(&s_llp[i].n), atomic_load(&s_llp[i].wait_ns) / 1000, atomic_load(&s_llp[i].hold_ns) / 1000);
    }
}
static __attribute__((noinline)) void spu_lockline_lock_profiled(uintptr_t site)
{
    const uint64_t t0 = llp_now();
    if (pthread_mutex_lock(&g_lockline)) abort();
    const uint64_t t1 = llp_now();
    lockline_prof* p = llp_slot(site);
    if (p) { atomic_fetch_add_explicit(&p->n, 1, memory_order_relaxed);
             atomic_fetch_add_explicit(&p->wait_ns, t1 - t0, memory_order_relaxed); }
    t_llp.site = site; t_llp.t = t1;
}
/* Lock-policy experiment (local measurement only):
 *   SPU_LOCKLINE_MODE=spin    trylock in a bare loop, never park (upstream's
 *                             test-and-set spin on the same lock word)
 *   SPU_LOCKLINE_MODE=mutex   pthread_mutex_lock at once (park when contended)
 *   SPU_LOCKLINE_MODE=hybrid  the default below: up to 20 us of spaced
 *                             retries, then park
 *   SPU_LOCKLINE_HIST=1       log2 histograms of wait (lock call to acquire)
 *                             and hold (acquire to release) wall time, printed
 *                             every two seconds. A hold far beyond the
 *                             sub-microsecond critical sections means the
 *                             holder stopped running while it held the lock. */
static int s_llx_mode;          /* 0 hybrid, 1 spin, 2 mutex */
static int s_llx_hist;
static atomic_ullong s_llx_wait[48], s_llx_hold[48];
static __thread uint64_t t_llx_t;
static int spu_lockline_config(void)
{
    const char* m = getenv("SPU_LOCKLINE_MODE");
    s_llx_mode = m && !strcmp(m, "spin") ? 1 : m && !strcmp(m, "mutex") ? 2 : 0;
    s_llx_hist = getenv("SPU_LOCKLINE_HIST") != NULL;
    if (m || s_llx_hist) fprintf(stderr, "[lockline-hist] mode=%s hist=%d\n",
                                 s_llx_mode == 1 ? "spin" : s_llx_mode == 2 ? "mutex" : "hybrid", s_llx_hist);
    return (getenv("SPU_LOCKLINE_PROFILE") ? 1 : 0) | (m || s_llx_hist ? 2 : 0);
}
static unsigned llx_bucket(uint64_t ns) { return ns ? 64u - (unsigned)__builtin_clzll(ns) : 0u; }
static void llx_report(void)
{
    static atomic_ullong next;
    uint64_t now = llp_now(), due = atomic_load(&next);
    if (now < due || !atomic_compare_exchange_strong(&next, &due, now + 2000000000ull)) return;
    char w[1024], h[1024]; int wn = 0, hn = 0;
    for (unsigned b = 0; b < 48; ++b) {
        unsigned long long a = atomic_load(&s_llx_wait[b]), c = atomic_load(&s_llx_hold[b]);
        if (a) wn += snprintf(w + wn, sizeof w - (size_t)wn, " %u:%llu", b, a);
        if (c) hn += snprintf(h + hn, sizeof h - (size_t)hn, " %u:%llu", b, c);
    }
    fprintf(stderr, "[lockline-hist] t=%llu wait%s | hold%s\n", (unsigned long long)(now / 1000000u), w, h);
}
static void spu_lockline_lock_hybrid(void);
static __attribute__((noinline)) void spu_lockline_lock_experiment(void)
{
    const uint64_t t0 = s_llx_hist ? llp_now() : 0;
    if (s_llx_mode == 1) { while (pthread_mutex_trylock(&g_lockline)) { } }
    else if (s_llx_mode == 2) { if (pthread_mutex_lock(&g_lockline)) abort(); }
    else spu_lockline_lock_hybrid();
    if (s_llx_hist) {
        const uint64_t t1 = llp_now();
        atomic_fetch_add_explicit(&s_llx_wait[llx_bucket(t1 - t0)], 1, memory_order_relaxed);
        t_llx_t = t1;
    }
}
void spu_lockline_lock(void)
{
    /* Taken here: a cold-split profiling branch would report its own caller. */
    uintptr_t site = (uintptr_t)__builtin_return_address(0);
    __asm__ __volatile__("" : "+r"(site));
    if (__builtin_expect(s_llp_on != 0, 0)) {
        if (s_llp_on < 0) s_llp_on = spu_lockline_config();
        if (s_llp_on & 1) { spu_lockline_lock_profiled(site); return; }
        if (s_llp_on & 2) { spu_lockline_lock_experiment(); return; }
    }
    spu_lockline_lock_hybrid();
}
static void spu_lockline_lock_hybrid(void)
{
    if (!pthread_mutex_trylock(&g_lockline)) return;
    /* Up to 20 us of exponentially spaced retries, then park. A yield is
     * nearly free on current cores, so a count-bounded spin gave up after
     * about a microsecond and most contended waiters paid a kernel park and
     * wake for a sub-microsecond critical section. */
    const uint64_t give_up = spu_lockline_now_ns() + 20000u;
    for (unsigned backoff = 16;; backoff = backoff < 1024u ? backoff * 2u : backoff) {
        for (unsigned i = 0; i < backoff; ++i) spu_lockline_relax();
        if (!pthread_mutex_trylock(&g_lockline)) return;
        if (spu_lockline_now_ns() >= give_up) break;
    }
    if (pthread_mutex_lock(&g_lockline)) abort();
}
void spu_lockline_unlock(void)
{
    if (__builtin_expect(s_llp_on > 1, 0) && !(s_llp_on & 1)) {
        if (s_llx_hist) atomic_fetch_add_explicit(&s_llx_hold[llx_bucket(llp_now() - t_llx_t)], 1, memory_order_relaxed);
        if (pthread_mutex_unlock(&g_lockline)) abort();
        if (s_llx_hist) llx_report();
        return;
    }
    if (__builtin_expect(s_llp_on > 0, 0) && t_llp.site) {
        lockline_prof* p = llp_slot(t_llp.site);
        if (p) atomic_fetch_add_explicit(&p->hold_ns, llp_now() - t_llp.t, memory_order_relaxed);
        t_llp.site = 0;
        if (pthread_mutex_unlock(&g_lockline)) abort();
        llp_report();
        return;
    }
    if (pthread_mutex_unlock(&g_lockline)) abort();
}
#endif

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINE_SHIFT  7                                  /* 128-byte lines */
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

static unsigned char s_coh_bitmap[SPU_COH_BITMAP_SZ];
/* The subset of marked lines an SPU or a watch has ever had an interest in.
 * A line marked only because the PPU load-reserved it (spu_coh_line_epoch)
 * has no SPU reservation holder and no watch, so its notify needs only the
 * PPU-side bookkeeping. Every reservation holder's line is in here before the
 * reservation is published: the slow GETLLAR marks it under the lock, and the
 * fast GETLLAR requires it (spu_coh_reserve_fast). */
static unsigned char s_spu_bitmap[SPU_COH_BITMAP_SZ];
static int spu_coh_line_spu(uint32_t line)
{
    const uint32_t l = line >> SPU_COH_LINE_SHIFT;
    return (__atomic_load_n(&s_spu_bitmap[l >> 3], __ATOMIC_RELAXED) >> (l & 7)) & 1u;
}

/* PPU-only lines serialize on a per-line shard, not the lock-line lock.
 *
 * On a line the PPU load-reserved and no SPU or watch has an interest in, the
 * only transaction to protect is a PPU store-conditional's epoch check and CAS
 * against a store landing in between (the store bumps the epoch after writing).
 * Taking the process-wide lock-line lock for that made every plain PPU store to
 * such a line -- stack slots beside a lock word, refcounted fields -- contend
 * with every SPU atomic in the process. Instead:
 *  - a plain PPU store to a PPU-only line holds only the line's shard;
 *  - a PPU store-conditional holds the lock-line lock, then the shard, so it
 *    excludes the shard-only stores and every writer holding the lock-line lock;
 *  - a line gains SPU interest (spu_coh_reserve, under the lock-line lock) only
 *    while holding its shard, and a shard-only store re-checks the interest
 *    under the shard, so none is in flight on the shard path once an SPU can
 *    hold a reservation on the line.
 * A lock-line-lock writer and a shard-only store may overlap on one line: both
 * only write guest bytes and bump the epoch atomically, and the
 * store-conditional excludes both. */
#if defined(_WIN32)
#include <windows.h>
static inline void spu_coh_yield(void) { SwitchToThread(); }
#else
#include <sched.h>
static inline void spu_coh_yield(void) { sched_yield(); }
#endif
static inline void spu_coh_spin_relax(void)
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
#define SPU_COH_PPU_SHARDS 256u
static struct { uint32_t v; char pad[60]; } s_ppu_shard[SPU_COH_PPU_SHARDS];
static inline uint32_t* spu_coh_shard(uint32_t ea)
{
    return &s_ppu_shard[(ea >> SPU_COH_LINE_SHIFT) & (SPU_COH_PPU_SHARDS - 1u)].v;
}
void spu_coh_line_shard_lock(uint32_t ea)
{
    uint32_t* l = spu_coh_shard(ea);
    for (;;) {
        uint32_t z = 0;
        if (__atomic_compare_exchange_n(l, &z, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        for (unsigned n = 0; __atomic_load_n(l, __ATOMIC_RELAXED); )
            if (++n < 256u) spu_coh_spin_relax(); else { n = 0; spu_coh_yield(); }
    }
}
void spu_coh_line_shard_unlock(uint32_t ea) { __atomic_store_n(spu_coh_shard(ea), 0u, __ATOMIC_RELEASE); }

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code, and keeps every existing
 * test that stores through vm_write* paying one compare against a hot global.
 * Written once, under the lock; read unlocked, where a stale zero costs at most
 * one missed event on the very first reservation of the run. */
static int s_coh_armed;

/* Every live virtual context may retain a reservation between scheduling
 * quanta; physical SPU count is not a bound on this set. All access is under
 * the lock-line lock. Nodes are removed before their context's lifetime ends. */
typedef struct spu_coh_entry {
    spu_context* ctx;
    struct spu_coh_entry* next;
} spu_coh_entry;
static spu_coh_entry* s_coh_ctxs;
static spu_coh_watch* s_coh_watches;

/* Only PPU reservation lines need epochs. Entries contain no context pointers
 * and live for the process, like the existing reserved-line bitmap. */
typedef struct coh_epoch { uint32_t line; uint64_t value; struct coh_epoch* next; } coh_epoch;
static coh_epoch* s_epochs[1024];
static coh_epoch** epoch_bucket(uint32_t line) { return &s_epochs[(line >> 7) & 1023u]; }
uint64_t spu_coh_line_epoch(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    coh_epoch** bucket = epoch_bucket(line);
    for (coh_epoch* p = __atomic_load_n(bucket, __ATOMIC_ACQUIRE); p; p = p->next)
        if (p->line == line) return __atomic_load_n(&p->value, __ATOMIC_RELAXED);
    coh_epoch* p = (coh_epoch*)malloc(sizeof(*p));
    if (!p) abort();
    /* Inserted under the lock-line lock only; shard-path stores walk the
     * bucket concurrently, so the node is published complete. */
    p->line = line; p->value = 0; p->next = *bucket;
    __atomic_store_n(bucket, p, __ATOMIC_RELEASE);
    /* Marks the line for the PPU only: no SPU holder or watch exists on it. */
    { uint32_t l = line >> SPU_COH_LINE_SHIFT;
      s_coh_bitmap[l >> 3] |= (unsigned char)(1u << (l & 7)); s_coh_armed = 1; }
    return 0;
}
static void epoch_written(uint32_t line)
{
    for (coh_epoch* p = __atomic_load_n(epoch_bucket(line), __ATOMIC_ACQUIRE); p; p = p->next)
        if (p->line == line) { __atomic_fetch_add(&p->value, 1u, __ATOMIC_RELAXED); return; }
}

unsigned long g_spu_lr_raise = 0;

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    if (!spu_coh_line_spu(ea)) {
        /* Under the shard: no PPU-only store is mid-flight on this line. */
        spu_coh_line_shard_lock(ea);
        __atomic_fetch_or(&s_spu_bitmap[line >> 3], (unsigned char)(1u << (line & 7)), __ATOMIC_RELAXED);
        spu_coh_line_shard_unlock(ea);
    }
    s_coh_bitmap[line >> 3] |= (unsigned char)(1u << (line & 7));
    s_coh_armed = 1;

    if (!ctx) return;
    for (spu_coh_entry* entry = s_coh_ctxs; entry; entry = entry->next)
        if (entry->ctx == ctx) { __atomic_store_n(&ctx->coh_listed, 1, __ATOMIC_RELEASE); return; }
    spu_coh_entry* entry = (spu_coh_entry*)malloc(sizeof(*entry));
    if (!entry) {
        fprintf(stderr, "[spu-coh] cannot register reservation holder %p\n", (void*)ctx);
        fflush(stderr);
        abort(); /* A missed holder loses invalidation events, including ABA. */
    }
    entry->ctx = ctx;
    entry->next = s_coh_ctxs;
    s_coh_ctxs = entry;
    __atomic_store_n(&ctx->coh_listed, 1, __ATOMIC_RELEASE);
}

void spu_coh_watch_add(spu_coh_watch* watch, uint32_t ea,
                       void (*wake)(void*), void* opaque)
{
    /* Arming the existing bitmap makes PPU stores/CAS and MFC writes use
     * the same serialized commit/notify path as SPU reservations. */
    spu_coh_reserve(NULL, ea);
    watch->line = ea & ~127u;
    watch->wake = wake;
    watch->opaque = opaque;
    watch->next = s_coh_watches;
    s_coh_watches = watch;
}

void spu_coh_watch_remove(spu_coh_watch* watch)
{
    for (spu_coh_watch** link = &s_coh_watches; *link; link = &(*link)->next) {
        if (*link != watch) continue;
        *link = watch->next;
        watch->next = NULL;
        return;
    }
}

static void spu_coh_wake_watches(uint32_t line)
{
    for (spu_coh_watch* w = s_coh_watches; w; w = w->next)
        if (w->line == line) w->wake(w->opaque);
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before removal would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (spu_coh_entry** link = &s_coh_ctxs; *link; link = &(*link)->next) {
        if ((*link)->ctx != ctx) continue;
        __atomic_store_n(&ctx->coh_listed, 0, __ATOMIC_RELEASE);
        spu_coh_entry* entry = *link;
        *link = entry->next;
        free(entry);
        break;
    }
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (spu_coh_entry** link = &s_coh_ctxs; *link;) {
        spu_coh_entry* entry = *link;
        uintptr_t c = (uintptr_t)entry->ctx;
        if (c >= lo && c < hi) {
            static int _n = 0;
            if (PS3_LOG_FIRST(_n, 16))
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, entry->ctx->image_id);
            *link = entry->next;
            free(entry);
        } else {
            link = &entry->next;
        }
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    if (!s_coh_armed) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (s_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
/* ppu_loader.cpp, when linked. The loader is compiled per game, so the host,
 * the fixtures and any build without it have no PPU reservations to break.
 * POSIX: a weak no-op definition the loader's overrides (a weak undefined
 * reference is not enough: ld64 only lets those stay unresolved for dylib
 * symbols). link.exe/lld-link: the linker's own default (see ppu_gcm_pump in
 * ppu_loader.cpp). */
#if defined(_MSC_VER)
void ppu_resv_break_line(uint32_t ea);
void ppu_resv_break_line_absent(uint32_t ea) { (void)ea; }
#pragma comment(linker, "/alternatename:ppu_resv_break_line=ppu_resv_break_line_absent")
#else
__attribute__((weak)) void ppu_resv_break_line(uint32_t ea) { (void)ea; }
#endif

/* GETLLAR without the lock-line lock (spu_coh_reserve_fast).
 *
 * A writer copies its data and then, in the notifiers below, ends the
 * reservation of every holder of the line. The fast GETLLAR publishes its
 * reservation and then reads the line. With a full fence between the two
 * steps on both sides, at least one sees the other: either the reader's copy
 * contains the write, or the writer sees the reservation and ends it -- and a
 * reader whose reservation was ended may hold a torn copy, but its PUTLLC
 * fails and it takes SPU_EVENT_LR, exactly as for a write after a clean read. */
static inline void spu_coh_writer_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline int spu_coh_holds(const spu_context* c, uint32_t line)
{
    return __atomic_load_n(&c->resv_valid, __ATOMIC_RELAXED) &&
           (__atomic_load_n(&c->resv_ea, __ATOMIC_RELAXED) & ~127u) == line;
}

/* Only for a context already registered as a holder (coh_listed, set under the
 * lock below and read here by the context's own thread) and a line already in
 * the bitmap, so every writer of the line runs the notifier. Otherwise the
 * caller takes the lock and spu_coh_reserve. */
int spu_coh_reserve_fast(spu_context* ctx, uint32_t ea)
{
    if (!__atomic_load_n(&ctx->coh_listed, __ATOMIC_ACQUIRE) || !spu_coh_line_spu(ea & ~127u)) return 0;
    __atomic_store_n(&ctx->resv_valid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->resv_ea, ea, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->resv_valid, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 1;
}

void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    ppu_resv_break_line(ea);
    epoch_written(line);
    if (!spu_coh_line_spu(line)) return;   /* PPU-only line: no holder, no watch */
    spu_coh_wake_watches(line);
    spu_coh_writer_fence();
    for (spu_coh_entry* entry = s_coh_ctxs; entry; entry = entry->next) {
        spu_context* c = entry->ctx;
        if (!c || (const void*)c == self) continue;
        if (spu_coh_holds(c, line)) {
            __atomic_fetch_or(&c->event_status, SPU_EVENT_LR, __ATOMIC_SEQ_CST);
            __atomic_store_n(&c->resv_valid, 0, __ATOMIC_RELAXED);
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
}

int spu_coh_ppu_store_lock(uint32_t ea)
{
    if (spu_coh_line_spu(ea)) return 0;
    spu_coh_line_shard_lock(ea);
    if (!spu_coh_line_spu(ea)) return 1;
    spu_coh_line_shard_unlock(ea);
    return 0;
}
void spu_coh_ppu_store_unlock(uint32_t ea)
{
    ppu_resv_break_line(ea);
    epoch_written(ea & ~127u);
    spu_coh_line_shard_unlock(ea);
}

/* SPU_COH_LINE_HIST=1: which lines the locked notify is run for, how often,
 * from where, and how often a holder was actually there to be told. Called
 * under the lock-line lock, so the table needs no atomics. Every two seconds
 * the busiest lines are printed with the last caller (an address in this
 * process; subtract the printed slide to symbolize). */
#if defined(__APPLE__)
static int s_clh_on = -1;
static struct { uint32_t line; unsigned long long n, held; uintptr_t site; } s_clh[4096];
static void spu_coh_line_hist(uint32_t line, uintptr_t site, int held)
{
    unsigned i = (line >> 7) * 0x9E3779B1u >> 20;
    for (unsigned k = 0; k < 4096; ++k, i = (i + 1) & 4095u) {
        if (s_clh[i].n && s_clh[i].line != line) continue;
        s_clh[i].line = line; s_clh[i].n++; s_clh[i].held += held != 0; s_clh[i].site = site;
        break;
    }
    static uint64_t next;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now < next) return;
    next = now + 2000000000ull;
    unsigned top[12] = {0}; int nt = 0;
    for (unsigned j = 0; j < 4096; ++j) {
        if (!s_clh[j].n) continue;
        int pos = nt < 12 ? nt++ : 11;
        if (pos == 11 && nt == 12 && s_clh[top[11]].n >= s_clh[j].n) continue;
        top[pos] = j;
        for (; pos > 0 && s_clh[top[pos - 1]].n < s_clh[top[pos]].n; --pos) {
            unsigned t = top[pos]; top[pos] = top[pos - 1]; top[pos - 1] = t;
        }
    }
    fprintf(stderr, "[coh-line] slide=0x%lx\n", (unsigned long)_dyld_get_image_vmaddr_slide(0));
    for (int j = 0; j < nt; ++j)
        fprintf(stderr, "[coh-line] line=0x%08X n=%llu held=%llu site=0x%lx\n", s_clh[top[j]].line,
                s_clh[top[j]].n, s_clh[top[j]].held, (unsigned long)s_clh[top[j]].site);
}
#else
static const int s_clh_on = 0;
static void spu_coh_line_hist(uint32_t line, uintptr_t site, int held) { (void)line; (void)site; (void)held; }
#endif

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    if (__builtin_expect(s_clh_on != 0, 0)) {
#if defined(__APPLE__)
        if (s_clh_on < 0) s_clh_on = getenv("SPU_COH_LINE_HIST") ? 1 : 0;
#endif
        if (s_clh_on) {
            int held = 0;
            for (spu_coh_entry* e = s_coh_ctxs; e; e = e->next) held |= e->ctx && spu_coh_holds(e->ctx, line);
            spu_coh_line_hist(line, (uintptr_t)__builtin_return_address(0), held);
        }
    }
    ppu_resv_break_line(ea);
    epoch_written(line);
    if (!spu_coh_line_spu(line)) return;   /* PPU-only line: no holder, no watch */
    spu_coh_wake_watches(line);
    spu_coh_writer_fence();
    for (spu_coh_entry* entry = s_coh_ctxs; entry; entry = entry->next) {
        spu_context* c = entry->ctx;
        if (!c) continue;
        if (spu_coh_holds(c, line)) {
            __atomic_fetch_or(&c->event_status, SPU_EVENT_LR, __ATOMIC_SEQ_CST);
            __atomic_store_n(&c->resv_valid, 0, __ATOMIC_RELAXED);   /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
    }
}

#ifdef __cplusplus
}
#endif
