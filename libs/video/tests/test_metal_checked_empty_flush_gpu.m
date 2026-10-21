#include "../rsx_metal_backend.m"
float cellAvconfExt_display_gamma(void){return 1.0f;}   /* identity present gamma */
#include <assert.h>
#include <unistd.h>
int cellGcm_native_consumer_enabled(void){return 0;}
void cellGcm_fifo_kick_notify(void){assert(!"inactive consumer kick");}
void rsx_ring_progress_metal_committed(uint64_t ticket,uint64_t records){(void)ticket;(void)records;}
static unsigned observed_completions;
void rsx_ring_progress_metal_completed(uint64_t ticket,uint64_t status){assert(ticket==1 && status==MTLCommandBufferStatusCompleted);++observed_completions;}
int main(void) {
 @autoreleasepool {
  setenv("PS3RECOMP_METAL_EMPTY_FLUSH","0",1);
  setenv("PS3RECOMP_METAL_CHECKED_EMPTY_FLUSH","1",1);
  s_dev=MTLCreateSystemDefaultDevice();assert(s_dev);s_queue=[s_dev newCommandQueue];assert(s_queue);
  s_eng_active=1;s_headless=1;
  MTLTextureDescriptor* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
  desc.usage=MTLTextureUsageRenderTarget;desc.storageMode=MTLStorageModeShared;
  id<MTLTexture> texture=[s_dev newTextureWithDescriptor:desc];assert(texture);
  u32 handle=eng_obj_add(texture);const float rgba[4]={1,0,0,1};eng_clear_color(NULL,handle,rgba);
  assert(eng_submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);
  assert(!atomic_load(&s_eng_checked_failed) && s_eng_checked_completed);
  unsigned char pixels[16];[texture getBytes:pixels bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
  for(unsigned i=0;i<16;i+=4)assert(pixels[i]==255 && pixels[i+1]==0 && pixels[i+2]==0 && pixels[i+3]==255);
  id<MTLSharedEvent> gate=[s_dev newSharedEvent];assert(gate);
  id<MTLCommandBuffer> pending=[s_queue commandBuffer];assert(pending);
  [pending encodeWaitForEvent:gate value:1];eng_track_submission(pending);s_eng_fence_cbs=[NSMutableArray arrayWithObject:pending];s_eng_fence_next=1;s_eng_fence_completed=0;[pending commit];
  dispatch_semaphore_t released=dispatch_semaphore_create(0);
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{usleep(100000);gate.signaledValue=1;dispatch_semaphore_signal(released);});
  double t0=CACurrentMediaTime();assert(eng_submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);double elapsed=CACurrentMediaTime()-t0;
  assert(elapsed>=0.08);assert(pending.status==MTLCommandBufferStatusCompleted);
  assert(![s_eng_submitted count] && !atomic_load(&s_eng_checked_failed));
  dispatch_semaphore_wait(released,DISPATCH_TIME_FOREVER);
  assert(s_eng_fence_next==1 && s_eng_fence_completed==0 && [s_eng_fence_cbs count]==1);
  assert(!observed_completions);assert(eng_completion_status(NULL,1,0)==1 && s_eng_fence_completed==1 && observed_completions==1);
  s_eng_checked_completed=37;assert(eng_submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);
  assert(s_eng_checked_completed==37); /* checked empty proof creates no new GPU ticket or stale marker rewrite */
  puts("PASS real GPU clear preserved; pending event blocks flush until completion; subsequent checked empty fast path preserves real async receipt state");
 }
}
