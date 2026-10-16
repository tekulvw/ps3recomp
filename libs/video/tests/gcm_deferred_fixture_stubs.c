/* Optional backend seams for existing focused GCM fixtures. Strong fixture
 * definitions override these inert, default-off fallbacks. */
#include "../rsx_draw_engine.h"
#include "../../../runtime/diagnostics/critical_trace.h"
#define WEAK __attribute__((weak))
WEAK int rsx_draw_engine_async_available(void){return 0;}
WEAK u64 rsx_draw_engine_submit_async(void){return 0;}
WEAK int rsx_draw_engine_completion_status(u64 ticket,int wait){(void)ticket;(void)wait;return -1;}
WEAK void rsx_draw_engine_set_report_barrier(int(*barrier)(void)){(void)barrier;}
WEAK int rsx_draw_engine_enabled(void){return 0;}
WEAK int rsx_live_draw_enabled(void){return 0;}
WEAK void rsx_draw_engine_flush(void){}
WEAK void rsx_live_draw_flush(void){}
WEAK int rsx_draw_engine_flush_checked(void){return 0;}
WEAK uint64_t ps3_critical_capture_id(void){return 0;}
WEAK int ps3_critical_enabled(void){return 0;}
WEAK uint64_t ps3_critical_now_ns(void){return 0;}
WEAK void ps3_critical_emit_capture(uint64_t c,uint32_t k,uint64_t o,uint64_t s,const uint64_t*w,uint32_t n){(void)c;(void)k;(void)o;(void)s;(void)w;(void)n;}
WEAK uint32_t ppu_vm_size;
