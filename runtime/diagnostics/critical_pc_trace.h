/* Optional exact-PC arrival callback. Installed once before execution; no guest
 * mutation, allocation, clock or I/O on nonmatching instructions. */
#ifndef PS3_CRITICAL_PC_TRACE_H
#define PS3_CRITICAL_PC_TRACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(PS3RECOMP_CRITICAL_WRITE_AVAILABLE) || defined(PS3_CRITICAL_WRITE_IMPLEMENTATION)
extern uint32_t ps3_critical_arrival_pc;
extern void (*ps3_critical_arrival_callback)(void*,uint32_t,uint32_t);
static inline void ps3_critical_pc_arrive(void *ctx,uint32_t pc,uint32_t path){
 if(pc==__atomic_load_n(&ps3_critical_arrival_pc,__ATOMIC_RELAXED)){
  void (*fn)(void*,uint32_t,uint32_t)=ps3_critical_arrival_callback;
  if(fn)fn(ctx,pc,path);
 }
}
#else
static inline void ps3_critical_pc_arrive(void *ctx,uint32_t pc,uint32_t path){(void)ctx;(void)pc;(void)path;}
#endif
#ifdef __cplusplus
}
#endif
#endif
