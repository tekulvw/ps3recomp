/* Deliberately incorrect, launch-selected rendering diagnostics. These gates
 * never own submission/completion and are not renderer capability profiles. */
#include "render_ablation.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pthread_once_t s_once=PTHREAD_ONCE_INIT;
static unsigned s_configured;
static int s_status;
static atomic_uint s_active, s_metrics;
static atomic_uint_fast64_t s_cpu_observed, s_cpu_skipped, s_gpu_observed, s_gpu_skipped;
_Static_assert(sizeof(rsx_render_ablation_snapshot)==56,"render ablation snapshot ABI");
static int flag(const char *name) {
    const char *value=getenv(name);
    if(!value || !*value || !strcmp(value,"0"))return 0;
    if(!strcmp(value,"1"))return 1;
    return -1;
}
static void configure(void) {
    const int draw=flag("PS3_RENDER_ABLATE_DRAW");
    const int prepare=flag("PS3_RENDER_ABLATE_PREPARE");
    const int metrics=flag("PS3_RENDER_ABLATION_METRICS");
    if(draw<0 || prepare<0 || metrics<0 || (draw && prepare)) {
        s_status=-1;
        fprintf(stderr,"[rsx ablation] INVALID launch configuration; flags require exclusive 0/1 modes; arming refused\n");
        return;
    }
    s_configured=prepare?RSX_RENDER_ABLATION_NO_PREPARE:draw?RSX_RENDER_ABLATION_NO_DRAW:RSX_RENDER_ABLATION_OFF;
    atomic_store_explicit(&s_metrics,metrics || s_configured!=0,memory_order_relaxed);
    if(s_configured || metrics)fprintf(stderr,"[rsx ablation] configured=%u active=0 metrics=1; explicit after-HUD arm required\n",s_configured);
}
int rsx_render_ablation_init(void) {
    pthread_once(&s_once,configure);
    return s_status;
}
int rsx_render_ablation_arm(void) {
    if(rsx_render_ablation_init()!=0)return -1;
    const unsigned previous=atomic_exchange_explicit(&s_active,s_configured,memory_order_relaxed);
    if(previous!=s_configured)fprintf(stderr,"[rsx ablation] ARMED mode=%u (diagnostic, rendering intentionally invalid)\n",s_configured);
    return (int)s_configured;
}
int rsx_render_ablation_skip_prepare(void) {
    const int skip=atomic_load_explicit(&s_active,memory_order_relaxed)==RSX_RENDER_ABLATION_NO_PREPARE;
    if(atomic_load_explicit(&s_metrics,memory_order_relaxed)) {
        atomic_fetch_add_explicit(&s_cpu_observed,1,memory_order_relaxed);
        if(skip)atomic_fetch_add_explicit(&s_cpu_skipped,1,memory_order_relaxed);
    }
    return skip;
}
int rsx_render_ablation_skip_draw(void) {
    /* B includes A for pre-arm records still awaiting real Metal encoding. */
    const int skip=atomic_load_explicit(&s_active,memory_order_relaxed)!=RSX_RENDER_ABLATION_OFF;
    if(atomic_load_explicit(&s_metrics,memory_order_relaxed)) {
        atomic_fetch_add_explicit(&s_gpu_observed,1,memory_order_relaxed);
        if(skip)atomic_fetch_add_explicit(&s_gpu_skipped,1,memory_order_relaxed);
    }
    return skip;
}
void rsx_render_ablation_get_snapshot(rsx_render_ablation_snapshot *output) {
    if(!output)return;
    rsx_render_ablation_init();
    *output=(rsx_render_ablation_snapshot){.bytes=sizeof(*output),.configured_mode=s_configured,
        .active_mode=atomic_load_explicit(&s_active,memory_order_relaxed),
        .metrics_enabled=atomic_load_explicit(&s_metrics,memory_order_relaxed),.config_status=s_status,
        .cpu_end_observed=atomic_load_explicit(&s_cpu_observed,memory_order_relaxed),
        .cpu_end_skipped=atomic_load_explicit(&s_cpu_skipped,memory_order_relaxed),
        .gpu_draw_observed=atomic_load_explicit(&s_gpu_observed,memory_order_relaxed),
        .gpu_draw_skipped=atomic_load_explicit(&s_gpu_skipped,memory_order_relaxed)};
}
