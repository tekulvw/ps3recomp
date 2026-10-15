#include "../rsx_metal_backend.m"
#include <assert.h>
static unsigned allocations,commits,waits;
static NSMutableArray* pending_fixture;
@interface EmptyBuffer:NSObject
@property MTLCommandBufferStatus fixture_status;
@property int fixture_error;
@end
@implementation EmptyBuffer
- (void)commit {++commits;}
- (void)waitUntilCompleted {
 ++waits;self.fixture_status=MTLCommandBufferStatusCompleted;
 for(EmptyBuffer* cb in pending_fixture)if(!cb.fixture_error)cb.fixture_status=MTLCommandBufferStatusCompleted;
}
- (MTLCommandBufferStatus)status {return self.fixture_status;}
- (NSError*)error {return self.fixture_error?[NSError errorWithDomain:@"fixture" code:1 userInfo:nil]:nil;}
@end
@interface EmptyQueue:NSObject @end
@implementation EmptyQueue
- (id)commandBuffer {++allocations;EmptyBuffer* cb=[EmptyBuffer new];cb.fixture_status=MTLCommandBufferStatusCompleted;return cb;}
@end
@interface EmptyDevice:NSObject @end
@implementation EmptyDevice
- (id)newBufferWithBytes:(const void*)p length:(NSUInteger)n options:(NSUInteger)o {(void)p;(void)n;(void)o;return nil;}
@end
int main(int argc,char** argv) {
 @autoreleasepool {
  assert(argc==3);int enabled=atoi(argv[1]);const char* scenario=argv[2];
  setenv("PS3RECOMP_METAL_EMPTY_FLUSH",enabled?"1":"0",1);
  unsetenv("PS3RECOMP_METAL_EMPTY_FLUSH_DIAGNOSTICS");
  s_dev=(id<MTLDevice>)[EmptyDevice new];s_queue=(id<MTLCommandQueue>)[EmptyQueue new];
  s_eng_active=1;s_headless=1;s_eng_checked_completed=37;
  pending_fixture=[NSMutableArray new];
  int fast_allowed=enabled,expected_allocations=1,expected_failure=0,checked=0;
  if(!strcmp(scenario,"completed") || !strcmp(scenario,"pending") || !strcmp(scenario,"mixed") || !strcmp(scenario,"error")) {
   EmptyBuffer* cb=[EmptyBuffer new];cb.fixture_status=MTLCommandBufferStatusCompleted;
   if(!strcmp(scenario,"pending") || !strcmp(scenario,"mixed"))cb.fixture_status=MTLCommandBufferStatusCommitted;
   if(!strcmp(scenario,"error")){cb.fixture_status=MTLCommandBufferStatusError;cb.fixture_error=1;}
   eng_track_submission((id<MTLCommandBuffer>)cb);[pending_fixture addObject:cb];
   if(!strcmp(scenario,"mixed")){EmptyBuffer* done=[EmptyBuffer new];done.fixture_status=MTLCommandBufferStatusCompleted;eng_track_submission((id<MTLCommandBuffer>)done);}
   if(strcmp(scenario,"completed"))fast_allowed=0;
   if(!strcmp(scenario,"error"))expected_failure=1;
  } else if(!strcmp(scenario,"record")) {s_eng_rec_count=1;s_eng_rec[0].kind=0x7fffffff;fast_allowed=0;expected_failure=1;}
  else if(!strcmp(scenario,"stage")){s_eng_stage_used=16;fast_allowed=0;expected_allocations=1;expected_failure=1;}
  else if(!strcmp(scenario,"dropped")){s_eng_dropped=1;fast_allowed=0;expected_failure=1;}
  else if(!strcmp(scenario,"prior_error")){eng_checked_fail();fast_allowed=0;expected_failure=1;}
  else if(!strcmp(scenario,"inactive")){s_eng_active=0;fast_allowed=0;}
  else if(!strcmp(scenario,"missing_device")){s_dev=nil;fast_allowed=0;expected_allocations=0;expected_failure=1;}
  else if(!strcmp(scenario,"missing_queue")){s_queue=nil;fast_allowed=0;expected_allocations=0;expected_failure=1;}
  else if(!strcmp(scenario,"tracking_failure")){eng_track_submission(nil);fast_allowed=0;expected_failure=1;}
  else if(!strcmp(scenario,"checked")){fast_allowed=0;checked=1;}
  else if(!strcmp(scenario,"retired")){s_eng_obj[0]=(id<MTLTexture>)[NSObject new];s_eng_obj_count=1;s_eng_obj_retired[0]=1;}
  else assert(!strcmp(scenario,"empty"));
  if(checked)assert(eng_submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);
  else eng_submit_and_wait(NULL,RSX_BE_FLUSH_GUEST_REFERENCE);
  if(fast_allowed){assert(!allocations && !commits && !waits);assert(s_eng_checked_completed==37);}
  else {assert(allocations==(unsigned)expected_allocations);if(!strcmp(scenario,"stage"))assert(!commits && !waits);else assert(commits==allocations && waits==allocations);}
  assert(atomic_load(&s_eng_checked_failed)==expected_failure);
  if(!strcmp(scenario,"retired")){assert(s_eng_obj[0]==nil && s_eng_obj_free_count==1);eng_submit_and_wait(NULL,1);assert(s_eng_obj_free_count==1);}
  if(!strcmp(scenario,"completed") || !strcmp(scenario,"pending") || !strcmp(scenario,"mixed") || !strcmp(scenario,"error"))assert(![s_eng_submitted count]);
  printf("PASS mode=%d scenario=%s allocations=%u commits=%u waits=%u failed=%d\n",enabled,scenario,allocations,commits,waits,atomic_load(&s_eng_checked_failed));
 }
}
