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
void spu_dma_disposition_begin(spu_dma_disposition_token*,uint32_t,uint64_t,uint32_t,uint32_t);
/* These hooks receive only pointers already validated by the real transfer. */
void spu_dma_disposition_before(spu_dma_disposition_token*,const uint8_t*,uint64_t,uint32_t);
void spu_dma_disposition_after(spu_dma_disposition_token*,const uint8_t*);
int spu_dma_disposition_finish(const spu_dma_disposition_token*,const spu_context*,uint32_t,uint64_t,uint32_t,uint32_t,int,unsigned);
#ifdef __cplusplus
}
#endif
#endif
