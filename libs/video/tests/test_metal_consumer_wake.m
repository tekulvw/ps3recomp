/* Real Metal completion only wakes the host consumer; it never publishes VM. */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <unistd.h>
static atomic_uint wake_count;
static int fixture_enabled;
int cellGcm_native_consumer_enabled(void){return fixture_enabled;}
void cellGcm_fifo_kick_notify(void){assert(fixture_enabled);atomic_fetch_add(&wake_count,1);}
int main(int argc,char**argv){@autoreleasepool{
 assert(argc==2);fixture_enabled=atoi(argv[1]);s_dev=MTLCreateSystemDefaultDevice();assert(s_dev);s_queue=[s_dev newCommandQueue];assert(s_queue);
 id<MTLBuffer> bytes=[s_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];assert(bytes);memset(bytes.contents,0,256);
 id<MTLCommandBuffer> cb=[s_queue commandBuffer];id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];[blit fillBuffer:bytes range:NSMakeRange(0,256) value:0x5a];[blit endEncoding];
 eng_track_submission(cb);assert(atomic_load(&wake_count)==0);[cb commit];[cb waitUntilCompleted];assert(cb.status==MTLCommandBufferStatusCompleted&&!cb.error);
 for(unsigned i=0;fixture_enabled&&!atomic_load(&wake_count)&&i<1000;i++)usleep(1000);
 assert(atomic_load(&wake_count)==(fixture_enabled?1u:0u));for(unsigned i=0;i<256;i++)assert(((uint8_t*)bytes.contents)[i]==0x5a);
 eng_check_submissions(1);assert(!s_eng_submitted.count&&!atomic_load(&s_eng_checked_failed));puts("real Metal consumer completion wake PASS");
}return 0;}
