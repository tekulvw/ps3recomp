/* PS3RECOMP_GCM_VBLANK_THREAD: the 60 Hz beat on a thread of its own. With it
 * running, the host's cellGcmTickVBlank/cellGcmTickFlip do nothing; the beat
 * advances the vblank count about 60 times a second on its own, and completes
 * an executed flip within one period of its execution, whatever the host
 * loop is doing. Real cellGcm beat and flip state; synthetic clock and
 * threads. */
#define main checked_reference_fixture_main
#include "test_gcm_checked_reference.c"
#undef main
static uint64_t now_ns(void) { return gcm_vbt_now_ns(); }
static void sleep_ms(unsigned ms) { const struct timespec t = { ms / 1000u, (long)(ms % 1000u) * 1000000L }; nanosleep(&t, NULL); }
int main(void)
{
    /* Off: the host's ticks are the beat. */
    const u32 v0 = s_vblank_count;
    cellGcmTickVBlank();
    assert(s_vblank_count == v0 + 1 && !s_vbt_running);
    (void)GCM_PENDING_TAKE();

    setenv("PS3RECOMP_GCM_VBLANK_THREAD", "1", 1);
    gcm_vbt_start();
    assert(s_vbt_running);
    gcm_vbt_start();                     /* once */
    /* The host's ticks are ignored now. */
    const u32 v1 = s_vblank_count;
    for (int i = 0; i < 1000; i++) cellGcmTickVBlank();
    assert(s_vblank_count - v1 < 5);
    /* About 60 beats a second, from nobody but the beat thread. */
    const u32 v2 = s_vblank_count;
    const uint64_t t0 = now_ns();
    sleep_ms(500);
    const double beats = (double)(s_vblank_count - v2) * 1e9 / (double)(now_ns() - t0);
    assert(beats > 50.0 && beats < 70.0);
    /* An executed flip completes within a period: status WAITING, the drain
     * reached its marker, and the beat marks it pending without any host tick. */
    int worst_us = 0;
    for (int k = 0; k < 10; k++) {
        (void)GCM_PENDING_TAKE();
        FLIP_LOCK(); s_flip_status = CELL_GCM_FLIP_STATUS_WAITING; s_flip_req_seq++; FLIP_UNLOCK();
        const uint64_t t = now_ns();
        s_fifo_flips_reached++;
        while (!(GCM_PENDING_TAKE() & 2)) { sleep_ms(0); if (now_ns() - t > 200000000ull) assert(!"flip never completed"); }
        const int us = (int)((now_ns() - t) / 1000u);
        if (us > worst_us) worst_us = us;
        assert(s_flip_tick_seq == s_flip_req_seq);
        sleep_ms(3 * k % 17);
    }
    assert(worst_us < 16667 + 8000);      /* one period, plus scheduling slack */
    /* No flip outstanding: the beat completes nothing. */
    (void)GCM_PENDING_TAKE();
    FLIP_LOCK(); s_flip_status = CELL_GCM_FLIP_STATUS_DONE; FLIP_UNLOCK();
    sleep_ms(60);
    assert(!(GCM_PENDING_TAKE() & 2));
    printf("gcm vblank thread: PASS (%.1f beats/s, worst flip completion %d us)\n", beats, worst_us);
    return 0;
}
