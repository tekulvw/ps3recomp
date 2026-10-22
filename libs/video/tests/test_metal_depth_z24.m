/* eng_depth_store_z24 against the CPU conversion the engine applies to
 * depth_readback's floats: every depth bit pattern sampled (all of [0, 1]
 * with DEPTH_Z24_EXHAUSTIVE=1), the specials, rendered depth, padded rows and
 * draws recorded before the store. No window or original game data.
 * clang -fobjc-arc -I include -I libs/video
 * libs/video/tests/test_metal_depth_z24.m libs/video/rsx_texture_layout.c libs/video/rsx_shader_msl.cpp -lc++
 * -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore
 * -o /tmp/test_metal_depth_z24 */
#include "../rsx_metal_backend.m"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr,"FAIL %s\n",name); } } while (0)

/* rsx_draw_engine.c's eng_depth_row_to_guest, D24S8 branch. */
static void cpu_z24(u8* dst, float d)
{
    d = d > 0.0f ? d : 0.0f;
    d = d < 1.0f ? d : 1.0f;
    const u32 v = (u32)((double)d * 16777215.0 + 0.5);
    dst[0] = (u8)(v >> 16); dst[1] = (u8)(v >> 8); dst[2] = (u8)v; dst[3] = 0;
}

enum { W = 4096, H = 1024 };

/* Fill the depth plane with raw float bits, bypassing rasterization. */
static int fill_depth(u32 handle, const uint32_t* bits, u32 w, u32 h)
{
    id<MTLBuffer> b = [s_dev newBufferWithBytes:bits length:(size_t)w * h * 4u
                                        options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:w * 4u sourceBytesPerImage:(NSUInteger)w * h * 4u
           sourceSize:MTLSizeMake(w, h, 1) toTexture:eng_obj(handle) destinationSlice:0
     destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)
              options:MTLBlitOptionDepthFromDepthStencil];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted && !cb.error;
}

static unsigned long long s_mismatch;
static int compare_batch(u32 depth, const uint32_t* bits, u8* got, u32 n)
{
    if (!fill_depth(depth, bits, W, H)) return 0;
    if (eng_depth_store_z24(NULL, depth, W, H, got, W * 4u) != 0) return 0;
    for (u32 i = 0; i < n; i++) {
        float f; memcpy(&f, &bits[i], 4);
        u8 want[4]; cpu_z24(want, f);
        /* A signalling NaN never comes out of a depth write, and what the
         * CPU's clamp makes of one depends on the instructions the compiler
         * picked (fmaxnm quiets it and the second clamp yields 1). The GPU
         * store gives every NaN the documented 0. */
        if ((bits[i] & 0x7FC00000u) == 0x7F800000u && (bits[i] & 0x003FFFFFu))
            memset(want, 0, 4);
        if (memcmp(want, got + 4u * i, 4)) {
            if (s_mismatch++ < 8)
                fprintf(stderr, "mismatch bits %08X: cpu %02X%02X%02X%02X gpu %02X%02X%02X%02X\n",
                        bits[i], want[0], want[1], want[2], want[3],
                        got[4*i], got[4*i+1], got[4*i+2], got[4*i+3]);
        }
    }
    return 1;
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice(); CHECK(s_dev != nil, "Metal device"); if (!s_dev) return 1;
        s_queue = [s_dev newCommandQueue]; CHECK(eng_init(NULL, 4, 3) == 0, "backend helpers");
        CHECK(s_engine_backend.depth_store_z24 != NULL, "backend advertises the z24 store");
        u32 depth = eng_depth_target_create(NULL, W, H); CHECK(depth != 0, "depth target");
        uint32_t* bits = malloc((size_t)W * H * 4u); u8* got = malloc((size_t)W * H * 4u);
        if (!bits || !got || !depth) return 1;

        /* Specials and the edges of the clamp and of each rounding step. */
        static const uint32_t special[] = {
            0x00000000u, 0x80000000u, 0x00000001u, 0x007FFFFFu, 0x00800000u, 0x3F800000u,
            0x3F7FFFFFu, 0x3F800001u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u,
            0x7F800001u, 0xBF800000u, 0x33800000u, 0x33000000u, 0x337FFFFFu, 0x33800001u,
            0x40000000u, 0x7F7FFFFFu };
        u32 n = 0;
        for (u32 i = 0; i < sizeof special / sizeof special[0]; i++) bits[n++] = special[i];
        /* Every value whose 24-bit quantization sits next to a rounding
         * boundary: k/16777215 +- a few ulps for a spread of k. */
        for (u32 k = 0; n + 9 <= (u32)W * H && k < 16777216u; k += 4099u) {
            float c = (float)((k + 0.5) / 16777215.0); uint32_t cb; memcpy(&cb, &c, 4);
            for (int d = -4; d <= 4; d++) bits[n++] = cb + (uint32_t)d;
        }
        while (n < (u32)W * H) { bits[n] = (uint32_t)n * 2654435761u; n++; }   /* arbitrary patterns */
        CHECK(compare_batch(depth, bits, got, n), "special and boundary batch stored");

        const int exhaustive = getenv("DEPTH_Z24_EXHAUSTIVE") != NULL;
        const uint32_t step = exhaustive ? 1u : 257u;
        unsigned long long tested = 0;
        for (uint64_t base = 0; base <= 0x3F800000u; ) {
            u32 m = 0;
            for (; m < (u32)W * H && base <= 0x3F800000u; m++, base += step) bits[m] = (uint32_t)base;
            for (u32 i = m; i < (u32)W * H; i++) bits[i] = 0;
            if (!compare_batch(depth, bits, got, m)) { CHECK(0, "range batch stored"); break; }
            tested += m;
        }
        printf("compared %llu bit patterns in [0, 1]%s\n", tested, exhaustive ? " (all)" : "");
        CHECK(s_mismatch == 0, "GPU z24 store equals the CPU conversion bit for bit");

        /* Padded destination rows, a sub-rectangle, and work recorded before
         * the store: a clear queued as a record must land first. */
        u32 small = eng_depth_target_create(NULL, 8, 4); CHECK(small != 0, "small depth target");
        eng_clear_depth_stencil(NULL, small, 3, 0.25f, 0);
        u8 padded[4 * 40]; memset(padded, 0xEE, sizeof padded);
        CHECK(eng_depth_store_z24(NULL, small, 5, 3, padded, 40) == 0, "store after a recorded clear");
        u8 want[4]; cpu_z24(want, 0.25f); int ok = 1;
        for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 10; x++) {
            const u8* p = padded + y * 40 + x * 4;
            if (y < 3 && x < 5) ok &= !memcmp(p, want, 4);
            else ok &= p[0] == 0xEE && p[1] == 0xEE && p[2] == 0xEE && p[3] == 0xEE;
        }
        CHECK(ok, "recorded clear visible; padding and rows outside the rectangle untouched");

        /* The same bytes as depth_readback plus the CPU conversion. */
        float fl[32]; CHECK(eng_depth_readback(NULL, small, 0, 0, 5, 3, fl, 20) == 0, "float readback");
        u8 via_cpu[60], via_gpu[60];
        for (u32 i = 0; i < 15; i++) cpu_z24(via_cpu + 4 * i, fl[i]);
        CHECK(eng_depth_store_z24(NULL, small, 5, 3, via_gpu, 20) == 0, "tight store");
        CHECK(!memcmp(via_cpu, via_gpu, sizeof via_cpu), "store equals readback + CPU conversion");

        u32 color = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 3, NULL, 0);
        u8 guard[16]; memset(guard, 0x5A, sizeof guard);
        CHECK(eng_depth_store_z24(NULL, color, 1, 1, guard, 4) != 0, "colour target rejected");
        CHECK(eng_depth_store_z24(NULL, small, 9, 1, guard, 36) != 0, "wider than the target rejected");
        CHECK(eng_depth_store_z24(NULL, small, 2, 1, guard, 4) != 0, "short pitch rejected");
        CHECK(eng_depth_store_z24(NULL, small, 1, 1, NULL, 4) != 0, "null destination rejected");
        ok = 1; for (u32 i = 0; i < sizeof guard; i++) ok &= guard[i] == 0x5A;
        CHECK(ok, "rejections leave the destination alone");

        eng_obj_release(NULL, color); eng_obj_release(NULL, small); eng_obj_release(NULL, depth);
        free(bits); free(got);
        printf("%d checks, %d failures\n", checks, failures); eng_shutdown(NULL);
        return failures ? 1 : 0;
    }
}
