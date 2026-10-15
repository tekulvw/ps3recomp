/* Protocol-only fixtures: no NSApplication, window, Metal device, queue, or
 * GPU submission. Exercise the real callback adapters and control-file parser. */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <unistd.h>
@interface MeasurementBuffer : NSObject
@property(copy) MTLCommandBufferHandler completion;
@property int failed;
@end
@implementation MeasurementBuffer
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {self.completion=handler;}
- (MTLCommandBufferStatus)status {return self.failed?MTLCommandBufferStatusError:MTLCommandBufferStatusCompleted;}
- (NSError*)error {return self.failed?[NSError errorWithDomain:@"fixture" code:1 userInfo:nil]:nil;}
@end
@interface MeasurementDrawable : NSObject
@property double presentedTime;
@property(copy) MTLDrawablePresentedHandler presentation;
@end
@implementation MeasurementDrawable
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)handler {self.presentation=handler;}
@end
/* Minimal Metal protocol boundary for the REAL engine commit path. Encoding
 * records is empty; the encoder accepts the existing fullscreen display blit.
 * No Metal object or GPU queue is created. Completion runs inside commit to
 * exercise the ordering race with post-commit telemetry bookkeeping. */
@interface MeasurementEncoder : NSObject @end
@implementation MeasurementEncoder
- (void)setRenderPipelineState:(id)p {(void)p;}
- (void)setFragmentTexture:(id)t atIndex:(NSUInteger)i {(void)t;(void)i;}
- (void)setFragmentSamplerState:(id)t atIndex:(NSUInteger)i {(void)t;(void)i;}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n {(void)t;(void)s;assert(n==3);}
- (void)endEncoding {}
@end
@interface MeasurementTexture : NSObject @end
@implementation MeasurementTexture
- (MTLPixelFormat)pixelFormat {return MTLPixelFormatBGRA8Unorm;}
@end
@interface CommittingDrawable : MeasurementDrawable
@property id texture;
@end
@implementation CommittingDrawable
@end
@interface CommittingBuffer : MeasurementBuffer
@property NSMutableArray *handlers;
@property CommittingDrawable *drawable;
@property unsigned commits;
@end
@implementation CommittingBuffer
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
 if(!self.handlers)self.handlers=[NSMutableArray new];[self.handlers addObject:[handler copy]];
}
- (id)renderCommandEncoderWithDescriptor:(id)d {(void)d;return [MeasurementEncoder new];}
- (void)presentDrawable:(id)d {self.drawable=d;}
- (void)commit {
 ++self.commits;self.drawable.presentedTime=CACurrentMediaTime();
 self.drawable.presentation((id<MTLDrawable>)self.drawable);
 for(MTLCommandBufferHandler handler in self.handlers)handler((id<MTLCommandBuffer>)self);
}
@end
@interface MeasurementQueue : NSObject
@property CommittingBuffer *buffer;
@end
@implementation MeasurementQueue
- (id)commandBuffer {return self.buffer;}
@end
@interface MeasurementLayer : NSObject
@property CommittingDrawable *drawable;
@end
@implementation MeasurementLayer
- (id)nextDrawable {return self.drawable;}
@end
static void command(NSString *path,const char *text) {
 FILE *f=fopen(path.fileSystemRepresentation,"w");assert(f);fputs(text,f);fclose(f);
}
int main(void) {
 @autoreleasepool {
  char path[]="/private/tmp/metal-measurement-control-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
  setenv("PS3RECOMP_METAL_FPS_WINDOW_CONTROL",path,1);
  setenv("PS3RECOMP_METAL_FPS_DIAGNOSTICS","1",1);
  MetalFrameRateSession *old=[MetalFrameRateSession new];s_fps_session=old;
  double now=CACurrentMediaTime();
  command(old->window_control,"-1 1\n");fps_window_poll(old,now);assert(!old->measurement);
  command(old->window_control,"1 1 trailing\n");fps_window_poll(old,now);assert(!old->measurement);
  command(old->window_control,"1 1\n");fps_window_poll(old,now);
  assert(old->measurement && old->measurement->request_id==1);
  command(old->window_control,"2 1\n");fps_window_poll(old,now+.01);
  assert(old->measurement->request_id==1); /* busy request does not reset it */
  s_present_guest_id=42;s_present_buffer_id=3;
  rsx_metal_backend_note_guest_frame();
  MetalFrameTicket *ticket=fps_frame_ticket();assert(ticket && ticket->guest_flip_id==42 && ticket->buffer_id==3);
  MeasurementBuffer *buffer=[MeasurementBuffer new];MeasurementDrawable *drawable=[MeasurementDrawable new];
  fps_track_frame_completion_tagged((id<MTLCommandBuffer>)buffer,ticket,1);
  fps_track_drawable_tagged((id<CAMetalDrawable>)drawable,ticket);
  /* Callbacks can race post-commit bookkeeping. */
  drawable.presentedTime=now+.1;drawable.presentation((id<MTLDrawable>)drawable);
  buffer.completion((id<MTLCommandBuffer>)buffer);fps_frame_committed(ticket);
  MetalFrameMeasurementResult result;
  assert(metal_measurement_finish(old->measurement,now+3,&result));
  assert(result.request_id==1 && result.committed==1 && result.unique_guest_displayed==1 && result.seconds==1);
  /* A late callback must not enter another window in the SAME session. */
  assert(metal_measurement_begin(old->measurement,3,now,1,2));
  drawable.presentedTime=now+.2;drawable.presentation((id<MTLDrawable>)drawable);
  buffer.completion((id<MTLCommandBuffer>)buffer);fps_frame_committed(ticket);
  assert(old->measurement->count==0);
  /* Nor may it contaminate a replacement backend session. */
  MetalFrameRateSession *fresh=[MetalFrameRateSession new];s_fps_session=fresh;
  fresh->measurement=calloc(1,sizeof(*fresh->measurement));assert(fresh->measurement);
  assert(metal_measurement_begin(fresh->measurement,1,now,1,2));
  buffer.completion((id<MTLCommandBuffer>)buffer);
  assert(fresh->measurement->count==0 && fresh->stats.gpu_completed==0);
  /* A zero-time presentation stays zero even after successful completion. */
  s_present_guest_id=43;MetalFrameTicket *zero=fps_frame_ticket();
  MeasurementBuffer *zb=[MeasurementBuffer new];MeasurementDrawable *zd=[MeasurementDrawable new];
  fps_track_frame_completion_tagged((id<MTLCommandBuffer>)zb,zero,1);
  fps_track_drawable_tagged((id<CAMetalDrawable>)zd,zero);fps_frame_committed(zero);
  zb.completion((id<MTLCommandBuffer>)zb);zd.presentation((id<MTLDrawable>)zd);
  assert(metal_measurement_finish(fresh->measurement,now+3,&result));
  assert(result.gpu_completed==1 && result.zero_presented_time==1 && result.unique_guest_displayed==0 && result.verified_guest_fps==0 && !result.qualified);
  /* Actual engine frame submission must record exactly the committed buffer,
   * never a pre-submit admission or a successful offscreen report flush. */
  fresh->measurement->finalized=1;
  now=CACurrentMediaTime();assert(metal_measurement_begin(fresh->measurement,2,now,1,2));
  MeasurementTexture *texture=[MeasurementTexture new];CommittingDrawable *cd=[CommittingDrawable new];cd.texture=texture;
  MeasurementLayer *layer=[MeasurementLayer new];layer.drawable=cd;
  CommittingBuffer *cb=[CommittingBuffer new];MeasurementQueue *queue=[MeasurementQueue new];queue.buffer=cb;
  s_dev=(id<MTLDevice>)[NSObject new];s_queue=(id<MTLCommandQueue>)queue;s_layer=(CAMetalLayer*)layer;
  s_eng_helper_lib=(id<MTLLibrary>)[NSObject new];s_eng_blit_pso=(id<MTLRenderPipelineState>)[NSObject new];
  s_eng_blit_pso_fmt=MTLPixelFormatBGRA8Unorm;s_headless=0;s_inflight=dispatch_semaphore_create(3);
  s_present_guest_id=44;s_present_buffer_id=1;
  assert(eng_encode_and_commit((id<MTLTexture>)texture)==1 && cb.commits==1);
  MetalFrameMeasurement *copy=malloc(sizeof(*copy));assert(copy);memcpy(copy,fresh->measurement,sizeof(*copy));
  assert(metal_measurement_finish(copy,now+3,&result));free(copy);
  assert(result.committed==1 && result.gpu_completed==1 && result.unique_guest_displayed==1);
  assert(result.qualified && result.verified_guest_fps==1);
  fresh->last_request_id=2;fps_window_poll(fresh,now+3);assert(fresh->measurement->finalized);
  eng_check_submissions(0);s_eng_submitted=nil;s_inflight=nil;
  /* A ticket owns its original session exactly through its callback lifetime. */
  __weak MetalFrameRateSession *weak_session;
  @autoreleasepool {
   MetalFrameRateSession *ephemeral=[MetalFrameRateSession new];weak_session=ephemeral;s_fps_session=ephemeral;
   MetalFrameTicket *owner=fps_frame_ticket();assert(owner);
   MeasurementBuffer *pending=[MeasurementBuffer new];
   fps_track_frame_completion_tagged((id<MTLCommandBuffer>)pending,owner,1);
   s_fps_session=nil;ephemeral=nil;owner=nil;
   assert(weak_session!=nil); /* callback still owns the old session */
   pending.completion=nil;pending=nil;
  }
  assert(weak_session==nil);
  unsetenv("PS3RECOMP_METAL_FPS_WINDOW_CONTROL");
  s_fps_session=[MetalFrameRateSession new];assert(!fps_frame_ticket());
  fps_stop();unlink(path);
  puts("Tagged callbacks, same/new-session lifecycle, control boundaries, and zero timestamps passed");
 }
 return 0;
}
