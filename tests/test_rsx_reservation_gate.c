/* The reservation gate (libs/video/rsx_native_reservation.c) with synthetic
 * spans and threads: no backend, no GPU. `off` checks the default-off path. */
#include "../libs/video/rsx_native_reservation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE thread_t;
static DWORD WINAPI thunk(LPVOID p);
static void sleep_ms(unsigned ms) { Sleep(ms); }
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t thread_t;
static void *thunk(void *p);
static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif
#include <stdatomic.h>

static int failures;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            failures++;                                                         \
        }                                                                       \
    } while (0)

typedef struct {
    int cause;
    rsx_reservation_span touched;
    int has_touched;
    atomic_int done;
    int waited;
} gate_job;
static void run_gate(gate_job *j)
{
    j->waited = rsx_reservation_gate(j->cause, j->has_touched ? &j->touched : NULL);
    atomic_store(&j->done, 1);
}
#ifdef _WIN32
static DWORD WINAPI thunk(LPVOID p) { run_gate(p); return 0; }
static thread_t start(gate_job *j) { return CreateThread(NULL, 0, thunk, j, 0, NULL); }
static void join(thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
static void *thunk(void *p) { run_gate(p); return NULL; }
static thread_t start(gate_job *j) { pthread_t t; pthread_create(&t, NULL, thunk, j); return t; }
static void join(thread_t t) { pthread_join(t, NULL); }
#endif

static rsx_reservation_span span(uint32_t space, uint64_t begin, uint64_t end)
{
    rsx_reservation_span s = {space, 0, begin, end};
    return s;
}

/* A gate that must block: it is still waiting after a pause, and returns 1
 * once the reservation resolves. */
static void blocks_until_resolved(int cause, const rsx_reservation_span *touched, uint64_t token)
{
    gate_job j;
    memset(&j, 0, sizeof j);
    j.cause = cause;
    if (touched) { j.touched = *touched; j.has_touched = 1; }
    thread_t t = start(&j);
    sleep_ms(50);
    CHECK(!atomic_load(&j.done));
    CHECK(rsx_reservation_resolve(token) == 0);
    join(t);
    CHECK(atomic_load(&j.done) && j.waited == 1);
}

static int off(void)
{
    uint64_t token = 1;
    rsx_reservation_span s = span(RSX_RESERVATION_LOCAL, 0, 64);
    CHECK(!rsx_reservation_enabled());
    CHECK(rsx_reservation_open(1, &s, 1, &token) == -2 && token == 0);
    CHECK(rsx_reservation_gate(RSX_RESERVATION_PRESENT, NULL) == 0);
    CHECK(rsx_reservation_gate(RSX_RESERVATION_PINNED_WRITE, &s) == 0);
    rsx_reservation_stats st;
    rsx_reservation_get_stats(&st);
    CHECK(st.opened == 0 && st.waits[RSX_RESERVATION_PRESENT] == 0);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "off")) return off();
    set_env("PS3RECOMP_RSX_RESERVATION_GATE", "1");
    CHECK(rsx_reservation_enabled());

    /* No reservation: nothing waits. */
    rsx_reservation_span a = span(RSX_RESERVATION_LOCAL, 0x1000, 0x2000);
    CHECK(rsx_reservation_gate(RSX_RESERVATION_PINNED_WRITE, &a) == 0);
    CHECK(rsx_reservation_gate(RSX_RESERVATION_COMPLETION, NULL) == 0);

    /* Pinned spans in three spaces, given unsorted and overlapping. */
    rsx_reservation_span pinned[] = {
        span(RSX_RESERVATION_MAIN, 0x30000, 0x30100),
        span(RSX_RESERVATION_LOCAL, 0x8000, 0x9000),
        span(RSX_RESERVATION_IMAGE, 7, 8),
        span(RSX_RESERVATION_LOCAL, 0x1000, 0x6000),
        span(RSX_RESERVATION_LOCAL, 0x2000, 0x3000),
    };
    uint64_t token = 0, other = 0;
    CHECK(rsx_reservation_open(1, pinned, 5, &token) == 0 && token != 0);
    /* One outstanding reservation. */
    CHECK(rsx_reservation_open(1, NULL, 0, &other) == -4 && other == 0);
    CHECK(rsx_reservation_resolve(token + 1) == -3);

    /* Writes outside every pinned span proceed: adjacent, other space, gap. */
    rsx_reservation_span free_spans[] = {
        span(RSX_RESERVATION_LOCAL, 0x6000, 0x8000), /* between [..6000) and [8000..) */
        span(RSX_RESERVATION_LOCAL, 0x9000, 0xa000), /* adjacent after */
        span(RSX_RESERVATION_LOCAL, 0x0, 0x1000),    /* adjacent before */
        span(RSX_RESERVATION_MAIN, 0x8000, 0x9000),  /* same range, other space */
        span(RSX_RESERVATION_IMAGE, 8, 9),
        span(RSX_RESERVATION_IMAGE, 6, 7),
    };
    for (unsigned i = 0; i < sizeof free_spans / sizeof *free_spans; i++) {
        CHECK(!rsx_reservation_affects(RSX_RESERVATION_PINNED_WRITE, &free_spans[i]));
        CHECK(rsx_reservation_gate(RSX_RESERVATION_PINNED_WRITE, &free_spans[i]) == 0);
    }
    /* Overlaps, including one only the earlier, longer span covers. */
    rsx_reservation_span hit[] = {
        span(RSX_RESERVATION_LOCAL, 0x5fff, 0x6000),
        span(RSX_RESERVATION_LOCAL, 0x4000, 0x4001),
        span(RSX_RESERVATION_LOCAL, 0x8fff, 0xa000),
        span(RSX_RESERVATION_LOCAL, 0x0, 0x10000),
        span(RSX_RESERVATION_MAIN, 0x300ff, 0x30200),
        span(RSX_RESERVATION_IMAGE, 7, 8),
    };
    for (unsigned i = 0; i < sizeof hit / sizeof *hit; i++)
        CHECK(rsx_reservation_affects(RSX_RESERVATION_PINNED_WRITE, &hit[i]));
    blocks_until_resolved(RSX_RESERVATION_PINNED_WRITE, &hit[1], token);
    /* Resolved: nothing waits; a second resolve is refused. */
    CHECK(!rsx_reservation_affects(RSX_RESERVATION_PINNED_WRITE, &hit[1]));
    CHECK(rsx_reservation_resolve(token) == -3);

    /* Every other cause waits while any reservation is outstanding, even
     * with no pinned spans (abandon resolves the same way). */
    const int causes[] = {RSX_RESERVATION_PRESENT, RSX_RESERVATION_READBACK,
                          RSX_RESERVATION_COMPLETION, RSX_RESERVATION_BOUNDARY,
                          RSX_RESERVATION_CLOSE};
    for (unsigned i = 0; i < sizeof causes / sizeof *causes; i++) {
        CHECK(rsx_reservation_open(2, NULL, 0, &token) == 0);
        CHECK(rsx_reservation_affects(causes[i], NULL));
        CHECK(!rsx_reservation_affects(RSX_RESERVATION_PINNED_WRITE, &a));
        blocks_until_resolved(causes[i], NULL, token);
    }

    /* Invalid spans and too many spans are refused, leaving nothing open. */
    rsx_reservation_span empty = span(RSX_RESERVATION_LOCAL, 5, 5);
    CHECK(rsx_reservation_open(3, &empty, 1, &token) == -6);
    rsx_reservation_span bad_space = span(9, 0, 1);
    CHECK(rsx_reservation_open(3, &bad_space, 1, &token) == -6);
    CHECK(rsx_reservation_open(3, pinned, RSX_RESERVATION_MAX_SPANS + 1, &token) == -6);
    CHECK(rsx_reservation_gate(RSX_RESERVATION_PRESENT, NULL) == 0);

    rsx_reservation_stats st;
    rsx_reservation_get_stats(&st);
    CHECK(st.opened == 6 && st.resolved == 6 && st.busy == 1 && st.refused == 3);
    CHECK(st.waits[RSX_RESERVATION_PINNED_WRITE] == 1);
    for (unsigned i = 0; i < sizeof causes / sizeof *causes; i++)
        CHECK(st.waits[causes[i]] == 1 && st.wait_ns[causes[i]] > 0);
    if (!failures) printf("rsx reservation gate: ok\n");
    return failures ? 1 : 0;
}
