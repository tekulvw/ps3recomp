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
int main(int argc, char** argv)
{
    assert(argc == 3);
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice(); assert(s_dev);
        s_queue = [s_dev newCommandQueue]; assert(s_queue);
        s_headless = strcmp(argv[1], "windowed") != 0;
        const char* path = argv[2];
        setenv("PS3RECOMP_METAL_FRAME_DUMP", path, 1);
        const unsigned char black[] = {0,0,0,255, 0,0,0,255};
        const unsigned char colored[] = {0,0,0,255, 17,23,31,255};
        const unsigned char rgb[] = {0,0,0, 17,23,31};
        const unsigned char bgr[] = {0,0,0, 31,23,17};
        const unsigned char zero[] = {0,0,0,0,0,0};
        id<MTLTexture> dark = texture(MTLPixelFormatRGBA8Unorm, black);
        int bgra = strcmp(argv[1], "bgra") == 0;
        id<MTLTexture> light = texture(bgra ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm, colored);
        if (!strcmp(argv[1], "default") || !strcmp(argv[1], "disabled")) {
            if (!strcmp(argv[1], "disabled"))
                setenv("PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK", "0", 1);
            else unsetenv("PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK");
            for (int i=0; i<119; ++i) eng_dump_frame(light, 1);
            assert(access(path, F_OK) != 0);
            eng_dump_frame(dark, 1);
            expect_image(path, zero); /* Existing periodic mode captures black. */
            for (int i=0; i<119; ++i) eng_dump_frame(light, 1);
            expect_image(path, zero);
            eng_dump_frame(light, 1);
            expect_image(path, rgb);
        } else {
            setenv("PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK", "1", 1);
            /* Opaque black must not publish even at the old 120-frame gate. */
            int black_frames = !strcmp(argv[1], "black") ? 121 : 5;
            for (int i=0; i<black_frames; ++i) eng_dump_frame(dark, 1);
            assert(access(path, F_OK) != 0);
            eng_dump_frame(texture(MTLPixelFormatR8Unorm, colored + 4), 1);
            eng_dump_frame(light, 0); /* No drawable/presentation submission. */
            assert(access(path, F_OK) != 0);
            if (!strcmp(argv[1], "retry")) {
                char bad_path[4096];
                snprintf(bad_path, sizeof(bad_path), "%s/missing/frame.ppm", path);
                setenv("PS3RECOMP_METAL_FRAME_DUMP", bad_path, 1);
                eng_dump_frame(light, 1);
                setenv("PS3RECOMP_METAL_FRAME_DUMP", path, 1);
            }
            int presented = 1;
            if (!strcmp(argv[1], "presentation")) {
                id<MTLTexture> target = texture(MTLPixelFormatRGBA8Unorm, black);
                id<MTLCommandBuffer> cb = [s_queue commandBuffer];
                assert(!eng_blit_to_display(cb, light, target)); /* No helper. */
                assert(eng_init(NULL, 2, 1) == 0);
                assert(!eng_blit_to_display(nil, light, target));
                assert(!eng_blit_to_display(cb, nil, target));
                assert(!eng_blit_to_display(cb, light, nil));
                presented = eng_blit_to_display(cb, light, target);
                assert(presented);
                [cb commit]; [cb waitUntilCompleted];
                assert([cb status] == MTLCommandBufferStatusCompleted);
            }
            eng_dump_frame(light, presented);
            expect_image(path, bgra ? bgr : rgb);
            for (int i=0; i<240; ++i) eng_dump_frame(dark, 1);
            expect_image(path, bgra ? bgr : rgb);
            /* A completed one-shot must return before messaging any texture,
             * including pixelFormat/getBytes, or submitting another fence. */
            eng_dump_frame((id<MTLTexture>)[NSObject new], 1);
            expect_image(path, bgra ? bgr : rgb);
        }
        printf("Metal frame capture %s PASS\n", argv[1]);
    }
}
