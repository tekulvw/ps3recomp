#ifndef PS3_CRITICAL_WRITE_TRACE_H
#define PS3_CRITICAL_WRITE_TRACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {uint64_t capture,owner,span,ea;uint32_t size,kind,cmd,pc,link,generation,coherent,offset,count;uint16_t before[8],source[8];} ps3_critical_write_token;
#if defined(PS3RECOMP_CRITICAL_WRITE_AVAILABLE) || defined(PS3_CRITICAL_WRITE_IMPLEMENTATION)
extern uint64_t ps3_critical_watch_range;
void ps3_critical_watch_bind(uint64_t capture,uint64_t owner,uint32_t container,uint32_t array,uint32_t marker_base);
void ps3_critical_write_begin(ps3_critical_write_token*,uint64_t,uint64_t,uint32_t,const void*,const void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
void ps3_critical_write_end(const ps3_critical_write_token*,const void*);
uint32_t ps3_critical_context_generation(const void*);
static inline int ps3_critical_write_watched(uint64_t ea,uint32_t size){
 uint64_t range=__atomic_load_n(&ps3_critical_watch_range,__ATOMIC_ACQUIRE);
 return range && ea<(range>>32) && ea+size>(uint32_t)range;
}
#else
/* Standalone DMA fixtures do not link the runtime diagnostics target. */
static inline int ps3_critical_write_watched(uint64_t ea,uint32_t size){(void)ea;(void)size;return 0;}
static inline uint32_t ps3_critical_context_generation(const void *ctx){(void)ctx;return 0;}
static inline void ps3_critical_write_begin(ps3_critical_write_token *t,uint64_t owner,uint64_t ea,uint32_t size,const void *dst,const void *src,uint32_t kind,uint32_t cmd,uint32_t pc,uint32_t link,uint32_t generation,uint32_t coherent){(void)owner;(void)ea;(void)size;(void)dst;(void)src;(void)kind;(void)cmd;(void)pc;(void)link;(void)generation;(void)coherent;t->capture=0;}
static inline void ps3_critical_write_end(const ps3_critical_write_token *t,const void *dst){(void)t;(void)dst;}
#endif
#ifdef __cplusplus
}
#endif
#endif
