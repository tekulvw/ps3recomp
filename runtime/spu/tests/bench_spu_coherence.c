/*
 * Micro-benchmark: lock-line coherence cost, the current scheme against
 * per-line version counters. Synthetic; no game content or runs.
 *
 * It reproduces the cost structure seen in profiles of a SPURS title: PPU
 * threads storing into lines SPUs keep reserved (workload flags, queue heads),
 * PPU threads doing load-reserve/store-conditional on shared counters, and SPU
 * threads polling those lines with GETLLAR and committing with PUTLLC.
 *
 * "legacy" calls the real spu_coherency.c entry points (the lock-line lock,
 * the holder registration, the notify walk, the PPU shard and epoch paths) in
 * the same order spu_channels.c's GETLLAR/PUTLLC and ppu_loader.cpp's
 * VM_WRITE_COH / ppu_load_reserve32 / ppu_stwcx32 call them, with
 * PS3RECOMP_PPU_STORE_FASTPATH=1 (MAG's default). ppu_resv_break_line walks a
 * registry of PPU contexts as ppu_loader.cpp's does. "version" runs the same
 * workloads through spu_coh_version.c.
 *
 * Build:
 *   clang -std=gnu17 -O2 -I include -I runtime/spu -c runtime/spu/spu_coherency.c -o /tmp/coh.o
 *   clang -std=gnu17 -O2 -I include -I runtime/spu -o /tmp/bench_spu_coherence \
 *         runtime/spu/tests/bench_spu_coherence.c runtime/spu/spu_coh_version.c /tmp/coh.o -lpthread
 * Run:
 *   bench_spu_coherence [seconds-per-case=0.5] [spu-threads=4] [ppu-threads=2]
 *                       [polls-per-claim=4]
 * Prints ns per operation per thread (wall time x threads / operations) and
 * checks that the counters the threads increment come out exact.
 */

#include "spu_coherency.h"
#include "spu_coh_version.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Runtime pieces spu_coherency.c links against
 * -------------------------------------------------------------------------*/
static _Alignas(128) uint8_t g_mem[1u << 20];
#define EA_BASE 0x00100000u
static inline uint8_t* host(uint32_t ea) { return g_mem + (ea - EA_BASE); }

unsigned long long g_ch_wakes;
void spu_ch_wake(spu_context* ctx) { (void)ctx; __atomic_fetch_add(&g_ch_wakes, 1ull, __ATOMIC_RELAXED); }

/* ppu_loader.cpp's line break: a scan of every registered PPU context (MAG
 * registers one per guest thread; PS3RECOMP_PPU_RESV_OPEN_COUNT is off). */
#define PPU_REG 48
static struct { uint64_t reserve_addr; char pad[56]; } g_ppu_reg[PPU_REG];
void ppu_resv_break_line(uint32_t ea)
{
    const uint32_t line = ea & ~127u;
    for (int i = 0; i < PPU_REG; i++) {
        uint64_t a = __atomic_load_n(&g_ppu_reg[i].reserve_addr, __ATOMIC_RELAXED);
        if (a < 0x100000000ull && ((uint32_t)a & ~127u) == line)
            __atomic_store_n(&g_ppu_reg[i].reserve_addr, 0x100000000ull, __ATOMIC_RELAXED);
    }
}

/* ---------------------------------------------------------------------------
 * The legacy paths, in the runtime's call order
 * -------------------------------------------------------------------------*/
static void legacy_getllar(spu_context* c, uint32_t ea, uint8_t* ls)
{
    const int fast = spu_coh_reserve_fast(c, ea);
    if (!fast) { spu_lockline_lock(); spu_coh_reserve(c, ea); }
    spu_coh_live_track(c, ea);
    memcpy(ls, host(ea), 128);
    memcpy(c->resv_line, ls, 128);
    if (!fast) { c->resv_ea = ea; c->resv_valid = 1; }
    if (!fast) spu_lockline_unlock();
}

static int legacy_putllc(spu_context* c, uint32_t ea, const uint8_t* ls)
{
    if (!__atomic_load_n(&c->resv_valid, __ATOMIC_ACQUIRE) || c->resv_ea != ea ||
        memcmp(host(ea), c->resv_line, 128) != 0) {
        __atomic_store_n(&c->resv_valid, 0, __ATOMIC_RELAXED);
        spu_coh_live_untrack(c);
        return 0;
    }
    int ok = 0;
    spu_lockline_lock();
    if (c->resv_valid && c->resv_ea == ea && memcmp(host(ea), c->resv_line, 128) == 0) {
        memcpy(host(ea), ls, 128);
        c->resv_valid = 0;
        spu_coh_notify_write(ea);
        ok = 1;
    }
    c->resv_valid = 0;
    spu_coh_live_untrack(c);
    spu_lockline_unlock();
    return ok;
}

/* ppu_loader.cpp VM_WRITE_COH */
static void legacy_ppu_store32(uint32_t ea, uint32_t v)
{
    if (spu_coh_is_reserved(ea)) {
        const int ppu_only = spu_coh_ppu_store_lock(ea);
        if (!ppu_only) spu_lockline_lock();
        memcpy(host(ea), &v, 4);
        if (ppu_only) spu_coh_ppu_store_unlock(ea);
        else { spu_coh_notify_write(ea); spu_lockline_unlock(); }
    } else {
        memcpy(host(ea), &v, 4);
    }
}

/* ppu_load_reserve32 / ppu_stwcx32 with the line token (new lifts). */
typedef struct { uint32_t addr, value; uint64_t epoch; int live; } legacy_resv;
static __thread legacy_resv t_resv;
static volatile int32_t g_resv_slot[1024];
static void slot_lock(volatile int32_t* l) { while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) while (*l) { } }
static void slot_unlock(volatile int32_t* l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

static uint32_t legacy_lwarx(uint32_t ea)
{
    spu_lockline_lock();
    if (t_resv.live) { t_resv.live = 0; spu_coh_ppu_live_drop(t_resv.addr); }
    spu_coh_line_shard_lock(ea);
    spu_coh_ppu_live_add(ea);
    t_resv.live = 1;
    t_resv.epoch = spu_coh_line_epoch(ea);
    memcpy(&t_resv.value, host(ea), 4);
    t_resv.addr = ea;
    spu_coh_line_shard_unlock(ea);
    spu_lockline_unlock();
    return t_resv.value;
}

static int legacy_stwcx(uint32_t ea, uint32_t val)
{
    volatile int32_t* L = &g_resv_slot[(ea >> 4) & 1023u];
    slot_lock(L);
    const int coh = spu_coh_is_reserved(ea);
    if (coh) { spu_lockline_lock(); spu_coh_line_shard_lock(ea); }
    int ok = 0;
    if (!coh || t_resv.epoch == spu_coh_line_epoch(ea)) {
        uint32_t expected = t_resv.value;
        ok = __atomic_compare_exchange_n((uint32_t*)host(ea), &expected, val, 0,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    if (coh) {
        if (ok) spu_coh_notify_write(ea);
        spu_coh_line_shard_unlock(ea);
        spu_lockline_unlock();
    }
    if (ok) ppu_resv_break_line(ea);   /* ppu_resv_break: the same registry scan */
    slot_unlock(L);
    if (t_resv.live) { t_resv.live = 0; spu_coh_ppu_live_drop(ea); }
    return ok;
}

/* ---------------------------------------------------------------------------
 * The version paths
 * -------------------------------------------------------------------------*/
static unsigned char g_marks[(1u << 20) / 128 / 8];
static inline int marked(uint32_t ea) { uint32_t l = (ea - EA_BASE) >> 7; return (g_marks[l >> 3] >> (l & 7)) & 1; }
static inline void mark(uint32_t ea)
{
    uint32_t l = (ea - EA_BASE) >> 7;
    if (!marked(ea)) { spu_cohv_prepare(ea); __atomic_fetch_or(&g_marks[l >> 3], (unsigned char)(1u << (l & 7)), __ATOMIC_SEQ_CST); }
}

typedef struct { uint32_t ea, count; int valid; uint32_t event_status; _Alignas(8) uint8_t snap[128]; } v_spu;

static void v_getllar(v_spu* s, uint32_t ea, uint8_t* ls)
{
    spu_cohv_settle(&s->valid, s->ea, s->count, &s->event_status);
    mark(ea);
    s->count = spu_cohv_read_line(ea, host(ea), ls);
    memcpy(s->snap, ls, 128);
    s->ea = ea; s->valid = 1;
}

static int v_putllc(v_spu* s, uint32_t ea, const uint8_t* ls)
{
    int ok = s->valid && s->ea == ea && spu_cohv_putllc(ea, host(ea), s->count, s->snap, ls);
    if (!ok) spu_cohv_settle(&s->valid, s->ea, s->count, &s->event_status);
    s->valid = 0;
    return ok;
}

static void v_ppu_store32(uint32_t ea, uint32_t v)
{
    if (marked(ea)) spu_cohv_store(ea, host(ea), &v, 4);
    else memcpy(host(ea), &v, 4);
}

static __thread struct { uint32_t count; uint64_t value; } t_vres;
static uint32_t v_lwarx(uint32_t ea)
{
    mark(ea);
    t_vres.count = spu_cohv_read_value(ea, host(ea), 4, &t_vres.value);
    return (uint32_t)t_vres.value;
}
static int v_stwcx(uint32_t ea, uint32_t val)
{
    return spu_cohv_store_conditional(ea, host(ea), 4, t_vres.count, t_vres.value, val);
}

/* ---------------------------------------------------------------------------
 * Workloads
 * -------------------------------------------------------------------------*/
static int g_version;
static volatile int g_stop;
static int g_spus = 4, g_ppus = 2;
static double g_secs = 0.5;
static int g_claim_every = 4;

#define MGMT_LINE(i)   (EA_BASE + 0x1000u + (uint32_t)(i) * 128u)   /* 4 adjacent SPURS-like lines */
#define CTR_LINE(i)    (EA_BASE + 0x8000u + (uint32_t)(i) * 128u)   /* PPU atomic counters */
#define IDLE_LINE(i)   (EA_BASE + 0x10000u + (uint32_t)(i) * 128u)  /* marked once, nobody holds them */

typedef struct { int id; unsigned long long ops, ok, claims; } th;

static int g_case;  /* 0 spurs, 1 ppu-atomics, 2 idle-marked stores */

/* SPU: poll a management line, claim it now and then. */
static void* spu_thread(void* arg)
{
    th* t = (th*)arg;
    _Alignas(8) uint8_t ls[128];
    spu_context* c = g_version ? NULL : (spu_context*)calloc(1, sizeof(spu_context));
    v_spu vs; memset(&vs, 0, sizeof vs);
    unsigned seed = (unsigned)t->id * 2654435761u + 1u;
    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t ea = MGMT_LINE((seed >> 16) & 3u);
        if (g_version) v_getllar(&vs, ea, ls); else legacy_getllar(c, ea, ls);
        t->ops++;
        /* Most polls find nothing to claim; one in g_claim_every commits. */
        if (((seed >> 8) % (unsigned)g_claim_every) == 0) {
            uint32_t ctr; memcpy(&ctr, ls + 4 * (t->id & 7), 4);
            ctr++;
            memcpy(ls + 4 * (t->id & 7), &ctr, 4);
            const int ok = g_version ? v_putllc(&vs, ea, ls) : legacy_putllc(c, ea, ls);
            t->ok += ok;
            t->claims++;
        }
        /* The poll that follows in guest code: read the event status. Legacy
         * events were raised by writers; a version holder settles here. */
        if (g_version) spu_cohv_settle(&vs.valid, vs.ea, vs.count, &vs.event_status);
    }
    if (c) { spu_coh_unregister(c); free(c); }
    return NULL;
}

static void* ppu_thread(void* arg)
{
    th* t = (th*)arg;
    unsigned seed = (unsigned)t->id * 40503u + 7u;
    uint32_t v = 0;
    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        seed = seed * 1103515245u + 12345u;
        switch (g_case) {
        case 0: {   /* post work: a flag word in a line the SPUs poll (not their counters) */
            const uint32_t ea = MGMT_LINE((seed >> 16) & 3u) + 64u + 4u * (uint32_t)(t->id & 7);
            if (g_version) v_ppu_store32(ea, ++v); else legacy_ppu_store32(ea, ++v);
            t->ok++;
            break;
        }
        case 1: {   /* lock-free counter increment */
            const uint32_t ea = CTR_LINE((seed >> 16) & 7u);
            uint32_t x = g_version ? v_lwarx(ea) : legacy_lwarx(ea);
            t->ok += g_version ? v_stwcx(ea, x + 1) : legacy_stwcx(ea, x + 1);
            break;
        }
        default: {  /* stores to lines once reserved, now idle */
            const uint32_t ea = IDLE_LINE((seed >> 16) & 63u) + 4u * (uint32_t)(t->id & 7);
            if (g_version) v_ppu_store32(ea, ++v); else legacy_ppu_store32(ea, ++v);
            t->ok++;
        }
        }
        t->ops++;
    }
    return NULL;
}

static double now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void run_case(int version, int which, const char* name, int spus)
{
    g_version = version; g_case = which; g_stop = 0;
    memset(g_mem, 0, sizeof g_mem);
    /* Lines that have been reserved once: mark them in both schemes. */
    for (int i = 0; i < 64; i++) {
        const uint32_t ea = IDLE_LINE(i);
        if (version) mark(ea);
        else { spu_lockline_lock(); spu_coh_line_epoch(ea); spu_lockline_unlock(); }
    }
    th ts[64]; memset(ts, 0, sizeof ts);
    pthread_t pt[64];
    int n = 0;
    for (int i = 0; i < spus; i++, n++) { ts[n].id = i; pthread_create(&pt[n], NULL, spu_thread, &ts[n]); }
    const int first_ppu = n;
    for (int i = 0; i < g_ppus; i++, n++) { ts[n].id = i; pthread_create(&pt[n], NULL, ppu_thread, &ts[n]); }
    const double t0 = now_s();
    struct timespec d = { (time_t)g_secs, (long)((g_secs - (double)(time_t)g_secs) * 1e9) };
    nanosleep(&d, NULL);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < n; i++) pthread_join(pt[i], NULL);
    const double wall = now_s() - t0;
    unsigned long long sops = 0, sok = 0, scl = 0, pops = 0, pok = 0;
    for (int i = 0; i < first_ppu; i++) { sops += ts[i].ops; sok += ts[i].ok; scl += ts[i].claims; }
    for (int i = first_ppu; i < n; i++) { pops += ts[i].ops; pok += ts[i].ok; }
    /* Exactness: each SPU's counter word, and the PPU counters. */
    int exact = 1;
    if (which == 0) {
        for (int i = 0; i < spus; i++) {
            unsigned long long sum = 0;
            for (int l = 0; l < 4; l++) { uint32_t w; memcpy(&w, host(MGMT_LINE(l)) + 4 * (i & 7), 4); sum += w; }
            if (sum != ts[i].ok) exact = 0;
        }
    } else if (which == 1) {
        unsigned long long sum = 0;
        for (int l = 0; l < 8; l++) { uint32_t w; memcpy(&w, host(CTR_LINE(l)), 4); sum += w; }
        if (sum != pok) exact = 0;
    }
    printf("%-8s %-22s spus=%d ppus=%d | SPU poll %7.1f ns (claims %5.1f%% commit) | PPU op %8.1f ns/op (%5.1f%% ok) | %s\n",
           version ? "version" : "legacy", name, spus, g_ppus,
           sops ? wall * 1e9 * spus / (double)sops : 0.0, scl ? 100.0 * (double)sok / (double)scl : 0.0,
           pops ? wall * 1e9 * g_ppus / (double)pops : 0.0, pops ? 100.0 * (double)pok / (double)pops : 0.0,
           exact ? "exact" : "COUNTERS WRONG");
    fflush(stdout);
}

int main(int argc, char** argv)
{
    if (argc > 1) g_secs = atof(argv[1]);
    if (argc > 2) g_spus = atoi(argv[2]);
    if (argc > 3) g_ppus = atoi(argv[3]);
    if (argc > 4 && atoi(argv[4]) > 0) g_claim_every = atoi(argv[4]);
    setenv("PS3RECOMP_PPU_STORE_FASTPATH", "1", 1);
    /* Every guest thread is registered; give the scan a realistic population. */
    for (int i = 0; i < PPU_REG; i++) g_ppu_reg[i].reserve_addr = 0x100000000ull;
    static const char* names[] = { "spurs-lines", "ppu-lwarx-stwcx", "idle-marked-stores" };
    for (int which = 0; which < 3; which++)
        for (int version = 0; version < 2; version++) {
            spu_cohv_force(version);
            run_case(version, which, names[which], which == 0 ? g_spus : 0);
            if (which == 1) run_case(version, which, "ppu-atomics+spu-polls", g_spus);
        }
    printf("legacy LR raises %lu, channel wakes %llu; version writer wakes %llu\n",
           g_spu_lr_raise, g_ch_wakes, g_spu_cohv_wakes);
    return 0;
}
