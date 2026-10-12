#include "../rsx_metal_backend.m"
#include <assert.h>
static int checked(void* u,u32 reason) {
#ifdef PS3RECOMP_METAL_CHECKED_IMPLEMENTATION
 return eng_submit_and_wait_checked(u,reason);
#else
 (void)u;(void)reason;return 0;
#endif
}
@interface CheckedNilQueue:NSObject @end
@implementation CheckedNilQueue
- (id)commandBuffer {return nil;}
@end
@interface CheckedNilDevice:NSObject @end
@implementation CheckedNilDevice
- (id)newBufferWithBytes:(const void*)p length:(NSUInteger)n options:(NSUInteger)o {(void)p;(void)n;(void)o;return nil;}
@end
@interface CheckedBuffer:NSObject
@property int failed;
@end
@implementation CheckedBuffer
- (id)renderCommandEncoderWithDescriptor:(id)d {(void)d;return nil;}
- (void)commit {}
- (void)waitUntilCompleted {}
- (MTLCommandBufferStatus)status {return self.failed?MTLCommandBufferStatusError:MTLCommandBufferStatusCompleted;}
- (NSError*)error {return self.failed?[NSError errorWithDomain:@"fixture" code:1 userInfo:nil]:nil;}
@end
@interface CheckedQueue:NSObject
@property CheckedBuffer* buffer;
@end
@implementation CheckedQueue
- (id)commandBuffer {return self.buffer;}
@end
#ifndef PS3RECOMP_METAL_TRACKED_COMPLETION
#define eng_track_submission(cb) ((void)cb)
#endif
int main(int argc,char**argv) {
 @autoreleasepool {
  assert(argc==2);s_dev=MTLCreateSystemDefaultDevice();assert(s_dev);s_queue=[s_dev newCommandQueue];assert(s_queue);
  if(!strcmp(argv[1],"prior_error")) {CheckedBuffer*prior=[CheckedBuffer new];prior.failed=1;eng_track_submission((id<MTLCommandBuffer>)prior);assert(!checked(NULL,1));}
  else if(!strcmp(argv[1],"nil_cb")) {s_queue=(id<MTLCommandQueue>)[CheckedNilQueue new];assert(!checked(NULL,1));}
  else if(!strcmp(argv[1],"nil_stage")) {s_eng_stage_used=16;s_dev=(id<MTLDevice>)[CheckedNilDevice new];assert(!checked(NULL,1));}
  else if(!strcmp(argv[1],"nil_encoder") || !strcmp(argv[1],"gpu_error")) {
   CheckedQueue* q=[CheckedQueue new];q.buffer=[CheckedBuffer new];
   if(!strcmp(argv[1],"gpu_error"))q.buffer.failed=1;
   else {MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];d.usage=MTLTextureUsageRenderTarget;s_eng_rec_count=1;s_eng_rec[0].kind=ENG_REC_CLEAR_COLOR;s_eng_rec[0].nrt=1;s_eng_rec[0].rt[0]=eng_obj_add([s_dev newTextureWithDescriptor:d]);}
   s_queue=(id<MTLCommandQueue>)q;assert(!checked(NULL,1));
  }
  else if(!strcmp(argv[1],"clear")) {
   MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];d.usage=MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModeShared;
   id<MTLTexture> t=[s_dev newTextureWithDescriptor:d];assert(t);u32 h=eng_obj_add(t);
   float rgba[4]={1,0,0,1};eng_clear_color(NULL,h,rgba);assert(checked(NULL,1)==1);
   unsigned char pixel[4]={0};[t getBytes:pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];assert(pixel[0]==255 && pixel[1]==0 && pixel[2]==0 && pixel[3]==255);
  }
  else if(!strcmp(argv[1],"empty")) {
   assert(checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);
  } else if(!strcmp(argv[1],"missing_queue")) {
   id<MTLCommandQueue> saved=s_queue;s_queue=nil;
   assert(!checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE));s_queue=saved;
   assert(!checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE));
  } else if(!strcmp(argv[1],"invalid_record")) {
   s_eng_rec_count=1;s_eng_rec[0].kind=ENG_REC_DRAW;s_eng_rec[0].nrt=1;s_eng_rec[0].rt[0]=12345;
   assert(!checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE));
   s_eng_rec_count=0;assert(!checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE));
  } else {assert(!strcmp(argv[1],"dropped"));s_eng_dropped=1;assert(!checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE));s_eng_dropped=0;assert(!checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE));}
  puts("Metal checked completion scenario passed");
 }
}
