/* PS3RECOMP_RSX_KICK_STATS: the walker's idle waits sorted by how they end --
 * a kick, a timeout, and a timeout during which `put` moved, put down to an
 * SPU DMA over the put word or to another writer. Real cellGcm wait and kick;
 * synthetic guest memory and threads. */
#define main checked_reference_fixture_main
#include "test_gcm_checked_reference.c"
#undef main
static atomic_int g_action;   /* 1: move put directly, 2: move put by an SPU DMA */
static void store_put(u32 v)
{
    u8* p = vm_base + GCM_CONTROL_GUEST_ADDR;
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static void* mover(void* unused)
{
    (void)unused;
    const struct timespec ms = {0, 1000000};
    nanosleep(&ms, NULL);                       /* inside the walker's 4 ms wait */
    const int a = atomic_load(&g_action);
    if (a == 2) gcm_note_spu_put(GCM_CONTROL_GUEST_ADDR, 4);
    store_put(0x100u * (u32)a);
    return NULL;
}
static void wait_with(int action)
{
    atomic_store(&g_action, action);
    pthread_t t; assert(!pthread_create(&t, NULL, mover, NULL));
    cellGcm_fifo_kick_wait(4);
    assert(!pthread_join(t, NULL));
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
    /* put moves without a kick: owed one, by another writer. */
    wait_with(1);
    assert(s_ks_timeouts == 2 && s_ks_moved == 1 && s_ks_moved_other == 1);
    /* An SPU DMA over the put word moves it. */
    wait_with(2);
    assert(s_ks_moved == 2 && s_ks_moved_spu == 1 && atomic_load(&s_ks_spu_put_writes) == 1);
    /* A DMA elsewhere is not a put write. */
    gcm_note_spu_put(GCM_CONTROL_GUEST_ADDR + 64, 16);
    assert(atomic_load(&s_ks_spu_put_writes) == 1);
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
