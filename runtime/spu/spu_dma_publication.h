/* Opt-in bounded snapshots for validated DMA (incoming opt-in). */
#ifndef SPU_DMA_PUBLICATION_H
#define SPU_DMA_PUBLICATION_H
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
void spu_dma_publication_observe(const spu_context*, uint32_t lsa,
    uint64_t ea, uint32_t size, uint32_t cmd);
enum spu_dma_publication_atomic_outcome {
    SPU_DMA_ATOMIC_GETLLAR_COMPLETE=1,
    SPU_DMA_ATOMIC_PUTLLC_SUCCESS=2,
    SPU_DMA_ATOMIC_PUTLLC_FAILURE=3
};
void spu_dma_publication_observe_atomic(const spu_context*, uint32_t lsa,
    uint64_t ea, uint32_t cmd, enum spu_dma_publication_atomic_outcome);
#ifdef __cplusplus
}
#endif
#endif
