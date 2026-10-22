/*
 * SPU-to-SPU lock-line coherence: a write by one SPU against a line another
 * SPU has reserved (runtime/spu/spu_channels.c, runtime/spu/spu_dma.h).
 *
 * test_spu_coherency.c covers the PPU as the writer. This covers the other
 * writer, which is the one a SPURS title actually spends its time on: five SPU
 * kernel threads sharing one management line, every one of them reserving it
 * with GETLLAR and committing with PUTLLC. If a commit does not break the
 * peers' reservations, a peer's pending PUTLLC compares its snapshot against a
 * line the committer has already replaced, finds what it expects, and commits
 * over the top. Both SPUs believe they won the same compare-and-swap. Nothing
 * in the tree reports it: the queue is simply wrong afterwards.
 *
 * So the assertions are on the peer, not the writer: after a PUTLLC commit, a
 * PUTLLUC and a plain DMA PUT into a reserved line, the peer holds SPU_EVENT_LR
 * and no reservation, and its own PUTLLC then FAILS and writes nothing. The
 * cases that must stay quiet are here for the same reason they are in the PPU
 * test -- an over-eager notify is a livelock, not a safe approximation.
 *
 * The last tests are the event acknowledge and the MFC event producers. An SPU
 * acknowledging the events it has read is a read-modify-write of the same word
 * every one of those raises writes, so an edge landing between the read and
 * the write would be dropped and the SPU would go back to sleep holding a
 * reservation it has already lost. Every writer of event_status is an atomic
 * RMW and none of them takes the lock-line lock: the tests pin that they
 * complete while the lock is held, that a raise after them is kept, and race
 * an acknowledge against a raise for real.
 *
 * spu_channels.c is compiled in rather than linked, because spu_mfc_atomic()
 * is file-static: the same reason test_spu_mfc_slots.c does it.
 *
 * Build:
 *   clang -std=gnu17 -I include -I runtime/spu -I runtime/platform \
 *         -o /tmp/test_spu_lockline_peer \
 *         runtime/spu/tests/test_spu_lockline_peer.c \
 *         runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c \
 *         runtime/spu/spu_coherency.c runtime/spu/spu_workload.c \
 *         runtime/spu/spurs_policy.c runtime/spu/spurs_job.c \
 *         runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c \
 *         runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c \
 *         runtime/platform/win32_compat.c -lpthread -lm
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints a summary.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guest memory. The DMA and atomic paths index straight off this, so it has to
 * cover every EA the test uses; a PUT below 0x10000 is rejected as malformed
 * (the null-destination guard), so the lines live well above that. */
static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;
/* Job I/O recorder (runtime/diagnostics): never armed here, so the fixture
 * only needs the inactive pointer and an event sink that is never reached. */
struct spu_context;
void* ps3_jobio_active;
void ps3_jobio_event(struct spu_context* c, uint32_t k, uint32_t cmd, uint32_t lsa, uint64_t ea, uint32_t n, const uint8_t* b, uint32_t x)
{ (void)c; (void)k; (void)cmd; (void)lsa; (void)ea; (void)n; (void)b; (void)x; }

/* The unit under test. */
#include "../spu_channels.c"

/* ---------------------------------------------------------------------------
 * Harness
 * -----------------------------------------------------------------------*/
static int g_pass, g_fail;
static const char* g_current = "(none)";

#define TEST(name) g_current = (name)

#define CHECK(cond) do {                                          \
    if (cond) { g_pass++; }                                       \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n",             \
                g_current, __FILE__, __LINE__, #cond);            \
    }                                                             \
} while (0)

#define CHECK_EQ_U32(actual, expected) do {                       \
    uint32_t _a = (uint32_t)(actual), _e = (uint32_t)(expected);  \
    if (_a == _e) { g_pass++; }                                   \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n"              \
                        "  actual   0x%08X\n  expected 0x%08X\n", \
                g_current, __FILE__, __LINE__, #actual, _a, _e);  \
    }                                                             \
} while (0)

/* Three 128-byte lines, and a local-store address per context to stage from.
 * LINE_D..LINE_D+0x180 is the multi-line span the wide PUT covers. */
#define LINE_A   0x00020000u
#define LINE_B   0x00020180u
#define LINE_D   0x00020400u

#define LSA_A    0x00001000u
#define LSA_B    0x00002000u

/* 256 KB of local store each: not on the stack. */
static spu_context g_a, g_b;

/* ---------------------------------------------------------------------------
 * Driving the real channel path
 *
 * Every command goes through spu_wrch, so what runs is spu_mfc_atomic() and
 * mfc_do_transfer() themselves rather than a copy of their reservation rules.
 * -----------------------------------------------------------------------*/
static void wrch(spu_context* ctx, uint32_t channel, uint32_t v)
{
    spu_wrch(ctx, channel, spu_make_preferred_u32(v));
}

static void spu_atomic(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, lsa);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Cmd, cmd);
}

static void spu_put(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t size)
{
    wrch(ctx, MFC_LSA, lsa);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Size, size);
    wrch(ctx, MFC_TagID, 0);
    wrch(ctx, MFC_Cmd, MFC_PUT_CMD);
}

/* Fill a context's staging buffer with a recognisable pattern, so "the write
 * landed" and "the write landed from the SPU that should have won" are
 * different assertions. */
static void stage(spu_context* ctx, uint32_t lsa, uint8_t tag, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        ctx->ls[(lsa + i) & SPU_LS_MASK] = (uint8_t)(tag + (i & 0x0F));
}

static int mem_is(uint32_t ea, uint8_t tag, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (g_guest_mem[ea + i] != (uint8_t)(tag + (i & 0x0F))) return 0;
    return 1;
}

/* A context is reset rather than re-created: spu_coherency.c's table of
 * reserving contexts keys on the address and is never emptied, so reusing the
 * two keeps the test inside the eight slots it holds. */
static void reset_ctx(spu_context* ctx, uint32_t spu_id)
{
    spu_context_init(ctx, 0);
    ctx->spu_id = spu_id;
}

static void reset_pair(void)
{
    reset_ctx(&g_a, 0);
    reset_ctx(&g_b, 1);
    memset(g_guest_mem + LINE_A, 0, 0x600);
}

/* ===========================================================================
 * A peer's PUTLLC commit
 * ===========================================================================*/
static void test_putllc_breaks_peer(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    /* Both SPUs take the same line, exactly as five SPURS kernels do. */
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    CHECK(g_a.resv_valid == 1);
    CHECK(g_b.resv_valid == 1);

    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLC_CMD);

    TEST("the committing SPU's PUTLLC succeeds");
    CHECK_EQ_U32(g_b.atomic_stat, 0);
    CHECK(mem_is(LINE_A, 0xB0, 128));

    TEST("...and raises SPU_EVENT_LR on the peer holding the line");
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_spu_lr_raise == raised + 1);

    TEST("...and drops the peer's reservation");
    CHECK(g_a.resv_valid == 0);

    /* The committer consumed its reservation; hardware does not report that as
     * a loss, so it takes no event of its own. */
    TEST("...and does not raise a self-LR on the committer");
    CHECK_EQ_U32(g_b.event_status & SPU_EVENT_LR, 0);
    CHECK(g_b.resv_valid == 0);

    /* The whole point of the event: the peer's own commit has to lose. */
    TEST("the peer's PUTLLC then fails and writes nothing");
    stage(&g_a, LSA_A, 0xA0, 128);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 1);
    CHECK(mem_is(LINE_A, 0xB0, 128));
}

/* A PUTLLC that fails its own compare stored nothing, so there is nothing to
 * report -- the peer keeps its reservation. */
static void test_failed_putllc_is_quiet(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    g_guest_mem[LINE_A + 8] = 0x5A;          /* the line moves under B */
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);

    TEST("a PUTLLC that fails its compare raises nothing");
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_b.atomic_stat, 1);
    CHECK_EQ_U32(g_a.event_status, 0);
    CHECK(g_a.resv_valid == 1);
    CHECK(g_spu_lr_raise == raised);
    CHECK_EQ_U32(g_guest_mem[LINE_A + 8], 0x5A);   /* nothing was committed */
}

/* ===========================================================================
 * A peer's PUTLLUC
 * ===========================================================================*/
static void test_putlluc_breaks_peer(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);

    TEST("a peer's PUTLLUC raises SPU_EVENT_LR and drops the reservation");
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLUC_CMD);
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_a.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);
    CHECK(mem_is(LINE_A, 0xB0, 128));

    TEST("...so the peer's PUTLLC fails");
    stage(&g_a, LSA_A, 0xA0, 128);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 1);
    CHECK(mem_is(LINE_A, 0xB0, 128));

    /* Unlike PUTLLC, a PUTLLUC is unconditional: it invalidates every
     * reservation on the line including the issuer's own, and the issuer is
     * told, because it never made the compare that would have said so. */
    reset_pair();
    raised = g_spu_lr_raise;
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);

    TEST("a PUTLLUC over the issuer's own reservation reports it lost");
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLUC_CMD);
    CHECK_EQ_U32(g_b.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_b.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);
}

/* ===========================================================================
 * A peer's plain DMA PUT
 * ===========================================================================*/
static void test_plain_put_breaks_peer(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);

    TEST("a peer's plain PUT into the reserved line raises SPU_EVENT_LR");
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_put(&g_b, LSA_B, LINE_A, 128);
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_a.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);

    TEST("...and the transfer itself still lands");
    CHECK(mem_is(LINE_A, 0xB0, 128));

    TEST("...so the peer's PUTLLC fails");
    stage(&g_a, LSA_A, 0xA0, 128);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 1);
    CHECK(mem_is(LINE_A, 0xB0, 128));

    /* A PUT smaller than a line still covers it: the reservation is per line,
     * not per byte. */
    reset_pair();
    raised = g_spu_lr_raise;
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);

    TEST("a 16-byte PUT into the tail of the reserved line raises it too");
    stage(&g_b, LSA_B, 0xC0, 16);
    spu_put(&g_b, LSA_B, LINE_A + 112, 16);
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_a.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);
}

static void test_plain_put_elsewhere_is_quiet(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);

    TEST("a PUT into a different line leaves the reservation alone");
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_put(&g_b, LSA_B, LINE_B, 128);
    CHECK_EQ_U32(g_a.event_status, 0);
    CHECK(g_a.resv_valid == 1);
    CHECK(g_spu_lr_raise == raised);
    CHECK(mem_is(LINE_B, 0xB0, 128));

    TEST("...and a PUT ending one byte short of it does too");
    stage(&g_b, LSA_B, 0xC0, 128);
    spu_put(&g_b, LSA_B, LINE_A - 128, 128);
    CHECK_EQ_U32(g_a.event_status, 0);
    CHECK(g_a.resv_valid == 1);
    CHECK(g_spu_lr_raise == raised);
}

/* A bulk PUT covers many lines at once, and the reserved one can be anywhere
 * in the span -- including the last, which is the case a scan that stops early
 * gets wrong. */
static void test_wide_put_finds_the_reserved_line(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_a, LSA_A, LINE_D + 0x180, MFC_GETLLAR_CMD);   /* the 4th line */

    TEST("a PUT spanning four lines raises once, for the reserved one");
    stage(&g_b, LSA_B, 0xB0, 0x200);
    spu_put(&g_b, LSA_B, LINE_D, 0x200);
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_a.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);

    TEST("...and the whole span landed");
    CHECK(mem_is(LINE_D, 0xB0, 0x200));
}

/* The reserved line is copied under the lock and the rest of the span outside
 * it, so the partial first and last lines are their own pieces: each has to
 * land exactly, and nothing either side of the span may move. */
static void test_unaligned_wide_put_around_a_reserved_line(void)
{
    reset_pair();
    memset(g_guest_mem + LINE_D, 0xEE, 0x200);
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_a, LSA_A, LINE_D + 0x80, MFC_GETLLAR_CMD);    /* the middle line */

    TEST("an unaligned PUT over a reserved line raises once");
    stage(&g_b, LSA_B, 0xC0, 0x100);
    spu_put(&g_b, LSA_B, LINE_D + 0x40, 0x100);
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_a.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);

    TEST("...the span landed, partial lines included");
    CHECK(mem_is(LINE_D + 0x40, 0xC0, 0x100));

    TEST("...and the bytes either side of it did not move");
    int untouched = 1;
    for (uint32_t i = 0; i < 0x40; i++)    untouched &= g_guest_mem[LINE_D + i] == 0xEE;
    for (uint32_t i = 0x140; i < 0x200; i++) untouched &= g_guest_mem[LINE_D + i] == 0xEE;
    CHECK(untouched);
}

/* ===========================================================================
 * The event acknowledge against a concurrent raise
 *
 * Every producer of an event bit -- a peer's reservation-lost notification,
 * the PPU coherent store, a DMA stall-and-notify, a tag-status update -- sets
 * it with an atomic OR, and the acknowledge clears with an atomic AND. None of
 * them takes the lock-line lock, so the interleaving can no longer be built by
 * holding that lock. Two things are checked instead:
 *
 *   - that the acknowledge and the producers really are lock-free (they
 *     complete while the lock is held), so the notify that happens under the
 *     lock afterwards is an ordinary later raise that must survive; and
 *   - a real race, many rounds, of an acknowledge against a notify. A bare
 *     read-modify-write on either side can drop the other side's update; the
 *     atomics cannot, so every round must end with the LR edge kept and the
 *     acknowledged bit gone. The race is a smoke test, not a proof: the proof
 *     is that every writer of event_status is an atomic RMW.
 * ===========================================================================*/
#define ACK_TAG_EVENT  0x1u    /* MFC tag-status update: what the ack asks for */

static atomic_int g_ack_entered, g_ack_done;

static void* ack_thread(void* arg)
{
    (void)arg;
    atomic_store(&g_ack_entered, 1);
    wrch(&g_a, SPU_WrEventAck, ACK_TAG_EVENT);
    atomic_store(&g_ack_done, 1);
    return NULL;
}

static void test_ack_keeps_a_concurrent_raise(void)
{
    reset_pair();
    atomic_store(&g_ack_entered, 0);
    atomic_store(&g_ack_done, 0);

    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    g_a.event_status = ACK_TAG_EVENT;      /* the event the SPU is acking */

    spu_lockline_lock();
    pthread_t t;
    if (pthread_create(&t, NULL, ack_thread, NULL) != 0) {
        spu_lockline_unlock();
        TEST("the acknowledging thread starts");
        CHECK(0);
        return;
    }
    for (int i = 0; i < 2000 && !atomic_load(&g_ack_done); i++) Sleep(1);

    TEST("the acknowledge does not wait for the lock-line lock");
    CHECK(atomic_load(&g_ack_done) == 1);
    CHECK_EQ_U32(g_a.event_status, 0);

    /* The raise a peer's commit would make, under the lock that peer holds. */
    spu_coh_notify_write(LINE_A);
    spu_lockline_unlock();
    pthread_join(t, NULL);

    TEST("a raise after the acknowledge is kept");
    CHECK_EQ_U32(g_a.event_status, SPU_EVENT_LR);
    CHECK(g_a.resv_valid == 0);
}

#define ACK_RACE_ROUNDS 200000
static atomic_int g_ackr_go, g_ackr_ack_done, g_ackr_raise_done;

static void* ackr_acker(void* arg)
{
    (void)arg;
    for (int k = 1; k <= ACK_RACE_ROUNDS; k++) {
        while (atomic_load(&g_ackr_go) != k) { }
        for (unsigned i = 0; i < ((unsigned)(k * 7) & 15u); i++) YieldProcessor();
        wrch(&g_a, SPU_WrEventAck, ACK_TAG_EVENT);
        atomic_store(&g_ackr_ack_done, k);
    }
    return NULL;
}

static void* ackr_raiser(void* arg)
{
    (void)arg;
    for (int k = 1; k <= ACK_RACE_ROUNDS; k++) {
        while (atomic_load(&g_ackr_go) != k) { }
        for (unsigned i = 0; i < ((unsigned)(k * 13) & 15u); i++) YieldProcessor();
        spu_lockline_lock();
        spu_coh_notify_write(LINE_A);
        spu_lockline_unlock();
        atomic_store(&g_ackr_raise_done, k);
    }
    return NULL;
}

static void test_ack_races_a_raise(void)
{
    reset_pair();
    atomic_store(&g_ackr_go, 0);
    atomic_store(&g_ackr_ack_done, 0);
    atomic_store(&g_ackr_raise_done, 0);

    pthread_t a, r;
    if (pthread_create(&a, NULL, ackr_acker, NULL) != 0) {
        TEST("ack race threads start"); CHECK(0); return;
    }
    if (pthread_create(&r, NULL, ackr_raiser, NULL) != 0) {
        atomic_store(&g_ackr_go, -1);
        TEST("ack race threads start"); CHECK(0); return;
    }

    unsigned long lost_raise = 0, kept_ack = 0;
    for (int k = 1; k <= ACK_RACE_ROUNDS; k++) {
        spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
        g_a.event_status = ACK_TAG_EVENT;
        atomic_store(&g_ackr_go, k);
        while (atomic_load(&g_ackr_ack_done) != k ||
               atomic_load(&g_ackr_raise_done) != k) { }
        uint32_t s = __atomic_load_n(&g_a.event_status, __ATOMIC_ACQUIRE);
        if (!(s & SPU_EVENT_LR)) lost_raise++;
        if (s & ACK_TAG_EVENT)   kept_ack++;
    }
    pthread_join(a, NULL);
    pthread_join(r, NULL);

    TEST("an acknowledge racing a reservation-lost raise never drops the raise");
    CHECK(lost_raise == 0);
    TEST("...and always clears the bit it acknowledged");
    CHECK(kept_ack == 0);
}


/* A real MFC producer started while another thread holds the lock-line lock.
 * Its event update is an atomic OR and does not wait for the lock; a peer's LR
 * raise made under the lock afterwards must not replace the DMA notification,
 * nor the notification the raise. Atomic fixture flags synchronize the host
 * threads. */
static atomic_int g_raise_entered, g_raise_done;
static unsigned g_raise_mode;
static void* raise_thread(void* ignored)
{
    (void)ignored;
    atomic_store(&g_raise_entered, 1);
    if (g_raise_mode == 2) {
        /* One zero-length, stall-and-notify list element, tag five. */
        g_a.ls[0x1000] = 0x80;
        mfc_run_list(&g_a, 0x1000, 1, 0, 0, MFC_GET_CMD, 5);
    } else {
        wrch(&g_a, MFC_WrTagMask, 1);
        wrch(&g_a, MFC_WrTagUpdate, 1);
    }
    atomic_store(&g_raise_done, 1);
    return NULL;
}
static void test_producer_keeps_peer_event(unsigned bit)
{
    reset_pair();
    g_a.event_mask = bit;
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    g_a.event_status = 0;
    g_raise_mode = bit;
    atomic_store(&g_raise_entered, 0);
    atomic_store(&g_raise_done, 0);
    spu_lockline_lock();
    pthread_t t;
    if (pthread_create(&t, NULL, raise_thread, NULL)) {
        spu_lockline_unlock(); CHECK(0); return;
    }
    for (int i = 0; i < 2000 && !atomic_load(&g_raise_done); i++) Sleep(1);
    TEST("MFC event producer does not wait for the lock-line lock");
    CHECK(atomic_load(&g_raise_done) == 1);
    CHECK_EQ_U32(g_a.event_status, bit);
    spu_coh_notify_write(LINE_A);
    spu_lockline_unlock();
    pthread_join(t, NULL);
    TEST("MFC notification retains the concurrent reservation event");
    CHECK_EQ_U32(g_a.event_status, bit | SPU_EVENT_LR);
    if (bit == 2) {
        TEST("actual stalled tag remains pending for handler");
        CHECK_EQ_U32(g_a.list_stall_stat, 0x20);
        CHECK_EQ_U32(g_a.list_stall_mask, 0x20);
    }
}

/* ===========================================================================
 * main
 * ===========================================================================*/
int main(void)
{
    test_putllc_breaks_peer();
    test_failed_putllc_is_quiet();
    test_putlluc_breaks_peer();
    test_plain_put_breaks_peer();
    test_plain_put_elsewhere_is_quiet();
    test_wide_put_finds_the_reserved_line();
    test_unaligned_wide_put_around_a_reserved_line();
    test_ack_keeps_a_concurrent_raise();
    test_ack_races_a_raise();
    test_producer_keeps_peer_event(2);
    test_producer_keeps_peer_event(1);

    printf("\nSPU-to-SPU lock-line coherence tests: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the SPU channel layer, stubbed. These are what
 * spu_channels.c reaches for outside its own subsystem: the PPU's reservation
 * and watch helpers, the host clock, and the title-specific SPURS hooks. None
 * of them is on the path this test drives.
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

/* Loader diagnostics and worker publication are outside this fixture. */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
