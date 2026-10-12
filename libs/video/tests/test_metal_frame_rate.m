#define main checked_fixture_main
#include "test_metal_checked_completion.m"
#undef main

int main(int argc, char **argv) {
 @autoreleasepool {
  assert(argc == 2);
  if (!strcmp(argv[1], "window") || !strcmp(argv[1], "drawable") || !strcmp(argv[1], "disabled")) {
   setenv("PS3RECOMP_METAL_FPS", "1", 1);
   if (!strcmp(argv[1], "disabled")) unsetenv("PS3RECOMP_METAL_FPS");
   s_dev = MTLCreateSystemDefaultDevice(); assert(s_dev);
   s_width = s_height = 64;
   assert(create_window_impl("FPS fixture") == 0);
   if (!strcmp(argv[1], "disabled")) {
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.2]];
    assert([s_window.title isEqualToString:@"FPS fixture"]);
#ifdef PS3RECOMP_METAL_FRAME_RATE
    assert(!s_fps_session && !s_fps_timer);
#endif
   } else if (!strcmp(argv[1], "drawable")) {
#ifdef PS3RECOMP_METAL_FRAME_RATE
    s_queue=[s_dev newCommandQueue];assert(s_queue);
    [NSApp finishLaunching];
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
    [s_window display];[CATransaction flush];
    id<MTLCommandBuffer> flush=[s_queue commandBuffer];[flush commit];[flush waitUntilCompleted];
    assert(s_fps_session->stats.displayed==0); /* GPU completion is not a presentation */
    for (int frame=0;frame<6;frame++) {
    id<CAMetalDrawable> drawable=[s_layer nextDrawable];assert(drawable);
    fps_track_drawable(drawable);
    rsx_metal_backend_note_guest_frame();
    id<MTLCommandBuffer> cb=[s_queue commandBuffer];
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=drawable.texture;
    pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.colorAttachments[0].clearColor=MTLClearColorMake(0.1,0.3,0.5,1);
    id<MTLRenderCommandEncoder> encoder=[cb renderCommandEncoderWithDescriptor:pass];
    assert(encoder);[encoder endEncoding];
    [cb presentDrawable:drawable];[cb commit];[cb waitUntilCompleted];
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    }
    pthread_mutex_lock(&s_fps_session->lock);
    assert(s_fps_session->stats.displayed>0 && s_fps_session->stats.displayed<=6);
    assert(s_fps_session->stats.guest==6);
    pthread_mutex_unlock(&s_fps_session->lock);
    MetalFrameRateSession *old=s_fps_session;
    fps_stop();fps_start();
    assert(s_fps_session!=old && s_fps_session->stats.displayed==0);
#else
    assert(!"Metal drawable tracking is missing");
#endif
   } else {
   /* No rendering thread is running: the title must still update on a stall. */
   [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.2]];
   assert([s_window.title containsString:@"display 0.0 FPS"]);
   assert([s_window.title containsString:@"guest 0.0 FPS"]);
   }
#ifdef PS3RECOMP_METAL_FRAME_RATE
   fps_stop();
#endif
   [s_window close];
  } else {
#ifdef PS3RECOMP_METAL_FRAME_RATE
   MetalFrameRate stats = {0};
   metal_frame_rate_reset(&stats, 10.0);
   for (int i=1;i<=60;i++) metal_frame_rate_guest(&stats);
   for (int i=1;i<=10;i++) metal_frame_rate_presented(&stats, 10.0+i*0.1);
   MetalFrameRateSample sample = metal_frame_rate_sample(&stats, 11.0);
   assert(fabs(sample.display_fps-10.0)<0.001);
   assert(fabs(sample.guest_fps-60.0)<0.001);
   assert(fabs(sample.average_ms-100.0)<0.001);
   assert(fabs(sample.p95_ms-100.0)<0.001);
   sample = metal_frame_rate_sample(&stats, 13.0);
   assert(sample.display_fps==0 && sample.guest_fps==0);
   assert(sample.average_ms==0 && sample.p95_ms==0);
   assert(fabs(sample.age_seconds-2.0)<0.001);
   metal_frame_rate_presented(&stats, 13.1);
   metal_frame_rate_presented(&stats, 13.1); /* duplicate time cannot add a frame */
   metal_frame_rate_presented(&stats, 12.9); /* out-of-order callback */
   sample = metal_frame_rate_sample(&stats, 14.0);
   assert(sample.display_fps==1 && sample.guest_fps==0);
   assert(fabs(sample.worst_ms-2100.0)<0.001);
   metal_frame_rate_reset(&stats, 20.0);
   metal_frame_rate_presented(&stats, 20.01);
   for (int i=1;i<=300;i++) metal_frame_rate_presented(&stats, 20.01+i*0.01);
   sample=metal_frame_rate_sample(&stats, 24.0);
   assert(sample.display_fps==75.25);
   assert(fabs(sample.p95_ms-10.0)<0.001);
   assert(fabs(sample.worst_ms-10.0)<0.001);
#else
   assert(!"Metal frame-rate measurement is missing");
#endif
  }
  puts("Metal frame-rate scenario passed");
 }
}
