/* SPU interpreter time attribution; see spu_interp_attribution.h. Included by
 * spu_interp.c so every build or test of the interpreter carries it. */
#include "spu_interp_attribution.h"

#ifdef _WIN32
/* The attribution sink uses POSIX threads; Windows builds keep it disabled. */
int spu_interp_attr_enabled(void) { return 0; }
void spu_interp_attr_begin(const spu_context* c, uint32_t e, int r, const void* f) { (void)c; (void)e; (void)r; (void)f; }
void spu_interp_attr_add_steps(uint64_t s) { (void)s; }
void spu_interp_attr_end(const void* f) { (void)f; }
void spu_interp_attr_unwind(void) {}
int spu_interp_attr_flush(void) { return -1; }
const char* spu_interp_attr_reason_name(int r) { (void)r; return "?"; }
#else

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IATTR_SLOTS 2048u

typedef struct {
    int image_id, policy_mode, resident_ovl, resident_task, code_image, reason;
    uint32_t code_source_ea, entry, ls1330, code_size;
    uint64_t entry_hash, ls4000_hash;
    uint8_t first16[16];
} iattr_key;

typedef struct {
    int used;
    iattr_key key;
    uint64_t wall_ns, steps, entries;
} iattr_row;

static atomic_int s_mode;              /* 0 unread, 1 off, 2 on */
static char s_path[1024];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static iattr_row s_rows[IATTR_SLOTS];
static uint64_t s_dropped, s_unwound, s_start_ns, s_reason_ns[SPU_IATTR_COUNT];
static pthread_once_t s_thread_once = PTHREAD_ONCE_INIT;

static _Thread_local struct {
    int active;
    const void* frame;
    uint64_t start_ns, steps;
    iattr_key key;
} t_run;

static const char* const s_reason_names[SPU_IATTR_COUNT] = {
    "direct", "unlifted", "other-image", "verified-mismatch",
    "verified-no-entry", "verified-ambiguous", "resident-no-entry",
};

const char* spu_interp_attr_reason_name(int reason)
{
    return reason >= 0 && reason < SPU_IATTR_COUNT ? s_reason_names[reason] : "?";
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void* report_thread(void* unused)
{
    (void)unused;
    for (;;) {
        struct timespec d = {2, 0};
        while (nanosleep(&d, &d) && errno == EINTR) {}
        spu_interp_attr_flush();
    }
    return NULL;
}

static void start_thread(void)
{
    pthread_t thread;
    if (!pthread_create(&thread, NULL, report_thread, NULL)) pthread_detach(thread);
    atexit((void (*)(void))spu_interp_attr_flush);
}

int spu_interp_attr_enabled(void)
{
    int mode = atomic_load_explicit(&s_mode, memory_order_acquire);
    if (mode) return mode == 2;
    const char* path = getenv("SPU_INTERP_ATTRIBUTION");
    int on = path && *path && strlen(path) < sizeof s_path - 8;
    if (on) {
        pthread_mutex_lock(&s_lock);
        if (!s_start_ns) { snprintf(s_path, sizeof s_path, "%s", path); s_start_ns = now_ns(); }
        pthread_mutex_unlock(&s_lock);
    }
    int expected = 0;
    atomic_compare_exchange_strong(&s_mode, &expected, on ? 2 : 1);
    return atomic_load_explicit(&s_mode, memory_order_acquire) == 2;
}

static uint64_t fnv64(const uint8_t* p, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void make_key(iattr_key* k, const spu_context* ctx, uint32_t entry, int reason)
{
    memset(k, 0, sizeof *k);
    entry &= SPU_LS_MASK & ~3u;
    k->image_id = ctx->image_id; k->policy_mode = ctx->policy_mode;
    k->resident_ovl = ctx->resident_ovl; k->resident_task = ctx->resident_task;
    k->reason = reason; k->entry = entry;
    for (unsigned i = 0; i < 4; i++) {
        const uint32_t lsa = ctx->resident_code[i].lsa, size = ctx->resident_code[i].size;
        if (ctx->resident_code[i].image_id && entry >= lsa && entry - lsa < size) {
            k->code_image = ctx->resident_code[i].image_id;
            k->code_source_ea = ctx->resident_code[i].source_ea;
            k->code_size = size;
            break;
        }
    }
    const uint8_t* ls = ctx->ls;
    if (!ls) return;
    const uint32_t span = entry <= SPU_LS_SIZE - 64u ? 64u : SPU_LS_SIZE - entry;
    k->entry_hash = fnv64(ls + entry, span);
    memcpy(k->first16, ls + entry, span < 16u ? span : 16u);
    if (ctx->policy_mode) {
        /* Policy-module bookkeeping word and the job-code buffer head: hints
         * for identifying the streamed job module offline, not ownership. */
        k->ls1330 = be32(ls + 0x1330);
        k->ls4000_hash = fnv64(ls + 0x4000, 64);
    }
}

static void record(const iattr_key* k, uint64_t wall, uint64_t steps)
{
    uint64_t h = fnv64((const uint8_t*)k, sizeof *k);
    pthread_mutex_lock(&s_lock);
    if (k->reason >= 0 && k->reason < SPU_IATTR_COUNT) s_reason_ns[k->reason] += wall;
    for (unsigned probe = 0; probe < IATTR_SLOTS; probe++) {
        iattr_row* r = &s_rows[(h + probe) & (IATTR_SLOTS - 1)];
        if (!r->used) { r->used = 1; r->key = *k; }
        else if (memcmp(&r->key, k, sizeof *k)) continue;
        r->wall_ns += wall; r->steps += steps; r->entries++;
        pthread_mutex_unlock(&s_lock);
        return;
    }
    s_dropped++;
    pthread_mutex_unlock(&s_lock);
}

static void close_run(int unwound)
{
    const uint64_t wall = now_ns() - t_run.start_ns;
    t_run.active = 0;
    record(&t_run.key, wall, t_run.steps);
    if (unwound) {
        pthread_mutex_lock(&s_lock); s_unwound++; pthread_mutex_unlock(&s_lock);
    }
}

void spu_interp_attr_begin(const spu_context* ctx, uint32_t entry, int reason, const void* frame)
{
    if (!ctx || !spu_interp_attr_enabled()) return;
    pthread_once(&s_thread_once, start_thread);
    if (t_run.active) {
        if ((uintptr_t)frame < (uintptr_t)t_run.frame) return;   /* nested in the open run */
        close_run(1);                                             /* open run was unwound */
    }
    make_key(&t_run.key, ctx, entry, reason);
    t_run.frame = frame; t_run.steps = 0; t_run.active = 1;
    t_run.start_ns = now_ns();
}

void spu_interp_attr_add_steps(uint64_t steps)
{
    if (t_run.active) t_run.steps += steps;
}

void spu_interp_attr_end(const void* frame)
{
    if (t_run.active && t_run.frame == frame) close_run(0);
}

void spu_interp_attr_unwind(void)
{
    if (t_run.active) close_run(1);
}

static int by_wall(const void* a, const void* b)
{
    const iattr_row *x = a, *y = b;
    return x->wall_ns < y->wall_ns ? 1 : x->wall_ns > y->wall_ns ? -1 : 0;
}

int spu_interp_attr_flush(void)
{
    if (atomic_load_explicit(&s_mode, memory_order_acquire) != 2) return -1;
    iattr_row* rows = malloc(sizeof s_rows);
    if (!rows) return -1;
    uint64_t reasons[SPU_IATTR_COUNT], dropped, unwound, total = 0, steps = 0, entries = 0;
    pthread_mutex_lock(&s_lock);
    memcpy(rows, s_rows, sizeof s_rows);
    memcpy(reasons, s_reason_ns, sizeof reasons);
    dropped = s_dropped; unwound = s_unwound;
    const uint64_t elapsed = now_ns() - s_start_ns;
    char path[sizeof s_path], tmp[sizeof s_path + 8];
    memcpy(path, s_path, sizeof path);
    pthread_mutex_unlock(&s_lock);
    unsigned n = 0;
    for (unsigned i = 0; i < IATTR_SLOTS; i++)
        if (rows[i].used) { rows[n++] = rows[i]; total += rows[i].wall_ns; steps += rows[i].steps; entries += rows[i].entries; }
    qsort(rows, n, sizeof *rows, by_wall);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) { free(rows); return -1; }
    fprintf(f, "# spu-interp-attribution v1\n");
    fprintf(f, "elapsed_ns=%llu interp_wall_ns=%llu steps=%llu entries=%llu rows=%u dropped=%llu unwound=%llu\n",
            (unsigned long long)elapsed, (unsigned long long)total, (unsigned long long)steps,
            (unsigned long long)entries, n, (unsigned long long)dropped, (unsigned long long)unwound);
    for (int r = 0; r < SPU_IATTR_COUNT; r++)
        fprintf(f, "reason %s wall_ns=%llu\n", s_reason_names[r], (unsigned long long)reasons[r]);
    fprintf(f, "wall_ns\tsteps\tentries\treason\timage\tpolicy\tovl\ttask\tcode_image\tcode_src_ea\tcode_size\tentry\tls1330\tentry_hash\tls4000_hash\tfirst16\n");
    for (unsigned i = 0; i < n; i++) {
        const iattr_key* k = &rows[i].key;
        char hex[33];
        for (int b = 0; b < 16; b++) snprintf(hex + 2 * b, 3, "%02X", k->first16[b]);
        fprintf(f, "%llu\t%llu\t%llu\t%s\t%d\t%d\t%d\t%d\t%d\t0x%08X\t0x%X\t0x%05X\t0x%08X\t%016llx\t%016llx\t%s\n",
                (unsigned long long)rows[i].wall_ns, (unsigned long long)rows[i].steps,
                (unsigned long long)rows[i].entries, spu_interp_attr_reason_name(k->reason),
                k->image_id, k->policy_mode, k->resident_ovl, k->resident_task, k->code_image,
                k->code_source_ea, k->code_size, k->entry, k->ls1330,
                (unsigned long long)k->entry_hash, (unsigned long long)k->ls4000_hash, hex);
    }
    int failed = ferror(f) != 0;
    if (fclose(f)) failed = 1;
    if (!failed && rename(tmp, path)) failed = 1;
    free(rows);
    return failed ? -1 : 0;
}
#endif
