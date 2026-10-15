/* Host-only identity for accepted guest FLIP boundaries. This does not complete
 * a guest flip, submit work, or change its buffer. Legacy hosts can ignore it. */
#ifndef PS3RECOMP_GCM_FLIP_RECEIPT_H
#define PS3RECOMP_GCM_FLIP_RECEIPT_H
#include <stdint.h>
typedef struct {
    uint64_t guest_flip_id; /* zero is unknown/exhausted, never a guest frame */
    uint32_t buffer_id, flip_count;
} CellGcmFlipReceipt;
#ifdef __cplusplus
extern "C" {
#endif
int cellGcm_take_flip_receipt(CellGcmFlipReceipt *out);
int cellGcm_latest_flip_receipt(CellGcmFlipReceipt *out);
#ifdef __cplusplus
}
#endif
#endif
