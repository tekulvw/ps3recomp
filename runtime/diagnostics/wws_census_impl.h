/* Opt-in, read-only observers of a SPURS job-manager policy module's guest
 * interface. Included once, by runtime/spu/spurs_policy.c, so they add no link
 * dependency to any fixture; the policy runner calls wws_census_note_run
 * before each policy run when either variable below is set. Off (the
 * default), nothing here runs.
 *
 * Queue census: PS3RECOMP_WWS_CENSUS=<policy image id>[:<start s>:<seconds>]
 *   A background thread samples, every PS3RECOMP_WWS_CENSUS_US microseconds
 *   (default 250), the workload data block of every workload run by that
 *   policy image. It prints the block's first 256 bytes once, every change of
 *   the four 16-byte progress rows at +0x40, the 8-byte entries of the slot
 *   array named by +0x30+4*q between the old and the new first halfword of
 *   row q (slot = array + 8*index), the 16 bytes each entry's second word
 *   points to, when a posted entry's slot later changes, and the 16-byte
 *   targets of entries whose first word is below 0x100, tracked for changes.
 *   PS3RECOMP_WWS_CENSUS_DIR=<dir> also saves the first 1 KiB that other
 *   entries point to (at most 8 per workload and first word, 4096 in all).
 *   PS3RECOMP_WWS_CENSUS_MAX=<lines> caps the output (default 200000).
 *
 * Job-interface census: PS3RECOMP_WWS_API=<policy image id>[:<start s>:<seconds>]
 *   Classifies each indirect transfer of a context running that policy as
 *   kernel (< 0xA00), policy (the policy's load span), stub (>= 0x3FE00) or
 *   job (anything else) and reports a job's entry registers, each call from a
 *   job into the policy span with its argument registers, each such call's
 *   return with result registers and duration, the job's final transfer back,
 *   and interrupts taken while a job runs. PS3RECOMP_WWS_API_SAMPLES=<n>
 *   (default 16) bounds the full lines per (kind, job image, target); every
 *   event is counted in a summary printed when the window ends and at exit.
 *   PS3RECOMP_WWS_JOBIO_DIR=<dir> arms the job I/O recorder
 *   (job_io_recorder.h) for the first run of each job image in the window.
 *
 * The window counts from the first policy run. Neither census writes guest
 * memory, local store or registers, or changes a scheduling decision; their
 * locks and output do perturb timing. Multi-word samples are not atomic. */
#ifndef PS3_WWS_CENSUS_IMPL_H
#define PS3_WWS_CENSUS_IMPL_H
#include "job_io_recorder.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif defined(__linux__)
#include <sys/types.h>
#include <sys/uio.h>
extern ssize_t process_vm_readv(pid_t, const struct iovec*, unsigned long, const struct iovec*, unsigned long, unsigned long);
#endif

extern uint8_t* vm_base;

/* The job I/O recorder is linked wherever an SPU channel unit is; a fixture
 * built without it gets these refusing defaults. */
#if defined(_MSC_VER)
int ps3_jobio_begin_wws_absent(struct spu_context* c, uint32_t t, const char* d) { (void)c; (void)t; (void)d; return -1; }
int ps3_jobio_end_wws_absent(struct spu_context* c) { (void)c; return -1; }
#pragma comment(linker, "/alternatename:ps3_jobio_begin=ps3_jobio_begin_wws_absent")
#pragma comment(linker, "/alternatename:ps3_jobio_end=ps3_jobio_end_wws_absent")
#else
__attribute__((weak)) int ps3_jobio_begin(struct spu_context* c, uint32_t t, const char* d) { (void)c; (void)t; (void)d; return -1; }
__attribute__((weak)) int ps3_jobio_end(struct spu_context* c) { (void)c; return -1; }
#endif

/* ---- helpers ---------------------------------------------------------------- */
static uint64_t wws_now_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);   /* the ftrace / critical-trace clock */
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

#ifdef _WIN32
static CRITICAL_SECTION wws_mu;
static INIT_ONCE wws_mu_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK wws_mu_init(PINIT_ONCE o, PVOID p, PVOID* c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&wws_mu); return TRUE; }
static void wws_lock(void) { InitOnceExecuteOnce(&wws_mu_once, wws_mu_init, NULL, NULL); EnterCriticalSection(&wws_mu); }
static void wws_unlock(void) { LeaveCriticalSection(&wws_mu); }
#else
static pthread_mutex_t wws_mu = PTHREAD_MUTEX_INITIALIZER;
static void wws_lock(void) { pthread_mutex_lock(&wws_mu); }
static void wws_unlock(void) { pthread_mutex_unlock(&wws_mu); }
#endif

/* Reads guest memory that may be unmapped without faulting: an entry word is
 * only presumed to be a pointer. Returns 0 on success. */
static int wws_guest_read(uint32_t ea, void* dst, uint32_t n)
{
    if (!vm_base || !n || (uint64_t)ea + n > 0x100000000ull) return -1;
    const uint8_t* src = vm_base + ea;
#if defined(_WIN32)
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n ? 0 : -1;
#elif defined(__APPLE__)
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)src, n,
                                  (mach_vm_address_t)(uintptr_t)dst, &got) == KERN_SUCCESS && got == n ? 0 : -1;
#elif defined(__linux__)
    struct iovec l = { dst, n }, r = { (void*)src, n };
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
#else
    memcpy(dst, src, n); return 0;
#endif
}
static uint32_t wws_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t wws_be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

static FILE* wws_stream;
static unsigned long wws_lines, wws_max_lines = 200000, wws_dropped;
static void wws_out(const char* fmt, ...)
{
    if (wws_lines >= wws_max_lines) { wws_dropped++; return; }
    wws_lines++;
    va_list ap; va_start(ap, fmt);
    vfprintf(wws_stream ? wws_stream : stderr, fmt, ap);
    va_end(ap);
}

/* <image>[:<start s>:<seconds>] */
struct wws_window { int image; double start, secs; uint64_t from, to; };
static void wws_parse_window(const char* e, struct wws_window* w)
{
    memset(w, 0, sizeof *w);
    if (!e || !*e) return;
    char* end = NULL;
    const long v = strtol(e, &end, 0);
    if (end == e || v <= 0) return;
    w->image = (int)v;
    if (!(*end == ':' && sscanf(end + 1, "%lf:%lf", &w->start, &w->secs) == 2 && w->secs > 0)) w->start = w->secs = 0;
}
static void wws_window_open(struct wws_window* w, uint64_t t)
{
    w->from = t + (uint64_t)(w->start * 1e9);
    w->to = w->secs > 0 ? w->from + (uint64_t)(w->secs * 1e9) : UINT64_MAX;
}
static int wws_in_window(const struct wws_window* w, uint64_t t) { return w->image && t >= w->from && t < w->to; }

static struct wws_window wws_qw, wws_aw;      /* queue census, job-interface census */
static int wws_env_read, wws_qdone, wws_adone;
static unsigned wws_poll_us = 250, wws_api_samples = 16;
static char wws_dump_dir[512], wws_jobio_dir[512];

/* ---- queue census ------------------------------------------------------------ */
enum { WWS_MAX_CONT = 32, WWS_MAX_PENDING = 1024, WWS_MAX_GATES = 128, WWS_MAX_DUMP_KEYS = 512 };
struct wws_cont {
    uint32_t b, wid, spurs; int image, seen;
    uint32_t cap[4], base[4];
    uint8_t row[4][16];
    int row_valid[4];
};
struct wws_pending { uint32_t wid, q, idx, ea, w0, w1; uint64_t t; };
struct wws_gate { uint32_t ea; uint8_t v[16]; uint64_t t_change; };
struct wws_dump_key { uint32_t wid, w0, n; };
static struct wws_cont wws_conts[WWS_MAX_CONT];
static unsigned wws_nconts;
static struct wws_pending wws_pend[WWS_MAX_PENDING];
static unsigned wws_npend;
static struct wws_gate wws_gates[WWS_MAX_GATES];
static unsigned wws_ngates;
static struct wws_dump_key wws_dkeys[WWS_MAX_DUMP_KEYS];
static unsigned wws_ndkeys, wws_ndumps;
/* Last workload data seen per (instance slot, wid): a policy run costs a compare. */
static uint64_t wws_last_data[8][16];
static uint32_t wws_last_spurs[8];

static void wws_dump_target(const struct wws_cont* c, unsigned q, uint32_t idx, uint32_t w0, uint32_t w1)
{
    if (!wws_dump_dir[0] || wws_ndumps >= 4096) return;
    struct wws_dump_key* k = NULL;
    for (unsigned i = 0; i < wws_ndkeys; i++)
        if (wws_dkeys[i].wid == c->wid && wws_dkeys[i].w0 == w0) { k = &wws_dkeys[i]; break; }
    if (!k) {
        if (wws_ndkeys >= WWS_MAX_DUMP_KEYS) return;
        k = &wws_dkeys[wws_ndkeys++]; k->wid = c->wid; k->w0 = w0; k->n = 0;
    }
    if (k->n >= 8) return;
    static uint8_t buf[1024];
    uint32_t n = sizeof buf;
    if (wws_guest_read(w1, buf, n)) { n = 256; if (wws_guest_read(w1, buf, n)) return; }
    char path[700];
    snprintf(path, sizeof path, "%s/wws-entry-w%02u-q%u-%04X-%08X-%08X.bin", wws_dump_dir, c->wid, q, idx, w0, w1);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(buf, 1, n, f); fclose(f);
    k->n++; wws_ndumps++;
    wws_out("[wws-census] kind=dump wid=%u q=%u idx=0x%04X w0=%08X w1=%08X bytes=%u file=%s\n", c->wid, q, idx, w0, w1, n, path);
}

static void wws_gate_track(uint32_t ea, uint64_t t)
{
    for (unsigned i = 0; i < wws_ngates; i++) if (wws_gates[i].ea == ea) return;
    struct wws_gate g; g.ea = ea; g.t_change = t;
    if (wws_guest_read(ea, g.v, 16)) return;
    if (wws_ngates >= WWS_MAX_GATES) {   /* replace the longest-unchanged */
        unsigned old = 0;
        for (unsigned i = 1; i < wws_ngates; i++) if (wws_gates[i].t_change < wws_gates[old].t_change) old = i;
        wws_gates[old] = g;
    } else wws_gates[wws_ngates++] = g;
    wws_out("[wws-census] t=%llu kind=target-watch ea=%08X v=%08X.%08X.%08X.%08X\n", (unsigned long long)t, ea,
            wws_be32(g.v), wws_be32(g.v + 4), wws_be32(g.v + 8), wws_be32(g.v + 12));
}

static void wws_post_entries(struct wws_cont* c, unsigned q, uint16_t from, uint16_t to, uint64_t t)
{
    const unsigned d = (uint16_t)(to - from), n = d > 512 ? 512 : d;
    for (unsigned i = 0; i < n; i++) {
        const uint32_t idx = (uint16_t)(from + i), ea = c->base[q] + 8u * idx;
        uint8_t e[8], tg[16];
        if (wws_guest_read(ea, e, 8)) {
            wws_out("[wws-census] t=%llu kind=post-unreadable wid=%u q=%u idx=0x%04X ea=%08X\n",
                    (unsigned long long)t, c->wid, q, idx, ea);
            continue;
        }
        const uint32_t w0 = wws_be32(e), w1 = wws_be32(e + 4);
        if (w1 && !wws_guest_read(w1, tg, 16))
            wws_out("[wws-census] t=%llu kind=post wid=%u q=%u idx=0x%04X cap=0x%X w0=%08X w1=%08X tgt=%08X.%08X.%08X.%08X\n",
                    (unsigned long long)t, c->wid, q, idx, c->cap[q], w0, w1,
                    wws_be32(tg), wws_be32(tg + 4), wws_be32(tg + 8), wws_be32(tg + 12));
        else
            wws_out("[wws-census] t=%llu kind=post wid=%u q=%u idx=0x%04X cap=0x%X w0=%08X w1=%08X tgt=-\n",
                    (unsigned long long)t, c->wid, q, idx, c->cap[q], w0, w1);
        if (wws_npend < WWS_MAX_PENDING) {
            struct wws_pending* p = &wws_pend[wws_npend++];
            p->wid = c->wid; p->q = q; p->idx = idx; p->ea = ea; p->w0 = w0; p->w1 = w1; p->t = t;
        }
        if (w1) { if (w0 < 0x100) wws_gate_track(w1, t); else wws_dump_target(c, q, idx, w0, w1); }
    }
    if (n < d)
        wws_out("[wws-census] t=%llu kind=post-truncated wid=%u q=%u from=0x%04X to=0x%04X\n",
                (unsigned long long)t, c->wid, q, from, to);
}

static void wws_poll_container(struct wws_cont* c, uint64_t t)
{
    uint8_t h[256];
    if (wws_guest_read(c->b, h, sizeof h)) {   /* the rows end at +0x80 */
        memset(h, 0, sizeof h);
        if (wws_guest_read(c->b, h, 0x80)) return;
    }
    if (!c->seen) {
        c->seen = 1;
        wws_out("[wws-census] t=%llu kind=container wid=%u spurs=%08X image=%d b=%08X hdr=", (unsigned long long)t,
                c->wid, c->spurs, c->image, c->b);
        for (unsigned i = 0; i < 256; i += 4) wws_out("%08X%s", wws_be32(h + i), i + 4 < 256 ? "." : "\n");
    }
    for (unsigned q = 0; q < 4; q++) {
        const uint32_t cap = wws_be32(h + 0x20 + 4 * q), base = wws_be32(h + 0x30 + 4 * q);
        const uint8_t* row = h + 0x40 + 16 * q;
        if (cap != c->cap[q] || base != c->base[q]) {
            wws_out("[wws-census] t=%llu kind=array wid=%u q=%u cap=0x%X base=%08X\n", (unsigned long long)t, c->wid, q, cap, base);
            c->cap[q] = cap; c->base[q] = base;
        }
        if (c->row_valid[q] && !memcmp(row, c->row[q], 16)) continue;
        wws_out("[wws-census] t=%llu kind=row wid=%u q=%u h=%04X.%04X.%04X.%04X.%04X.%04X.%04X.%04X\n", (unsigned long long)t,
                c->wid, q, wws_be16(row), wws_be16(row + 2), wws_be16(row + 4), wws_be16(row + 6),
                wws_be16(row + 8), wws_be16(row + 10), wws_be16(row + 12), wws_be16(row + 14));
        if (c->row_valid[q] && base) {
            const uint16_t before = wws_be16(c->row[q]), after = wws_be16(row);
            if (before != after) {
                if ((uint16_t)(after - before) < 0x8000) wws_post_entries(c, q, before, after, t);
                else wws_out("[wws-census] t=%llu kind=reset wid=%u q=%u from=0x%04X to=0x%04X\n",
                             (unsigned long long)t, c->wid, q, before, after);
            }
        }
        memcpy(c->row[q], row, 16); c->row_valid[q] = 1;
    }
}

static void wws_poll_pending(uint64_t t)
{
    for (unsigned i = 0; i < wws_npend;) {
        const struct wws_pending* p = &wws_pend[i];
        uint8_t e[8];
        const int gone = wws_guest_read(p->ea, e, 8) != 0;
        const uint32_t w0 = gone ? 0 : wws_be32(e), w1 = gone ? 0 : wws_be32(e + 4);
        if (gone || w0 != p->w0 || w1 != p->w1)
            wws_out("[wws-census] t=%llu kind=slot wid=%u q=%u idx=0x%04X after_us=%llu w0=%08X w1=%08X\n", (unsigned long long)t,
                    p->wid, p->q, p->idx, (unsigned long long)((t - p->t) / 1000u), w0, w1);
        else if (t - p->t < 2000000000ull) { i++; continue; }
        else wws_out("[wws-census] t=%llu kind=slot-kept wid=%u q=%u idx=0x%04X w0=%08X w1=%08X\n", (unsigned long long)t,
                     p->wid, p->q, p->idx, p->w0, p->w1);
        wws_pend[i] = wws_pend[--wws_npend];
    }
    for (unsigned i = 0; i < wws_ngates; i++) {
        uint8_t v[16];
        if (wws_guest_read(wws_gates[i].ea, v, 16) || !memcmp(v, wws_gates[i].v, 16)) continue;
        wws_out("[wws-census] t=%llu kind=target ea=%08X v=%08X.%08X.%08X.%08X\n", (unsigned long long)t, wws_gates[i].ea,
                wws_be32(v), wws_be32(v + 4), wws_be32(v + 8), wws_be32(v + 12));
        memcpy(wws_gates[i].v, v, 16); wws_gates[i].t_change = t;
    }
}

/* One sampling pass (also the test seam). */
static void wws_census_poll_once(uint64_t t)
{
    wws_lock();
    if (wws_in_window(&wws_qw, t)) {
        for (unsigned i = 0; i < wws_nconts; i++) wws_poll_container(&wws_conts[i], t);
        wws_poll_pending(t);
    }
    wws_unlock();
}

/* ---- job-interface census ---------------------------------------------------- */
enum { WWS_MAX_CTX = 64, WWS_MAX_CALLS = 16, WWS_MAX_KEYS = 4096 };
enum { WWS_K_ENTRY, WWS_K_CALL, WWS_K_RET, WWS_K_LEAVE, WWS_K_IRQ, WWS_K_JOBJUMP, WWS_K_COUNT };
static const char* const wws_kind_name[WWS_K_COUNT] = { "entry", "call", "ret", "leave", "irq", "job-jump" };
struct wws_call { uint32_t link, target; uint64_t t; };
struct wws_actx {
    spu_context* ctx; uint32_t pm_hi;
    int in_job, job_img, jobio;
    uint32_t entry_pc; uint64_t entry_t;
    struct wws_call calls[WWS_MAX_CALLS]; unsigned depth;
};
struct wws_key { uint32_t kind, img, target, used; unsigned long long n, ns; };
static struct wws_actx wws_actxs[WWS_MAX_CTX];
static unsigned wws_nactx;
static struct wws_key wws_keys[WWS_MAX_KEYS];
static unsigned wws_nkeys;
static uint8_t wws_jobio_done[1024];

/* Appended under the lock and published by the count; afterwards an entry is
 * only touched by the thread running its context. */
static struct wws_actx* wws_actx_find(const spu_context* c)
{
    const unsigned n = __atomic_load_n(&wws_nactx, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < n; i++) if (wws_actxs[i].ctx == c) return &wws_actxs[i];
    return NULL;
}
/* Counts an event (lock held). Nonzero when it should also be printed. */
static int wws_key_count(uint32_t kind, uint32_t img, uint32_t target, uint64_t ns)
{
    uint32_t h = (kind * 2654435761u ^ img * 40503u ^ target) & (WWS_MAX_KEYS - 1);
    for (unsigned probe = 0; probe < WWS_MAX_KEYS; probe++, h = (h + 1) & (WWS_MAX_KEYS - 1)) {
        struct wws_key* k = &wws_keys[h];
        if (!k->used) {
            if (wws_nkeys >= WWS_MAX_KEYS * 3 / 4) return 0;
            k->used = 1; k->kind = kind; k->img = img; k->target = target; wws_nkeys++;
        }
        if (k->kind == kind && k->img == img && k->target == target) { k->n++; k->ns += ns; return k->n <= wws_api_samples; }
    }
    return 0;
}
static void wws_api_summary(const char* why)
{
    wws_lock();
    wws_out("[wws-api-sum] reason=%s keys=%u lines=%lu dropped=%lu\n", why, wws_nkeys, wws_lines, wws_dropped);
    for (unsigned i = 0; i < WWS_MAX_KEYS; i++) {
        const struct wws_key* k = &wws_keys[i];
        if (k->used)
            wws_out("[wws-api-sum] kind=%s img=%u target=0x%05X n=%llu mean_ns=%llu\n", wws_kind_name[k->kind], k->img,
                    k->target, k->n, k->n ? k->ns / k->n : 0ull);
    }
    fflush(wws_stream ? wws_stream : stderr);
    wws_unlock();
}
static void wws_api_atexit(void) { if (!wws_adone) { wws_adone = 1; wws_api_summary("exit"); } }

static char wws_region(const struct wws_actx* a, uint32_t pc)
{
    if (pc < 0xA00) return 'K';
    if (pc < a->pm_hi) return 'P';
    if (pc >= 0x3FE00) return 'S';
    return 'J';
}
static uint32_t wws_ls32(const spu_context* c, uint32_t lsa) { return wws_be32(c->ls + (lsa & SPU_LS_MASK)); }
#define WWS_Q(r) c->gpr[r]._u32[0], c->gpr[r]._u32[1], c->gpr[r]._u32[2], c->gpr[r]._u32[3]

static void wws_api_branch(spu_context* c)
{
    struct wws_actx* a = wws_actx_find(c);
    if (!a) return;
    const uint32_t pc = c->pc & SPU_LS_MASK, link = c->gpr[0]._u32[0] & SPU_LS_MASK;
    const char to = wws_region(a, pc);
    const int img = c->resident_ovl;
    /* The policy's own transfers and a job's internal ones take no lock. */
    if (!a->in_job && to != 'J') return;
    if (a->in_job && to == 'J' && img == a->job_img) return;
    const uint64_t t = wws_now_ns();
    if (!wws_in_window(&wws_aw, t)) return;
    wws_lock();
    if (c->irq_saved) {   /* the interrupt handler's own transfers */
        if (a->in_job) wws_key_count(WWS_K_IRQ, (uint32_t)a->job_img, pc | 0x80000000u, 0);
    } else if (to == 'J' && !a->in_job) {
        a->in_job = 1; a->job_img = img; a->entry_pc = pc; a->entry_t = t; a->depth = 0;
        if (wws_key_count(WWS_K_ENTRY, (uint32_t)img, pc, 0))
            wws_out("[wws-api] t=%llu kind=entry ctx=%p spu=%u wid=%u img=%d pc=0x%05X link=0x%05X sp=0x%05X int=%u "
                    "r3=%08X.%08X.%08X.%08X r4=%08X.%08X.%08X.%08X r5=%08X.%08X.%08X.%08X r6=%08X.%08X.%08X.%08X "
                    "r7=%08X r8=%08X r9=%08X r10=%08X\n", (unsigned long long)t, (void*)c, wws_ls32(c, 0x1C8), wws_ls32(c, 0x1DC),
                    img, pc, link, c->gpr[1]._u32[0] & SPU_LS_MASK, c->int_enable, WWS_Q(3), WWS_Q(4), WWS_Q(5), WWS_Q(6),
                    c->gpr[7]._u32[0], c->gpr[8]._u32[0], c->gpr[9]._u32[0], c->gpr[10]._u32[0]);
        if (wws_jobio_dir[0] && img > 0 && img < (int)sizeof wws_jobio_done && !wws_jobio_done[img] &&
            !ps3_jobio_begin(c, (uint32_t)img, wws_jobio_dir)) { wws_jobio_done[img] = 1; a->jobio = 1; }
    } else if (to == 'J') {
        wws_key_count(WWS_K_JOBJUMP, (uint32_t)img, pc, 0);
    } else if (wws_region(a, link) == 'J' && to != 'K') {
        if (wws_key_count(WWS_K_CALL, (uint32_t)a->job_img, pc, 0))
            wws_out("[wws-api] t=%llu kind=call ctx=%p img=%d target=0x%05X link=0x%05X sp=0x%05X int=%u depth=%u "
                    "r3=%08X.%08X.%08X.%08X r4=%08X.%08X.%08X.%08X r5=%08X.%08X.%08X.%08X r6=%08X r7=%08X r8=%08X\n",
                    (unsigned long long)t, (void*)c, a->job_img, pc, link, c->gpr[1]._u32[0] & SPU_LS_MASK, c->int_enable,
                    c->host_depth, WWS_Q(3), WWS_Q(4), WWS_Q(5), c->gpr[6]._u32[0], c->gpr[7]._u32[0], c->gpr[8]._u32[0]);
        if (a->depth < WWS_MAX_CALLS) { a->calls[a->depth].link = link; a->calls[a->depth].target = pc; a->calls[a->depth].t = t; }
        a->depth++;
    } else {
        const uint64_t dt = t - a->entry_t;
        if (wws_key_count(WWS_K_LEAVE, (uint32_t)a->job_img, pc, dt))
            wws_out("[wws-api] t=%llu kind=leave ctx=%p img=%d entry=0x%05X target=0x%05X link=0x%05X dt_ns=%llu open_calls=%u "
                    "r3=%08X.%08X.%08X.%08X\n", (unsigned long long)t, (void*)c, a->job_img, a->entry_pc, pc, link,
                    (unsigned long long)dt, a->depth, WWS_Q(3));
        if (a->jobio) { ps3_jobio_end(c); a->jobio = 0; }
        a->in_job = 0; a->depth = 0;
    }
    wws_unlock();
}

static void wws_api_drain_return(spu_context* c, uint32_t return_pc)
{
    struct wws_actx* a = wws_actx_find(c);
    if (!a || !a->in_job || !a->depth || a->depth > WWS_MAX_CALLS ||
        a->calls[a->depth - 1].link != (return_pc & SPU_LS_MASK)) return;
    const uint64_t t = wws_now_ns();
    wws_lock();
    const struct wws_call* k = &a->calls[--a->depth];
    const uint64_t dt = t - k->t;
    if (wws_key_count(WWS_K_RET, (uint32_t)a->job_img, k->target, dt))
        wws_out("[wws-api] t=%llu kind=ret ctx=%p img=%d target=0x%05X link=0x%05X dt_ns=%llu int=%u "
                "r3=%08X.%08X.%08X.%08X r4=%08X.%08X.%08X.%08X r5=%08X\n", (unsigned long long)t, (void*)c, a->job_img,
                k->target, k->link, (unsigned long long)dt, c->int_enable, WWS_Q(3), WWS_Q(4), c->gpr[5]._u32[0]);
    wws_unlock();
}

static void wws_api_irq_taken(spu_context* c)
{
    struct wws_actx* a = wws_actx_find(c);
    if (!a || !a->in_job) return;
    const uint64_t t = wws_now_ns();
    if (!wws_in_window(&wws_aw, t)) return;
    wws_lock();
    if (wws_key_count(WWS_K_IRQ, (uint32_t)a->job_img, c->srr0 & SPU_LS_MASK, 0))
        wws_out("[wws-api] t=%llu kind=irq ctx=%p img=%d srr0=0x%05X events=0x%X mask=0x%X depth=%u\n", (unsigned long long)t,
                (void*)c, a->job_img, c->srr0 & SPU_LS_MASK, c->event_status, c->event_mask, a->depth);
    wws_unlock();
}
#undef WWS_Q

static const spu_wws_observer wws_observer_vtbl = { wws_api_branch, wws_api_drain_return, wws_api_irq_taken };

/* ---- arming --------------------------------------------------------------------- */
static void wws_window_ends(uint64_t t)
{
    if (!wws_qdone && t >= wws_qw.to) {
        wws_qdone = 1;
        wws_lock();
        wws_out("[wws-census] kind=end containers=%u pending=%u targets=%u dumps=%u lines=%lu dropped=%lu\n",
                wws_nconts, wws_npend, wws_ngates, wws_ndumps, wws_lines, wws_dropped);
        fflush(wws_stream ? wws_stream : stderr);
        wws_unlock();
    }
    if (!wws_adone && t >= wws_aw.to) { wws_adone = 1; wws_api_summary("window"); }
}

#ifdef _WIN32
static DWORD WINAPI wws_census_thread(LPVOID p)
#else
static void* wws_census_thread(void* p)
#endif
{
    (void)p;
    while (!(wws_qdone && wws_adone)) {
        const uint64_t t = wws_now_ns();
        wws_census_poll_once(t);
        wws_window_ends(t);
#ifdef _WIN32
        Sleep(wws_poll_us >= 1000 ? wws_poll_us / 1000 : 1);
#else
        usleep(wws_poll_us);
#endif
    }
    return 0;
}

static void wws_census_start(uint64_t t)
{
    const char* e;
    wws_parse_window(getenv("PS3RECOMP_WWS_CENSUS"), &wws_qw);
    wws_parse_window(getenv("PS3RECOMP_WWS_API"), &wws_aw);
    if ((e = getenv("PS3RECOMP_WWS_CENSUS_US")) && atoi(e) > 0) wws_poll_us = (unsigned)atoi(e);
    if ((e = getenv("PS3RECOMP_WWS_CENSUS_MAX")) && atol(e) > 0) wws_max_lines = (unsigned long)atol(e);
    if ((e = getenv("PS3RECOMP_WWS_API_SAMPLES")) && atoi(e) >= 0) wws_api_samples = (unsigned)atoi(e);
    if ((e = getenv("PS3RECOMP_WWS_CENSUS_DIR"))) snprintf(wws_dump_dir, sizeof wws_dump_dir, "%s", e);
    if ((e = getenv("PS3RECOMP_WWS_JOBIO_DIR"))) snprintf(wws_jobio_dir, sizeof wws_jobio_dir, "%s", e);
    wws_window_open(&wws_qw, t); wws_window_open(&wws_aw, t);
    wws_qdone = !wws_qw.image; wws_adone = !wws_aw.image;
    if (wws_qdone && wws_adone) return;
    fprintf(stderr, "[wws-census] armed queue_image=%d api_image=%d poll_us=%u\n", wws_qw.image, wws_aw.image, wws_poll_us);
    if (wws_aw.image) atexit(wws_api_atexit);
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, wws_census_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
#else
    pthread_t th;
    if (!pthread_create(&th, NULL, wws_census_thread, NULL)) pthread_detach(th);
#endif
}

static void wws_note_container(uint32_t b, uint32_t wid, uint32_t spurs, int image)
{
    for (unsigned i = 0; i < wws_nconts; i++) if (wws_conts[i].b == b) { wws_conts[i].wid = wid; return; }
    if (wws_nconts >= WWS_MAX_CONT) return;
    struct wws_cont* c = &wws_conts[wws_nconts++];
    memset(c, 0, sizeof *c);
    c->b = b; c->wid = wid; c->spurs = spurs; c->image = image;
}

/* Before each policy run, after the context's entry state is set. */
static void wws_census_note_run(spu_context* ctx, int image_id, uint32_t pm_size,
                                uint64_t wkl_data, uint32_t wid, uint32_t spurs_ea)
{
    if (!__atomic_load_n(&wws_env_read, __ATOMIC_ACQUIRE)) {
        wws_lock();
        if (!wws_env_read) { wws_census_start(wws_now_ns()); __atomic_store_n(&wws_env_read, 1, __ATOMIC_RELEASE); }
        wws_unlock();
    }
    if (!wws_qdone && wws_qw.image == image_id && wid < 16) {
        unsigned slot = 8;
        for (unsigned i = 0; i < 8; i++)
            if (__atomic_load_n(&wws_last_spurs[i], __ATOMIC_RELAXED) == spurs_ea) { slot = i; break; }
        if (slot == 8 || __atomic_load_n(&wws_last_data[slot][wid], __ATOMIC_RELAXED) != wkl_data) {
            wws_lock();
            if (slot == 8)
                for (unsigned i = 0; i < 8; i++)
                    if (!wws_last_spurs[i] || wws_last_spurs[i] == spurs_ea) { wws_last_spurs[i] = spurs_ea; slot = i; break; }
            if (slot < 8) __atomic_store_n(&wws_last_data[slot][wid], wkl_data, __ATOMIC_RELAXED);
            if ((uint32_t)wkl_data) wws_note_container((uint32_t)wkl_data, wid, spurs_ea, image_id);
            wws_unlock();
        }
    }
    ctx->wws_observer = NULL;
    if (!wws_adone && wws_aw.image == image_id) {
        struct wws_actx* a = wws_actx_find(ctx);
        if (!a) {
            wws_lock();
            if (wws_nactx < WWS_MAX_CTX) {
                a = &wws_actxs[wws_nactx]; memset(a, 0, sizeof *a); a->ctx = ctx;
                __atomic_store_n(&wws_nactx, wws_nactx + 1, __ATOMIC_RELEASE);
            }
            wws_unlock();
        }
        if (a) {
            if (a->jobio) { ps3_jobio_end(ctx); a->jobio = 0; }
            a->pm_hi = 0xA00u + pm_size; a->in_job = 0; a->depth = 0;   /* a run starts in the policy */
            ctx->wws_observer = &wws_observer_vtbl;
        }
    }
}
#endif
