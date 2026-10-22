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

        /* The GPU-built alias samples as the decoded upload does: same
         * texels through read() for both formats and a spread of crossbars. */
        {
            enum { AW = 37, AH = 9, SW = 40 };   /* texture inside a wider store */
            u32 big = eng_depth_target_create(NULL, SW, AH + 2);
            for (u32 i = 0; i < SW * (AH + 2); i++) {
                float f = (float)((i * 2654435761u) & 0xFFFFFF) / 16777215.0f;
                memcpy(&bits[i], &f, 4);
            }
            CHECK(fill_depth(big, bits, SW, AH + 2), "alias source depth");
            u8 words[SW * (AH + 2) * 4];
            CHECK(eng_depth_store_z24(NULL, big, SW, AH + 2, words, SW * 4) == 0, "alias store");
            NSError* err = nil;
            id<MTLLibrary> lib = [s_dev newLibraryWithSource:
                @"#include <metal_stdlib>\nusing namespace metal;\n"
                 "kernel void rd(texture2d<float, access::read> t [[texture(0)]], device float4* o [[buffer(0)]],"
                 " uint2 p [[thread_position_in_grid]]) { if (p.x < t.get_width() && p.y < t.get_height())"
                 " o[p.y * t.get_width() + p.x] = t.read(p); }" options:nil error:&err];
            id<MTLComputePipelineState> rd = [s_dev newComputePipelineStateWithFunction:
                [lib newFunctionWithName:@"rd"] error:&err];
            CHECK(rd != nil, "read kernel");
            static const u32 remaps[] = { 0xAAE4u, 0xAA93u, 0xAA1Bu, 0x55E4u, 0xAAD8u, 0x0000u, 0xFF39u, 0xAA4Eu };
            static const u32 fmts[] = { 0xA5u, 0xBEu };
            int same = 1, built = 1;
            for (u32 fi = 0; fi < 2; fi++) for (u32 ri = 0; ri < sizeof remaps / sizeof remaps[0]; ri++) {
                rsx_tex_layout tl; rsx_texture_layout(fmts[fi], AW, AH, &tl);
                tl.row_bytes = SW * 4;    /* the texture's pitch is the store's */
                u8 dec[AW * AH * 4];
                rsx_texture_decode(dec, AW * 4, words, AW, AH, &tl, rsx_texture_argb_is_rgba());
                u32 cpu = eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, AW, AH, 1, 1, remaps[ri], fmts[fi]);
                eng_texture_upload(NULL, cpu, 0, 0, AW, AH, dec, AW * 4, AH);
                u32 gpu = eng_depth_z24_texture(NULL, AW, AH, remaps[ri], fmts[fi]);
                built &= gpu != 0;
                if (!gpu) { eng_obj_release(NULL, cpu); continue; }
                id<MTLBuffer> a = [s_dev newBufferWithLength:AW * AH * 16 options:MTLResourceStorageModeShared];
                id<MTLBuffer> b = [s_dev newBufferWithLength:AW * AH * 16 options:MTLResourceStorageModeShared];
                id<MTLCommandBuffer> cb = [s_queue commandBuffer];
                id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
                [ce setComputePipelineState:rd];
                [ce setTexture:eng_obj(cpu) atIndex:0]; [ce setBuffer:a offset:0 atIndex:0];
                [ce dispatchThreads:MTLSizeMake(AW, AH, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [ce setTexture:eng_obj(gpu) atIndex:0]; [ce setBuffer:b offset:0 atIndex:0];
                [ce dispatchThreads:MTLSizeMake(AW, AH, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
                if (memcmp([a contents], [b contents], AW * AH * 16)) {
                    same = 0;
                    const float* x = [a contents]; const float* y = [b contents];
                    fprintf(stderr, "fmt %02X remap %04X: cpu %g %g %g %g gpu %g %g %g %g\n", fmts[fi], remaps[ri],
                            x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
                }
                eng_obj_release(NULL, cpu); eng_obj_release(NULL, gpu);
            }
            CHECK(built, "alias textures built on the GPU");
            CHECK(same, "GPU alias texels equal the decoded upload's");
            CHECK(eng_depth_z24_texture(NULL, SW + 1, AH, 0xAAE4u, 0xA5u) == 0, "alias wider than the store rejected");
            CHECK(eng_depth_z24_texture(NULL, AW, AH, 0xAAE4u, 0x81u) == 0, "other formats rejected");
            float fl1; CHECK(eng_depth_readback(NULL, big, 0, 0, 1, 1, &fl1, 4) == 0, "float readback reuses staging");
            CHECK(eng_depth_z24_texture(NULL, AW, AH, 0xAAE4u, 0xA5u) == 0, "staging reused: alias refused");
            eng_obj_release(NULL, big);
        }

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
