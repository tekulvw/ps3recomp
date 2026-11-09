/*
 * Per-line version counters (runtime/spu/spu_coh_version.c): semantics and
 * torture tests, synthetic, on host threads.
 *
 * The scheme replaces "a writer finds every holder of the line and tells it"
 * with "a writer advances the line's count, and a holder compares when it next
 * looks". So what has to be shown is:
 *
 *  - a store-conditional (SPU PUTLLC, PPU stwcx.) commits only if no write of
 *    any kind landed on the line since the reservation, including a write of
 *    the same bytes (ABA) and a write to another word of the line, and the
 *    commit itself is atomic against every other writer. The torture test
 *    checks it the way a lost update would show: counters that SPU and PPU
 *    threads increment through the same line must come out exact, and every
 *    snapshot must see the line's paired words consistent;
 *  - a holder parked until its reservation is lost is always woken, and never
 *    needs a timeout to notice. The wake test waits with no timeout at all,
 *    under a watchdog;
 *  - the cases that must stay quiet do: a write to a neighbouring line, and a
 *    store-conditional that fails, leave the reservation and count alone.
 *
 * Build (also under -fsanitize=thread):
 *   clang -std=gnu17 -O2 -Wall -Wextra -I runtime/spu \
 *         -o /tmp/test_spu_coh_version \
 *         runtime/spu/tests/test_spu_coh_version.c runtime/spu/spu_coh_version.c -lpthread
 *
 * COHV_TEST_SCALE=<n> multiplies the torture iteration counts (default 1);
 * COHV_TEST_DIV=<n> divides them (use 20 or so under ThreadSanitizer, which
 * makes every atomic in these spin loops expensive).
 * Exit code: 0 if all passed, 1 if any failed. Final line prints a summary.
 */

#include "../spu_coh_version.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int g_pass, g_fail;
static const char* g_current = "(none)";
#define TEST(name) g_current = (name)
#define CHECK(cond) do {                                                     \
    if (cond) __atomic_fetch_add(&g_pass, 1, __ATOMIC_RELAXED);              \
    else { __atomic_fetch_add(&g_fail, 1, __ATOMIC_RELAXED);                 \
           fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n", g_current,           \
                   __FILE__, __LINE__, #cond); }                             \
} while (0)

/* Guest memory: a few lines well apart, 128-byte aligned. */
static _Alignas(128) uint8_t g_mem[1u << 16];
#define EA_BASE 0x00100000u
static inline uint8_t* host(uint32_t ea) { return g_mem + (ea - EA_BASE); }

static unsigned g_scale = 1, g_div = 1;

/* A model SPU's reservation, the fields the runtime keeps in spu_context. */
typedef struct {
    uint32_t resv_ea, resv_count;
    int resv_valid;
    uint32_t event_status;
    _Alignas(8) uint8_t snapshot[128];
    _Alignas(8) uint8_t ls[128];
} model_spu;

static void spu_getllar(model_spu* s, uint32_t ea)
{
    /* Settle the reservation being replaced first: a write that ended it
     * already must still show up as LR. */
    spu_cohv_settle(&s->resv_valid, s->resv_ea, s->resv_count, &s->event_status);
    s->resv_count = spu_cohv_read_line(ea, host(ea), s->ls);
    memcpy(s->snapshot, s->ls, 128);
    s->resv_ea = ea;
    s->resv_valid = 1;
}

/* 1 on commit. */
static int spu_putllc(model_spu* s, uint32_t ea)
{
    int ok = 0;
    if (s->resv_valid && s->resv_ea == ea)
        ok = spu_cohv_putllc(ea, host(ea), s->resv_count, s->snapshot, s->ls);
    /* A failure ends the reservation; settle first so its LR is not lost. */
    if (!ok) spu_cohv_settle(&s->resv_valid, s->resv_ea, s->resv_count, &s->event_status);
    s->resv_valid = 0;
    return ok;
}

static uint32_t rd32(uint32_t ea) { uint32_t v; memcpy(&v, host(ea), 4); return v; }

/* ---------------------------------------------------------------------------
 * Semantics, one thread
 * -------------------------------------------------------------------------*/
static void test_semantics(void)
{
    const uint32_t L = EA_BASE + 0x1000u, N = L + 128u;
    model_spu s; memset(&s, 0, sizeof s);
    spu_cohv_prepare(L); spu_cohv_prepare(N);

    TEST("neighbour lines use different host cache lines");
    CHECK((uintptr_t)spu_cohv_word(N) - (uintptr_t)spu_cohv_word(L) >= 128u ||
          (uintptr_t)spu_cohv_word(L) - (uintptr_t)spu_cohv_word(N) >= 128u);
    CHECK(spu_cohv_word(L) != spu_cohv_word(L + 0x4000u));

    TEST("a store to a neighbouring line leaves the reservation alone");
    spu_getllar(&s, L);
    uint32_t v = 7;
    spu_cohv_store(N, host(N), &v, 4);
    CHECK(!spu_cohv_moved(L, s.resv_count));
    CHECK(spu_cohv_settle(&s.resv_valid, L, s.resv_count, &s.event_status) == 0);
    CHECK(s.resv_valid && !(s.event_status & 0x400u));

    TEST("PUTLLC with an intact reservation commits and advances the count");
    const uint32_t c0 = s.resv_count;
    s.ls[0] = 0x11;
    CHECK(spu_putllc(&s, L) == 1);
    CHECK(host(L)[0] == 0x11);
    CHECK(spu_cohv_count(L) == c0 + SPU_COHV_STEP);
    CHECK(!(s.event_status & 0x400u));   /* a commit consumes, it does not lose */

    TEST("a store to another word of the line loses the reservation, lazily");
    spu_getllar(&s, L);
    v = 0x22;
    spu_cohv_store(L + 64u, host(L + 64u), &v, 4);
    CHECK(s.resv_valid);                  /* nobody told the holder... */
    CHECK(spu_cohv_settle(&s.resv_valid, L, s.resv_count, &s.event_status) == 1);
    CHECK(!s.resv_valid && (s.event_status & 0x400u));   /* ...it found out */
    CHECK(spu_cohv_settle(&s.resv_valid, L, s.resv_count, &s.event_status) == 0);

    TEST("PUTLLC after a lost reservation fails and writes nothing");
    s.event_status = 0;
    spu_getllar(&s, L);
    v = 0x33;
    spu_cohv_store(L + 4u, host(L + 4u), &v, 4);
    s.ls[0] = 0x44;
    CHECK(spu_putllc(&s, L) == 0);
    CHECK(host(L)[0] == 0x11);
    CHECK(s.event_status & 0x400u);       /* settled on the way out */

    TEST("a store of the same bytes (ABA) still loses the reservation");
    s.event_status = 0;
    spu_getllar(&s, L);
    uint32_t same = rd32(L + 8u);
    spu_cohv_store(L + 8u, host(L + 8u), &same, 4);
    s.ls[1] = 0x55;
    CHECK(spu_putllc(&s, L) == 0);
    CHECK(host(L)[1] != 0x55);

    TEST("bytes changed outside the protocol fail PUTLLC and keep the count");
    spu_getllar(&s, L);
    host(L)[2] ^= 0xFFu;                  /* a host write that skipped the line */
    const uint32_t before = spu_cohv_count(L);
    s.ls[3] = 0x66;
    CHECK(spu_putllc(&s, L) == 0);
    CHECK(spu_cohv_count(L) == before);
    CHECK(!(__atomic_load_n(spu_cohv_word(L), __ATOMIC_RELAXED) & SPU_COHV_WRITING));

    TEST("PUTLLUC always writes and ends every reservation on the line");
    model_spu peer; memset(&peer, 0, sizeof peer);
    spu_getllar(&peer, L);
    uint8_t line[128]; memset(line, 0xA5, sizeof line);
    spu_cohv_putlluc(L, host(L), line);
    CHECK(host(L)[127] == 0xA5);
    CHECK(spu_cohv_settle(&peer.resv_valid, L, peer.resv_count, &peer.event_status) == 1);

    TEST("a store-conditional fails while another writer owns the line");
    const uint32_t c1 = spu_cohv_count(L);
    spu_cohv_lock(L);
    CHECK(spu_cohv_lock_if(L, c1) == 0);
    spu_cohv_unlock_written(L);
    CHECK(spu_cohv_lock_if(L, c1) == 0);  /* and after it, the count moved */

    TEST("PPU load-reserve / store-conditional against an SPU commit");
    uint64_t raw = 0;
    uint32_t c = spu_cohv_read_value(L, host(L), 4, &raw);
    spu_getllar(&s, L);
    s.ls[64] ^= 1u;                       /* the SPU commits another word */
    CHECK(spu_putllc(&s, L) == 1);
    CHECK(spu_cohv_store_conditional(L, host(L), 4, c, raw, 0x01020304u) == 0);
    c = spu_cohv_read_value(L, host(L), 4, &raw);
    CHECK(spu_cohv_store_conditional(L, host(L), 4, c, raw, 0x01020304u) == 1);
    CHECK(rd32(L) == 0x01020304u);
    TEST("an SPU reservation is lost to a PPU store-conditional");
    spu_getllar(&s, L);
    c = spu_cohv_read_value(L, host(L), 4, &raw);
    CHECK(spu_cohv_store_conditional(L, host(L), 4, c, raw, 0x05060708u) == 1);
    s.ls[0] ^= 1u;
    CHECK(spu_putllc(&s, L) == 0);

    TEST("a failed store-conditional keeps the line's count");
    c = spu_cohv_read_value(L, host(L), 4, &raw);
    const uint32_t c2 = spu_cohv_count(L);
    CHECK(spu_cohv_store_conditional(L, host(L), 4, c, raw ^ 1u, 9) == 0);
    CHECK(spu_cohv_count(L) == c2);
}

/* ---------------------------------------------------------------------------
 * The WAITERS bit, one thread
 * -------------------------------------------------------------------------*/
static int g_wake_calls;
static void count_wake(void* p) { (void)p; g_wake_calls++; }

static void test_waiter_bit(void)
{
    const uint32_t L = EA_BASE + 0x2000u;
    spu_cohv_prepare(L);
    spu_cohv_waiter a, b;

    TEST("a registered waiter is woken by a write and stays registered");
    const uint32_t c = spu_cohv_wait_register(&a, L, count_wake, NULL);
    CHECK(c == spu_cohv_count(L));
    g_wake_calls = 0;
    uint32_t v = 1;
    spu_cohv_store(L, host(L), &v, 4);
    CHECK(g_wake_calls == 1);
    CHECK(__atomic_load_n(spu_cohv_word(L), __ATOMIC_RELAXED) & SPU_COHV_WAITERS);

    TEST("WAITERS set while a writer owns the line survives the release");
    spu_cohv_lock(L);
    spu_cohv_wait_register(&b, L, count_wake, NULL);
    spu_cohv_unlock_written(L);
    CHECK(g_wake_calls == 3);             /* both waiters */

    TEST("the last waiter to leave clears WAITERS");
    spu_cohv_wait_unregister(&a);
    CHECK(__atomic_load_n(spu_cohv_word(L), __ATOMIC_RELAXED) & SPU_COHV_WAITERS);
    spu_cohv_wait_unregister(&b);
    CHECK(!(__atomic_load_n(spu_cohv_word(L), __ATOMIC_RELAXED) & SPU_COHV_WAITERS));
    spu_cohv_store(L, host(L), &v, 4);
    CHECK(g_wake_calls == 3);

    TEST("a failed store-conditional wakes nobody");
    spu_cohv_wait_register(&a, L, count_wake, NULL);
    const uint32_t c3 = spu_cohv_count(L);
    CHECK(spu_cohv_lock_if(L, c3) == 1);
    spu_cohv_unlock_unchanged(L);
    CHECK(g_wake_calls == 3);
    spu_cohv_wait_unregister(&a);
}

/* ---------------------------------------------------------------------------
 * Torture: SPU PUTLLC, PPU stwcx. and PPU plain stores on one line
 *
 * Line layout (host byte order; the scheme does not care):
 *   +0   u64  SPU counter    -- incremented with GETLLAR/PUTLLC
 *   +8   u64  PPU counter    -- incremented with load-reserve/store-conditional
 *   +16  u64  plain value x  \  written together by one 16-byte versioned
 *   +24  u64  ~x             /  store; a snapshot must never see them apart
 *   +32  u64  SPU pair a     \  written together by the SPU's PUTLLC
 *   +40  u64  ~a             /
 * Every SPU commit rewrites the whole line from its snapshot, so a PPU commit
 * slipping between an SPU's snapshot and its commit would be undone, and the
 * PPU counter would come out short; and the reverse for the SPU counter.
 * -------------------------------------------------------------------------*/
#define T_LINE (EA_BASE + 0x3000u)
enum { T_SPUS = 4, T_PPU_SC = 3, T_PPU_ST = 2 };
static unsigned g_iters;
static volatile int g_stop;

static void* spu_inc_thread(void* arg)
{
    model_spu* s = (model_spu*)arg;
    unsigned done = 0;
    while (done < g_iters) {
        spu_getllar(s, T_LINE);
        uint64_t w[6];
        memcpy(w, s->ls, sizeof w);
        if (w[3] != ~w[2] || w[5] != ~w[4]) {
            TEST("torture: a snapshot saw a paired store half done");
            CHECK(0);
            break;
        }
        w[0] += 1;
        w[4] += 1; w[5] = ~w[4];
        memcpy(s->ls, w, 8);
        memcpy(s->ls + 32, &w[4], 16);
        if (spu_putllc(s, T_LINE)) done++;
    }
    return NULL;
}

static void* ppu_sc_thread(void* arg)
{
    (void)arg;
    unsigned done = 0;
    while (done < g_iters) {
        uint64_t raw;
        const uint32_t c = spu_cohv_read_value(T_LINE + 8u, host(T_LINE + 8u), 8, &raw);
        if (spu_cohv_store_conditional(T_LINE + 8u, host(T_LINE + 8u), 8, c, raw, raw + 1)) done++;
    }
    return NULL;
}

static void* ppu_store_thread(void* arg)
{
    uint64_t x = (uintptr_t)arg;
    while (!__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) {
        uint64_t pair[2] = { x, ~x };
        x += 0x10001;
        spu_cohv_lock(T_LINE);
        spu_cohv_store_bytes(host(T_LINE + 16u), &pair[0], 8);
        spu_cohv_store_bytes(host(T_LINE + 24u), &pair[1], 8);
        spu_cohv_unlock_written(T_LINE);
    }
    return NULL;
}

static void test_torture_counters(void)
{
    spu_cohv_prepare(T_LINE);
    memset(host(T_LINE), 0, 128);
    { uint64_t init[2] = { 0, ~0ull }; memcpy(host(T_LINE + 16u), init, 16); memcpy(host(T_LINE + 32u), init, 16); }
    g_iters = 20000u * g_scale / g_div + 1u;
    g_stop = 0;
    pthread_t spu[T_SPUS], sc[T_PPU_SC], st[T_PPU_ST];
    model_spu* spus = (model_spu*)calloc(T_SPUS, sizeof(model_spu));
    for (int i = 0; i < T_PPU_ST; i++) pthread_create(&st[i], NULL, ppu_store_thread, (void*)(uintptr_t)(i * 7 + 1));
    for (int i = 0; i < T_SPUS; i++) pthread_create(&spu[i], NULL, spu_inc_thread, &spus[i]);
    for (int i = 0; i < T_PPU_SC; i++) pthread_create(&sc[i], NULL, ppu_sc_thread, NULL);
    for (int i = 0; i < T_SPUS; i++) pthread_join(spu[i], NULL);
    for (int i = 0; i < T_PPU_SC; i++) pthread_join(sc[i], NULL);
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < T_PPU_ST; i++) pthread_join(st[i], NULL);

    uint64_t w[6];
    memcpy(w, host(T_LINE), sizeof w);
    TEST("torture: every SPU PUTLLC increment survived");
    CHECK(w[0] == (uint64_t)T_SPUS * g_iters);
    CHECK(w[4] == (uint64_t)T_SPUS * g_iters && w[5] == ~w[4]);
    TEST("torture: every PPU store-conditional increment survived");
    CHECK(w[1] == (uint64_t)T_PPU_SC * g_iters);
    TEST("torture: the plain paired store is whole");
    CHECK(w[3] == ~w[2]);
    TEST("torture: the line is released");
    CHECK(!(__atomic_load_n(spu_cohv_word(T_LINE), __ATOMIC_RELAXED) & SPU_COHV_WRITING));
    if (w[0] != (uint64_t)T_SPUS * g_iters || w[1] != (uint64_t)T_PPU_SC * g_iters)
        fprintf(stderr, "  spu=%llu (want %llu) ppu=%llu (want %llu)\n",
                (unsigned long long)w[0], (unsigned long long)T_SPUS * g_iters,
                (unsigned long long)w[1], (unsigned long long)T_PPU_SC * g_iters);
    free(spus);
}

/* ---------------------------------------------------------------------------
 * Torture: no lost lost-reservation wakeups
 *
 * Each waiter reserves its line, registers, and then sleeps on its own
 * condition variable with NO timeout until the reservation is lost. Writers
 * store to the waiters' lines at random moments relative to that sequence --
 * before the registration, between registration and sleep, during the sleep --
 * and also to a neighbouring line, which must not end anyone's wait. A lost
 * wakeup is a waiter asleep forever; the watchdog turns it into a failure.
 * -------------------------------------------------------------------------*/
enum { W_WAITERS = 4, W_WRITERS = 2 };
#define W_LINE(i) (EA_BASE + 0x4000u + (uint32_t)(i) * 0x80u * 3u)

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    model_spu spu;
    unsigned id;
    unsigned rounds;
    unsigned long long woken_by_writer;
} waiter_ctx;

static waiter_ctx g_w[W_WAITERS];
static unsigned g_wait_rounds;
static unsigned long long g_progress;

static void waiter_wake(void* p)
{
    waiter_ctx* w = (waiter_ctx*)p;
    pthread_mutex_lock(&w->m);
    w->woken_by_writer++;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->m);
}

static void* waiter_thread(void* arg)
{
    waiter_ctx* w = (waiter_ctx*)arg;
    const uint32_t line = W_LINE(w->id);
    for (unsigned r = 0; r < g_wait_rounds; r++) {
        spu_getllar(&w->spu, line);
        w->spu.event_status = 0;
        spu_cohv_waiter reg;
        spu_cohv_wait_register(&reg, line, waiter_wake, w);
        pthread_mutex_lock(&w->m);
        while (!spu_cohv_settle(&w->spu.resv_valid, line, w->spu.resv_count, &w->spu.event_status))
            pthread_cond_wait(&w->cv, &w->m);
        pthread_mutex_unlock(&w->m);
        spu_cohv_wait_unregister(&reg);
        if (!(w->spu.event_status & 0x400u)) { TEST("wake: woke without LR"); CHECK(0); }
        __atomic_fetch_add(&g_progress, 1ull, __ATOMIC_RELAXED);
        w->rounds++;
    }
    return NULL;
}

static volatile int g_writers_stop;

static void* writer_thread(void* arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    while (!__atomic_load_n(&g_writers_stop, __ATOMIC_RELAXED)) {
        seed = seed * 1103515245u + 12345u;
        const unsigned i = (seed >> 16) % W_WAITERS;
        /* Mostly the waiter's line, sometimes the next line over. */
        const uint32_t ea = W_LINE(i) + (((seed >> 8) & 7u) == 0 ? 128u : 0u);
        uint32_t v = seed;
        spu_cohv_store(ea, host(ea), &v, 4);
        for (unsigned k = (seed >> 4) & 255u; k; k--) __asm__ __volatile__("" ::: "memory");
        if (((seed >> 12) & 63u) == 0) sched_yield();
    }
    return NULL;
}

static void* watchdog_thread(void* arg)
{
    (void)arg;
    unsigned long long last = (unsigned long long)-1;
    for (unsigned still = 0;; ) {
        usleep(100000);
        const unsigned long long now = __atomic_load_n(&g_progress, __ATOMIC_RELAXED);
        if (now == (unsigned long long)W_WAITERS * g_wait_rounds) return NULL;
        still = now == last ? still + 1 : 0;
        if (still >= 50) {
            fprintf(stderr, "FAIL: wake: no waiter progressed in 5 s (a lost wakeup)\n");
            for (int i = 0; i < W_WAITERS; i++)
                fprintf(stderr, "  waiter %d: round %u of %u, line count %u resv %u valid %d\n",
                        i, g_w[i].rounds, g_wait_rounds, spu_cohv_count(W_LINE(i)),
                        g_w[i].spu.resv_count, g_w[i].spu.resv_valid);
            fflush(stderr);
            _exit(1);
        }
        last = now;
    }
}

static void test_torture_wakeups(void)
{
    g_wait_rounds = 5000u * g_scale / g_div + 1u;
    for (int i = 0; i < W_WAITERS; i++) {
        memset(&g_w[i].spu, 0, sizeof g_w[i].spu);
        pthread_mutex_init(&g_w[i].m, NULL);
        pthread_cond_init(&g_w[i].cv, NULL);
        g_w[i].id = (unsigned)i;
        spu_cohv_prepare(W_LINE(i));
    }
    pthread_t wd, wt[W_WAITERS], wr[W_WRITERS];
    pthread_create(&wd, NULL, watchdog_thread, NULL);
    for (int i = 0; i < W_WAITERS; i++) pthread_create(&wt[i], NULL, waiter_thread, &g_w[i]);
    for (int i = 0; i < W_WRITERS; i++) pthread_create(&wr[i], NULL, writer_thread, (void*)(uintptr_t)(i + 17));
    for (int i = 0; i < W_WAITERS; i++) pthread_join(wt[i], NULL);
    __atomic_store_n(&g_writers_stop, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < W_WRITERS; i++) pthread_join(wr[i], NULL);
    pthread_join(wd, NULL);

    TEST("wake: every waiter finished every round");
    unsigned long long woken = 0;
    for (int i = 0; i < W_WAITERS; i++) { CHECK(g_w[i].rounds == g_wait_rounds); woken += g_w[i].woken_by_writer; }
    TEST("wake: writers did wake parked waiters (the test exercised sleeping)");
    CHECK(woken > 0);
    TEST("wake: WAITERS is clear once nobody waits");
    for (int i = 0; i < W_WAITERS; i++)
        CHECK(!(__atomic_load_n(spu_cohv_word(W_LINE(i)), __ATOMIC_RELAXED) & SPU_COHV_WAITERS));
    printf("wake: %llu writer wakes over %u rounds x %d waiters\n", woken, g_wait_rounds, W_WAITERS);
}

int main(void)
{
    const char* sc = getenv("COHV_TEST_SCALE");
    if (sc && atoi(sc) > 0) g_scale = (unsigned)atoi(sc);
    const char* dv = getenv("COHV_TEST_DIV");
    if (dv && atoi(dv) > 0) g_div = (unsigned)atoi(dv);
    spu_cohv_force(1);
    test_semantics();
    test_waiter_bit();
    test_torture_counters();
    test_torture_wakeups();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
