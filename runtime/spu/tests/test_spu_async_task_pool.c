/*
 * Async SPU tasks through the real dispatch (spu_workload.c), with
 * PS3RECOMP_SPU_TASK_POOL unset or =1: what a reused host thread must not
 * carry from one task into the next.
 *
 *  - Registrations keyed by a stack address. Each task leaks a stack-local
 *    spu_context into the lock-line reserver set and the MFC engine table, as
 *    a halt unwinding a nested context would. The next task on the same
 *    thread must find neither: its own context lands at the same address and
 *    would inherit the engine, and a PPU store would write into its frames.
 *  - Per-task thread-local state. A cri task (image 22) treats its first
 *    taskset EXIT as the bootstrap's and returns; a later EXIT halts it. Each
 *    task gets that first EXIT, also on a reused thread.
 *  - Results. The same with the pool off and on (tools/ compares them).
 *
 * Build and run: tools/test_spu_async_task_pool.py (POSIX).
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t g_guest[1u << 22];
uint8_t* vm_base = g_guest;
uint32_t ppu_vm_size = 0;

/* The registries under test, compiled in so the fixture can walk them. */
#include "../spu_channels.c"
#include "../spu_coherency.c"
#include "../spu_workload.h"
/* spu_task_thread_stack: the range spu_workload.c scrubs, from the same code. */
#include "../spu_task_pool.inc"

/* Runtime pieces a game build supplies. */
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void) { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0, g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) {}
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)ls; (void)a; (void)b; (void)c; (void)d; return 0; }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

static int g_pass, g_fail;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}

#define LINE       0x00100000u   /* the guest line every leaked context reserves */
#define MAX_TASKS  512u
static atomic_uint g_started, g_done, g_arrived, g_wave;
static atomic_uint g_inherited, g_no_stack, g_exit_returned, g_latch_timeout;
static uintptr_t g_leaks[MAX_TASKS];
static atomic_uint g_n_leaks;
static uint32_t g_result[MAX_TASKS];

/* Registrations whose context lies in [lo, hi). */
static unsigned registered_in(uintptr_t lo, uintptr_t hi)
{
    unsigned n = 0;
    spu_lockline_lock();
    for (spu_coh_entry* e = s_coh_ctxs; e; e = e->next)
        n += (uintptr_t)e->ctx >= lo && (uintptr_t)e->ctx < hi;
    spu_lockline_unlock();
    AcquireSRWLockShared(&s_mfc_claim_lock);
    for (spu_mfc_slot* s = s_mfc_slots; s; s = s->next)
        n += (uintptr_t)s->ctx >= lo && (uintptr_t)s->ctx < hi;
    ReleaseSRWLockShared(&s_mfc_claim_lock);
    return n;
}
static unsigned leaks_registered(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < atomic_load(&g_n_leaks); i++)
        n += registered_in(g_leaks[i], g_leaks[i] + 1);
    return n;
}

static __attribute__((noinline)) void leak_context(void)
{
    spu_context leak;   /* a stack local that never deregisters */
    spu_context_init(&leak, 0);
    spu_lockline_lock();
    spu_coh_reserve(&leak, LINE);
    spu_lockline_unlock();
    (void)mfc_for(&leak);
    const unsigned k = atomic_fetch_add(&g_n_leaks, 1);
    if (k < MAX_TASKS) g_leaks[k] = (uintptr_t)&leak;
}

/* The lifted entry of the leaking image. r3.word1 carries the task index. */
static void leak_entry(spu_context* c)
{
    const unsigned index = atomic_fetch_add(&g_started, 1);
    uintptr_t lo, hi;
    if (!spu_task_thread_stack(&lo, &hi)) atomic_fetch_add(&g_no_stack, 1);
    /* Nothing an earlier task on this thread left may still be registered;
     * the live job context `c` has not registered anything yet. */
    else if (registered_in(lo, hi)) atomic_fetch_add(&g_inherited, 1);
    leak_context();
    const unsigned wave = atomic_load(&g_wave);
    if (wave) {   /* everyone in the wave must be running at once */
        atomic_fetch_add(&g_arrived, 1);
        unsigned waited = 0;
        while (atomic_load(&g_arrived) < wave && waited < 10000) { sleep_ms(1); waited++; }
        if (waited >= 10000) atomic_fetch_add(&g_latch_timeout, 1);
    }
    uint32_t r = index * 0x9E3779B1u;
    for (unsigned i = 0; i < 64; i++) { c->ls[0x1000 + i] = (uint8_t)(r >> (i & 24)); r = r * 1664525u + 1013904223u + c->ls[0x1000 + i]; }
    if (index < MAX_TASKS) g_result[index] = r;
    atomic_fetch_add(&g_done, 1);
}

/* The lifted entry of the cri image (id 22): its first EXIT must return. */
static void cri_entry(spu_context* c)
{
    c->gpr[3]._u32[0] = 0;                  /* taskset syscall 0: EXIT */
    spu_spurs_taskset_syscall(c);           /* a halt longjmps past the next line */
    atomic_fetch_add(&g_exit_returned, 1);
    atomic_fetch_add(&g_done, 1);
}

static void put_be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
/* A minimal SPU ELF: one PT_LOAD of 16 bytes at LS 0x3000. `tag` makes the
 * fingerprint distinct per image. */
static uint32_t make_image(uint32_t ea, uint8_t tag)
{
    uint8_t* e = vm_base + ea;
    memset(e, 0, 100);
    memcpy(e, "\177ELF", 4); e[4] = 1; e[5] = 2; e[6] = 1;
    put_be16(e + 0x10, 2); put_be16(e + 0x12, 23); put_be32(e + 0x14, 1);
    put_be32(e + 0x18, 0x3000); put_be32(e + 0x1C, 52);
    put_be16(e + 0x28, 52); put_be16(e + 0x2A, 32); put_be16(e + 0x2C, 1);
    put_be32(e + 52, 1); put_be32(e + 56, 84); put_be32(e + 60, 0x3000);
    put_be32(e + 68, 16); put_be32(e + 72, 16); put_be32(e + 76, 5);
    memset(e + 84, tag, 16);
    return 100;
}

static int wait_until(atomic_uint* v, unsigned n)
{
    for (unsigned waited = 0; waited < 20000; waited++) {
        if (atomic_load(v) >= n) return 1;
        sleep_ms(1);
    }
    return 0;
}

int main(void)
{
    const uint32_t LEAK_EA = 0x10000, CRI_EA = 0x20000;
    const uint32_t leak_size = make_image(LEAK_EA, 0xA5), cri_size = make_image(CRI_EA, 0x5A);
    spu_workload_register_img(spu_workload_fingerprint(vm_base + LEAK_EA, leak_size), leak_entry, 9, "leak fixture");
    spu_workload_register_img(spu_workload_fingerprint(vm_base + CRI_EA, cri_size), cri_entry, 22, "cri exit fixture");

    /* 1. Back to back: with the pool on, each task reuses the last one's thread. */
    unsigned tasks = 0;
    for (unsigned i = 0; i < 200; i++) {
        CHECK(spu_workload_dispatch_async_owned(vm_base + LEAK_EA, leak_size, 0, 0, 0) == 1);
        CHECK(wait_until(&g_done, ++tasks));
        sleep_ms(i % 50 == 0 ? 5 : 0);   /* let the thread finish its scrub and go idle sometimes */
    }
    /* 2. Concurrent waves. */
    for (unsigned wave = 0; wave < 3; wave++) {
        atomic_store(&g_arrived, 0);
        atomic_store(&g_wave, 24);
        for (unsigned i = 0; i < 24; i++)
            CHECK(spu_workload_dispatch_async_owned(vm_base + LEAK_EA, leak_size, 0, 0, 0) == 1);
        tasks += 24;
        CHECK(wait_until(&g_done, tasks));
        atomic_store(&g_wave, 0);
        sleep_ms(20);
    }
    /* 3. cri tasks: every one returns from its first EXIT. */
    for (unsigned i = 0; i < 40; i++) {
        const unsigned before = atomic_load(&g_exit_returned);
        CHECK(spu_workload_dispatch_async_owned(vm_base + CRI_EA, cri_size, 0, 0, 0) == 1);
        unsigned waited = 0;
        while (atomic_load(&g_exit_returned) == before && waited < 1000) { sleep_ms(1); waited++; }
        sleep_ms(1);
    }
    CHECK(atomic_load(&g_exit_returned) == 40);

    /* Every leaked registration is gone once its thread has finished the task. */
    unsigned left = 0;
    for (unsigned waited = 0; waited < 5000 && (left = leaks_registered()); waited += 10) sleep_ms(10);
    CHECK(left == 0);
    CHECK(atomic_load(&g_n_leaks) == 272);
    CHECK(atomic_load(&g_inherited) == 0);
    CHECK(atomic_load(&g_no_stack) == 0);
    CHECK(atomic_load(&g_latch_timeout) == 0);
    CHECK(spu_workload_async_pool_shutdown(5000) == 0);

    uint32_t sum = 0;
    for (unsigned i = 0; i < 272; i++) sum = sum * 31u + g_result[i];
    printf("RESULT tasks=%u cri_exits=%u sum=%08X\n", atomic_load(&g_started),
           atomic_load(&g_exit_returned), sum);
    printf("async SPU task pool: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
