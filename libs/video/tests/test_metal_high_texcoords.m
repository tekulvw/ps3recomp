/*
 * Synthetic RSX programs -> decompilers -> HLSL -> MSL -> Metal pixels.
 * Every TEXCOORD must carry all four lanes through the stage interface.
 * A projected lookup divides xy by w=2 and selects a known texture corner;
 * zeroing a coordinate or dropping its w lane selects the wrong color.
 * Also check the existing color output and both vertex-input layouts.
 * No game data, drawable, or window is used.
 */
#import <Metal/Metal.h>
#include "../rsx_vp_decompiler.h"
#include "../rsx_fp_decompiler.h"
#include "../rsx_shader_msl.h"
#include "../rsx_test_programs.h"
#include "rsx_source_saturate_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static const u8 corners[4][4] = {
    {255, 0, 0, 255}, {0, 255, 0, 255},
    {0, 0, 255, 255}, {255, 255, 255, 255}
};

static id<MTLFunction> shader_function(id<MTLDevice> dev, const char* hlsl, int stage)
{
    char msl[65536], log[4096];
    if (rsx_hlsl_to_msl(hlsl, stage, msl, sizeof msl, log, sizeof log)) {
        fprintf(stderr, "FAIL translation: %s\n", log);
        failures++;
        return nil;
    }
    NSError* error = nil;
    MTLCompileOptions* options = [MTLCompileOptions new];
#if defined(MAC_OS_VERSION_15_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_15_0
    if (@available(macOS 15.0, *)) options.mathMode = MTLMathModeSafe;
    else
#endif
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    } /* retain NaN behavior of the production guest shaders */
    id<MTLLibrary> library = [dev newLibraryWithSource:[NSString stringWithUTF8String:msl]
                                             options:options error:&error];
    if (!library) {
        fprintf(stderr, "FAIL Metal compile: %s\n", error.localizedDescription.UTF8String);
        failures++;
        return nil;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"main0"];
    if (!function) {
        fprintf(stderr, "FAIL Metal entry point\n");
        failures++;
    }
    return function;
}

/* DIV r1, TCn.xyzw, TCn.wwww; TEX r0, r1, unit 0. */
static void projected_fp(u8* fp, u32 tc)
{
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(0x3A) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_OUTREG(1) | RSX_TEST_FP_INSRC(4 + tc),
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(fp + 8, RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ(3,3,3,3));
    rsx_test_fp_instr(fp + 16, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_TEX) |
        RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
        (1u << 2) | RSX_TEST_FP_SWZ_IDENT); /* temporary r1 */
}

static void draw_case(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLFunction> vs,
                      const u8* fp, u32 bytes, id<MTLBuffer> vertices,
                      id<MTLBuffer> constants, id<MTLTexture> texture, u32 stride,
                      const char* name, const u8 expected[4])
{
    char hlsl[65536];
    if (rsx_fp_decompile(fp, bytes, 0x40, hlsl, sizeof hlsl) <= 0) {
        fprintf(stderr, "FAIL %s FP decompile\n", name);
        failures++;
        return;
    }
    id<MTLFunction> fs = shader_function(dev, hlsl, RSX_SHADER_STAGE_FRAGMENT);
    if (!fs) return;

    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs;
    pd.fragmentFunction = fs;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    /* glslang numbers the declared inputs in order. The production engine
     * packs enabled ATTRn into dense float4 slots for compact shaders. */
    for (u32 a = 0; a < stride / 16; a++) {
        vd.attributes[a].format = MTLVertexFormatFloat4;
        vd.attributes[a].offset = a * 16;
        vd.attributes[a].bufferIndex = 30;
    }
    vd.layouts[30].stride = stride;
    pd.vertexDescriptor = vd;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pipeline) {
        fprintf(stderr, "FAIL %s pipeline: %s\n", name, error.localizedDescription.UTF8String);
        failures++;
        return;
    }

    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:4 height:4 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> result = [dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = result;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pipeline];
    [enc setVertexBuffer:vertices offset:0 atIndex:30];
    [enc setVertexBuffer:constants offset:0 atIndex:0];
    [enc setFragmentTexture:texture atIndex:0];
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    [enc setFragmentSamplerState:[dev newSamplerStateWithDescriptor:sd] atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "FAIL %s GPU: %s\n", name, cb.error.localizedDescription.UTF8String);
        failures++;
        return;
    }

    u8 actual[64];
    [result getBytes:actual bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
    for (u32 i = 0; i < 16; i++) {
        for (u32 c = 0; c < 4; c++) {
            if (abs((int)actual[i * 4 + c] - (int)expected[c]) > 1) {
                fprintf(stderr, "FAIL %s pixel%u: got %u,%u,%u,%u expected %u,%u,%u,%u\n",
                    name, i, actual[i*4], actual[i*4+1], actual[i*4+2], actual[i*4+3],
                    expected[0], expected[1], expected[2], expected[3]);
                failures++;
                return;
            }
        }
    }
    printf("PASS %s: all 16 pixels %u,%u,%u,%u\n", name,
        expected[0], expected[1], expected[2], expected[3]);
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev || !rsx_hlsl_to_msl_available()) {
            fprintf(stderr, "Metal/translation unavailable\n");
            return 2;
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        u8 vp[12 * 16];
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 0);
        for (u32 tc = 0; tc < 10; tc++) {
            u32 attr = tc < 8 ? tc + 8 : tc - 7;
            u32 out = tc < 8 ? tc + 7 : (tc == 8 ? 15 : 6);
            rsx_test_vp_mov_out(vp + (tc + 2) * 16, attr, RSX_TEST_VP_SWZ_IDENT, out, tc == 9);
        }
        float verts[3][16][4] = {0};
        const float pos[3][4] = {{-1,-1,.5,1}, {3,-1,.5,1}, {-1,3,.5,1}};
        const float color[4] = {.25,.5,.75,1};
        for (u32 i = 0; i < 3; i++) {
            memcpy(verts[i][0], pos[i], sizeof pos[i]);
            memcpy(verts[i][3], color, sizeof color);
            for (u32 tc = 0; tc < 10; tc++) {
                u32 attr = tc < 8 ? tc + 8 : tc - 7;
                u32 corner = tc % 3;
                const float uv[4] = {(corner & 1) ? 1.5f : .5f,
                    (corner & 2) ? 1.5f : .5f, 0, 2};
                memcpy(verts[i][attr], uv, sizeof uv);
            }
        }
        float vc[514][4] = {0};
        vc[512][0] = vc[512][1] = vc[512][2] = 1;
        id<MTLBuffer> constants = [dev newBufferWithBytes:vc length:sizeof vc options:MTLResourceStorageModeShared];
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:2 height:2 mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [dev newTextureWithDescriptor:td];
        [texture replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 withBytes:corners bytesPerRow:8];

        for (u32 compact = 0; compact < 2; compact++) {
            char hlsl[65536];
            int count = compact ? rsx_vp_decompile_compact_ex(vp, sizeof vp, 0, 0xFF0Fu, hlsl, sizeof hlsl)
                                : rsx_vp_decompile(vp, sizeof vp, hlsl, sizeof hlsl);
            if (count != 12) {
                fprintf(stderr, "FAIL VP decompile: %d instructions\n", count);
                return 1;
            }
            id<MTLFunction> vs = shader_function(dev, hlsl, RSX_SHADER_STAGE_VERTEX);
            if (!vs) return 1;
            float packed[3 * 16 * 4];
            u32 slots = 0;
            for (u32 attr = 0; attr < 16; attr++)
                if (!compact || (0xFF0Fu & (1u << attr))) slots++;
            for (u32 i = 0; i < 3; i++) {
                u32 slot = 0;
                for (u32 attr = 0; attr < 16; attr++) {
                    if (compact && !(0xFF0Fu & (1u << attr))) continue;
                    memcpy(packed + (i * slots + slot++) * 4, verts[i][attr], 16);
                }
            }
            id<MTLBuffer> vertices = [dev newBufferWithBytes:packed length:3 * slots * 16
                                                   options:MTLResourceStorageModeShared];
            u8 fp[32];
            char name[64];
            for (u32 tc = 0; tc < 10; tc++) {
                projected_fp(fp, tc);
                snprintf(name, sizeof name, "%s TC%u projected", compact ? "compact" : "full", tc);
                draw_case(dev, queue, vs, fp, sizeof fp, vertices, constants, texture, slots * 16, name, corners[tc % 3]);
            }
            const u8 expected_color[4] = {64,128,191,255};
            rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
            snprintf(name, sizeof name, "%s COL0 preserved", compact ? "compact" : "full");
            draw_case(dev, queue, vs, fp, 16, vertices, constants, texture, slots * 16, name, expected_color);
        }
        /* A minimal full-screen program carries the finite test input in TC0. */
        u8 sat_vp[32];
        rsx_test_vp_mov_out(sat_vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(sat_vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
        char sat_hlsl[65536];
        if (rsx_vp_decompile(sat_vp, sizeof sat_vp, sat_hlsl, sizeof sat_hlsl) != 2) return 1;
        id<MTLFunction> sat_vs = shader_function(dev, sat_hlsl, RSX_SHADER_STAGE_VERTEX);
        if (!sat_vs) return 1;
        const struct {
            const char* name;
            u32 operand, modifier;
            int absolute, negate, destination_sat;
            float input, offset;
            u8 gray;
        } cases[] = {
            {"SAT src0 lower/log", 0,4,0,0,0,-.5f,.125f,32},
            {"SAT src1 lower/log", 1,4,0,0,0,-.5f,.125f,32},
            {"SAT src2 lower/log", 2,4,0,0,0,-.5f,.5f,64},
            {"SAT src0 upper", 0,4,0,0,0,1.5f,-.5f,128},
            {"SAT before abs/neg src0", 0,4,1,1,0,-.5f,.125f,32},
            {"SAT before abs/neg src1", 1,4,1,1,0,-.5f,.125f,32},
            {"unmodified source preserved", 0,0,0,0,0,-.5f,.75f,64},
            {"unknown modifier preserved", 0,5,0,0,0,-.5f,.75f,64},
            {"destination SAT preserved", 0,0,0,0,1,1.5f,.125f,255}
        };
        for (u32 c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            for (u32 i = 0; i < 3; i++)
                for (u32 lane = 0; lane < 4; lane++) verts[i][8][lane] = cases[c].input;
            id<MTLBuffer> vertices = [dev newBufferWithBytes:verts length:sizeof verts
                                                   options:MTLResourceStorageModeShared];
            u8 fp[64];
            source_saturate_fp(fp, cases[c].operand, cases[c].modifier,
                cases[c].absolute, cases[c].negate, cases[c].destination_sat, cases[c].offset);
            const u8 gray[4] = {cases[c].gray,cases[c].gray,cases[c].gray,cases[c].gray};
            draw_case(dev, queue, sat_vs, fp, sizeof fp, vertices, constants, texture,
                sizeof verts[0], cases[c].name, gray);
        }
    }
    return failures ? 1 : 0;
}
