/* Bounded diagnostics, using monotonic seconds. Caller supplies synchronization.
 * Display events are actual MTLDrawable presentations; guest events are explicit
 * host submissions. Keeping them separate exposes repeated host-only redraws. */
#ifndef PS3RECOMP_METAL_FRAME_RATE_H
#define PS3RECOMP_METAL_FRAME_RATE_H
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define PS3RECOMP_METAL_FRAME_RATE 1
#define PS3RECOMP_METAL_FRAME_RATE_SIGNALS 1
#define METAL_FRAME_RATE_INTERVALS 256
typedef struct {
    double epoch, last_presented, last_gpu_completed, interval_sum, worst_ms;
    double intervals[METAL_FRAME_RATE_INTERVALS];
    unsigned long long displayed, guest, interval_count;
    unsigned long long gpu_completed, gpu_failed, present_callbacks;
    unsigned long long positive_presented_time, zero_presented_time;
    unsigned long long invalid_presented_time, duplicate_presented_time, stale_presented_time;
    unsigned long long drawable_attempts, nil_drawables, skipped_blits;
} MetalFrameRate;
typedef struct {
    double display_fps, guest_fps, average_ms, p95_ms, worst_ms, age_seconds;
    double gpu_fps, last_gpu_completed, gpu_age_seconds, last_presented;
    unsigned long long guest_admitted, gpu_completed, gpu_failed, present_callbacks;
    unsigned long long positive_presented_time, zero_presented_time;
    unsigned long long invalid_presented_time, duplicate_presented_time, stale_presented_time;
    unsigned long long drawable_attempts, nil_drawables, skipped_blits;
} MetalFrameRateSample;
static inline void metal_frame_rate_reset(MetalFrameRate *s, double now) {
    memset(s, 0, sizeof(*s)); s->epoch=now;
}
static inline void metal_frame_rate_guest(MetalFrameRate *s) { ++s->guest; }
/* Only frame command buffers count here; GPU-only flushes are excluded. This
 * is completion of GPU work, independent of visible compositor presentation. */
static inline void metal_frame_rate_completed(MetalFrameRate *s, int success, double now) {
    if (success) {
        ++s->gpu_completed;
        if (isfinite(now) && now>0 && now>s->last_gpu_completed) s->last_gpu_completed=now;
    } else ++s->gpu_failed;
}
static inline void metal_frame_rate_drawable(MetalFrameRate *s, int available) {
    ++s->drawable_attempts; if (!available) ++s->nil_drawables;
}
static inline void metal_frame_rate_presented(MetalFrameRate *s, double now) {
    ++s->present_callbacks;
    if (!isfinite(now) || now<0) { ++s->invalid_presented_time; return; }
    if (now==0) { ++s->zero_presented_time; return; }
    ++s->positive_presented_time;
    if (now==s->last_presented) { ++s->duplicate_presented_time; return; }
    if (now<s->last_presented) { ++s->stale_presented_time; return; }
    if (s->last_presented>0) {
        double ms=(now-s->last_presented)*1000;
        s->intervals[s->interval_count%METAL_FRAME_RATE_INTERVALS]=ms;
        s->interval_sum+=ms; ++s->interval_count;
        if (ms>s->worst_ms) s->worst_ms=ms;
    }
    s->last_presented=now; ++s->displayed;
}
static inline int metal_frame_rate_compare(const void *a,const void *b) {
    double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);
}
static inline MetalFrameRateSample metal_frame_rate_sample(MetalFrameRate *s, double now) {
    MetalFrameRateSample r={0}; double elapsed=now-s->epoch;
    if (elapsed<=0) return r;
    r.display_fps=s->displayed/elapsed; r.guest_fps=s->guest/elapsed;
    r.gpu_fps=s->gpu_completed/elapsed; r.gpu_failed=s->gpu_failed;
    r.guest_admitted=s->guest; r.gpu_completed=s->gpu_completed;
    r.last_gpu_completed=s->last_gpu_completed; r.last_presented=s->last_presented;
    r.gpu_age_seconds=s->last_gpu_completed>0?fmax(0,now-s->last_gpu_completed):0;
    r.present_callbacks=s->present_callbacks;
    r.positive_presented_time=s->positive_presented_time;
    r.zero_presented_time=s->zero_presented_time;
    r.invalid_presented_time=s->invalid_presented_time;
    r.duplicate_presented_time=s->duplicate_presented_time;
    r.stale_presented_time=s->stale_presented_time;
    r.drawable_attempts=s->drawable_attempts; r.nil_drawables=s->nil_drawables;
    r.skipped_blits=s->skipped_blits;
    if (s->interval_count) {
        r.average_ms=s->interval_sum/s->interval_count;
        r.worst_ms=s->worst_ms;
        unsigned n=s->interval_count<METAL_FRAME_RATE_INTERVALS?(unsigned)s->interval_count:METAL_FRAME_RATE_INTERVALS;
        double sorted[METAL_FRAME_RATE_INTERVALS];memcpy(sorted,s->intervals,n*sizeof(double));
        qsort(sorted,n,sizeof(double),metal_frame_rate_compare);
        r.p95_ms=sorted[(95*n+99)/100-1];
    }
    r.age_seconds=s->last_presented>0?fmax(0,now-s->last_presented):0;
    s->epoch=now; s->displayed=s->guest=s->interval_count=0;
    s->gpu_completed=s->gpu_failed=s->present_callbacks=0;
    s->positive_presented_time=s->zero_presented_time=0;
    s->invalid_presented_time=s->duplicate_presented_time=s->stale_presented_time=0;
    s->drawable_attempts=s->nil_drawables=s->skipped_blits=0;
    s->interval_sum=s->worst_ms=0;
    return r;
}
#endif
