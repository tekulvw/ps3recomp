/*
 * Synthetic RSX programs -> decompilers -> HLSL -> MSL -> Metal pixels.
 * TEX_BX2 expands sampled normal-map lanes before destination SAT. The
 * tests include actual normal reconstruction, unaffected plain sampling,
 * forced channels, and formats that do not permit opcode expansion.
 * No game data, drawable, or window is used.
 */
#import <Metal/Metal.h>
#include "../rsx_vp_decompiler.h"
#include "../rsx_fp_decompiler.h"
#include "../rsx_shader_msl.h"
#include "../rsx_test_programs.h"
#include "../rsx_texture_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

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

static void draw_case(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLFunction> vs,
                      const u8* fp, u32 bytes, id<MTLBuffer> vertices,
                      id<MTLBuffer> constants, id<MTLTexture> texture, u32 stride,
                      const char* name, const u8 expected[4], const u8 masks[16])
{
    char hlsl[65536];
    int count;
#ifdef RSX_FP_TEXTURE_BX2_MASKS
    rsx_fp_constant_block values;
    rsx_fp_collect_constants(fp, bytes, &values);
    count = rsx_fp_decompile_buffered_texture_ex(fp, bytes, 0x40, 0, masks,
                                                hlsl, sizeof hlsl, NULL);
#else
    (void)masks;
    count = rsx_fp_decompile(fp, bytes, 0x40, hlsl, sizeof hlsl);
#endif
    if (count <= 0) {
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
#ifdef RSX_FP_TEXTURE_BX2_MASKS
    id<MTLBuffer> fc = [dev newBufferWithBytes:values.values length:sizeof values.values
                                      options:MTLResourceStorageModeShared];
    [enc setFragmentBuffer:fc offset:0 atIndex:1];
#endif
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

/* TEX r1, TC0; MAD r0, r1, .5, .5. The last instruction maps signed
 * samples back to [0,1], so negative expansion errors remain visible. */
static void expanded_fp(u8 fp[48], int expand, int destination_sat)
{
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(0x17) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_OUTREG(1) |
        (expand ? 1u << 21 : 0) | (destination_sat ? 1u << 31 : 0),
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_instr(fp + 16, RSX_TEST_FP_OPC(4) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_END, (1u << 2) | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(fp + 24, 2u | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(fp + 28, 2u | RSX_TEST_FP_SWZ_IDENT);
    for (u32 c = 0; c < 4; c++) rsx_test_fp_put_word(fp + 32 + c * 4, 0x3F000000u);
}

/* TEX_BX2 normal.GA; DP2 n.xy,n.xy; ADD 1,-len2; RSQ; RCP.
 * A flat packed normal reconstructs z=1. Missing expansion gives z~=sqrt(.5). */
static void reconstructed_fp(u8 fp[96])
{
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(0x17) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_OUTREG(1) | (1u << 21),
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_instr(fp + 16, RSX_TEST_FP_OPC(0x38) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_OUTREG(2), (1u << 2) | RSX_TEST_FP_SWZ(1,3,1,3));
    rsx_test_fp_put_word(fp + 24, (1u << 2) | RSX_TEST_FP_SWZ(1,3,1,3));
    rsx_test_fp_instr(fp + 32, RSX_TEST_FP_OPC(3) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_OUTREG(2), (2u << 2) | (1u << 17) | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(fp + 40, 2u | RSX_TEST_FP_SWZ_IDENT);
    for (u32 c = 0; c < 4; c++) rsx_test_fp_put_word(fp + 48 + c * 4, 0x3F800000u);
    rsx_test_fp_instr(fp + 64, RSX_TEST_FP_OPC(0x1B) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_OUTREG(2), (2u << 2) | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_instr(fp + 80, RSX_TEST_FP_OPC(0x1A) | RSX_TEST_FP_MASK_XYZW |
        RSX_TEST_FP_END, (2u << 2) | RSX_TEST_FP_SWZ_IDENT);
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev || !rsx_hlsl_to_msl_available()) return 2;
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        u8 vp[32];
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
        char hlsl[65536];
        if (rsx_vp_decompile(vp, sizeof vp, hlsl, sizeof hlsl) != 2) return 1;
        id<MTLFunction> vs = shader_function(dev, hlsl, RSX_SHADER_STAGE_VERTEX);
        if (!vs) return 1;
        float verts[3][16][4] = {0};
        const float pos[3][4] = {{-1,-1,.5,1},{3,-1,.5,1},{-1,3,.5,1}};
        for (u32 i = 0; i < 3; i++) {
            memcpy(verts[i][0], pos[i], 16);
            verts[i][8][0] = verts[i][8][1] = .5f;
        }
        float vc[514][4] = {0};
        vc[512][0] = vc[512][1] = vc[512][2] = 1;
        id<MTLBuffer> vertices = [dev newBufferWithBytes:verts length:sizeof verts options:MTLResourceStorageModeShared];
        id<MTLBuffer> constants = [dev newBufferWithBytes:vc length:sizeof vc options:MTLResourceStorageModeShared];
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [dev newTextureWithDescriptor:td];
        const struct {
            const char* name;
            int expand, destination_sat, reconstruct;
            u8 mask, sample[4], expected[4];
        } cases[] = {
            {"TEX_BX2 signed lanes",1,0,0,15,{64,128,192,255},{64,128,192,255}},
            {"plain TEX unchanged",0,0,0,15,{64,128,192,255},{160,191,224,255}},
            {"BX2 before destination SAT",1,1,0,15,{64,128,192,255},{128,128,192,255}},
            {"forced ZERO/ONE retained",1,0,0,12,{0,255,192,255},{128,255,192,255}},
            {"float format retained",1,0,0,0,{64,128,192,255},{160,191,224,255}},
            {"depth format retained",1,0,0,0,{64,128,192,255},{160,128,128,255}},
            {"signed channel precedence",1,0,0,14,{64,128,192,255},{160,128,192,255}},
            {"flat normal reconstruction",1,0,1,15,{0,128,0,127},{255,255,255,255}}
        };
        for (u32 c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            id<MTLTexture> case_texture = texture;
            if (strstr(cases[c].name, "float")) {
                td.pixelFormat = MTLPixelFormatRGBA32Float;
                case_texture = [dev newTextureWithDescriptor:td];
                const float rgba[4] = {64.f/255.f,128.f/255.f,192.f/255.f,1};
                [case_texture replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:rgba bytesPerRow:16];
            } else if (strstr(cases[c].name, "depth")) {
                /* The production backend exposes decoded depth as R32Float. */
                td.pixelFormat = MTLPixelFormatR32Float;
                case_texture = [dev newTextureWithDescriptor:td];
                const float z = 64.f/255.f;
                [case_texture replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&z bytesPerRow:4];
            } else {
                [case_texture replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:cases[c].sample bytesPerRow:4];
            }
            u8 fp[96], masks[16] = {0};
            u32 fmt = 0x88, remap = 0xAAE4, filter = 0;
            if (strstr(cases[c].name, "float")) fmt = 0x9B;
            if (strstr(cases[c].name, "depth")) fmt = 0x90;
            if (strstr(cases[c].name, "forced")) remap = 0x92E4;
            if (strstr(cases[c].name, "signed channel")) filter = 1u << 29;
            masks[0] = rsx_texture_bx2_mask(fmt, remap, filter);
            if (masks[0] != cases[c].mask) {
                fprintf(stderr, "FAIL %s eligibility mask %u expected %u\n",
                    cases[c].name, masks[0], cases[c].mask);
                failures++;
                continue;
            }
            if (cases[c].reconstruct) reconstructed_fp(fp);
            else expanded_fp(fp, cases[c].expand, cases[c].destination_sat);
            draw_case(dev, queue, vs, fp, cases[c].reconstruct ? 96 : 48,
                vertices, constants, case_texture, sizeof verts[0], cases[c].name, cases[c].expected, masks);
        }
    }
    return failures ? 1 : 0;
}
