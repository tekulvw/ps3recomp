/*
 * Host threads for async SPU tasks (runtime/spu/spu_task_pool.inc), on
 * Windows and POSIX, with PS3RECOMP_SPU_TASK_POOL unset or =1.
 *
 * Every task here does what spu_workload.c's task body does to a reused
 * thread: it puts back per-task thread-local state first, and after the task
 * drops every registration keyed by an address on its stack. The fixture
 * checks the pool keeps the promises that body relies on:
 *  - every task starts at once on a thread of its own, however many are
 *    blocked (a task waiting on a later one cannot be queued behind it);
 *  - every thread has the 256 MB reservation (and on Windows the 256 KB stack
 *    guarantee), first task or reused;
 *  - spu_task_thread_stack covers the task's frames and does not move, so a
 *    scrub after one task leaves nothing of it for the next;
 *  - the pool reuses threads, trims idle ones and shuts down;
 *  - results are the same with the pool off and on (tools/ compares them).
 *
 * Build: tools/test_spu_task_pool.py. Exit code 0 if all passed.
 */
#include "../spu_task_pool.inc"
#include <stdatomic.h>

static int g_pass, g_fail;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* ---- Registrations keyed by stack addresses ----------------------------
 * Stands in for the lock-line reserver set and the MFC engines: entries keyed
 * by the address of a task's stack local, dropped by range. */
#define REG_MAX 4096
static uintptr_t g_reg[REG_MAX];
static atomic_int g_reg_lock;
static void reg_lock(void)   { while (atomic_exchange(&g_reg_lock, 1)) sleep_ms(0); }
static void reg_unlock(void) { atomic_store(&g_reg_lock, 0); }
static void reg_add(const void* p)
{
    reg_lock();
    for (unsigned i = 0; i < REG_MAX; i++) if (!g_reg[i]) { g_reg[i] = (uintptr_t)p; break; }
    reg_unlock();
}
static unsigned reg_count_in(uintptr_t lo, uintptr_t hi)
{
    unsigned n = 0;
    reg_lock();
    for (unsigned i = 0; i < REG_MAX; i++) n += g_reg[i] >= lo && g_reg[i] < hi;
    reg_unlock();
    return n;
}
static void reg_forget(uintptr_t lo, uintptr_t hi)
{
    reg_lock();
    for (unsigned i = 0; i < REG_MAX; i++) if (g_reg[i] >= lo && g_reg[i] < hi) g_reg[i] = 0;
    reg_unlock();
}

/* ---- The task ------------------------------------------------------------ */
static _Thread_local int t_task_state;      /* per-task state a task leaves dirty */
static _Thread_local unsigned t_tasks_here; /* tasks this thread has run */

typedef struct job {
    spu_task_item item;
    unsigned index;
    unsigned wait_for;        /* block until this many tasks are running at once */
    int deep;                 /* use 32 MB of stack */
    struct job* child;        /* start this task and wait for it */
} job;

static atomic_uint g_done, g_running, g_max_running, g_reused_threads;
static atomic_uint g_bad_state, g_bad_stack, g_bad_reg, g_bad_latch, g_bad_guarantee;
static atomic_uint g_arrived;
static uint32_t g_result[4096];

static uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16; return x; }

/* 32 frames of 1 MB: a fresh 256 MB reservation takes it, a default stack does not. */
static uint32_t deep_use(unsigned depth, uint32_t seed)
{
    volatile unsigned char frame[1u << 20];
    frame[0] = (unsigned char)seed; frame[sizeof frame - 1] = (unsigned char)(seed >> 8);
    uint32_t r = depth ? deep_use(depth - 1, mix(seed)) : seed;
    return r ^ frame[0] ^ frame[sizeof frame - 1];
}

static void run_job(spu_task_item* item)
{
    job* j = (job*)item;
    /* What spu_async_task does first: a fresh thread's per-task state. */
    t_task_state = 0;
    uintptr_t lo = 0, hi = 0;
    unsigned char local[256];
    const int known = spu_task_thread_stack(&lo, &hi);
    if (!known || (uintptr_t)local < lo || (uintptr_t)local >= hi ||
        hi - lo < SPU_TASK_STACK_RESERVE - (1u << 20))
        atomic_fetch_add(&g_bad_stack, 1);
    /* No registration from an earlier task on this thread survived. */
    if (known && reg_count_in(lo, hi)) atomic_fetch_add(&g_bad_reg, 1);
#ifdef _WIN32
    { ULONG g = 0; SetThreadStackGuarantee(&g);   /* 0 queries */
      if (g < SPU_TASK_STACK_GUARANTEE) atomic_fetch_add(&g_bad_guarantee, 1); }
#endif
    if (t_tasks_here++) atomic_fetch_add(&g_reused_threads, 1);
    const unsigned now = atomic_fetch_add(&g_running, 1) + 1;
    for (unsigned m = atomic_load(&g_max_running); now > m && !atomic_compare_exchange_weak(&g_max_running, &m, now);) {}

    /* The work: a result that depends only on the task, through its stack. */
    for (unsigned i = 0; i < sizeof local; i++) local[i] = (unsigned char)mix(j->index * 257u + i);
    uint32_t r = j->index;
    for (unsigned i = 0; i < sizeof local; i++) r = mix(r ^ local[i]);
    if (j->deep && deep_use(32, r) == 0x5EEDu && j->index == ~0u) r++;   /* never true */
    if (t_task_state) atomic_fetch_add(&g_bad_state, 1);
    t_task_state = (int)j->index + 1;   /* dirty, for the next task here */
    reg_add(local);                      /* "leaked": only the scrub drops it */

    if (j->wait_for) {   /* everyone must be running at once: no queueing */
        atomic_fetch_add(&g_arrived, 1);
        unsigned waited = 0;
        while (atomic_load(&g_arrived) < j->wait_for && waited < 10000) { sleep_ms(1); waited++; }
        if (waited >= 10000) atomic_fetch_add(&g_bad_latch, 1);
    }
    if (j->child) {      /* start a later task and wait for it */
        const unsigned before = atomic_load(&g_done);
        CHECK(spu_task_start(&j->child->item));
        unsigned waited = 0;
        while (atomic_load(&g_done) == before && waited < 10000) { sleep_ms(1); waited++; }
        if (waited >= 10000) atomic_fetch_add(&g_bad_latch, 1);
    }
    g_result[j->index] = r;
    atomic_fetch_sub(&g_running, 1);
    free(j);
    /* What spu_async_task does last. */
    if (known) reg_forget(lo, hi);
    atomic_fetch_add(&g_done, 1);
}

static job* new_job(unsigned index)
{
    job* j = (job*)calloc(1, sizeof *j);
    j->item.run = run_job;
    j->index = index;
    return j;
}
static int wait_done(unsigned n)
{
    for (unsigned waited = 0; waited < 20000; waited++) {
        if (atomic_load(&g_done) >= n) return 1;
        sleep_ms(1);
    }
    return 0;
}
/* Pool on: wait until `n` pool threads are idle, so the next start reuses. */
static void wait_idle(int pooled, unsigned n)
{
    for (unsigned waited = 0; pooled && waited < 20000; waited++) {
        spu_task_pool_lock();
        const unsigned idle = s_task_pool.idle;
        spu_task_pool_unlock();
        if (idle >= n) return;
        sleep_ms(1);
    }
}
static void pool_counts(unsigned* threads, unsigned* idle, unsigned long long* created,
                        unsigned long long* reused, unsigned long long* retired)
{
    spu_task_pool_lock();
    *threads = s_task_pool.threads; *idle = s_task_pool.idle;
    *created = s_task_pool.created; *reused = s_task_pool.reused; *retired = s_task_pool.retired;
    spu_task_pool_unlock();
}

int main(void)
{
    const int pooled = spu_task_pool_enabled();
    s_task_pool.idle_ms = 1500;   /* long enough to span the waves below */
    unsigned index = 0, threads, idle;
    unsigned long long created, reused, retired;

    /* 1. Back to back. */
    const unsigned SERIAL = 400;
    for (unsigned i = 0; i < SERIAL; i++) {
        job* j = new_job(index++);
        j->deep = i % 100 == 0;
        CHECK(spu_task_start(&j->item));
        CHECK(wait_done(index));
        wait_idle(pooled, 1);
    }
    pool_counts(&threads, &idle, &created, &reused, &retired);
    if (pooled) { CHECK(created == 1); CHECK(reused + created == SERIAL); }
    else        { CHECK(created == 0 && reused == 0 && threads == 0); }

    /* 2. Concurrent waves: all of a wave must be running at once. */
    const unsigned WAVE = 48;
    for (unsigned wave = 0; wave < 3; wave++) {
        atomic_store(&g_arrived, 0);
        const unsigned base = index;
        for (unsigned i = 0; i < WAVE; i++) {
            job* j = new_job(index++);
            j->wait_for = WAVE;
            j->deep = i == 7;
            CHECK(spu_task_start(&j->item));
        }
        CHECK(wait_done(base + WAVE));
        wait_idle(pooled, WAVE);
    }
    CHECK(atomic_load(&g_max_running) >= WAVE);
    pool_counts(&threads, &idle, &created, &reused, &retired);
    /* Later waves run on the first wave's threads (the serial one among them). */
    if (pooled) CHECK(created == WAVE && reused == SERIAL - 1 + 2 * WAVE + 1);

    /* 3. A task that starts another and waits for it. */
    for (unsigned i = 0; i < 16; i++) {
        job* parent = new_job(index++);
        parent->child = new_job(index++);
        CHECK(spu_task_start(&parent->item));
        CHECK(wait_done(index));
    }

    CHECK(atomic_load(&g_bad_state) == 0);
    CHECK(atomic_load(&g_bad_stack) == 0);
    CHECK(atomic_load(&g_bad_reg) == 0);
    CHECK(atomic_load(&g_bad_latch) == 0);
    CHECK(atomic_load(&g_bad_guarantee) == 0);
    if (pooled) CHECK(atomic_load(&g_reused_threads) > 0);
    else        CHECK(atomic_load(&g_reused_threads) == 0);

    /* 4. Idle threads leave on their own. */
    if (pooled) {
        unsigned waited = 0;
        do { sleep_ms(50); waited += 50; pool_counts(&threads, &idle, &created, &reused, &retired); }
        while (threads && waited < 10000);
        CHECK(threads == 0 && idle == 0 && retired == created);
    }

    /* 5. Shutdown: idle threads leave now; later tasks still run, unpooled. */
    {
        atomic_store(&g_arrived, 0);
        const unsigned base = index;
        for (unsigned i = 0; i < 8; i++) {
            job* j = new_job(index++);
            j->wait_for = 8;
            CHECK(spu_task_start(&j->item));
        }
        CHECK(wait_done(base + 8));
        unsigned long long created_before;
        pool_counts(&threads, &idle, &created_before, &reused, &retired);
        CHECK(spu_task_pool_shutdown(5000) == 0);
        pool_counts(&threads, &idle, &created, &reused, &retired);
        CHECK(threads == 0 && idle == 0);
        CHECK(spu_task_start(&new_job(index++)->item));
        CHECK(wait_done(index));
        pool_counts(&threads, &idle, &created, &reused, &retired);
        CHECK(created == created_before && threads == 0);
    }

    uint32_t sum = 0;
    for (unsigned i = 0; i < index; i++) sum = mix(sum ^ g_result[i]);
    pool_counts(&threads, &idle, &created, &reused, &retired);
    printf("pool=%d tasks=%u created=%llu reused=%llu retired=%llu max_running=%u\n",
           pooled, index, created, reused, retired, atomic_load(&g_max_running));
    printf("RESULT tasks=%u sum=%08X\n", index, sum);
    printf("spu task pool: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
