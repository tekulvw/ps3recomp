/* Hot syscalls read their diagnostic switches once, not on every call.
 *
 * Semaphore and condition-variable syscalls run many times per frame. Each
 * opt-in diagnostic they test (SEMTID, SEMCHAIN, PS3_COND_PEEK, ...) used to
 * call getenv() on every call; on Windows that was measurable CPU time per
 * frame. This fixture compiles one syscall unit with getenv() routed through a
 * counter, makes the same call many times, and requires the number of getenv
 * calls to stay bounded by the number of distinct switches, not grow with the
 * number of calls. Build with HOT_UNIT_SEMAPHORE or HOT_UNIT_COND. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long s_getenv_calls;
static char* hot_counting_getenv(const char* name)
{
    s_getenv_calls++;
    return getenv(name);
}
#define getenv hot_counting_getenv
#if defined(HOT_UNIT_SEMAPHORE)
#include "../sys_semaphore.c"
#elif defined(HOT_UNIT_COND)
#include "../sys_cond.c"
#else
#error "define HOT_UNIT_SEMAPHORE or HOT_UNIT_COND"
#endif
#undef getenv

/* Symbols the unit references that these calls never reach. */
uint8_t* vm_base;
void ppu_thread_note_block(uint64_t tid) { (void)tid; }
#if defined(HOT_UNIT_SEMAPHORE)
void lbp_breadcrumb_dump(const char* why) { (void)why; }
void ppu_log_host_chain(const char* why) { (void)why; }
void ps3_spu_job_complete_pending(void) {}
#else
sys_mutex_info g_sys_mutexes[SYS_MUTEX_MAX];
#endif

#define CALLS 10000ul
#define BOUND 16ul   /* more than the switches one call path reads */

static int check(const char* what, unsigned long before)
{
    const unsigned long used = s_getenv_calls - before;
    printf("%s: %lu getenv calls over %lu syscalls\n", what, used, CALLS);
    if (used > BOUND) {
        fprintf(stderr, "FAIL: %s calls getenv per call (%lu > %lu)\n", what, used, BOUND);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failed = 0;
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
#if defined(HOT_UNIT_SEMAPHORE)
    /* Handle 0 is never valid: every switch ahead of the id check still runs. */
    unsigned long before = s_getenv_calls;
    for (unsigned long i = 0; i < CALLS; i++) { ctx.gpr[3] = 0; ctx.gpr[4] = 1; sys_semaphore_post(&ctx); }
    failed |= check("sys_semaphore_post", before);
    before = s_getenv_calls;
    for (unsigned long i = 0; i < CALLS; i++) { ctx.gpr[3] = 0; ctx.gpr[4] = 0; sys_semaphore_wait(&ctx); }
    failed |= check("sys_semaphore_wait", before);
#else
    unsigned long before = s_getenv_calls;
    for (unsigned long i = 0; i < CALLS; i++) { ctx.gpr[3] = 0; ctx.gpr[4] = 0; sys_cond_wait(&ctx); }
    failed |= check("sys_cond_wait", before);
#endif
    if (!failed) puts("PASS");
    return failed;
}
