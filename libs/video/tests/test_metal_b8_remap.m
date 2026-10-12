/* Real engine texture upload and GPU sampling of MAG's B8 glyph crossbar. */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <math.h>

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice(); assert(s_dev);
        s_queue = [s_dev newCommandQueue]; assert(s_queue);
        NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void sample_b8(texture2d<float> t [[texture(0)]], device float4* out [[buffer(0)]], uint i [[thread_position_in_grid]]) {"
            "constexpr sampler s(coord::pixel, address::clamp_to_edge, filter::nearest);"
            "out[i]=t.sample(s,float2(float(i)+0.5,0.5));}";
        NSError* error = nil;
        id<MTLLibrary> lib = [s_dev newLibraryWithSource:source options:nil error:&error];
        assert(lib && !error);
        id<MTLComputePipelineState> pipeline = [s_dev newComputePipelineStateWithFunction:
            [lib newFunctionWithName:@"sample_b8"] error:&error];
        assert(pipeline && !error);
        const u8 bytes[] = {0, 173};
        const u32 remaps[] = {0x02FF, 0xAAE4, 0xAA00, 0x00FF};
        const float wants[4][8] = {
            {0,0,0,0, 0,0,0,173.0f/255.0f},
            {0,0,0,1, 173.0f/255.0f,173.0f/255.0f,173.0f/255.0f,1},
            {1,1,1,1, 1,1,1,1},
            {0,0,0,0, 0,0,0,0}
        };
        for (u32 mode = 0; mode < 4; ++mode) {
            u32 handle = eng_texture_create(NULL, RSX_BE_FMT_R8, 2, 1, 1, 1,
                                            remaps[mode], 0xA1);
            assert(handle);
            eng_texture_upload(NULL, handle, 0, 0, 2, 1, bytes, 2, 1);
            id<MTLBuffer> result = [s_dev newBufferWithLength:8*sizeof(float)
                options:MTLResourceStorageModeShared]; assert(result);
            id<MTLCommandBuffer> cb = [s_queue commandBuffer]; assert(cb);
            id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder]; assert(encoder);
            [encoder setComputePipelineState:pipeline];
            [encoder setTexture:eng_obj(handle) atIndex:0];
            [encoder setBuffer:result offset:0 atIndex:0];
            [encoder dispatchThreads:MTLSizeMake(2,1,1) threadsPerThreadgroup:MTLSizeMake(2,1,1)];
            [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
            assert([cb status] == MTLCommandBufferStatusCompleted);
            const float* actual = [result contents];
            for (u32 i=0; i<8; ++i) {
                if (fabsf(actual[i]-wants[mode][i]) > 0.00001f) {
                    fprintf(stderr,"B8 sample remap=%04x lane=%u actual=%g expected=%g\n",
                        remaps[mode],i,actual[i],wants[mode][i]);
                    return 1;
                }
            }
            eng_obj_release(NULL, handle);
        }
        puts("Metal B8 remap PASS: font alpha, identity RGB/opaque alpha, alpha selection, forced zero");
    }
    return 0;
}
