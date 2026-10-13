/* Opt-in bounded source snapshots for validated outgoing DMA. */
#ifndef SPU_DMA_PUBLICATION_H
#define SPU_DMA_PUBLICATION_H
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
void spu_dma_publication_observe(const spu_context*, uint32_t lsa,
    uint64_t ea, uint32_t size, uint32_t cmd);
#ifdef __cplusplus
}
#endif
#endif
