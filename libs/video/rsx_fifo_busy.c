/* Opt-in RSX FIFO consumer busy accounting; see rsx_fifo_busy.h. */
#include "rsx_fifo_busy.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
#elif defined(__APPLE__)
#  include <time.h>
#else
#  include <time.h>
#endif

#if defined(_MSC_VER)
#  define FB_TLS __declspec(thread)
#else
#  define FB_TLS _Thread_local
#endif

#define FB_CATEGORIES (RSX_FIFO_BUSY_BUILTIN_COUNT + RSX_FIFO_BUSY_EXTENSION_SLOTS)
#define FB_DEPTH 16
#define FB_SAMPLES 1024

static const char* const s_builtin_names[RSX_FIFO_BUSY_BUILTIN_COUNT] = {
    "other", "idle_put", "idle_sema", "idle_writer", "idle_other", "drain", "draw",
    "sync", "flush", "handoff", "present", "drawable", "pump", "present_host", "lease_wait"};

static atomic_int s_enabled = -1;            /* -1 unread, 0 off, 1 on */
static atomic_int s_names_lock;
static atomic_int s_extension_count;
static char s_extension_names[RSX_FIFO_BUSY_EXTENSION_SLOTS][24];
static atomic_int s_owner_claimed;
static FB_TLS int t_owner;

static uint64_t (*s_clock)(void);
static void (*s_sink)(const char*);
static uint64_t s_period_ns;

/* Consumer-thread state: only the owner thread touches it. */
static struct {
    int stack[FB_DEPTH];
    int depth, ignored;          /* ignored: opens past FB_DEPTH, closed silently */
    uint64_t last_ns, period_start_ns, flip_ns, idle_at_flip_ns;
    uint64_t acc_ns[FB_CATEGORIES], spans[FB_CATEGORIES];
    uint64_t updates, unbalanced, overflow;
    uint32_t samples_us[FB_SAMPLES];
    unsigned samples;
    int stop;
} s;

static uint64_t default_clock(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    const uint64_t ticks = (uint64_t)counter.QuadPart, f = (uint64_t)frequency.QuadPart;
    return (ticks / f) * 1000000000ull + (ticks % f) * 1000000000ull / f;
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void default_sink(const char* line) { fprintf(stderr, "%s\n", line); }

static void reset_state(void)
{
    memset(&s, 0, sizeof s);
    s.stop = RSX_FIFO_BUSY_STOP_OTHER;
}

int rsx_fifo_busy_enabled(void)
{
    int on = atomic_load_explicit(&s_enabled, memory_order_relaxed);
    if (on >= 0) return on;
    const char* e = getenv("PS3RECOMP_RSX_FIFO_BUSY");
    on = e && !strcmp(e, "1");
    if (on) {
        const char* p = getenv("PS3RECOMP_RSX_FIFO_BUSY_PERIOD_MS");
        const long ms = p ? strtol(p, NULL, 10) : 0;
        s_period_ns = (uint64_t)(ms > 0 ? ms : 2000) * 1000000ull;
        s_clock = default_clock;
        s_sink = default_sink;
        reset_state();
    }
    int expected = -1;
    if (!atomic_compare_exchange_strong(&s_enabled, &expected, on)) on = expected;
    return on;
}

void rsx_fifo_busy_test_configure(int enabled, uint64_t (*clock_ns)(void),
                                  void (*sink)(const char*), uint64_t period_ns)
{
    s_clock = clock_ns ? clock_ns : default_clock;
    s_sink = sink ? sink : default_sink;
    s_period_ns = period_ns ? period_ns : 2000000000ull;
    reset_state();
    atomic_store(&s_owner_claimed, 0);
    t_owner = 0;
    atomic_store(&s_enabled, enabled ? 1 : 0);
}

int rsx_fifo_busy_category(const char* name)
{
    if (!rsx_fifo_busy_enabled() || !name || !*name) return -1;
    while (atomic_exchange_explicit(&s_names_lock, 1, memory_order_acquire)) {}
    int id = -1;
    const int n = atomic_load_explicit(&s_extension_count, memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (!strncmp(s_extension_names[i], name, sizeof s_extension_names[i] - 1)) id = i;
    if (id < 0 && n < RSX_FIFO_BUSY_EXTENSION_SLOTS) {
        snprintf(s_extension_names[n], sizeof s_extension_names[n], "%s", name);
        id = n;
        atomic_store_explicit(&s_extension_count, n + 1, memory_order_release);
    }
    atomic_store_explicit(&s_names_lock, 0, memory_order_release);
    return id < 0 ? -1 : RSX_FIFO_BUSY_BUILTIN_COUNT + id;
}

/* Charge the time since the last event to the innermost open span. */
static uint64_t charge(void)
{
    const uint64_t now = s_clock();
    const int top = s.depth ? s.stack[s.depth - 1] : RSX_FIFO_BUSY_OTHER;
    if (s.last_ns && now > s.last_ns) s.acc_ns[top] += now - s.last_ns;
    s.last_ns = now;
    return now;
}

static int is_idle(int c) { return c >= RSX_FIFO_BUSY_IDLE_PUT && c <= RSX_FIFO_BUSY_IDLE_OTHER; }

static uint64_t idle_total(void)
{
    uint64_t t = 0;
    for (int c = RSX_FIFO_BUSY_IDLE_PUT; c <= RSX_FIFO_BUSY_IDLE_OTHER; ++c) t += s.acc_ns[c];
    return t;
}

static void push(int category)
{
    if (s.depth >= FB_DEPTH) { ++s.ignored; ++s.overflow; return; }
    charge();
    s.stack[s.depth++] = category;
    ++s.spans[category];
}

static void pop(int category)
{
    if (s.ignored) { --s.ignored; return; }
    int at = s.depth - 1;
    while (at >= 0 && s.stack[at] != category) --at;
    if (at < 0) return;                      /* not open: ignore */
    charge();
    if (at != s.depth - 1) ++s.unbalanced;
    s.depth = at;
}

static int owner(void)
{
    return rsx_fifo_busy_enabled() && t_owner;
}

void rsx_fifo_busy_enter(int category)
{
    if (!owner() || category < 0 || category >= FB_CATEGORIES) return;
    push(category);
}

void rsx_fifo_busy_leave(int category)
{
    if (!owner() || category < 0 || category >= FB_CATEGORIES) return;
    pop(category);
}

void rsx_fifo_busy_pass_stopped(int stop)
{
    if (!owner()) return;
    s.stop = (stop >= RSX_FIFO_BUSY_STOP_PUT && stop <= RSX_FIFO_BUSY_STOP_OTHER) ? stop
                                                                                  : RSX_FIFO_BUSY_STOP_OTHER;
}

void rsx_fifo_busy_flip(void)
{
    if (!owner()) return;
    const uint64_t now = charge();
    const uint64_t idle = idle_total();
    if (s.flip_ns) {
        const uint64_t wall = now - s.flip_ns, waited = idle - s.idle_at_flip_ns;
        const uint64_t busy = wall > waited ? wall - waited : 0;
        if (s.samples < FB_SAMPLES) s.samples_us[s.samples++] = (uint32_t)(busy / 1000u);
    }
    s.flip_ns = now;
    s.idle_at_flip_ns = idle;
    ++s.updates;
}

static int compare_u32(const void* a, const void* b)
{
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static void report(uint64_t now)
{
    const uint64_t wall = now - s.period_start_ns;
    const uint64_t idle = idle_total(), busy = wall > idle ? wall - idle : 0;
    const int per_update = s.updates != 0;
    const double div = per_update ? (double)s.updates : (double)wall / 1e9;
    uint32_t sorted[FB_SAMPLES];
    const unsigned n = s.samples;
    memcpy(sorted, s.samples_us, n * sizeof sorted[0]);
    qsort(sorted, n, sizeof sorted[0], compare_u32);
    const double p50 = n ? sorted[n / 2] / 1e3 : 0, p95 = n ? sorted[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1] / 1e3 : 0,
                 max = n ? sorted[n - 1] / 1e3 : 0;
    unsigned over = 0;
    for (unsigned i = 0; i < n; ++i) over += sorted[i] > 16667u;
    char line[2048];
    int len = snprintf(line, sizeof line,
                       "[rsx fifo busy] seconds=%.2f updates=%llu per=%s wall=%.2f busy=%.2f busy_pct=%.1f "
                       "busy_p50=%.2f busy_p95=%.2f busy_max=%.2f busy_over_16.7=%u/%u",
                       wall / 1e9, (unsigned long long)s.updates, per_update ? "update" : "second",
                       wall / 1e6 / div, busy / 1e6 / div, wall ? 100.0 * (double)busy / (double)wall : 0.0,
                       p50, p95, max, over, n);
    const int extensions = atomic_load_explicit(&s_extension_count, memory_order_acquire);
    for (int c = 0; c < RSX_FIFO_BUSY_BUILTIN_COUNT + extensions && len > 0 && (size_t)len < sizeof line; ++c) {
        if (c > RSX_FIFO_BUSY_OTHER && !is_idle(c) && !s.acc_ns[c] && !s.spans[c]) continue;
        const char* name = c < RSX_FIFO_BUSY_BUILTIN_COUNT ? s_builtin_names[c]
                                                         : s_extension_names[c - RSX_FIFO_BUSY_BUILTIN_COUNT];
        len += snprintf(line + len, sizeof line - (size_t)len, " %s=%.2f/%.1f", name, s.acc_ns[c] / 1e6 / div,
                        (double)s.spans[c] / div);
    }
    if (len > 0 && (size_t)len < sizeof line)
        snprintf(line + len, sizeof line - (size_t)len, " unbalanced=%llu overflow=%llu",
                 (unsigned long long)s.unbalanced, (unsigned long long)s.overflow);
    s_sink(line);
    /* Carry the idle time since the last flip into the next period, so the
     * update that straddles the report keeps its own busy figure (modular). */
    s.idle_at_flip_ns = 0 - (idle - s.idle_at_flip_ns);
    memset(s.acc_ns, 0, sizeof s.acc_ns);
    memset(s.spans, 0, sizeof s.spans);
    s.updates = 0; s.samples = 0; s.unbalanced = 0; s.overflow = 0;
    s.period_start_ns = now;
}

void rsx_fifo_busy_idle_begin(void)
{
    if (!rsx_fifo_busy_enabled()) return;
    if (!t_owner) {
        int expected = 0;
        if (!atomic_compare_exchange_strong(&s_owner_claimed, &expected, 1)) return;
        t_owner = 1;
        s.last_ns = s.period_start_ns = s_clock();
    }
    static const int idle_of_stop[] = {RSX_FIFO_BUSY_IDLE_PUT, RSX_FIFO_BUSY_IDLE_SEMA,
                                       RSX_FIFO_BUSY_IDLE_WRITER, RSX_FIFO_BUSY_IDLE_OTHER};
    push(idle_of_stop[s.stop]);
}

void rsx_fifo_busy_idle_end(void)
{
    if (!owner()) return;
    const int top = s.depth ? s.stack[s.depth - 1] : -1;
    if (top >= 0 && is_idle(top)) pop(top);
    else charge();
    if (s.depth == 0 && !s.ignored && s.last_ns - s.period_start_ns >= s_period_ns) report(s.last_ns);
}
