/* Headless production HLSL-to-Metal WPOS observation; synthetic coordinates. */
#include "../rsx_metal_backend.m"
u8* vm_base;
u32 cellGcmResolveLocated(int local,u32 off){(void)local;return off;}
int main(int argc,char** argv)
{
 if(argc!=2)return 2;
 @autoreleasepool {
 s_dev=MTLCreateSystemDefaultDevice();if(!s_dev)return 2;s_queue=[s_dev newCommandQueue];NSError* error=nil;
 const char* vs="#include <metal_stdlib>\nusing namespace metal;vertex float4 vs(uint i[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i]*2,0.5,2);}";
 id<MTLLibrary> vl=[s_dev newLibraryWithSource:[NSString stringWithUTF8String:vs] options:nil error:&error];if(!vl)return 2;
 unsigned checks=0,failures=0;
 for(unsigned sx=1;sx<=2;sx++)for(unsigned sy=1;sy<=2;sy++){
 char path[1024],source[65536];snprintf(path,sizeof path,"%s/wpos-%u-%u.hlsl",argv[1],sx,sy);FILE* f=fopen(path,"rb");if(!f)return 2;size_t n=fread(source,1,sizeof source-1,f);source[n]=0;fclose(f);
 id<MTLFunction> fp=eng_function(source,RSX_SHADER_STAGE_FRAGMENT,"synthetic WPOS");if(!fp)return 2;
 MTLRenderPipelineDescriptor* pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[vl newFunctionWithName:@"vs"];pd.fragmentFunction=fp;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;
 id<MTLRenderPipelineState> pipeline=[s_dev newRenderPipelineStateWithDescriptor:pd error:&error];if(!pipeline){fprintf(stderr,"%s\n",error.description.UTF8String);return 2;}
 unsigned w=8*sx,h=4*sy;MTLTextureDescriptor* td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:w height:h mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;id<MTLTexture> output=[s_dev newTextureWithDescriptor:td];
 MTLRenderPassDescriptor* rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=output;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
 id<MTLCommandBuffer> cb=[s_queue commandBuffer];id<MTLRenderCommandEncoder> e=[cb renderCommandEncoderWithDescriptor:rp];[e setRenderPipelineState:pipeline];[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];[cb commit];[cb waitUntilCompleted];if(cb.status!=MTLCommandBufferStatusCompleted||cb.error)return 2;
 float pixels[16*8*4];[output getBytes:pixels bytesPerRow:w*16 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
 for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){const float expected[4]={(x+.5f)/sx,(y+.5f)/sy,.25f,.5f};for(unsigned c=0;c<4;c++){++checks;float got=pixels[(y*w+x)*4+c];if(fabsf(got-expected[c])>1e-6f){if(failures<8)fprintf(stderr,"FAIL %ux%u pixel%u,%u lane%u got%g expected%g\n",sx,sy,x,y,c,got,expected[c]);++failures;}}}
 }
 printf("WPOS Metal: %u checks/%u failures\n",checks,failures);return failures?1:0;
 }
}
