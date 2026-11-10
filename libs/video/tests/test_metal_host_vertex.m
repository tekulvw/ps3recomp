/* Host-format vertex streams on the Metal backend (rsx_metal_backend.m),
 * headless: no device. The vertex descriptor a host-format pipeline is built
 * with puts slot s at attribute s, read from MTL_HOST_VB_INDEX(s) in the
 * slot's host format with its packed stride, clear of the constant blocks
 * and the float4 buffer; the float4 descriptor is unchanged. A recorded
 * draw_host draw is encoded with each stream bound at its own offset and
 * 16-bit indices, through a fake encoder.
 *
 * python3 tools/test_metal_host_vertex.py */
#include "../rsx_metal_backend.m"
#include <assert.h>
float cellAvconfExt_display_gamma(void){return 1.0f;}
int cellGcm_native_consumer_enabled(void){return 0;}
void cellGcm_fifo_kick_notify(void){}
void rsx_ring_progress_metal_committed(uint64_t ticket,uint64_t records){(void)ticket;(void)records;}

@interface FakeEncoder : NSObject
@end
@implementation FakeEncoder {
@public
    const void* vbuf[32]; NSUInteger voff[32]; int vset[32];
    MTLIndexType itype; NSUInteger icount, ioff; int draws;
}
- (void)setRenderPipelineState:(id)s { (void)s; }
- (void)setDepthStencilState:(id)s { (void)s; }
- (void)setCullMode:(MTLCullMode)m { (void)m; }
- (void)setFrontFacingWinding:(MTLWinding)w { (void)w; }
- (void)setStencilReferenceValue:(uint32_t)v { (void)v; }
- (void)setViewport:(MTLViewport)v { (void)v; }
- (void)setScissorRect:(MTLScissorRect)r { (void)r; }
- (void)setVertexBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { vbuf[i] = (__bridge const void*)b; voff[i] = o; vset[i] = 1; }
- (void)setFragmentBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { (void)b; (void)o; (void)i; }
- (void)setFragmentTexture:(id)t atIndex:(NSUInteger)i { (void)t; (void)i; }
- (void)setFragmentSamplerState:(id)s atIndex:(NSUInteger)i { (void)s; (void)i; }
- (void)setVertexTexture:(id)t atIndex:(NSUInteger)i { (void)t; (void)i; }
- (void)setVertexSamplerState:(id)s atIndex:(NSUInteger)i { (void)s; (void)i; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(id)b indexBufferOffset:(NSUInteger)o { (void)t; (void)b; itype = it; icount = n; ioff = o; draws++; }
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n { (void)t; (void)s; (void)n; draws++; }
@end

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("[PASS] " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    @autoreleasepool {
        rsx_vertex_layout_plan lp; memset(&lp, 0, sizeof lp);
        lp.count = 4; lp.stride = 64;
        const u8 f[4] = { RSX_HOST_VF_F32_3, RSX_HOST_VF_UN8_4, RSX_HOST_VF_F16_4, RSX_HOST_VF_SN16_4 };
        const MTLVertexFormat want[4] = { MTLVertexFormatFloat3, MTLVertexFormatUChar4Normalized,
                                          MTLVertexFormatHalf4, MTLVertexFormatShort4Normalized };
        const NSUInteger stride[4] = { 12, 4, 8, 8 };
        MTLVertexDescriptor* vd = eng_vertex_descriptor(&lp, 0, f);
        int ok = 1;
        for (u32 s = 0; s < 4; s++)
            ok &= vd.attributes[s].format == want[s] && vd.attributes[s].offset == 0 &&
                  vd.attributes[s].bufferIndex == MTL_HOST_VB_INDEX(s) &&
                  vd.layouts[MTL_HOST_VB_INDEX(s)].stride == stride[s];
        CHECK(ok, "host formats: slot s is attribute s from its own packed buffer in its format");
        CHECK(MTL_HOST_VB_INDEX(0) > 1 && MTL_HOST_VB_INDEX(RSX_DSP_NUM_VERTEX_ATTR - 1) < MTL_VB_INDEX,
              "host stream buffers sit between the constant blocks and the float4 buffer");
        MTLVertexDescriptor* v4 = eng_vertex_descriptor(&lp, 64, NULL);
        ok = 1;
        for (u32 s = 0; s < 4; s++)
            ok &= v4.attributes[s].format == MTLVertexFormatFloat4 && v4.attributes[s].offset == s * 16u &&
                  v4.attributes[s].bufferIndex == MTL_VB_INDEX;
        CHECK(ok && v4.layouts[MTL_VB_INDEX].stride == 64, "the float4 descriptor is unchanged");
        CHECK(eng_host_vertex_format(RSX_HOST_VF_NONE) == MTLVertexFormatInvalid, "no format for none");

        /* Encoding a host-stream record. */
        s_null_tex = (id<MTLTexture>)[NSObject new];
        s_default_sampler = (id<MTLSamplerState>)[NSObject new];
        s_eng_pipe[0].pso = (id<MTLRenderPipelineState>)[NSObject new];
        s_eng_pipe[0].ds = (id<MTLDepthStencilState>)[NSObject new];
        s_eng_pipe_count = 1;
        id<MTLBuffer> stage = (id<MTLBuffer>)[NSObject new];
        EngRecord r; memset(&r, 0, sizeof r);
        r.kind = ENG_REC_DRAW; r.pipeline = 1; r.vertex_count = 30; r.index_count = 54; r.ib_off = 4096;
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) r.samp[u] = -1;
        for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) r.vsamp[u] = -1;
        r.host_streams = 3; r.index_u16 = 1;
        r.host_off[0] = 256; r.host_off[1] = 256 + 360; r.host_off[2] = 256 + 480;
        FakeEncoder* fake = [FakeEncoder new];
        eng_encode_draw((id<MTLRenderCommandEncoder>)fake, &r, stage);
        ok = fake->draws == 1 && fake->itype == MTLIndexTypeUInt16 && fake->icount == 54 && fake->ioff == 4096 &&
             !fake->vset[MTL_VB_INDEX];
        for (u32 s = 0; s < 3; s++)
            ok &= fake->vset[MTL_HOST_VB_INDEX(s)] && fake->vbuf[MTL_HOST_VB_INDEX(s)] == (__bridge const void*)stage &&
                  fake->voff[MTL_HOST_VB_INDEX(s)] == r.host_off[s];
        CHECK(ok, "a host-stream draw binds each stream at its offset and draws with 16-bit indices");
        /* A float4 record still binds the one vertex buffer and 32-bit indices. */
        FakeEncoder* fake4 = [FakeEncoder new];
        r.host_streams = 0; r.index_u16 = 0; r.vb_off = 512;
        eng_encode_draw((id<MTLRenderCommandEncoder>)fake4, &r, stage);
        CHECK(fake4->vset[MTL_VB_INDEX] && fake4->voff[MTL_VB_INDEX] == 512 && fake4->itype == MTLIndexTypeUInt32 &&
              !fake4->vset[MTL_HOST_VB_INDEX(0)], "a float4 draw is encoded as before");
    }
    printf("test_metal_host_vertex: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
