/*
 * ps3_tprof -- an in-process sampling profile of chosen host threads.
 *
 * PS3_TPROF=<path> samples every thread that called ps3_tprof_thread() (SPU
 * threads, PPU threads) once per PS3_TPROF_INTERVAL_US (default 1000) for
 * PS3_TPROF_SECONDS (default 10), starting when PS3_TPROF_TRIGGER=<file>
 * exists (immediately when unset). Each sample is the interrupted pc and the
 * frame-pointer chain, taken by a SIGPROF handler on the sampled thread. For
 * hosts where an external sampler cannot attach to the process.
 *
 * <path> gets one sample per line:
 *   <kind> <who> <t us> <pc> <return addresses...>
 * kind is S (SPU, who = SPU id) or P (PPU, who = thread id). Main-image
 * addresses are written image-relative in hex for offline symbolizing; frames
 * in other images are named "@symbol" with dladdr. Times are on the
 * PS3_FTRACE clock and relative to its base when it is recording, so samples
 * can be joined with the job spans of the same run.
 *
 * Header-only and weak-shared like ps3_ftrace.h, so the hooked files keep
 * building on their own in self-contained tests. Off (the default),
 * ps3_tprof_thread() reads the environment once and returns. macOS arm64;
 * elsewhere it compiles to nothing.
 */
#ifndef PS3EMU_PS3_TPROF_H
#define PS3EMU_PS3_TPROF_H

#include <stdint.h>
#include "ps3emu/ps3_ftrace.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <mach-o/dyld.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PS3_TPROF_THREADS = 64, PS3_TPROF_DEPTH = 40 };

typedef struct {
    uint64_t t;
    uint32_t who;
    uint8_t kind, depth;
    uintptr_t pc[PS3_TPROF_DEPTH];
} ps3_tprof_rec;

typedef struct {
    int on;                       /* 0 unknown, 1 initialising, 2 armed, 3 off/done */
    int nthreads;                 /* slots reserved; a slot is used once ready */
    uint32_t cap, n;
    ps3_tprof_rec* buf;
    struct { pthread_t th; uintptr_t lo, hi; uint32_t who; uint8_t kind; int ready; } th[PS3_TPROF_THREADS];
} ps3_tprof_state;

PS3_FT_SHARED ps3_tprof_state g_ps3_tprof = { 0 };

static inline void ps3_tprof_handler(int sig, siginfo_t* si, void* ctx)
{
    (void)sig; (void)si;
    ps3_tprof_state* s = &g_ps3_tprof;
    const pthread_t self = pthread_self();
    const int nt = __atomic_load_n(&s->nthreads, __ATOMIC_ACQUIRE);
    int k = 0;
    while (k < nt && k < PS3_TPROF_THREADS && !(__atomic_load_n(&s->th[k].ready, __ATOMIC_ACQUIRE) && pthread_equal(s->th[k].th, self))) k++;
    if (k >= nt || k >= PS3_TPROF_THREADS) return;
    const uint32_t i = __atomic_fetch_add(&s->n, 1, __ATOMIC_RELAXED);
    if (i >= s->cap) return;
    ps3_tprof_rec* r = &s->buf[i];
    const ucontext_t* uc = (const ucontext_t*)ctx;
    unsigned d = 0;
    r->pc[d++] = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    r->pc[d++] = (uintptr_t)uc->uc_mcontext->__ss.__lr;   /* the caller of a leaf without a frame */
    uintptr_t fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
    const uintptr_t lo = s->th[k].lo, hi = s->th[k].hi;
    while (d < PS3_TPROF_DEPTH && fp >= lo && fp + 16 <= hi && !(fp & 7u)) {
        const uintptr_t next = ((const uintptr_t*)fp)[0], ret = ((const uintptr_t*)fp)[1];
        if (!ret) break;
        r->pc[d++] = ret;
        if (next <= fp) break;
        fp = next;
    }
    r->depth = (uint8_t)d;
    r->who = s->th[k].who;
    r->kind = s->th[k].kind;
    r->t = ps3_ft_now();
}

static inline void ps3_tprof_write(const char* out, uint32_t n)
{
    FILE* f = fopen(out, "w");
    if (!f) { fprintf(stderr, "[tprof] cannot write %s\n", out); return; }
    const intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    Dl_info self_info; uintptr_t self_base = 0;
    if (dladdr(_dyld_get_image_header(0), &self_info)) self_base = (uintptr_t)self_info.dli_fbase;
    const uint64_t base = g_ps3_ft.base;
    fprintf(f, "# tprof samples %u slide 0x%llx ftrace_base %d image %s\n", n,
            (unsigned long long)slide, base != 0, _dyld_get_image_name(0));
    for (uint32_t i = 0; i < n; i++) {
        const ps3_tprof_rec* r = &g_ps3_tprof.buf[i];
        if (!r->depth) continue;
        fprintf(f, "%c %u %.3f", r->kind, r->who, (double)(r->t - (base ? base : 0)) / 1e3);
        for (unsigned d = 0; d < r->depth; d++) {
            /* Slot 1 is the link register: a return address like the rest
             * of the chain, but it may repeat the first frame's. */
            const uintptr_t pc = r->pc[d];
            Dl_info info;
            if (dladdr((const void*)pc, &info) && (uintptr_t)info.dli_fbase != self_base && info.dli_sname)
                fprintf(f, " @%s", info.dli_sname);
            else
                fprintf(f, " %llx", (unsigned long long)(pc - (uintptr_t)slide));
        }
        fputc('\n', f);
    }
    fclose(f);
    fprintf(stderr, "[tprof] %u samples -> %s\n", n, out);
}

static inline void* ps3_tprof_sampler(void* arg)
{
    (void)arg;
    ps3_tprof_state* s = &g_ps3_tprof;
    const char* out = getenv("PS3_TPROF");
    const char* trig = getenv("PS3_TPROF_TRIGGER");
    const char* se = getenv("PS3_TPROF_SECONDS");
    const char* ie = getenv("PS3_TPROF_INTERVAL_US");
    const double secs = se && atof(se) > 0 ? atof(se) : 10.0;
    const long iv_us = ie && atol(ie) > 0 ? atol(ie) : 1000;
    struct timespec poll = {0, 100000000}, tick = {0, iv_us * 1000};
    while (trig && *trig && access(trig, F_OK) != 0) nanosleep(&poll, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ps3_tprof_handler; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    const uint64_t end = ps3_ft_now() + (uint64_t)(secs * 1e9);
    fprintf(stderr, "[tprof] sampling %d threads every %ld us for %.1f s\n",
            __atomic_load_n(&s->nthreads, __ATOMIC_ACQUIRE), iv_us, secs);
    while (ps3_ft_now() < end && __atomic_load_n(&s->n, __ATOMIC_RELAXED) < s->cap) {
        const int nt = __atomic_load_n(&s->nthreads, __ATOMIC_ACQUIRE);
        for (int k = 0; k < nt && k < PS3_TPROF_THREADS; k++)
            if (__atomic_load_n(&s->th[k].ready, __ATOMIC_ACQUIRE)) pthread_kill(s->th[k].th, SIGPROF);
        nanosleep(&tick, NULL);
    }
    nanosleep(&poll, NULL);   /* let in-flight handlers finish */
    uint32_t n = __atomic_load_n(&s->n, __ATOMIC_ACQUIRE);
    if (n > s->cap) n = s->cap;
    ps3_tprof_write(out, n);
    __atomic_store_n(&s->on, 3, __ATOMIC_RELEASE);
    return NULL;
}

static inline int ps3_tprof_init(void)
{
    ps3_tprof_state* s = &g_ps3_tprof;
    int expect = 0;
    if (!__atomic_compare_exchange_n(&s->on, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (expect == 1) expect = __atomic_load_n(&s->on, __ATOMIC_ACQUIRE);
        return expect == 2;
    }
    int on = 3;
    const char* out = getenv("PS3_TPROF");
    if (out && *out) {
        const char* c = getenv("PS3_TPROF_CAP");
        s->cap = c && atol(c) > 0 ? (uint32_t)atol(c) : 400000u;
        s->buf = (ps3_tprof_rec*)calloc(s->cap, sizeof(ps3_tprof_rec));
        if (s->buf) {
            pthread_t t;
            if (!pthread_create(&t, NULL, ps3_tprof_sampler, NULL)) { pthread_detach(t); on = 2; }
        }
        if (on != 2) fprintf(stderr, "[tprof] could not start\n");
    }
    __atomic_store_n(&s->on, on, __ATOMIC_RELEASE);
    return on == 2;
}

/* Add the calling thread to the sampled set: kind 'S' or 'P', who its id.
 * A registered thread calls ps3_tprof_thread_exit() before it ends. */
static inline void ps3_tprof_thread(char kind, uint32_t who)
{
    ps3_tprof_state* s = &g_ps3_tprof;
    int on = __atomic_load_n(&s->on, __ATOMIC_ACQUIRE);
    if (__builtin_expect(on == 3, 1)) return;
    if (on != 2 && !ps3_tprof_init()) return;
    const pthread_t self = pthread_self();
    /* Each header copy has its own statics, so slots are claimed atomically
     * from the shared state rather than under a lock. */
    const int k = __atomic_fetch_add(&s->nthreads, 1, __ATOMIC_ACQ_REL);
    if (k >= PS3_TPROF_THREADS) return;
    s->th[k].th = self;
    s->th[k].hi = (uintptr_t)pthread_get_stackaddr_np(self);
    s->th[k].lo = s->th[k].hi - pthread_get_stacksize_np(self);
    s->th[k].who = who;
    s->th[k].kind = (uint8_t)kind;
    __atomic_store_n(&s->th[k].ready, 1, __ATOMIC_RELEASE);
}

static inline void ps3_tprof_thread_exit(void)
{
    ps3_tprof_state* s = &g_ps3_tprof;
    if (__builtin_expect(__atomic_load_n(&s->on, __ATOMIC_ACQUIRE) != 2, 1)) return;
    const pthread_t self = pthread_self();
    const int nt = __atomic_load_n(&s->nthreads, __ATOMIC_ACQUIRE);
    for (int k = 0; k < nt && k < PS3_TPROF_THREADS; k++)
        if (__atomic_load_n(&s->th[k].ready, __ATOMIC_ACQUIRE) && pthread_equal(s->th[k].th, self))
            __atomic_store_n(&s->th[k].ready, 0, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
}
#endif

#else
static inline void ps3_tprof_thread(char kind, uint32_t who) { (void)kind; (void)who; }
static inline void ps3_tprof_thread_exit(void) {}
#endif

#endif
