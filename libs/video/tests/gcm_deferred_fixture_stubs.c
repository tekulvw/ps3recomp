/* Optional backend seams for existing focused GCM fixtures. Strong fixture
 * definitions override these inert, default-off fallbacks. */
#include "../rsx_draw_engine.h"
#include "../rsx_commands.h"
#include "../../../runtime/diagnostics/critical_trace.h"
#include "../../../runtime/platform/win32_compat.h"
#include "../../../runtime/syscalls/sys_memory_native.h"
#include <stdlib.h>
#define WEAK __attribute__((weak))
/* Legacy fixtures leave the consumer experiment OFF. Any attempted wake is
 * a test failure; the consumer event fixture links the strong real service. */
#ifndef _WIN32
WEAK BOOL SetEvent(HANDLE event){(void)event;abort();}
/* The FIFO-lag record (PS3RECOMP_RSX_LABEL_LAG) starts a sampler thread; the
 * fixtures leave it off, so an attempt to start one is a test failure. */
WEAK HANDLE CreateThread(void* sa,SIZE_T stack,PS3_THREAD_FN fn,LPVOID arg,DWORD flags,DWORD* out_tid)
{(void)sa;(void)stack;(void)fn;(void)arg;(void)flags;(void)out_tid;abort();}
WEAK BOOL CloseHandle(HANDLE h){(void)h;abort();}
#endif
/* The SPU line-commit hook (runtime/spu/spu_channels.c) that
 * PS3RECOMP_RSX_REPORT_SPURS_KICK calls after a label publication: no SPU
 * side in these fixtures, so no hook. */
WEAK void (*g_host_line_write_hook)(uint32_t line)=0;
WEAK int rsx_draw_engine_async_available(void){return 0;}
WEAK u64 rsx_draw_engine_submit_async(void){return 0;}
WEAK int rsx_draw_engine_completion_status(u64 ticket,int wait){(void)ticket;(void)wait;return -1;}
WEAK void rsx_draw_engine_set_report_barrier(int(*barrier)(void)){(void)barrier;}
WEAK uint64_t rsx_live_draw_report_batch_lifetime(void){return 0;}
WEAK int rsx_live_draw_report_batch_admit(uint64_t lifetime){(void)lifetime;return 0;}
WEAK uint64_t rsx_live_draw_report_batch_issue(uint64_t lifetime){(void)lifetime;return 0;}
WEAK int rsx_live_draw_report_batch_status(uint64_t lifetime,uint64_t ticket,int wait){(void)lifetime;(void)ticket;(void)wait;return -1;}
WEAK void rsx_live_draw_set_report_barrier(int(*barrier)(void)){(void)barrier;}
WEAK int rsx_live_draw_set_report_lease(void(*begin)(void),void(*end)(void)){return (begin==NULL)==(end==NULL);}
WEAK int rsx_draw_engine_enabled(void){return 0;}
/* No vtable backend: with the draw engine on, the walker hands rsx_state
 * only the methods with effects outside it. */
WEAK rsx_backend* rsx_get_backend(void){return NULL;}
WEAK int rsx_live_draw_enabled(void){return 0;}
WEAK void rsx_draw_engine_flush(void){}
WEAK void rsx_live_draw_flush(void){}
WEAK void rsx_live_draw_method_at(u32 method,u32 arg,u32 data_ea){(void)method;(void)arg;(void)data_ea;}
WEAK void rsx_draw_engine_method_at(u32 method,u32 arg,u32 data_ea){(void)method;(void)arg;(void)data_ea;}
WEAK void rsx_draw_engine_mark_guest_dirty(u32 ea,u32 bytes){(void)ea;(void)bytes;}
WEAK int rsx_draw_engine_flush_checked(void){return 0;}
WEAK int rsx_draw_engine_checked_ready(void){return 0;}
WEAK uint64_t ps3_critical_capture_id(void){return 0;}
WEAK int ps3_critical_enabled(void){return 0;}
WEAK uint64_t ps3_critical_now_ns(void){return 0;}
WEAK void ps3_critical_emit_capture(uint64_t c,uint32_t k,uint64_t o,uint64_t s,const uint64_t*w,uint32_t n){(void)c;(void)k;(void)o;(void)s;(void)w;(void)n;}
WEAK uint32_t ppu_vm_size;
WEAK int32_t sys_memory_native_acquire(const sys_memory_native_allocation* a){(void)a;return -1;}
WEAK int32_t sys_memory_native_unpin(const sys_memory_native_allocation* a){(void)a;return -1;}
WEAK int32_t sys_memory_native_mapping_ref(const sys_memory_native_allocation* a,int delta){(void)a;(void)delta;return -1;}

WEAK int rsx_draw_engine_native_source_target(rsx_native_source_target* out){if(out)*out=(rsx_native_source_target){0};return 0;}
