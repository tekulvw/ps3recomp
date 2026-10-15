#include "../rsx_metal_backend.m"
#include <assert.h>
#include <unistd.h>
int main(void) {
 @autoreleasepool {
  setenv("PS3RECOMP_METAL_EMPTY_FLUSH","1",1);
  s_dev=MTLCreateSystemDefaultDevice();assert(s_dev);s_queue=[s_dev newCommandQueue];assert(s_queue);
  s_eng_active=1;s_headless=1;
  MTLTextureDescriptor* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
  desc.usage=MTLTextureUsageRenderTarget;desc.storageMode=MTLStorageModeShared;
  id<MTLTexture> texture=[s_dev newTextureWithDescriptor:desc];assert(texture);
  u32 handle=eng_obj_add(texture);const float rgba[4]={1,0,0,1};eng_clear_color(NULL,handle,rgba);
  eng_submit_and_wait(NULL,RSX_BE_FLUSH_GUEST_REFERENCE);
  assert(!atomic_load(&s_eng_checked_failed) && s_eng_checked_completed);
  unsigned char pixels[16];[texture getBytes:pixels bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
  for(unsigned i=0;i<16;i+=4)assert(pixels[i]==255 && pixels[i+1]==0 && pixels[i+2]==0 && pixels[i+3]==255);
  id<MTLSharedEvent> gate=[s_dev newSharedEvent];assert(gate);
  id<MTLCommandBuffer> pending=[s_queue commandBuffer];assert(pending);
  [pending encodeWaitForEvent:gate value:1];eng_track_submission(pending);[pending commit];
  dispatch_semaphore_t released=dispatch_semaphore_create(0);
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{usleep(100000);gate.signaledValue=1;dispatch_semaphore_signal(released);});
  double t0=CACurrentMediaTime();eng_submit_and_wait(NULL,RSX_BE_FLUSH_GUEST_REFERENCE);double elapsed=CACurrentMediaTime()-t0;
  assert(elapsed>=0.08);assert(pending.status==MTLCommandBufferStatusCompleted);
  assert(![s_eng_submitted count] && !atomic_load(&s_eng_checked_failed));
  dispatch_semaphore_wait(released,DISPATCH_TIME_FOREVER);
  s_eng_checked_completed=37;eng_submit_and_wait(NULL,RSX_BE_FLUSH_GUEST_REFERENCE);
  assert(s_eng_checked_completed==37); /* unchecked empty path creates no checked certificate */
  puts("PASS real GPU clear preserved; pending event blocks flush until completion; subsequent empty fast path preserves checked marker");
 }
}
