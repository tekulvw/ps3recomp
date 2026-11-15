/* PS3RECOMP_RSX_KICK_STATS: the walker's idle waits sorted by how they end --
 * a kick, a timeout, and a timeout during which `put` moved, put down to an
 * SPU DMA over the put word or to another writer. Real cellGcm wait and kick;
 * synthetic guest memory and threads. */
#define main checked_reference_fixture_main
#include "test_gcm_checked_reference.c"
#undef main
static atomic_int g_action;   /* 1: move put directly, 2: move put by an SPU DMA */
static atomic_int g_landed;   /* the mover has moved put */
static u32 g_put;
static void store_put(u32 v)
{
    u8* p = vm_base + GCM_CONTROL_GUEST_ADDR;
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static void* mover(void* unused)
{
    (void)unused;
    const struct timespec ms = {0, 1000000};
    nanosleep(&ms, NULL);                       /* inside the walker's wait */
    const int a = atomic_load(&g_action);
    if (a == 2) gcm_note_spu_put(GCM_CONTROL_GUEST_ADDR, 4);
    store_put(g_put += 0x100u);
    atomic_store(&g_landed, 1);
    return NULL;
}
/* One wait with put moved by `action` from another thread. Returns 1 when the
 * move landed inside the wait (the wait's classification is then checked by
 * the caller), 0 when a loaded host ran the mover only after the wait had
 * timed out (inconclusive: the caller retries). */
static int wait_with(int action, const char* what, int attempt)
{
    atomic_store(&g_action, action);
    atomic_store(&g_landed, 0);
    const unsigned long long moved = s_ks_moved;
    pthread_t t; assert(!pthread_create(&t, NULL, mover, NULL));
    cellGcm_fifo_kick_wait(20);                 /* room for a slow mover thread */
    const int landed = atomic_load(&g_landed);
    assert(!pthread_join(t, NULL));
    /* A move inside the wait is counted. One the mover made only after the
     * wait read put again (a descheduled mover) cannot be. */
    const int counted = s_ks_moved == moved + 1;
    printf("  %s, attempt %d: mover %s the wait, %s\n", what, attempt,
           landed ? "landed within" : "ran after", counted ? "counted as moved" : "not counted");
    return counted;
}
int main(void)
{
    setenv("MAG_NATIVE_CONSUMER", "1", 1);
    setenv("PS3RECOMP_RSX_KICK_STATS", "1", 1);
    setenv("PS3RECOMP_GCM_CHECKED_REF", "0", 1);
    cellGcm_native_consumer_init();
    assert(cellGcm_native_consumer_enabled());
    vm_base = mmap(NULL, UINT64_C(0x100000000), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);
    s_deferred_reports = 0;
    store_put(0);
    /* A kick ends the wait. */
    cellGcm_fifo_kick_notify();
    cellGcm_fifo_kick_wait(4);
    assert(s_ks_waits == 1 && s_ks_kicked == 1 && s_ks_timeouts == 0);
    /* Nothing happens: a plain timeout. */
    cellGcm_fifo_kick_wait(4);
    assert(s_ks_timeouts == 1 && s_ks_moved == 0 && s_ks_timeout_us >= 3000);
    g_put = 0;
    /* put moves without a kick: owed one, by another writer. A loaded host
     * may run the mover only after the 4 ms wait has timed out; such an
     * attempt proves nothing either way, so it is retried (up to 3). Every
     * attempt that counts a move must classify it right. */
    int ok = 0;
    for (int k = 1; k <= 3 && !ok; k++) {
        const unsigned long long other = s_ks_moved_other, spu = s_ks_moved_spu;
        ok = wait_with(1, "put moved by another writer", k);
        if (ok) assert(s_ks_moved_other == other + 1 && s_ks_moved_spu == spu);
    }
    assert(ok);
    /* An SPU DMA over the put word moves it. */
    ok = 0;
    unsigned long long spu_writes = atomic_load(&s_ks_spu_put_writes);
    for (int k = 1; k <= 3 && !ok; k++) {
        const unsigned long long other = s_ks_moved_other, spu = s_ks_moved_spu;
        ok = wait_with(2, "put moved by an SPU DMA", k);
        assert(atomic_load(&s_ks_spu_put_writes) == ++spu_writes);   /* every DMA over put */
        if (ok) assert(s_ks_moved_spu == spu + 1 && s_ks_moved_other == other);
    }
    assert(ok);
    /* A DMA elsewhere is not a put write. */
    gcm_note_spu_put(GCM_CONTROL_GUEST_ADDR + 64, 16);
    assert(atomic_load(&s_ks_spu_put_writes) == spu_writes);
    /* A kick counts as a notify. */
    const unsigned long long n = atomic_load(&s_ks_notifies);
    cellGcm_fifo_kick_notify();
    assert(atomic_load(&s_ks_notifies) == n + 1);
    cellGcm_fifo_kick_wait(4);
    assert(s_ks_kicked == 2);
    assert(!munmap(vm_base, UINT64_C(0x100000000)));
    printf("gcm kick stats: PASS (waits=%llu kicked=%llu timeouts=%llu moved=%llu spu=%llu other=%llu)\n",
           s_ks_waits, s_ks_kicked, s_ks_timeouts, s_ks_moved, s_ks_moved_spu, s_ks_moved_other);
    return 0;
}
