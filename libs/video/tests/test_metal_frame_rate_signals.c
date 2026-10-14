#include <assert.h>
#include <stdio.h>
#include "metal_frame_rate.h"

int main(void)
{
#ifndef PS3RECOMP_METAL_FRAME_RATE_SIGNALS
    assert(!"Frame completion and callback signal diagnostics are missing");
#else
    MetalFrameRate stats;
    metal_frame_rate_reset(&stats, 10.0);
    metal_frame_rate_guest(&stats);
    metal_frame_rate_completed(&stats, 1, 10.5);
    metal_frame_rate_completed(&stats, 0, 10.6);
    metal_frame_rate_presented(&stats, 0.0);
    metal_frame_rate_presented(&stats, NAN);
    MetalFrameRateSample sample = metal_frame_rate_sample(&stats, 11.0);
    assert(sample.gpu_fps == 1.0 && sample.gpu_failed == 1);
    assert(sample.guest_fps == 1.0 && sample.display_fps == 0.0);
    assert(sample.present_callbacks == 2 && sample.invalid_presented_time == 1 && sample.zero_presented_time == 1);
    assert(sample.stale_presented_time == 0);
    assert(sample.age_seconds == 0.0);
    assert(sample.gpu_completed == 1 && sample.guest_admitted == 1);
    assert(sample.last_gpu_completed == 10.5 && sample.gpu_age_seconds == 0.5);
    metal_frame_rate_drawable(&stats, 1);
    metal_frame_rate_drawable(&stats, 0);
    ++stats.skipped_blits;
    metal_frame_rate_presented(&stats, 11.1);
    metal_frame_rate_presented(&stats, 11.1);
    metal_frame_rate_presented(&stats, 10.9);
    sample = metal_frame_rate_sample(&stats, 12.0);
    assert(sample.display_fps == 1.0 && sample.gpu_fps == 0.0);
    assert(sample.drawable_attempts == 2 && sample.nil_drawables == 1 && sample.skipped_blits == 1);
    assert(sample.last_gpu_completed == 10.5 && sample.last_presented == 11.1);
    assert(sample.present_callbacks == 3 && sample.invalid_presented_time == 0);
    assert(sample.stale_presented_time == 1 && sample.duplicate_presented_time == 1 && sample.positive_presented_time == 3 && sample.gpu_failed == 0);
    assert(fabs(sample.age_seconds - 0.9) < 0.000001);
    sample = metal_frame_rate_sample(&stats, 14.0);
    assert(sample.display_fps == 0.0 && sample.gpu_fps == 0.0);
    assert(sample.drawable_attempts == 0 && sample.nil_drawables == 0 && sample.skipped_blits == 0);
    assert(sample.present_callbacks == 0 && sample.stale_presented_time == 0);
    assert(fabs(sample.age_seconds - 2.9) < 0.000001);
    metal_frame_rate_reset(&stats, 15.0);
    sample = metal_frame_rate_sample(&stats, 16.0);
    assert(sample.gpu_fps == 0.0 && sample.gpu_failed == 0);
    assert(sample.present_callbacks == 0 && sample.invalid_presented_time == 0);
#endif
    puts("Metal frame-rate signal scenarios passed");
    return 0;
}
