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
    /* About 60 beats a second, from nobody but the beat thread. Wall-clock
     * measurements on a loaded host: each attempt is printed, and one of up
     * to 3 must pass. */
    double beats = 0;
    int ok = 0;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        const u32 v2 = s_vblank_count;
        const uint64_t t0 = now_ns();
        sleep_ms(500);
        beats = (double)(s_vblank_count - v2) * 1e9 / (double)(now_ns() - t0);
        ok = beats > 50.0 && beats < 70.0;
        printf("  beat rate, attempt %d: %.1f/s %s\n", attempt, beats, ok ? "ok" : "outside 50-70");
    }
    assert(ok);
    /* An executed flip completes within a period: status WAITING, the drain
     * reached its marker, and the beat marks it pending without any host
     * tick. Judged on the median of 15 flips (one period, 16.7 ms, plus 3 ms)
     * and their 80th percentile (two periods plus 3 ms): a flip the beat
     * waited for is at most a period late; a thread the host descheduled is
     * an outlier the median ignores. */
    int median_us = 0, p80_us = 0;
    ok = 0;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        enum { N = 15 };
        int us[N];
        for (int k = 0; k < N; k++) {
            (void)GCM_PENDING_TAKE();
            FLIP_LOCK(); s_flip_status = CELL_GCM_FLIP_STATUS_WAITING; s_flip_req_seq++; FLIP_UNLOCK();
            const uint64_t t = now_ns();
            s_fifo_flips_reached++;
            while (!(GCM_PENDING_TAKE() & 2)) { sleep_ms(0); if (now_ns() - t > 500000000ull) assert(!"flip never completed"); }
            us[k] = (int)((now_ns() - t) / 1000u);
            assert(s_flip_tick_seq == s_flip_req_seq);
            sleep_ms(3 * k % 17);
        }
        for (int i = 1; i < N; i++) for (int j = i; j > 0 && us[j] < us[j - 1]; j--) { const int x = us[j]; us[j] = us[j - 1]; us[j - 1] = x; }
        median_us = us[N / 2]; p80_us = us[N * 4 / 5];
        ok = median_us < 16667 + 3000 && p80_us < 2 * 16667 + 3000;
        printf("  flip completion, attempt %d: median %d us, p80 %d us, max %d us %s\n",
               attempt, median_us, p80_us, us[N - 1], ok ? "ok" : "too late");
    }
    assert(ok);
    /* No flip outstanding: the beat completes nothing. */
    (void)GCM_PENDING_TAKE();
    FLIP_LOCK(); s_flip_status = CELL_GCM_FLIP_STATUS_DONE; FLIP_UNLOCK();
    sleep_ms(60);
    assert(!(GCM_PENDING_TAKE() & 2));
    printf("gcm vblank thread: PASS (%.1f beats/s, flip completion median %d us, p80 %d us)\n", beats, median_us, p80_us);
    return 0;
}
