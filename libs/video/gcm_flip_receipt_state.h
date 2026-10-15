/* Internal publication state; public receipt users need no platform locks. */
#ifndef PS3RECOMP_GCM_FLIP_RECEIPT_STATE_H
#define PS3RECOMP_GCM_FLIP_RECEIPT_STATE_H
#include "gcm_flip_receipt.h"
#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK GcmFlipReceiptLock;
#define GCM_FLIP_RECEIPT_LOCK_INIT SRWLOCK_INIT
static inline void gcm_flip_receipt_lock(GcmFlipReceiptLock *lock) { AcquireSRWLockExclusive(lock); }
static inline void gcm_flip_receipt_unlock(GcmFlipReceiptLock *lock) { ReleaseSRWLockExclusive(lock); }
#else
#include <pthread.h>
typedef pthread_mutex_t GcmFlipReceiptLock;
#define GCM_FLIP_RECEIPT_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void gcm_flip_receipt_lock(GcmFlipReceiptLock *lock) { pthread_mutex_lock(lock); }
static inline void gcm_flip_receipt_unlock(GcmFlipReceiptLock *lock) { pthread_mutex_unlock(lock); }
#endif
typedef struct {
    GcmFlipReceiptLock lock;
    CellGcmFlipReceipt latest;
    int pending, exhausted;
} GcmFlipReceiptState;
#define GCM_FLIP_RECEIPT_STATE_INIT { GCM_FLIP_RECEIPT_LOCK_INIT, {0,0,0}, 0, 0 }
static inline void gcm_flip_receipt_publish(GcmFlipReceiptState *s, uint32_t buffer, int pending) {
    gcm_flip_receipt_lock(&s->lock);
    if(s->latest.guest_flip_id==UINT64_MAX) s->exhausted=1;
    s->latest.guest_flip_id=s->exhausted?0:s->latest.guest_flip_id+1;
    s->latest.buffer_id=buffer; ++s->latest.flip_count;
    if(pending) s->pending=1;
    gcm_flip_receipt_unlock(&s->lock);
}
static inline int gcm_flip_receipt_is_pending(GcmFlipReceiptState *s) {
    gcm_flip_receipt_lock(&s->lock);int pending=s->pending;
    gcm_flip_receipt_unlock(&s->lock);return pending;
}
static inline int gcm_flip_receipt_take(GcmFlipReceiptState *s, CellGcmFlipReceipt *out) {
    gcm_flip_receipt_lock(&s->lock);
    int pending=s->pending;
    if(pending) { if(out)*out=s->latest; s->pending=0; }
    gcm_flip_receipt_unlock(&s->lock);return pending;
}
static inline int gcm_flip_receipt_latest(GcmFlipReceiptState *s, CellGcmFlipReceipt *out) {
    gcm_flip_receipt_lock(&s->lock);
    int known=s->latest.guest_flip_id!=0;
    if(out)*out=s->latest;
    gcm_flip_receipt_unlock(&s->lock);return known;
}
#endif
