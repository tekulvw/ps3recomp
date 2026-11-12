#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
/* PS3RECOMP_RSX_ENCODE_THREAD for a backend that records as it is called:
 * the engine's calls queued to a thread of their own (rsx_encode_queue.h).
 *
 * One producer (the FIFO walker, through the engine) and one consumer (the
 * encode thread). Calls go into a ring of fixed records whose pointer
 * arguments are copied into a ring of bytes; both rings are indexed by
 * counters the producer advances and the consumer retires, so neither side
 * takes a lock per call. The consumer holds the backend lock while it makes a
 * run of calls; a call the walker makes itself takes the same lock, and the
 * consumer gives it up between calls when the walker is waiting for it. */
#include "rsx_encode_queue.h"
#include "rsx_fifo_busy.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef SRWLOCK q_mutex;
typedef CONDITION_VARIABLE q_cond;
#define Q_MUTEX_INIT SRWLOCK_INIT
#define Q_COND_INIT CONDITION_VARIABLE_INIT
static void q_lock(q_mutex* m) { AcquireSRWLockExclusive(m); }
static void q_unlock(q_mutex* m) { ReleaseSRWLockExclusive(m); }
static int q_trylock(q_mutex* m) { return TryAcquireSRWLockExclusive(m) ? 0 : 1; }
static void q_wait(q_cond* c, q_mutex* m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
static void q_wait_us(q_cond* c, q_mutex* m, unsigned us) { SleepConditionVariableSRW(c, m, us / 1000u ? us / 1000u : 1u, 0); }
static void q_broadcast(q_cond* c) { WakeAllConditionVariable(c); }
static void q_yield(void) { SwitchToThread(); }
typedef HANDLE q_thread;
static DWORD WINAPI q_main_win(LPVOID arg);
static int q_thread_start(q_thread* t) { *t = CreateThread(NULL, 0, q_main_win, NULL, 0, NULL); return *t ? 0 : -1; }
static void q_thread_join(q_thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static uint64_t q_now_ns(void)
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000ull + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t)f.QuadPart;
}
#else
#include <pthread.h>
#include <sched.h>
typedef pthread_mutex_t q_mutex;
typedef pthread_cond_t q_cond;
#define Q_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define Q_COND_INIT PTHREAD_COND_INITIALIZER
static void q_lock(q_mutex* m) { pthread_mutex_lock(m); }
static void q_unlock(q_mutex* m) { pthread_mutex_unlock(m); }
static int q_trylock(q_mutex* m) { return pthread_mutex_trylock(m); }
static void q_wait(q_cond* c, q_mutex* m) { pthread_cond_wait(c, m); }
static void q_wait_us(q_cond* c, q_mutex* m, unsigned us)
{
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += (long)us * 1000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(c, m, &t);
}
static void q_broadcast(q_cond* c) { pthread_cond_broadcast(c); }
static void q_yield(void) { sched_yield(); }
typedef pthread_t q_thread;
static void* q_main(void* arg);
static int q_thread_start(q_thread* t) { return pthread_create(t, NULL, q_main, NULL); }
static void q_thread_join(q_thread t) { pthread_join(t, NULL); }
static uint64_t q_now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#endif

enum {
    OP_CALL = 1, OP_TEXTURE_RELEASE, OP_COLOR_RELEASE, OP_DEPTH_RELEASE, OP_PIPELINE_RELEASE,
    OP_BIND_TARGETS, OP_BIND_PIPELINE, OP_BIND_VS, OP_BIND_VS_ROWS, OP_REBIND_VS, OP_BIND_PS,
    OP_BIND_TEXTURES, OP_BIND_VTEXTURES, OP_VIEWPORT, OP_SCISSOR, OP_STENCIL_REF, OP_DEPTH_BOUNDS,
    OP_DRAW, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_PRESENT, OP_COLOR_WRITE, OP_SNAPSHOT_COPY, OP_SUBMIT_ASYNC
};

typedef struct {
    u32 kind;
    u32 a[7];
    float f[4];
    u64 q;
    u64 p_end;           /* the byte ring's counter once this record's bytes are done */
    u32 p0, n0, p1, n1;  /* payload offsets (in the byte ring) and lengths */
    void (*fn)(void*);
    void* arg;
} q_op;

#define Q_OPS      16384u
#define Q_BYTES    (64u << 20)
#define Q_TICKETS  4096u
#define Q_CHUNK    64u
#define Q_VS_KEEP  ((RSX_DSP_NUM_CONSTANTS + 2u) * 16u)

static struct {
    const rsx_draw_backend* real;
    rsx_encq_hooks hooks;
    rsx_draw_backend proxy;
    int wrapped, running;
    q_thread thread;
    q_op* ops;
    u8* bytes;
    _Atomic(u64) head, tail;             /* records: producer / consumer counters */
    u64 bw;                              /* bytes written (producer) */
    _Atomic(u64) br;                     /* bytes retired (consumer) */
    q_mutex qm;                          /* sleeping and waking only */
    q_cond work, done;
    _Atomic(int) consumer_sleeping, producer_waiting, stop;
    q_mutex bm;                          /* the backend */
    _Atomic(int) want_backend;
    /* Tickets: the queue's own numbers and the backend's for them. */
    u64 next_ticket;                     /* producer */
    u64 tmap[Q_TICKETS];
    _Atomic(u64) submitted, completed, want_ticket;
    _Atomic(int) failed;
    /* The vertex constants last bound, kept by the consumer for a rebind
     * the backend refuses. */
    u8* vs_keep;
    u32 vs_bytes, vs_mask[RSX_DSP_NUM_CONSTANTS / 32u];
    int vs_kind;                         /* 0 none, 1 whole block, 2 rows */
    int vs_bound;                        /* producer: a bind was queued since init */
    /* counters */
    unsigned long long ops_n, payload, full_waits, drains, immediates, lock_waits, tickets, snapshots, ordered;
    uint64_t full_wait_ns, drain_ns, lock_wait_ns;
    _Atomic(uint64_t) encode_ns;
    int log;
} Q = { .qm = Q_MUTEX_INIT, .work = Q_COND_INIT, .done = Q_COND_INIT, .bm = Q_MUTEX_INIT };

#define RU (Q.real->user)

static int q_busy_category(void)
{
    static int category = -2;
    if (category == -2) category = rsx_fifo_busy_category("encode_wait");
    return category;
}

/* ---- producer side ------------------------------------------------------- */

static void q_wake_consumer(void)
{
    if (!atomic_load(&Q.consumer_sleeping)) return;
    q_lock(&Q.qm);
    q_broadcast(&Q.work);
    q_unlock(&Q.qm);
}

/* Wait (on `done`) until pred() holds. */
static void q_wait_for(int (*pred)(void), uint64_t* ns, unsigned long long* count)
{
    if (pred()) return;
    const uint64_t t0 = q_now_ns();
    rsx_fifo_busy_enter(q_busy_category());
    q_wake_consumer();
    q_lock(&Q.qm);
    atomic_store(&Q.producer_waiting, 1);
    while (!pred()) q_wait(&Q.done, &Q.qm);
    atomic_store(&Q.producer_waiting, 0);
    q_unlock(&Q.qm);
    rsx_fifo_busy_leave(q_busy_category());
    if (ns) *ns += q_now_ns() - t0;
    if (count) ++*count;
}

static int q_empty(void) { return atomic_load(&Q.tail) == atomic_load(&Q.head); }
static int q_has_room(void) { return atomic_load(&Q.head) - atomic_load(&Q.tail) < Q_OPS; }
static u64 q_need;
static int q_bytes_room(void) { return Q.bw - atomic_load(&Q.br) + q_need <= Q_BYTES; }

/* A record to fill, with room for `n0 + n1` payload bytes (each part 16-byte
 * aligned); NULL when the payload cannot fit the ring at all. */
static q_op* q_reserve(u32 n0, u32 n1, u8** b0, u8** b1)
{
    const u32 a0 = (n0 + 15u) & ~15u, a1 = (n1 + 15u) & ~15u;
    const u64 n = (u64)a0 + a1;
    if (n > Q_BYTES / 2u) return NULL;
    q_wait_for(q_has_room, &Q.full_wait_ns, &Q.full_waits);
    u64 skip = 0;
    if (n) {
        const u64 pos = Q.bw % Q_BYTES;
        if (pos + n > Q_BYTES) skip = Q_BYTES - pos;
    }
    q_need = skip + n;
    q_wait_for(q_bytes_room, &Q.full_wait_ns, &Q.full_waits);
    Q.bw += skip;
    q_op* op = &Q.ops[atomic_load(&Q.head) % Q_OPS];
    memset(op, 0, sizeof *op);
    const u64 pos = Q.bw % Q_BYTES;
    op->p0 = (u32)pos; op->n0 = n0;
    op->p1 = (u32)(pos + a0); op->n1 = n1;
    if (b0) *b0 = Q.bytes + op->p0;
    if (b1) *b1 = Q.bytes + op->p1;
    Q.bw += n;
    op->p_end = Q.bw;
    Q.payload += n;
    return op;
}

static void q_commit(void)
{
    atomic_fetch_add(&Q.head, 1);
    Q.ops_n++;
    q_wake_consumer();
}

/* The backend for a call the walker makes itself. */
static void q_backend_lock(void)
{
    if (!q_trylock(&Q.bm)) return;
    const uint64_t t0 = q_now_ns();
    rsx_fifo_busy_enter(q_busy_category());
    atomic_fetch_add(&Q.want_backend, 1);
    q_lock(&Q.bm);
    atomic_fetch_sub(&Q.want_backend, 1);
    rsx_fifo_busy_leave(q_busy_category());
    Q.lock_wait_ns += q_now_ns() - t0;
    Q.lock_waits++;
}

/* ---- consumer side ------------------------------------------------------- */

static void q_vs_keep(const u8* data, u32 bytes, const u32* mask, int kind)
{
    if (!Q.vs_keep || bytes > Q_VS_KEEP) { Q.vs_kind = 0; return; }
    if (kind == 1) memcpy(Q.vs_keep, data, bytes);
    if (mask) memcpy(Q.vs_mask, mask, sizeof Q.vs_mask);
    Q.vs_bytes = bytes; Q.vs_kind = kind;
}

static void q_run(const q_op* op)
{
    const rsx_draw_backend* be = Q.real;
    u8* p0 = Q.bytes + op->p0;
    u8* p1 = Q.bytes + op->p1;
    switch (op->kind) {
    case OP_CALL: op->fn(op->arg); break;
    case OP_TEXTURE_RELEASE: be->texture_release(RU, op->a[0]); break;
    case OP_COLOR_RELEASE: be->color_target_release(RU, op->a[0]); break;
    case OP_DEPTH_RELEASE: be->depth_target_release(RU, op->a[0]); break;
    case OP_PIPELINE_RELEASE: be->pipeline_release(RU, op->a[0]); break;
    case OP_BIND_TARGETS: be->bind_targets(RU, op->a, op->a[5], op->a[4]); break;
    case OP_BIND_PIPELINE: be->bind_pipeline(RU, op->a[0]); break;
    case OP_BIND_VS:
        q_vs_keep(p0, op->n0, NULL, 1);
        be->bind_vs_constants(RU, Q.vs_kind == 1 ? Q.vs_keep : p0, op->n0);
        break;
    case OP_BIND_VS_ROWS: {
        /* Compact row runs and the transform are copied by the producer;
         * the separate mask payload owns the selection until this retires. */
        const u32 bytes = op->a[0];
        const u32* mask = (const u32*)p1;
        if (Q.vs_keep && bytes <= Q_VS_KEEP) {
            u32 row = 0, count = 0, offset = 0;
            for (; rsx_be_row_run(mask, &row, &count); row += count) {
                memcpy(Q.vs_keep + row * 16u, p0 + offset, count * 16u);
                offset += count * 16u;
            }
            memcpy(Q.vs_keep + bytes - 32u, p0 + offset, 32u);
            q_vs_keep(NULL, bytes, mask, 2);
            be->bind_vs_constant_rows(RU, Q.vs_keep, bytes, Q.vs_mask);
        }
        break;
    }
    case OP_REBIND_VS:
        if (be->rebind_vs_constants && be->rebind_vs_constants(RU)) break;
        if (Q.vs_kind == 1) be->bind_vs_constants(RU, Q.vs_keep, Q.vs_bytes);
        else if (Q.vs_kind == 2) be->bind_vs_constant_rows(RU, Q.vs_keep, Q.vs_bytes, Q.vs_mask);
        break;
    case OP_BIND_PS: be->bind_ps_constants(RU, op->n0 ? p0 : NULL, op->n0); break;
    case OP_BIND_TEXTURES:
        be->bind_textures(RU, op->n0 ? (const u32*)p0 : NULL, op->n1 ? (const rsx_be_sampler_desc*)p1 : NULL, op->a[0]);
        break;
    case OP_BIND_VTEXTURES:
        be->bind_vertex_textures(RU, op->n0 ? (const u32*)p0 : NULL, op->n1 ? (const rsx_be_sampler_desc*)p1 : NULL, op->a[0]);
        break;
    case OP_VIEWPORT: be->set_viewport(RU, op->f[0], op->f[1], op->f[2], op->f[3]); break;
    case OP_SCISSOR: be->set_scissor(RU, op->a[0], op->a[1], op->a[2], op->a[3]); break;
    case OP_STENCIL_REF: be->set_stencil_ref(RU, op->a[0]); break;
    case OP_DEPTH_BOUNDS: be->set_depth_bounds(RU, op->f[0], op->f[1]); break;
    case OP_DRAW:
        be->draw(RU, (rsx_topology)op->a[0], p0, op->a[1], op->a[2],
                 op->a[4] ? (const u32*)p1 : NULL, op->a[3]);
        break;
    case OP_CLEAR_COLOR: be->clear_color(RU, op->a[0], op->f); break;
    case OP_CLEAR_DS: be->clear_depth_stencil(RU, op->a[0], op->a[1], op->f[0], (u8)op->a[2]); break;
    case OP_PRESENT: be->present(RU, op->a[0]); break;
    case OP_COLOR_WRITE:
        be->color_target_write(RU, op->a[0], op->a[1], op->a[2], op->a[3], op->a[4], p0, op->a[5]);
        break;
    case OP_SNAPSHOT_COPY: Q.hooks.color_snapshot_copy(RU, op->a[0], op->a[1], op->a[2]); break;
    case OP_SUBMIT_ASYNC: {
        const u64 t = be->submit_async(RU, op->a[0]);
        Q.tmap[op->q % Q_TICKETS] = t;
        if (!t) atomic_store(&Q.failed, 1);
        atomic_store(&Q.submitted, op->q);
        break;
    }
    default: break;
    }
}

/* What the backend reports complete, in order, without waiting. */
static int q_poll_tickets(void)
{
    int moved = 0;
    for (u64 p = atomic_load(&Q.completed) + 1u; p <= atomic_load(&Q.submitted); p++) {
        const int st = Q.real->completion_status(RU, Q.tmap[p % Q_TICKETS], 0);
        if (st < 0) { atomic_store(&Q.failed, 1); moved = 1; break; }
        if (st == 0) break;
        atomic_store(&Q.completed, p);
        moved = 1;
    }
    return moved;
}

static void q_signal_producer(void)
{
    if (!atomic_load(&Q.producer_waiting)) return;
    q_lock(&Q.qm);
    q_broadcast(&Q.done);
    q_unlock(&Q.qm);
}

static void q_loop(void)
{
    for (;;) {
        const u64 want = atomic_load(&Q.want_ticket);
        const int ticket_wait = want && want <= atomic_load(&Q.submitted) && want > atomic_load(&Q.completed);
        if (q_empty() && !ticket_wait) {
            /* Idle: keep the completions fresh while tickets are out. */
            int moved = 0;
            if (atomic_load(&Q.submitted) > atomic_load(&Q.completed) && !q_trylock(&Q.bm)) {
                moved = q_poll_tickets();
                q_unlock(&Q.bm);
            }
            if (moved) q_signal_producer();
            q_lock(&Q.qm);
            atomic_store(&Q.consumer_sleeping, 1);
            if (q_empty() && !atomic_load(&Q.stop) &&
                !(atomic_load(&Q.want_ticket) && atomic_load(&Q.want_ticket) <= atomic_load(&Q.submitted))) {
                q_broadcast(&Q.done);
                if (atomic_load(&Q.submitted) > atomic_load(&Q.completed)) q_wait_us(&Q.work, &Q.qm, 200u);
                else q_wait(&Q.work, &Q.qm);
            }
            atomic_store(&Q.consumer_sleeping, 0);
            const int stop = atomic_load(&Q.stop) && q_empty();
            q_unlock(&Q.qm);
            if (stop) return;
            continue;
        }
        const uint64_t t0 = q_now_ns();
        q_lock(&Q.bm);
        u64 t = atomic_load(&Q.tail);
        const u64 h = atomic_load(&Q.head);
        for (u32 k = 0; t < h && k < Q_CHUNK; k++, t++) {
            const q_op* op = &Q.ops[t % Q_OPS];
            q_run(op);
            atomic_store(&Q.br, op->p_end);
            atomic_store(&Q.tail, t + 1u);
            if (atomic_load(&Q.want_backend)) break;
        }
        /* A walker waiting on a ticket the queue has now submitted. */
        const u64 w = atomic_load(&Q.want_ticket);
        if (w && w <= atomic_load(&Q.submitted) && w > atomic_load(&Q.completed)) {
            const int st = Q.real->completion_status(RU, Q.tmap[w % Q_TICKETS], 1);
            if (st == 1) atomic_store(&Q.completed, w);
            else atomic_store(&Q.failed, 1);
        } else q_poll_tickets();
        q_unlock(&Q.bm);
        atomic_fetch_add(&Q.encode_ns, q_now_ns() - t0);
        q_signal_producer();
        while (atomic_load(&Q.want_backend)) q_yield();
    }
}

#ifdef _WIN32
static DWORD WINAPI q_main_win(LPVOID arg) { (void)arg; q_loop(); return 0; }
#else
static void* q_main(void* arg)
{
    (void)arg;
#ifdef __APPLE__
    pthread_setname_np("rsx encode");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "rsx encode");
#endif
    q_loop();
    return NULL;
}
#endif

/* ---- public -------------------------------------------------------------- */

int rsx_encq_active(void) { return Q.running; }

void rsx_encq_drain(void)
{
    if (!Q.running) return;
    if (q_empty()) return;
    Q.drains++;
    q_wait_for(q_empty, &Q.drain_ns, NULL);
}

void rsx_encq_lock(void) { if (Q.running) q_backend_lock(); }
void rsx_encq_unlock(void) { if (Q.running) q_unlock(&Q.bm); }

u64 rsx_encq_call(void (*fn)(void*), void* arg)
{
    if (!Q.running) { fn(arg); return 0; }
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_CALL; op->fn = fn; op->arg = arg;
    const u64 place = atomic_load(&Q.head) + 1u;
    q_commit();
    return place;
}

static u64 q_wait_place;
static int q_place_done(void) { return atomic_load(&Q.tail) >= q_wait_place; }
void rsx_encq_wait_done(u64 place)
{
    if (!Q.running || !place || atomic_load(&Q.tail) >= place) return;
    q_wait_place = place;
    Q.drains++;
    q_wait_for(q_place_done, &Q.drain_ns, NULL);
}

void rsx_encq_get_stats(rsx_encq_stats* out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->ops = Q.ops_n; out->payload_bytes = Q.payload; out->full_waits = Q.full_waits;
    out->drains = Q.drains; out->immediates = Q.immediates; out->lock_waits = Q.lock_waits;
    out->tickets = Q.tickets; out->snapshots_split = Q.snapshots; out->ordered_calls = Q.ordered;
    out->full_wait_ms = Q.full_wait_ns / 1e6; out->drain_ms = Q.drain_ns / 1e6;
    out->lock_wait_ms = Q.lock_wait_ns / 1e6; out->encode_ms = atomic_load(&Q.encode_ns) / 1e6;
}

static void q_report(void)
{
    static uint64_t last;
    if (!Q.log) return;
    const uint64_t now = q_now_ns();
    if (last && now - last < 2000000000ull) return;
    last = now;
    fprintf(stderr, "[rsx encode queue] ops=%llu payload_mb=%.1f encode_ms=%.1f full_waits=%llu full_wait_ms=%.1f "
            "drains=%llu drain_ms=%.1f ordered=%llu immediate=%llu lock_waits=%llu lock_wait_ms=%.1f "
            "tickets=%llu snapshots=%llu\n",
            Q.ops_n, Q.payload / 1048576.0, atomic_load(&Q.encode_ns) / 1e6, Q.full_waits, Q.full_wait_ns / 1e6,
            Q.drains, Q.drain_ns / 1e6, Q.ordered, Q.immediates, Q.lock_waits, Q.lock_wait_ns / 1e6,
            Q.tickets, Q.snapshots);
}

static int q_start(void)
{
    if (Q.running) return 0;
    if (!Q.ops) Q.ops = (q_op*)calloc(Q_OPS, sizeof *Q.ops);
    if (!Q.bytes) Q.bytes = (u8*)malloc(Q_BYTES);
    if (!Q.vs_keep) Q.vs_keep = (u8*)calloc(1, Q_VS_KEEP);
    if (!Q.ops || !Q.bytes || !Q.vs_keep) return -1;
    atomic_store(&Q.head, 0); atomic_store(&Q.tail, 0);
    Q.bw = 0; atomic_store(&Q.br, 0);
    atomic_store(&Q.stop, 0); atomic_store(&Q.want_backend, 0);
    Q.next_ticket = 0;
    atomic_store(&Q.submitted, 0); atomic_store(&Q.completed, 0); atomic_store(&Q.want_ticket, 0);
    atomic_store(&Q.failed, 0);
    Q.vs_kind = 0; Q.vs_bound = 0;
    { const char* l = getenv("PS3RECOMP_RSX_INFLIGHT_LOG"); Q.log = l && !strcmp(l, "1"); }
    if (q_thread_start(&Q.thread)) return -1;
    Q.running = 1;
    fprintf(stderr, "[rsx encode queue] encode thread on\n");
    return 0;
}

static void q_stop(void)
{
    if (!Q.running) return;
    rsx_encq_drain();
    q_lock(&Q.qm);
    atomic_store(&Q.stop, 1);
    q_broadcast(&Q.work);
    q_unlock(&Q.qm);
    q_thread_join(Q.thread);
    Q.running = 0;
}

/* ---- the proxy vtable ---------------------------------------------------- */

#define DIRECT_IF_OFF(call) do { if (!Q.running) return call; } while (0)
#define IMMEDIATE(type, call) do { q_backend_lock(); Q.immediates++; type r_ = call; q_unlock(&Q.bm); return r_; } while (0)
#define ORDERED(type, call) do { rsx_encq_drain(); q_backend_lock(); Q.ordered++; type r_ = call; q_unlock(&Q.bm); return r_; } while (0)
#define ORDERED_VOID(call) do { rsx_encq_drain(); q_backend_lock(); Q.ordered++; call; q_unlock(&Q.bm); } while (0)

static int p_init(void* u, u32 w, u32 h)
{
    (void)u;
    const int r = Q.real->init(RU, w, h);
    if (!r && q_start()) fprintf(stderr, "[rsx encode queue] no encode thread; the backend is called directly\n");
    return r;
}
static void p_shutdown(void* u)
{
    (void)u;
    q_stop();
    Q.real->shutdown(RU);
}
static void p_submit_and_wait(void* u, u32 reason)
{
    (void)u;
    if (!Q.running) { Q.real->submit_and_wait(RU, reason); return; }
    ORDERED_VOID(Q.real->submit_and_wait(RU, reason));
}
static u32 p_texture_create(void* u, rsx_be_format fmt, u32 w, u32 h, u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->texture_create(RU, fmt, w, h, mips, faces, remap, rsx_fmt));
    IMMEDIATE(u32, Q.real->texture_create(RU, fmt, w, h, mips, faces, remap, rsx_fmt));
}
/* Only ever a texture just made, before anything draws with it. */
static void p_texture_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h, const void* src, u32 row_bytes, u32 rows)
{
    (void)u;
    if (!Q.running) { Q.real->texture_upload(RU, t, face, mip, w, h, src, row_bytes, rows); return; }
    q_backend_lock(); Q.immediates++;
    Q.real->texture_upload(RU, t, face, mip, w, h, src, row_bytes, rows);
    q_unlock(&Q.bm);
}
static void p_release(u32 kind, u32 h)
{
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = kind; op->a[0] = h;
    q_commit();
}
static void p_texture_release(void* u, u32 t)
{ (void)u; if (!Q.running) { Q.real->texture_release(RU, t); return; } p_release(OP_TEXTURE_RELEASE, t); }
static void p_color_target_release(void* u, u32 s)
{ (void)u; if (!Q.running) { Q.real->color_target_release(RU, s); return; } p_release(OP_COLOR_RELEASE, s); }
static void p_depth_target_release(void* u, u32 d)
{ (void)u; if (!Q.running) { Q.real->depth_target_release(RU, d); return; } p_release(OP_DEPTH_RELEASE, d); }
static void p_pipeline_release(void* u, u32 p)
{ (void)u; if (!Q.running) { Q.real->pipeline_release(RU, p); return; } p_release(OP_PIPELINE_RELEASE, p); }
static u32 p_color_target_create(void* u, rsx_be_format fmt, u32 w, u32 h, const void* seed, u32 seed_row_bytes)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->color_target_create(RU, fmt, w, h, seed, seed_row_bytes));
    IMMEDIATE(u32, Q.real->color_target_create(RU, fmt, w, h, seed, seed_row_bytes));
}
static void p_color_target_write(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, const void* rows, u32 row_bytes)
{
    (void)u;
    if (!Q.running) { Q.real->color_target_write(RU, s, x, y, w, h, rows, row_bytes); return; }
    u8* b0 = NULL;
    const u64 n = (u64)h * row_bytes;
    q_op* op = n <= 0xFFFFFFFFull ? q_reserve((u32)n, 0, &b0, NULL) : NULL;
    if (!op) { ORDERED_VOID(Q.real->color_target_write(RU, s, x, y, w, h, rows, row_bytes)); return; }
    if (n && rows) memcpy(b0, rows, (size_t)n);
    op->kind = OP_COLOR_WRITE;
    op->a[0] = s; op->a[1] = x; op->a[2] = y; op->a[3] = w; op->a[4] = h; op->a[5] = row_bytes;
    q_commit();
}
static u32 p_surface_view(void* u, u32 s, u32 remap, u32 fmt)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->surface_view(RU, s, remap, fmt));
    IMMEDIATE(u32, Q.real->surface_view(RU, s, remap, fmt));
}
static u32 p_depth_target_create(void* u, u32 w, u32 h)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->depth_target_create(RU, w, h));
    IMMEDIATE(u32, Q.real->depth_target_create(RU, w, h));
}
static u32 p_depth_snapshot(void* u, u32 d, u32 w, u32 h)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->depth_snapshot(RU, d, w, h));
    ORDERED(u32, Q.real->depth_snapshot(RU, d, w, h));
}
static u32 p_color_snapshot(void* u, u32 s)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->color_snapshot(RU, s));
    if (Q.hooks.color_snapshot_alloc && Q.hooks.color_snapshot_copy) {
        u32 token = 0;
        q_backend_lock();
        const u32 snap = Q.hooks.color_snapshot_alloc(RU, s, &token);
        q_unlock(&Q.bm);
        if (!snap) return 0;
        q_op* op = q_reserve(0, 0, NULL, NULL);
        op->kind = OP_SNAPSHOT_COPY; op->a[0] = snap; op->a[1] = s; op->a[2] = token;
        q_commit();
        Q.snapshots++;
        return snap;
    }
    ORDERED(u32, Q.real->color_snapshot(RU, s));
}
static u32 p_pipeline_create(void* u, const char* vs, const char* ps, const rsx_be_render_state* rs,
                             const rsx_vertex_layout_plan* layout, u32 stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->pipeline_create(RU, vs, ps, rs, layout, stride, rt_fmt, rt_count));
    IMMEDIATE(u32, Q.real->pipeline_create(RU, vs, ps, rs, layout, stride, rt_fmt, rt_count));
}
static void p_bind_targets(void* u, const u32* s, u32 count, u32 depth)
{
    (void)u;
    if (!Q.running) { Q.real->bind_targets(RU, s, count, depth); return; }
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_BIND_TARGETS;
    for (u32 i = 0; i < count; i++) op->a[i] = s ? s[i] : 0;
    op->a[4] = depth; op->a[5] = count;
    q_commit();
}
static void p_bind_pipeline(void* u, u32 p)
{
    (void)u;
    if (!Q.running) { Q.real->bind_pipeline(RU, p); return; }
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_BIND_PIPELINE; op->a[0] = p;
    q_commit();
}
static void p_bind_vs(void* u, const void* data, u32 bytes)
{
    (void)u;
    if (!Q.running) { Q.real->bind_vs_constants(RU, data, bytes); return; }
    u8* b0 = NULL;
    q_op* op = q_reserve(data ? bytes : 0, 0, &b0, NULL);
    if (!op) { ORDERED_VOID(Q.real->bind_vs_constants(RU, data, bytes)); Q.vs_bound = 0; return; }
    if (data && bytes) memcpy(b0, data, bytes);
    op->kind = OP_BIND_VS;
    q_commit();
    Q.vs_bound = data && bytes;
}
static void p_bind_vs_rows(void* u, const void* data, u32 bytes, const u32* mask)
{
    (void)u;
    if (!Q.running) { Q.real->bind_vs_constant_rows(RU, data, bytes, mask); return; }
    if (!data || bytes > Q_VS_KEEP || !rsx_be_row_mask_fits(mask, bytes)) {
        p_bind_vs(u, data, bytes);
        return;
    }
    u32 row = 0, count = 0, payload = 32u;
    for (; rsx_be_row_run(mask, &row, &count); row += count) payload += count * 16u;
    u8* b0 = NULL; u8* b1 = NULL;
    q_op* op = q_reserve(payload, sizeof Q.vs_mask, &b0, &b1);
    const u8* src = (const u8*)data;
    row = 0;
    u32 offset = 0;
    for (; rsx_be_row_run(mask, &row, &count); row += count) {
        memcpy(b0 + offset, src + row * 16u, count * 16u);
        offset += count * 16u;
    }
    memcpy(b0 + offset, src + bytes - 32u, 32u);
    memcpy(b1, mask, sizeof Q.vs_mask);
    op->kind = OP_BIND_VS_ROWS; op->a[0] = bytes;
    q_commit();
    Q.vs_bound = 1;
}
static int p_rebind_vs(void* u)
{
    (void)u;
    if (!Q.running) return Q.real->rebind_vs_constants ? Q.real->rebind_vs_constants(RU) : 0;
    if (!Q.vs_bound) return 0;
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_REBIND_VS;
    q_commit();
    return 1;
}
static void p_bind_ps(void* u, const void* data, u32 bytes)
{
    (void)u;
    if (!Q.running) { Q.real->bind_ps_constants(RU, data, bytes); return; }
    u8* b0 = NULL;
    q_op* op = q_reserve(data ? bytes : 0, 0, &b0, NULL);
    if (!op) { ORDERED_VOID(Q.real->bind_ps_constants(RU, data, bytes)); return; }
    if (data && bytes) memcpy(b0, data, bytes);
    op->kind = OP_BIND_PS;
    q_commit();
}
static void p_bind_tex_common(u32 kind, u32 max, const u32* t, const rsx_be_sampler_desc* s, u32 mask)
{
    u8 *b0 = NULL, *b1 = NULL;
    q_op* op = q_reserve(t ? max * (u32)sizeof(u32) : 0, s ? max * (u32)sizeof(*s) : 0, &b0, &b1);
    if (t) memcpy(b0, t, max * sizeof(u32));
    if (s) memcpy(b1, s, max * sizeof(*s));
    op->kind = kind; op->a[0] = mask;
    q_commit();
}
static void p_bind_textures(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 mask)
{
    (void)u;
    if (!Q.running) { Q.real->bind_textures(RU, t, s, mask); return; }
    p_bind_tex_common(OP_BIND_TEXTURES, RSX_BE_MAX_TEXTURES, t, s, mask);
}
static void p_bind_vertex_textures(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 mask)
{
    (void)u;
    if (!Q.running) { Q.real->bind_vertex_textures(RU, t, s, mask); return; }
    p_bind_tex_common(OP_BIND_VTEXTURES, RSX_BE_MAX_VERTEX_TEXTURES, t, s, mask);
}
static void p_set_viewport(void* u, float x, float y, float w, float h)
{
    (void)u;
    if (!Q.running) { Q.real->set_viewport(RU, x, y, w, h); return; }
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_VIEWPORT; op->f[0] = x; op->f[1] = y; op->f[2] = w; op->f[3] = h;
    q_commit();
}
static void p_set_scissor(void* u, u32 x, u32 y, u32 w, u32 h)
{
    (void)u;
    if (!Q.running) { Q.real->set_scissor(RU, x, y, w, h); return; }
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_SCISSOR; op->a[0] = x; op->a[1] = y; op->a[2] = w; op->a[3] = h;
    q_commit();
}
static void p_set_stencil_ref(void* u, u32 ref)
{
    (void)u;
    if (!Q.running) { Q.real->set_stencil_ref(RU, ref); return; }
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_STENCIL_REF; op->a[0] = ref;
    q_commit();
}
static void p_set_depth_bounds(void* u, float lo, float hi)
{
    (void)u;
    if (!Q.running) { Q.real->set_depth_bounds(RU, lo, hi); return; }
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_DEPTH_BOUNDS; op->f[0] = lo; op->f[1] = hi;
    q_commit();
}
static void p_draw(void* u, rsx_topology topology, const void* vertices, u32 vertex_count, u32 stride,
                   const u32* indices, u32 index_count)
{
    (void)u;
    if (!Q.running) { Q.real->draw(RU, topology, vertices, vertex_count, stride, indices, index_count); return; }
    const u64 vb = vertices ? (u64)vertex_count * stride : 0, ib = indices ? (u64)index_count * 4u : 0;
    u8 *b0 = NULL, *b1 = NULL;
    q_op* op = (vb <= Q_BYTES && ib <= Q_BYTES) ? q_reserve((u32)vb, (u32)ib, &b0, &b1) : NULL;
    if (!op) {
        ORDERED_VOID(Q.real->draw(RU, topology, vertices, vertex_count, stride, indices, index_count));
        return;
    }
    if (vb) memcpy(b0, vertices, (size_t)vb);
    if (ib) memcpy(b1, indices, (size_t)ib);
    op->kind = OP_DRAW;
    op->a[0] = (u32)topology; op->a[1] = vertex_count; op->a[2] = stride; op->a[3] = index_count;
    op->a[4] = indices != NULL;
    q_commit();
    q_report();
}
static void p_clear_color(void* u, u32 s, const float rgba[4])
{
    (void)u;
    if (!Q.running) { Q.real->clear_color(RU, s, rgba); return; }
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_CLEAR_COLOR; op->a[0] = s;
    memcpy(op->f, rgba, sizeof op->f);
    q_commit();
}
static void p_clear_depth_stencil(void* u, u32 d, u32 flags, float depth, u8 stencil)
{
    (void)u;
    if (!Q.running) { Q.real->clear_depth_stencil(RU, d, flags, depth, stencil); return; }
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_CLEAR_DS; op->a[0] = d; op->a[1] = flags; op->a[2] = stencil; op->f[0] = depth;
    q_commit();
}
static void p_present(void* u, u32 s)
{
    (void)u;
    if (!Q.running) { Q.real->present(RU, s); return; }
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_PRESENT; op->a[0] = s;
    q_commit();
}
static void p_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, void* out, u32 pitch)
{
    (void)u;
    if (!Q.running) { Q.real->readback(RU, s, x, y, w, h, out, pitch); return; }
    ORDERED_VOID(Q.real->readback(RU, s, x, y, w, h, out, pitch));
}
static int p_depth_readback(void* u, u32 d, u32 x, u32 y, u32 w, u32 h, float* out, u32 pitch)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->depth_readback(RU, d, x, y, w, h, out, pitch));
    ORDERED(int, Q.real->depth_readback(RU, d, x, y, w, h, out, pitch));
}
static int p_depth_store_z24(void* u, u32 d, u32 w, u32 h, u8* dst, u32 pitch)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->depth_store_z24(RU, d, w, h, dst, pitch));
    ORDERED(int, Q.real->depth_store_z24(RU, d, w, h, dst, pitch));
}
static u32 p_depth_z24_texture(void* u, u32 w, u32 h, u32 remap, u32 fmt)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->depth_z24_texture(RU, w, h, remap, fmt));
    IMMEDIATE(u32, Q.real->depth_z24_texture(RU, w, h, remap, fmt));
}
static u32 p_depth_z24_view(void* u, u32 d, u32 w, u32 h, u32 remap, u32 fmt)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->depth_z24_view(RU, d, w, h, remap, fmt));
    ORDERED(u32, Q.real->depth_z24_view(RU, d, w, h, remap, fmt));
}
static u32 p_texture_create_3d(void* u, rsx_be_format fmt, u32 w, u32 h, u32 d, u32 mips, u32 remap, u32 rsx_fmt)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->texture_create_3d(RU, fmt, w, h, d, mips, remap, rsx_fmt));
    IMMEDIATE(u32, Q.real->texture_create_3d(RU, fmt, w, h, d, mips, remap, rsx_fmt));
}
static void p_texture_upload_3d(void* u, u32 t, u32 z, u32 mip, u32 w, u32 h, const void* src, u32 row_bytes)
{
    (void)u;
    if (!Q.running) { Q.real->texture_upload_3d(RU, t, z, mip, w, h, src, row_bytes); return; }
    q_backend_lock(); Q.immediates++;
    Q.real->texture_upload_3d(RU, t, z, mip, w, h, src, row_bytes);
    q_unlock(&Q.bm);
}
static int p_submit_and_wait_checked(void* u, u32 reason)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->submit_and_wait_checked(RU, reason));
    ORDERED(int, Q.real->submit_and_wait_checked(RU, reason));
}
static int p_capture_texture_rgba8(void* u, u32 t, const char* path)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->capture_texture_rgba8(RU, t, path));
    ORDERED(int, Q.real->capture_texture_rgba8(RU, t, path));
}
static u64 p_submit_async(void* u, u32 reason)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->submit_async(RU, reason));
    if (atomic_load(&Q.failed)) return 0;
    /* At most Q_TICKETS outstanding: wait for the oldest beyond that. */
    if (Q.next_ticket + 1u - atomic_load(&Q.completed) > Q_TICKETS) {
        const u64 oldest = Q.next_ticket + 1u - Q_TICKETS;
        if (Q.proxy.completion_status(NULL, oldest, 1) != 1) return 0;
    }
    q_op* op = q_reserve(0, 0, NULL, NULL);
    op->kind = OP_SUBMIT_ASYNC; op->a[0] = reason; op->q = ++Q.next_ticket;
    q_commit();
    Q.tickets++;
    return op->q;
}
static int q_ticket_done(void)
{
    const u64 w = atomic_load(&Q.want_ticket);
    return atomic_load(&Q.failed) || atomic_load(&Q.completed) >= w;
}
static int p_completion_status(void* u, u64 ticket, int wait)
{
    (void)u;
    DIRECT_IF_OFF(Q.real->completion_status(RU, ticket, wait));
    if (!ticket || ticket > Q.next_ticket || atomic_load(&Q.failed)) return -1;
    if (ticket <= atomic_load(&Q.completed)) return 1;
    /* Submitted already and the backend is free: ask it now. */
    if (ticket <= atomic_load(&Q.submitted) && !q_trylock(&Q.bm)) {
        q_poll_tickets();
        q_unlock(&Q.bm);
        if (atomic_load(&Q.failed)) return -1;
        if (ticket <= atomic_load(&Q.completed)) return 1;
    }
    if (!wait) return 0;
    atomic_store(&Q.want_ticket, ticket);
    q_wait_for(q_ticket_done, &Q.drain_ns, &Q.drains);
    atomic_store(&Q.want_ticket, 0);
    if (atomic_load(&Q.failed)) return -1;
    return ticket <= atomic_load(&Q.completed) ? 1 : 0;
}

const rsx_draw_backend* rsx_encq_wrap(const rsx_draw_backend* real, const rsx_encq_hooks* hooks)
{
    const char* e = getenv("PS3RECOMP_RSX_ENCODE_THREAD");
    if (!real || !e || strcmp(e, "1")) return NULL;
    if (Q.running) return NULL;
    Q.real = real;
    memset(&Q.hooks, 0, sizeof Q.hooks);
    if (hooks) Q.hooks = *hooks;
    rsx_draw_backend p;
    memset(&p, 0, sizeof p);
    p.user = &Q;
#define SET(name, fn) p.name = real->name ? fn : NULL
    SET(init, p_init); SET(shutdown, p_shutdown); SET(submit_and_wait, p_submit_and_wait);
    SET(texture_create, p_texture_create); SET(texture_upload, p_texture_upload);
    SET(texture_release, p_texture_release); SET(color_target_create, p_color_target_create);
    SET(color_target_release, p_color_target_release); SET(color_target_write, p_color_target_write);
    SET(surface_view, p_surface_view); SET(depth_target_create, p_depth_target_create);
    SET(depth_target_release, p_depth_target_release); SET(depth_snapshot, p_depth_snapshot);
    SET(pipeline_create, p_pipeline_create); SET(pipeline_release, p_pipeline_release);
    SET(bind_targets, p_bind_targets); SET(bind_pipeline, p_bind_pipeline);
    SET(bind_vs_constants, p_bind_vs); SET(bind_ps_constants, p_bind_ps);
    SET(bind_textures, p_bind_textures); SET(bind_vertex_textures, p_bind_vertex_textures);
    SET(set_viewport, p_set_viewport); SET(set_scissor, p_set_scissor);
    SET(set_stencil_ref, p_set_stencil_ref); SET(set_depth_bounds, p_set_depth_bounds);
    SET(draw, p_draw); SET(clear_color, p_clear_color); SET(clear_depth_stencil, p_clear_depth_stencil);
    SET(present, p_present); SET(readback, p_readback); SET(depth_readback, p_depth_readback);
    SET(depth_store_z24, p_depth_store_z24); SET(depth_z24_texture, p_depth_z24_texture);
    SET(depth_z24_view, p_depth_z24_view); SET(color_snapshot, p_color_snapshot);
    SET(texture_create_3d, p_texture_create_3d); SET(texture_upload_3d, p_texture_upload_3d);
    SET(submit_and_wait_checked, p_submit_and_wait_checked);
    SET(capture_texture_rgba8, p_capture_texture_rgba8);
    SET(submit_async, p_submit_async); SET(completion_status, p_completion_status);
    /* Answered here whether the backend can reuse an upload or not. */
    SET(bind_vs_constants, p_bind_vs);
    p.rebind_vs_constants = real->bind_vs_constants ? p_rebind_vs : NULL;
    SET(bind_vs_constant_rows, p_bind_vs_rows);
#undef SET
    /* Tickets need both halves. */
    if (!real->submit_async || !real->completion_status) { p.submit_async = NULL; p.completion_status = NULL; }
    Q.proxy = p;
    Q.wrapped = 1;
    return &Q.proxy;
}

void rsx_encq_unwrap(void)
{
    q_stop();
    Q.wrapped = 0;
    Q.real = NULL;
}
