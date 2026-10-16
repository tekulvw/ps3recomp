#ifndef PS3_CRITICAL_TRACE_H
#define PS3_CRITICAL_TRACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { PS3_CRITICAL_PPU=1, PS3_CRITICAL_REPORT=2, PS3_CRITICAL_FLUSH=3, PS3_CRITICAL_GUEST=4, PS3_CRITICAL_GPU=5, PS3_CRITICAL_PRESENT=6, PS3_CRITICAL_CLOCK=7, PS3_CRITICAL_PREDICATE=8 };
uint64_t ps3_critical_now_ns(void);
int ps3_critical_enabled(void);
uint64_t ps3_critical_capture_id(void);
void ps3_critical_emit_capture(uint64_t capture_id,uint32_t kind,uint64_t owner,uint64_t span,const uint64_t *words,uint32_t count);
void ps3_critical_emit(uint32_t kind,uint64_t owner,uint64_t span,const uint64_t *words,uint32_t count);
int ps3_critical_begin(uint64_t capture_id,uint64_t duration_ns);
int ps3_critical_finish(const char *private_output);
void ps3_critical_control_start(void);
#ifdef __cplusplus
}
#endif
#endif
