/* Reservation gate; see rsx_native_reservation.h. */
#include "rsx_native_reservation.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK res_mutex;
typedef CONDITION_VARIABLE res_cond;
#define RES_MUTEX_INIT SRWLOCK_INIT
#define RES_COND_INIT CONDITION_VARIABLE_INIT
static void res_lock(res_mutex *m) { AcquireSRWLockExclusive(m); }
static void res_unlock(res_mutex *m) { ReleaseSRWLockExclusive(m); }
static void res_wait(res_cond *c, res_mutex *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
static void res_wake_all(res_cond *c) { WakeAllConditionVariable(c); }
static uint64_t res_now_ns(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t res_mutex;
typedef pthread_cond_t res_cond;
#define RES_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define RES_COND_INIT PTHREAD_COND_INITIALIZER
static void res_lock(res_mutex *m) { pthread_mutex_lock(m); }
static void res_unlock(res_mutex *m) { pthread_mutex_unlock(m); }
static void res_wait(res_cond *c, res_mutex *m) { pthread_cond_wait(c, m); }
static void res_wake_all(res_cond *c) { pthread_cond_broadcast(c); }
static uint64_t res_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

static res_mutex s_mu = RES_MUTEX_INIT;
static res_cond s_cv = RES_COND_INIT;
/* Fast path for the FIFO: nonzero only while a reservation is outstanding. */
static atomic_uint_fast64_t s_active;
static uint64_t s_next_token;
static uint64_t s_lifetime;
static rsx_reservation_span s_spans[RSX_RESERVATION_MAX_SPANS];
static uint32_t s_count;
static rsx_reservation_stats s_stats;

int rsx_reservation_enabled(void)
{
    const char *v = getenv("PS3RECOMP_RSX_RESERVATION_GATE");
    return v && v[0] == '1' && v[1] == 0;
}

static int span_less(const rsx_reservation_span *a, const rsx_reservation_span *b)
{
    return a->space != b->space ? a->space < b->space : a->begin < b->begin;
}
static int span_cmp(const void *a, const void *b)
{
    const rsx_reservation_span *x = a, *y = b;
    return span_less(x, y) ? -1 : span_less(y, x) ? 1 : 0;
}

int rsx_reservation_open(uint64_t lifetime, const rsx_reservation_span *pinned,
                         uint32_t count, uint64_t *token)
{
    if (!token) return -6;
    *token = 0;
    if (!rsx_reservation_enabled()) return -2;
    if (count > RSX_RESERVATION_MAX_SPANS || (count && !pinned)) {
        res_lock(&s_mu);
        s_stats.refused++;
        res_unlock(&s_mu);
        return -6;
    }
    for (uint32_t i = 0; i < count; i++)
        if (pinned[i].begin >= pinned[i].end || pinned[i].space > RSX_RESERVATION_MAIN) {
            res_lock(&s_mu);
            s_stats.refused++;
            res_unlock(&s_mu);
            return -6;
        }
    res_lock(&s_mu);
    if (atomic_load(&s_active)) {
        s_stats.busy++;
        res_unlock(&s_mu);
        return -4;
    }
    memcpy(s_spans, pinned, (size_t)count * sizeof *pinned);
    qsort(s_spans, count, sizeof *s_spans, span_cmp);
    s_count = count;
    s_lifetime = lifetime;
    *token = ++s_next_token;
    s_stats.opened++;
    atomic_store(&s_active, *token);
    res_unlock(&s_mu);
    return 0;
}

int rsx_reservation_resolve(uint64_t token)
{
    res_lock(&s_mu);
    if (!token || atomic_load(&s_active) != token) {
        res_unlock(&s_mu);
        return -3;
    }
    atomic_store(&s_active, 0);
    s_count = 0;
    s_lifetime = 0;
    s_stats.resolved++;
    res_wake_all(&s_cv);
    res_unlock(&s_mu);
    return 0;
}

/* Under s_mu: whether `t` overlaps a pinned span (sorted by space, begin). */
static int pinned_overlap_locked(const rsx_reservation_span *t)
{
    if (!t || t->begin >= t->end) return 0;
    /* The first span that starts at or after t->end in t's space bounds the
     * candidates; scan back over spans of the same space that start before
     * it. Spans may overlap each other, so every earlier span is checked
     * until the space changes (counts are small). */
    uint32_t lo = 0, hi = s_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const rsx_reservation_span *s = &s_spans[mid];
        if (s->space < t->space || (s->space == t->space && s->begin < t->end)) lo = mid + 1;
        else hi = mid;
    }
    while (lo > 0) {
        const rsx_reservation_span *s = &s_spans[--lo];
        if (s->space != t->space) break;
        if (s->end > t->begin) return 1;
    }
    return 0;
}

static int affects_locked(int cause, const rsx_reservation_span *touched)
{
    if (!atomic_load(&s_active)) return 0;
    if (cause < 0 || cause >= RSX_RESERVATION_CAUSES) return 1; /* unknown: be safe */
    if (cause == RSX_RESERVATION_PINNED_WRITE) return pinned_overlap_locked(touched);
    return 1;
}

int rsx_reservation_affects(int cause, const rsx_reservation_span *touched)
{
    if (!atomic_load(&s_active)) return 0;
    res_lock(&s_mu);
    const int r = affects_locked(cause, touched);
    res_unlock(&s_mu);
    return r;
}

int rsx_reservation_gate(int cause, const rsx_reservation_span *touched)
{
    if (!atomic_load(&s_active)) return 0;
    res_lock(&s_mu);
    if (!affects_locked(cause, touched)) {
        res_unlock(&s_mu);
        return 0;
    }
    const uint64_t start = res_now_ns();
    while (affects_locked(cause, touched)) res_wait(&s_cv, &s_mu);
    const int k = cause >= 0 && cause < RSX_RESERVATION_CAUSES ? cause : RSX_RESERVATION_CLOSE;
    s_stats.waits[k]++;
    s_stats.wait_ns[k] += res_now_ns() - start;
    res_unlock(&s_mu);
    return 1;
}

void rsx_reservation_get_stats(rsx_reservation_stats *out)
{
    if (!out) return;
    res_lock(&s_mu);
    *out = s_stats;
    res_unlock(&s_mu);
}
