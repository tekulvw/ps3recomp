#define main checked_fixture_main
#include "test_metal_checked_completion.m"
#undef main

/* Protocol stand-ins exercise asynchronous lifetime/status handling without
 * opening a window, submitting GPU work, or changing native app state. */
@interface SignalsBuffer : CheckedBuffer
@property(copy) MTLCommandBufferHandler completion;
@end
@implementation SignalsBuffer
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler { self.completion=handler; }
@end
@interface SignalsDrawable : NSObject
@property double presentedTime;
@property(copy) MTLDrawablePresentedHandler presentation;
@end
@implementation SignalsDrawable
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)handler { self.presentation=handler; }
@end

int main(void) {
 @autoreleasepool {
  setenv("PS3RECOMP_METAL_FPS_DIAGNOSTICS","1",1);
  MetalFrameRateSession *session=[MetalFrameRateSession new];
  s_fps_session=session;
  SignalsBuffer *success=[SignalsBuffer new];
  fps_track_frame_completion((id<MTLCommandBuffer>)success);
  assert(success.completion && session->stats.gpu_completed==0);
  success.completion((id<MTLCommandBuffer>)success);
  assert(session->stats.gpu_completed==1 && session->stats.last_gpu_completed>0);
  assert(session->stats.displayed==0); /* GPU completion is not display. */
  SignalsBuffer *failed=[SignalsBuffer new];failed.failed=1;
  fps_track_frame_completion((id<MTLCommandBuffer>)failed);
  failed.completion((id<MTLCommandBuffer>)failed);
  assert(session->stats.gpu_failed==1 && session->stats.gpu_completed==1);

  /* A report flush has no frame drawable and must not register completion
   * telemetry, even though it successfully completes a command buffer. */
  CheckedQueue *queue=[CheckedQueue new];SignalsBuffer *flush=[SignalsBuffer new];
  queue.buffer=flush;s_queue=(id<MTLCommandQueue>)queue;
  s_dev=(id<MTLDevice>)[NSObject new];s_headless=1;
  assert(eng_encode_and_commit(nil)==0 && s_eng_checked_completed==1);
  assert(!flush.completion && session->stats.gpu_completed==1);
  fps_note_drawable(0);fps_note_drawable(1);fps_note_skipped_blit();
  assert(session->stats.drawable_attempts==2 && session->stats.nil_drawables==1 && session->stats.skipped_blits==1);

  SignalsDrawable *drawable=[SignalsDrawable new];
  fps_track_drawable((id<CAMetalDrawable>)drawable);
  drawable.presentedTime=0;drawable.presentation((id<MTLDrawable>)drawable);
  drawable.presentedTime=NAN;drawable.presentation((id<MTLDrawable>)drawable);
  drawable.presentedTime=CACurrentMediaTime();drawable.presentation((id<MTLDrawable>)drawable);
  drawable.presentation((id<MTLDrawable>)drawable);
  assert(session->stats.present_callbacks==4 && session->stats.displayed==1);
  assert(session->stats.zero_presented_time==1 && session->stats.invalid_presented_time==1 && session->stats.duplicate_presented_time==1);

  /* Pending old callbacks retain their original session across restart. */
  MetalFrameRateSession *fresh=[MetalFrameRateSession new];s_fps_session=fresh;
  success.completion((id<MTLCommandBuffer>)success);
  drawable.presentedTime+=0.1;drawable.presentation((id<MTLDrawable>)drawable);
  assert(fresh->stats.gpu_completed==0 && fresh->stats.displayed==0);
  assert(session->stats.gpu_completed==2 && session->stats.displayed==2);

  unsetenv("PS3RECOMP_METAL_FPS_DIAGNOSTICS");
  MetalFrameRateSession *disabled=[MetalFrameRateSession new];s_fps_session=disabled;
  SignalsBuffer *untouched=[SignalsBuffer new];
  fps_track_frame_completion((id<MTLCommandBuffer>)untouched);
  fps_note_drawable(0);fps_note_skipped_blit();
  assert(!untouched.completion && disabled->stats.drawable_attempts==0 && disabled->stats.skipped_blits==0);
  fps_stop();
  puts("Metal frame-rate callback/completion fixtures passed (no window/GPU work)");
 }
}
