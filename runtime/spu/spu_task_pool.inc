/* spu_task_pool.inc -- host threads for async SPU tasks.
 *
 * Included by spu_workload.c, which runs every async SPU task on a host thread
 * of its own, and by the fixture runtime/spu/tests/test_spu_task_pool.c. Kept
 * free of SPU types so the fixture can build it on its own on Windows and POSIX.
 *
 * spu_task_start(item) runs item->run(item) on a host thread with a 256 MB
 * stack reservation (and, on Windows, a 256 KB stack guarantee so an overflow
 * in lifted code still reaches the STACKOVERFLOW reporter).
 *
 * PS3RECOMP_SPU_TASK_POOL=1 (opt-in) reuses those threads instead of creating
 * and detaching one per task. A title can start a few hundred tasks a second,
 * and each new thread pays its creation, a cold stack and fresh thread-local
 * state (the SPU decode cache among it) again.
 *
 * Every task still starts at once on a thread of its own: a task is handed to
 * the pool only when a pool thread is idle and waiting for it (more idle
 * threads than queued tasks); otherwise a new pool thread is created for it,
 * exactly as with the pool off. So a task that waits for another task can never
 * be stuck behind it, and the number of threads is never below the number of
 * running tasks. An idle pool thread exits after two seconds without work.
 *
 * The run callback owns every per-task reset: a pooled thread runs the next
 * task with whatever thread-local state and stack contents the last one left.
 * spu_task_thread_stack() gives it the thread's stack range for that. */
#include "../diagnostics/original_work_threads.h"   /* ps3_original_pthread_create */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <errno.h>
#  include <pthread.h>
#  include <time.h>
#endif

typedef struct spu_task_item {
    struct spu_task_item* next;              /* pool queue link */
    void (*run)(struct spu_task_item* item); /* the task; may free the item */
} spu_task_item;

#define SPU_TASK_STACK_RESERVE   (256u << 20)
#define SPU_TASK_STACK_GUARANTEE (256u * 1024u)

static struct {
#ifdef _WIN32
    SRWLOCK mu;
    CONDITION_VARIABLE cv;    /* work queued, or closing */
    CONDITION_VARIABLE gone;  /* an idle thread left */
#else
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_cond_t gone;
#endif
    spu_task_item* head;
    spu_task_item* tail;
    unsigned queued, idle, threads;
    unsigned idle_ms;         /* idle time before a pool thread exits */
    int closing;              /* spu_task_pool_shutdown: no more reuse */
    unsigned long long reused, created, retired;
} s_task_pool = {
#ifdef _WIN32
    SRWLOCK_INIT, CONDITION_VARIABLE_INIT, CONDITION_VARIABLE_INIT,
#else
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER,
#endif
    NULL, NULL, 0, 0, 0, 2000u, 0, 0, 0, 0
};

#ifdef _WIN32
static void spu_task_pool_lock(void)   { AcquireSRWLockExclusive(&s_task_pool.mu); }
static void spu_task_pool_unlock(void) { ReleaseSRWLockExclusive(&s_task_pool.mu); }
static uint64_t spu_task_pool_now_ms(void) { return GetTickCount64(); }
/* Wait on `cv` (lock held) until woken or `deadline` (ms) passes. */
static void spu_task_pool_wait(CONDITION_VARIABLE* cv, uint64_t deadline)
{
    const uint64_t now = spu_task_pool_now_ms();
    if (now < deadline)
        SleepConditionVariableSRW(cv, &s_task_pool.mu, (DWORD)(deadline - now), 0);
}
#define spu_task_pool_signal(cv)    WakeConditionVariable(cv)
#define spu_task_pool_broadcast(cv) WakeAllConditionVariable(cv)
#else
static void spu_task_pool_lock(void)   { pthread_mutex_lock(&s_task_pool.mu); }
static void spu_task_pool_unlock(void) { pthread_mutex_unlock(&s_task_pool.mu); }
static uint64_t spu_task_pool_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);   /* the clock pthread_cond_timedwait uses */
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static void spu_task_pool_wait(pthread_cond_t* cv, uint64_t deadline)
{
    struct timespec until;
    until.tv_sec = (time_t)(deadline / 1000u);
    until.tv_nsec = (long)(deadline % 1000u) * 1000000L;
    pthread_cond_timedwait(cv, &s_task_pool.mu, &until);
}
#define spu_task_pool_signal(cv)    pthread_cond_signal(cv)
#define spu_task_pool_broadcast(cv) pthread_cond_broadcast(cv)
#endif

static int spu_task_pool_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_SPU_TASK_POOL");
        on = e && !strcmp(e, "1");
        if (on) { fprintf(stderr, "[spu_workload] async task thread pool on\n"); fflush(stderr); }
    }
    return on;
}

/* Lock held. */
static void spu_task_pool_report(void)
{
    const unsigned long long n = s_task_pool.reused + s_task_pool.created;
    if (n <= 2 || n % 2000 == 0)
        fprintf(stderr, "[spu_workload] task pool: tasks=%llu reused=%llu created=%llu retired=%llu threads=%u\n",
                n, s_task_pool.reused, s_task_pool.created, s_task_pool.retired, s_task_pool.threads);
}

/* The calling thread's stack, [lo, hi). 0 if the host cannot say. Read once
 * per thread: it does not move. */
#if defined(__linux__)
extern int pthread_getattr_np(pthread_t, pthread_attr_t*);
#endif
static int spu_task_thread_stack(uintptr_t* lo, uintptr_t* hi)
{
    static _Thread_local uintptr_t t_lo, t_hi;
    static _Thread_local int t_known;   /* 0 unread, 1 known, -1 unknown */
    if (!t_known) {
        t_known = -1;
#if defined(_WIN32)
        ULONG_PTR l = 0, h = 0;
        GetCurrentThreadStackLimits(&l, &h);
        if (h > l) { t_lo = (uintptr_t)l; t_hi = (uintptr_t)h; t_known = 1; }
#elif defined(__APPLE__)
        const uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
        const size_t size = pthread_get_stacksize_np(pthread_self());
        if (top && size) { t_lo = top - size; t_hi = top; t_known = 1; }
#elif defined(__linux__)
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* base = NULL; size_t size = 0;
            if (pthread_attr_getstack(&attr, &base, &size) == 0 && base && size) {
                t_lo = (uintptr_t)base; t_hi = (uintptr_t)base + size; t_known = 1;
            }
            pthread_attr_destroy(&attr);
        }
#endif
    }
    if (t_known < 0) return 0;
    *lo = t_lo; *hi = t_hi;
    return 1;
}

static void spu_task_thread_setup(void)
{
#ifdef _WIN32
    /* Reserve handler headroom so a stack overflow in lifted SPU code (e.g. a
     * runaway brsl recursion) can actually reach the STACKOVERFLOW reporter
     * instead of killing the process silently with 0x80000001. */
    ULONG g = SPU_TASK_STACK_GUARANTEE;
    SetThreadStackGuarantee(&g);
#endif
}

/* One task, then exit: the pool off. */
#ifdef _WIN32
static DWORD WINAPI spu_task_thread_once(LPVOID p)
#else
static void* spu_task_thread_once(void* p)
#endif
{
    spu_task_item* item = (spu_task_item*)p;
    spu_task_thread_setup();
    item->run(item);
    return 0;
}

/* A pool thread: its first task, then whatever is handed to it while idle. */
#ifdef _WIN32
static DWORD WINAPI spu_task_thread_pooled(LPVOID p)
#else
static void* spu_task_thread_pooled(void* p)
#endif
{
    spu_task_item* item = (spu_task_item*)p;
    spu_task_thread_setup();
    for (;;) {
        item->run(item);
        spu_task_pool_lock();
        s_task_pool.idle++;
        const uint64_t deadline = spu_task_pool_now_ms() + s_task_pool.idle_ms;
        while (!s_task_pool.head && !s_task_pool.closing && spu_task_pool_now_ms() < deadline)
            spu_task_pool_wait(&s_task_pool.cv, deadline);
        s_task_pool.idle--;
        if (!s_task_pool.head) {                 /* idle too long, or closing: retire */
            s_task_pool.threads--; s_task_pool.retired++;
            spu_task_pool_broadcast(&s_task_pool.gone);
            spu_task_pool_unlock();
            return 0;
        }
        item = s_task_pool.head;
        s_task_pool.head = item->next;
        if (!s_task_pool.head) s_task_pool.tail = NULL;
        s_task_pool.queued--;
        spu_task_pool_unlock();
    }
}

/* Pool on: queue `item` for an idle pool thread if one is waiting for it (1),
 * else count the pool thread about to be created for it (0). -1: the pool is
 * shutting down, so the task gets a thread of its own as with the pool off. */
static int spu_task_pool_hand_off(spu_task_item* item)
{
    int r;
    spu_task_pool_lock();
    if (s_task_pool.closing) {
        r = -1;
    } else if (s_task_pool.idle > s_task_pool.queued) {
        item->next = NULL;
        if (s_task_pool.tail) s_task_pool.tail->next = item; else s_task_pool.head = item;
        s_task_pool.tail = item;
        s_task_pool.queued++;
        s_task_pool.reused++;
        spu_task_pool_signal(&s_task_pool.cv);
        r = 1;
    } else {   /* counted before it runs, so a later hand-off cannot miss it */
        s_task_pool.threads++; s_task_pool.created++;
        r = 0;
    }
    if (r >= 0) spu_task_pool_report();
    spu_task_pool_unlock();
    return r;
}

/* Start `item` on a host thread now. 1: started. 0: no thread could be
 * created; item->run was not called and the caller still owns the item. */
static int spu_task_start(spu_task_item* item)
{
    int pooled = spu_task_pool_enabled();
    if (pooled) {
        const int r = spu_task_pool_hand_off(item);
        if (r > 0) return 1;
        pooled = r == 0;
    }
#ifdef _WIN32
    /* 256MB RESERVE (not commit): the Bink SPU decoder's per-macroblock brsl
     * chains are deep host call chains with fat lifted frames -- a 1MB stack
     * blew the guard page (STATUS_GUARD_PAGE_VIOLATION 0x80000001, silent
     * process death) the moment the real decode path first executed. */
    HANDLE th = CreateThread(NULL, SPU_TASK_STACK_RESERVE,
                             pooled ? spu_task_thread_pooled : spu_task_thread_once, item,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    const int ok = th != NULL;
    if (th) CloseHandle(th);   /* detached */
#else
    /* Same 256 MB reservation, for the same reason: pthread stacks are
     * mmap'd and committed on touch, so this reserves address space rather
     * than memory. The default is 512 KB on Darwin, half the 1 MB that blew
     * the guard page above. */
    void* (*thread_fn)(void*) = pooled ? spu_task_thread_pooled : spu_task_thread_once;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int prc = EINVAL;
    if (pthread_attr_setstacksize(&attr, SPU_TASK_STACK_RESERVE) == 0)
        prc = ps3_original_pthread_create(&th, &attr, thread_fn, item);
    pthread_attr_destroy(&attr);
    /* A host that will not reserve that much still gets the thread, on the
     * default stack: a shallow SPU job runs there, and a deep one dying
     * beats never dispatching at all. */
    if (prc != 0)
        prc = ps3_original_pthread_create(&th, NULL, thread_fn, item);
    const int ok = prc == 0;
    if (ok) pthread_detach(th);
#endif
    if (!ok && pooled) {
        spu_task_pool_lock();
        s_task_pool.threads--; s_task_pool.created--;
        spu_task_pool_unlock();
    }
    return ok;
}

/* Stop reusing threads: idle pool threads exit now, busy ones after their
 * task, and later tasks get a thread of their own as with the pool off.
 * Waits up to `wait_ms` for the idle ones to leave. Returns how many pool
 * threads are still alive (running a task, or not yet out). */
static unsigned spu_task_pool_shutdown(unsigned wait_ms)
{
    spu_task_pool_lock();
    s_task_pool.closing = 1;
    spu_task_pool_broadcast(&s_task_pool.cv);
    const uint64_t deadline = spu_task_pool_now_ms() + wait_ms;
    while (s_task_pool.idle && spu_task_pool_now_ms() < deadline)
        spu_task_pool_wait(&s_task_pool.gone, deadline);
    const unsigned left = s_task_pool.threads;
    spu_task_pool_unlock();
    return left;
}
