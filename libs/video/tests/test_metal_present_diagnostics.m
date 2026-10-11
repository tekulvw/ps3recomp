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

int main(int argc, char** argv) {
    assert(argc == 3);
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice(); assert(s_dev);
        s_queue = [s_dev newCommandQueue]; assert(s_queue);
        s_headless = 1;
        setenv("PS3RECOMP_METAL_FRAME_DUMP", argv[2], 1);
        setenv("PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK", "1", 1);
        if (strcmp(argv[1], "disabled")) setenv("PS3RECOMP_METAL_PRESENT_DIAGNOSTICS", "1", 1);
        else unsetenv("PS3RECOMP_METAL_PRESENT_DIAGNOSTICS");
        const unsigned char black[] = {0,0,0,255,0,0,0,255};
        const unsigned char colored[] = {0,0,0,255,17,23,31,255};
        id<MTLTexture> dark = texture(MTLPixelFormatRGBA8Unorm, black);
        id<MTLTexture> light = texture(MTLPixelFormatRGBA8Unorm, colored);
        u32 handle = eng_obj_add(!strcmp(argv[1], "color") ? light : dark);
        s_offscreen = texture(MTLPixelFormatRGBA8Unorm, black);
        if (strcmp(argv[1], "blit")) assert(eng_init(NULL, 2, 1) == 0);
        if (!strcmp(argv[1], "drawable") || !strcmp(argv[1], "drawable_periodic")) {
            s_headless = 0; s_layer = nil;
            s_inflight = dispatch_semaphore_create(1);
        }
        if (!strcmp(argv[1], "drawable_periodic")) unsetenv("PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK");
        if (!strcmp(argv[1], "missing")) handle = 0;
        int n = (!strcmp(argv[1], "budget") || !strcmp(argv[1], "flushed")) ? 1200 : 1;
        if (!strcmp(argv[1], "drawable_periodic")) n=120;
        for (int i=0; i<n; i++) {
            /* Real backend queue submission with an absent pipeline: it is
             * intentionally not counted as a successfully encoded GPU draw. */
            if (i>=500) { float v[4]={0}; eng_draw(NULL,RSX_TOPOLOGY_TRIANGLES,v,1,sizeof v,NULL,0); }
            if (i>=500 && !strcmp(argv[1], "flushed")) eng_submit_and_wait(NULL, 0);
            eng_present(NULL,handle);
        }
        if (!strcmp(argv[1],"color")) {
            const unsigned char rgb[]={0,0,0,17,23,31}; expect_image(argv[2],rgb);
        } else if (!strcmp(argv[1], "drawable_periodic")) {
            const unsigned char zero[]={0,0,0,0,0,0}; expect_image(argv[2],zero);
        } else assert(access(argv[2],F_OK)!=0);
    }
    return 0;
}
