#include "../rsx_metal_backend.m"
#include <assert.h>
/* This standalone Metal fixture intentionally has no GCM consumer. */
int cellGcm_native_consumer_enabled(void){return 0;}
void cellGcm_fifo_kick_notify(void){assert(!"disabled consumer must not be notified");}
void rsx_ring_progress_metal_committed(uint64_t ticket,uint64_t records){(void)ticket;(void)records;}
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
static id<MTLTexture> depth_texture(unsigned width,unsigned height) {
 MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:width height:height mipmapped:NO];
 d.usage=MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModePrivate;
 id<MTLTexture> t=[s_dev newTextureWithDescriptor:d];assert(t);return t;
}
static void check_depth_stencil(id<MTLTexture> t,float depth,unsigned char stencil) {
 NSUInteger row=256,size=row*t.height;
 id<MTLBuffer> db=[s_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
 id<MTLBuffer> sb=[s_dev newBufferWithLength:size options:MTLResourceStorageModeShared];assert(db && sb);
 id<MTLCommandBuffer> cb=[s_queue commandBuffer];id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];assert(blit);
 [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:db destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:size options:MTLBlitOptionDepthFromDepthStencil];
 [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(t.width,t.height,1) toBuffer:sb destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:size options:MTLBlitOptionStencilFromDepthStencil];
 [blit endEncoding];[cb commit];[cb waitUntilCompleted];assert(cb.status==MTLCommandBufferStatusCompleted);
 for(NSUInteger y=0;y<t.height;y++)for(NSUInteger x=0;x<t.width;x++) {
  float actual;memcpy(&actual,(unsigned char*)db.contents+y*row+x*4,4);assert(fabsf(actual-depth)<0.000001f);
  assert(*((unsigned char*)sb.contents+y*row+x)==stencil);
 }
}
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
  else if(!strcmp(argv[1],"independent_clears") || !strcmp(argv[1],"independent_depth_stencil")) {
   /* Independent clears must cover each entire surface, even when consecutive
    * records name targets of different dimensions. They are not a draw MRT. */
   MTLTextureDescriptor*d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];d.usage=MTLTextureUsageRenderTarget;d.storageMode=MTLStorageModeShared;
   id<MTLTexture> a=[s_dev newTextureWithDescriptor:d];assert(a);
   d.width=4;d.height=3;
   id<MTLTexture> b=[s_dev newTextureWithDescriptor:d];assert(b);
   u32 ha=eng_obj_add(a),hb=eng_obj_add(b);
   float red[4]={1,0,0,1},green[4]={0,1,0,1};
   int with_depth=!strcmp(argv[1],"independent_depth_stencil");
   id<MTLTexture> da=nil,db=nil;u32 hda=0,hdb=0;
   if(with_depth) {da=depth_texture(5,4);db=depth_texture(7,3);hda=eng_obj_add(da);hdb=eng_obj_add(db);}
   eng_clear_color(NULL,ha,red);
   if(with_depth)eng_clear_depth_stencil(NULL,hda,RSX_BE_CLEAR_DEPTH|RSX_BE_CLEAR_STENCIL,0.25f,7);
   eng_clear_color(NULL,hb,green);
   if(with_depth) {
    eng_clear_depth_stencil(NULL,hdb,RSX_BE_CLEAR_DEPTH|RSX_BE_CLEAR_STENCIL,0.33f,13);
    eng_clear_depth_stencil(NULL,hda,RSX_BE_CLEAR_DEPTH,0.75f,9);
    eng_clear_depth_stencil(NULL,hda,RSX_BE_CLEAR_STENCIL,0.5f,11);
   }
   assert(checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);
   unsigned char pixels[48]={0};
   [a getBytes:pixels bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
   for(unsigned j=0;j<16;j+=4)assert(pixels[j]==255 && pixels[j+1]==0 && pixels[j+2]==0 && pixels[j+3]==255);
   [b getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,3) mipmapLevel:0];
   for(unsigned j=0;j<48;j+=4)assert(pixels[j]==0 && pixels[j+1]==255 && pixels[j+2]==0 && pixels[j+3]==255);
   if(with_depth) {check_depth_stencil(da,0.75f,11);check_depth_stencil(db,0.33f,13);}
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
