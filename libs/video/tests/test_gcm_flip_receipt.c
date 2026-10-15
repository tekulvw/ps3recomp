/* Lost boolean/count pairing or an ID minted by present instead of guest flip
 * would fail coherence, coalescing, and unchanged latest-receipt assertions. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#if !__has_include("gcm_flip_receipt_state.h")
int main(void) { assert(!"gcm_flip_receipt.h feature is missing"); }
#else
#include "gcm_flip_receipt_state.h"
static GcmFlipReceiptState state=GCM_FLIP_RECEIPT_STATE_INIT;
static void *publish(void *p) {
 unsigned buffer=(unsigned)(uintptr_t)p;
 for(unsigned i=0;i<1000;i++)gcm_flip_receipt_publish(&state,buffer,1);
 return 0;
}
int main(void) {
 CellGcmFlipReceipt r={0};assert(!gcm_flip_receipt_take(&state,&r));
 gcm_flip_receipt_publish(&state,2,1);assert(gcm_flip_receipt_take(&state,&r));
 assert(r.guest_flip_id==1 && r.buffer_id==2 && r.flip_count==1);
 assert(!gcm_flip_receipt_take(&state,&r));
 assert(gcm_flip_receipt_latest(&state,&r) && r.guest_flip_id==1);
 gcm_flip_receipt_publish(&state,3,1);gcm_flip_receipt_publish(&state,4,1);
 assert(gcm_flip_receipt_take(&state,&r) && r.guest_flip_id==3 && r.buffer_id==4);
 gcm_flip_receipt_publish(&state,5,0);assert(!gcm_flip_receipt_take(&state,&r));
 assert(gcm_flip_receipt_latest(&state,&r) && r.guest_flip_id==4 && r.buffer_id==5);
 pthread_t a,b;assert(!pthread_create(&a,0,publish,(void*)(uintptr_t)6));
 assert(!pthread_create(&b,0,publish,(void*)(uintptr_t)7));
 for(unsigned i=0;i<10000;i++) {
  assert(gcm_flip_receipt_latest(&state,&r));
  assert(r.guest_flip_id==r.flip_count);
  assert(r.buffer_id==5 || r.buffer_id==6 || r.buffer_id==7);
 }
 pthread_join(a,0);pthread_join(b,0);assert(gcm_flip_receipt_take(&state,&r));
 assert(r.guest_flip_id==2004 && r.flip_count==2004 && (r.buffer_id==6 || r.buffer_id==7));
 /* Exhausted IDs stay unknown rather than recycling an old guest identity. */
 gcm_flip_receipt_lock(&state.lock);state.latest.guest_flip_id=UINT64_MAX;
 state.latest.flip_count=UINT32_MAX;gcm_flip_receipt_unlock(&state.lock);
 gcm_flip_receipt_publish(&state,1,1);assert(gcm_flip_receipt_take(&state,&r));
 assert(r.guest_flip_id==0 && r.flip_count==0);
 gcm_flip_receipt_publish(&state,2,1);assert(gcm_flip_receipt_take(&state,&r));
 assert(r.guest_flip_id==0 && r.flip_count==1 && !gcm_flip_receipt_latest(&state,&r));
 puts("Guest flip receipt coherence and concurrent publication passed");
}

#endif
