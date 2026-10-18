#ifndef PS3RECOMP_RENDER_ABLATION_H
#define PS3RECOMP_RENDER_ABLATION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { RSX_RENDER_ABLATION_OFF=0, RSX_RENDER_ABLATION_NO_DRAW=1,
       RSX_RENDER_ABLATION_NO_PREPARE=2 };
/* Launch-static PS3_RENDER_ABLATE_DRAW / PS3_RENDER_ABLATE_PREPARE select
 * exclusive modes. Both active is invalid. PS3_RENDER_ABLATION_METRICS counts
 * the control arm too. Selection does not arm: all startup rendering is real. */
int rsx_render_ablation_init(void); /* 0 valid, -1 invalid configuration */
int rsx_render_ablation_arm(void);  /* active mode or -1; idempotent */
/* Called only at CPU END and final Metal issue respectively. No getenv/IO. */
int rsx_render_ablation_skip_prepare(void);
int rsx_render_ablation_skip_draw(void);
typedef struct rsx_render_ablation_snapshot {
    uint32_t bytes, configured_mode, active_mode, metrics_enabled;
    int32_t config_status;
    uint32_t reserved;
    uint64_t cpu_end_observed, cpu_end_skipped, gpu_draw_observed, gpu_draw_skipped;
} rsx_render_ablation_snapshot;
void rsx_render_ablation_get_snapshot(rsx_render_ablation_snapshot *output);
#ifdef __cplusplus
}
#endif
#endif
