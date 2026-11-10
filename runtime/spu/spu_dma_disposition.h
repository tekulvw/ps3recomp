/* Optional bounded receipts for ordinary/list-element DMA copy outcomes.
 * Reservation atomic commands use the separate publication atomic observer. */
#ifndef SPU_DMA_DISPOSITION_H
#define SPU_DMA_DISPOSITION_H
#include "spu_context.h"
#include "../diagnostics/audio_chronology.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SPU_DMA_DISPOSITION_NONE 0
#define SPU_DMA_DISPOSITION_GET 1
#define SPU_DMA_DISPOSITION_PUT 2
#define SPU_DMA_DISPOSITION_ZERO_EA 3
#define SPU_DMA_DISPOSITION_ZERO_LS 4
#define SPU_DMA_DISPOSITION_REJECTED 5
#define SPU_DMA_DISPOSITION_MALFORMED 6
typedef struct spu_dma_disposition_token {
    audio_chronology_dma_token chronology;
    uint32_t seq, request_lsa, request_size, request_cmd;
    uint64_t request_ea;
    uint32_t offset, count, before[16], after[16];
} spu_dma_disposition_token;
/* Nonzero once the run's switches have been read and neither these receipts
 * (SPU_DMA_DISPOSITION) nor the audio chronology (AUDIO_CHRONOLOGY) is on.
 * Both are fixed for the run, so the flag only ever goes 0 -> 1, and from
 * then on every hook below is one plain load of a word nothing writes again,
 * with no call: these hooks run on every SPU DMA. A stale 0 only takes the
 * out-of-line path, which reaches the same answer. */
extern volatile int g_spu_dma_hooks_off;
void spu_dma_disposition_begin_slow(spu_dma_disposition_token*,uint32_t,uint64_t,uint32_t,uint32_t);
void spu_dma_disposition_before_slow(spu_dma_disposition_token*,const uint8_t*,uint64_t,uint32_t);
void spu_dma_disposition_after_slow(spu_dma_disposition_token*,const uint8_t*);
int spu_dma_disposition_finish_slow(const spu_dma_disposition_token*,const spu_context*,uint32_t,uint64_t,uint32_t,uint32_t,int,unsigned);
static inline void spu_dma_disposition_begin(spu_dma_disposition_token* t,uint32_t lsa,uint64_t ea,uint32_t size,uint32_t cmd) {
    if(g_spu_dma_hooks_off) { t->chronology.port=-1; t->seq=0; return; }
    spu_dma_disposition_begin_slow(t,lsa,ea,size,cmd);
}
/* These hooks receive only pointers already validated by the real transfer.
 * A token neither observer admitted (seq 0, no chronology port) needs none. */
static inline void spu_dma_disposition_before(spu_dma_disposition_token* t,const uint8_t* src,uint64_t ea,uint32_t size) {
    if(t->seq)spu_dma_disposition_before_slow(t,src,ea,size);
}
static inline void spu_dma_disposition_after(spu_dma_disposition_token* t,const uint8_t* dst) {
    if(t->seq || t->chronology.port>=0)spu_dma_disposition_after_slow(t,dst);
}
static inline int spu_dma_disposition_finish(const spu_dma_disposition_token* t,const spu_context* s,uint32_t lsa,uint64_t ea,uint32_t size,uint32_t cmd,int rc,unsigned outcome) {
    if(!t->seq && t->chronology.port<0)return rc;
    return spu_dma_disposition_finish_slow(t,s,lsa,ea,size,cmd,rc,outcome);
}
#ifdef __cplusplus
}
#endif
#endif
