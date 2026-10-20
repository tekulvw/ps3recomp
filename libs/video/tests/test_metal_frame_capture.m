/* Real readback regression plus present-context metadata. Synthetic pixels only. */
#define main texture_binding_capture_main
#include "test_metal_texture_binding_capture.m"
#undef main
int cellGcm_native_consumer_enabled(void){return 0;}
void cellGcm_fifo_kick_notify(void){}
void rsx_ring_progress_metal_committed(uint64_t ticket,uint64_t records){(void)ticket;(void)records;}
int main(int argc,char**argv)
{
 assert(!texture_binding_capture_main(argc,argv));
 @autoreleasepool {
  s_dev=MTLCreateSystemDefaultDevice();assert(s_dev);s_queue=[s_dev newCommandQueue];assert(s_queue);
  assert(!eng_init(NULL,4,4));
  u32 surface=eng_color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,2,2,NULL,0);assert(surface);
  const float green[4]={0,1,0,1};eng_clear_color(NULL,surface,green);
  eng_encode_and_commit(nil);
  id<MTLCommandBuffer>actual=[s_queue commandBuffer];[actual commit];[actual waitUntilCompleted];
  assert(actual.status==MTLCommandBufferStatusCompleted && !actual.error);
  s_capture_present_cb=actual;s_capture_guest=7;s_capture_command=8;s_capture_submission=9;s_capture_drawable=10;s_capture_presented=1;s_headless=0;
  assert(eng_capture_texture_rgba8(NULL,surface,argv[1]));
  const u8 want[4]={0,255,0,255};expect_file(argv[1],4,want);
  char path[2048],text[2048];snprintf(path,sizeof path,"%s.meta",argv[1]);FILE*f=fopen(path,"r");assert(f);
  size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
  assert(strstr(text,"guest_flip=7 command=8 submission=9 drawable=10 presented=1 headless=0 present_gpu_status=4 present_gpu_error=0"));
  assert(strstr(text,"backend_checked_failed=0"));
  eng_checked_fail();assert(eng_capture_texture_rgba8(NULL,surface,argv[1]));
  f=fopen(path,"r");assert(f);n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
  assert(strstr(text,"backend_checked_failed=1"));
  eng_shutdown(NULL);
  puts("PASS: completed surface pixels and exact retained actual-command metadata; not a window presentation test");
 }
 return 0;
}
