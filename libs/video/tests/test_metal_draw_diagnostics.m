/* Exercises actual Metal submission/readback and diagnostic sampling. */
/* Real Metal textures/readback; built by tools/test_metal_first_nonblack.py. */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <unistd.h>

static id<MTLTexture> texture(MTLPixelFormat format, const unsigned char* pixels)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
        width:2 height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> t = [s_dev newTextureWithDescriptor:d];
    assert(t);
    [t replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0
        withBytes:pixels bytesPerRow:8];
    return t;
}
static void expect_image(const char* path, const unsigned char* rgb)
{
    FILE* f = fopen(path, "rb"); assert(f);
    unsigned char actual[64]; size_t n = fread(actual, 1, sizeof(actual), f);
    assert(fclose(f) == 0);
    const char header[] = "P6\n2 1\n255\n";
    assert(n == sizeof(header) - 1 + 6);
    assert(memcmp(actual, header, sizeof(header) - 1) == 0);
    assert(memcmp(actual + sizeof(header) - 1, rgb, 6) == 0);
}

int main(int argc,char**argv) {
 assert(argc==2);
 @autoreleasepool {
  s_dev=MTLCreateSystemDefaultDevice();assert(s_dev);s_queue=[s_dev newCommandQueue];assert(s_queue);s_headless=1;
  if(strcmp(argv[1],"disabled"))setenv("PS3RECOMP_METAL_DRAW_DIAGNOSTICS","1",1);else unsetenv("PS3RECOMP_METAL_DRAW_DIAGNOSTICS");
  const unsigned char pixels[]={0,0,0,255,17,23,31,255};
  const unsigned char black[]={0,0,0,255,0,0,0,255};
  u32 handle=eng_obj_add(texture(MTLPixelFormatRGBA8Unorm,pixels));u32 dark=eng_obj_add(texture(MTLPixelFormatRGBA8Unorm,black));
  u32 alpha=eng_obj_add(texture(MTLPixelFormatRGBA8Unorm,black));
  assert(eng_init(NULL,2,1)==0);
  for(int i=0;i<10;i++) {
   s_eng_pending.nrt=1;s_eng_pending.rt[0]=handle;
   float v[]={1,2,3,4};
   for(int j=0;j<4;j++)eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,v,1,sizeof(v),NULL,0);
   if(i==0) {
    eng_clear_color(NULL,dark,(float[]){0,0.25,0,1});
    eng_clear_color(NULL,alpha,(float[]){0,0,0,1});
   }
   eng_submit_and_wait(NULL,0);
  }
 }
 return 0;
}
