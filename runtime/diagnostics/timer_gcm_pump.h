#ifndef PS3_TIMER_GCM_PUMP_H
#define PS3_TIMER_GCM_PUMP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Local controlled PR353 screen. Default disabled; the title's explicit
 * diagnostic controller may enable it. Counts mean invocations, not callbacks. */
void ppu_timer_gcm_pump_set_enabled(int enabled);
typedef struct PpuTimerGcmPumpSnapshot {
 uint32_t bytes,enabled;
 uint64_t eligible_141,eligible_142,pump_invocations,missing_pump,disabled_eligible,setter_transitions;
} PpuTimerGcmPumpSnapshot;
void ppu_timer_gcm_pump_get_snapshot(PpuTimerGcmPumpSnapshot *output);
#ifdef __cplusplus
}
#endif
#endif
