/* eng_encode_draw sends a render encoder only the state that differs from the
 * draw before it in the same pass (rsx_metal_backend.m). Headless: no device.
 * A fake encoder records each binding; at every draw call its state must be
 * exactly the draw's record, as if every binding had been sent. A random
 * stream of draws over a few pipelines, textures, samplers, offsets,
 * viewports and scissors, with new encoders between runs, checks that, and
 * that repeated state is actually skipped.
 *
 * python3 tools/test_metal_encode_filter.py */
#include "../rsx_metal_backend.m"
#include <assert.h>
float cellAvconfExt_display_gamma(void){return 1.0f;}
int cellGcm_native_consumer_enabled(void){return 0;}
void cellGcm_fifo_kick_notify(void){}
void rsx_ring_progress_metal_committed(uint64_t ticket,uint64_t records){(void)ticket;(void)records;}

typedef struct { const void* buf; NSUInteger off; } FakeBinding;
@interface FakeEncoder : NSObject
@end
@implementation FakeEncoder {
@public
    const void *pso, *ds; MTLCullMode cull; MTLWinding winding; uint32_t stencil;
    MTLViewport vp; MTLScissorRect sc; int have_sc;
    MTLDepthClipMode clip; float bias, slope, clamp;
    FakeBinding vbuf[32], fbuf[32];
    const void *ftex[32], *fsamp[32], *vtex[32], *vsamp[32];
    unsigned calls, draws;
}
- (void)setRenderPipelineState:(id)s { pso = (__bridge const void*)s; calls++; }
- (void)setDepthStencilState:(id)s { ds = (__bridge const void*)s; calls++; }
- (void)setDepthClipMode:(MTLDepthClipMode)m { clip = m; calls++; }
- (void)setDepthBias:(float)b slopeScale:(float)s clamp:(float)c { bias = b; slope = s; clamp = c; calls++; }
- (void)setCullMode:(MTLCullMode)m { cull = m; calls++; }
- (void)setFrontFacingWinding:(MTLWinding)w { winding = w; calls++; }
- (void)setStencilReferenceValue:(uint32_t)v { stencil = v; calls++; }
- (void)setViewport:(MTLViewport)v { vp = v; calls++; }
- (void)setScissorRect:(MTLScissorRect)r { sc = r; have_sc = 1; calls++; }
- (void)setVertexBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { vbuf[i] = (FakeBinding){ (__bridge const void*)b, o }; calls++; }
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { assert(vbuf[i].buf); vbuf[i].off = o; calls++; }
- (void)setFragmentBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i { fbuf[i] = (FakeBinding){ (__bridge const void*)b, o }; calls++; }
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { assert(fbuf[i].buf); fbuf[i].off = o; calls++; }
- (void)setFragmentTexture:(id)t atIndex:(NSUInteger)i { ftex[i] = (__bridge const void*)t; calls++; }
- (void)setFragmentSamplerState:(id)s atIndex:(NSUInteger)i { fsamp[i] = (__bridge const void*)s; calls++; }
- (void)setVertexTexture:(id)t atIndex:(NSUInteger)i { vtex[i] = (__bridge const void*)t; calls++; }
- (void)setVertexSamplerState:(id)s atIndex:(NSUInteger)i { vsamp[i] = (__bridge const void*)s; calls++; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(id)b indexBufferOffset:(NSUInteger)o { (void)t; (void)n; (void)it; (void)b; (void)o; draws++; }
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n { (void)t; (void)s; (void)n; draws++; }
@end

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng % n); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 20) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

/* The fake encoder's state against what sending every binding would leave. */
static void expect_record(FakeEncoder* e, const EngRecord* r, id<MTLBuffer> stage, const MTLScissorRect* sc, int have_sc, int n)
{
    const EngPipeline* p = &s_eng_pipe[r->pipeline - 1];
    CHECK(e->pso == (__bridge const void*)p->pso && e->ds == (__bridge const void*)p->ds &&
          e->cull == p->cull && e->winding == p->winding && e->stencil == r->stencil_ref, "draw %d: pipeline state", n);
    CHECK(e->clip == p->clip && e->bias == r->depth_bias && e->slope == r->depth_slope && e->clamp == 0.0f, "draw %d: depth clip/bias", n);
    CHECK(e->vp.originX == r->vp[0] && e->vp.originY == r->vp[1] && e->vp.width == r->vp[2] && e->vp.height == r->vp[3],
          "draw %d: viewport", n);
    CHECK(have_sc == e->have_sc && (!have_sc || !memcmp(&e->sc, sc, sizeof *sc)), "draw %d: scissor", n);
    const void* sb = (__bridge const void*)stage;
    CHECK(e->vbuf[MTL_VB_INDEX].buf == sb && e->vbuf[MTL_VB_INDEX].off == r->vb_off, "draw %d: vertices", n);
    if (r->vs_cb_bytes) CHECK(e->vbuf[0].buf == sb && e->vbuf[0].off == r->vs_cb_off, "draw %d: vertex constants", n);
    if (r->ps_cb_bytes) CHECK(e->fbuf[1].buf == sb && e->fbuf[1].off == r->ps_cb_off, "draw %d: fragment constants", n);
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        id<MTLTexture> t = eng_obj(r->tex[u]);
        const void* want = (__bridge const void*)(t ? t : s_null_tex);
        id<MTLSamplerState> s = r->samp[u] >= 0 ? s_eng_samp[r->samp[u]].samp : s_default_sampler;
        CHECK(e->ftex[u] == want && e->fsamp[u] == (__bridge const void*)s, "draw %d: fragment unit %u", n, u);
    }
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        id<MTLTexture> t = eng_obj(r->vtex[u]);
        const void* want = (__bridge const void*)(t ? t : s_null_tex);
        id<MTLSamplerState> s = r->vsamp[u] >= 0 ? s_eng_samp[r->vsamp[u]].samp : s_default_sampler;
        CHECK(e->vtex[MTL_VTEX_INDEX(u)] == want && e->vsamp[MTL_VSAMP_INDEX(u)] == (__bridge const void*)s,
              "draw %d: vertex unit %u", n, u);
    }
}

int main(void)
{
    @autoreleasepool {
        /* Stand-ins: only identity matters to the encoder filter. */
        s_null_tex = (id<MTLTexture>)[NSObject new];
        s_default_sampler = (id<MTLSamplerState>)[NSObject new];
        for (u32 i = 0; i < 3; i++) {
            s_eng_pipe[i].pso = (id<MTLRenderPipelineState>)[NSObject new];
            s_eng_pipe[i].ds = (id<MTLDepthStencilState>)[NSObject new];
            s_eng_pipe[i].cull = i == 2 ? MTLCullModeBack : MTLCullModeNone;
            s_eng_pipe[i].clip = i == 1 ? MTLDepthClipModeClamp : MTLDepthClipModeClip;
            s_eng_pipe[i].winding = i == 1 ? MTLWindingClockwise : MTLWindingCounterClockwise;
        }
        s_eng_pipe_count = 3;
        for (u32 i = 0; i < 3; i++) s_eng_samp[i].samp = (id<MTLSamplerState>)[NSObject new];
        u32 tex[4];
        for (u32 i = 0; i < 4; i++) tex[i] = eng_obj_add((id<MTLTexture>)[NSObject new]);
        id<MTLBuffer> stages[2] = { (id<MTLBuffer>)[NSObject new], (id<MTLBuffer>)[NSObject new] };

        unsigned total_calls = 0, total_draws = 0, unfiltered = 0;
        int n = 0;
        for (int pass = 0; pass < 200; pass++) {
            FakeEncoder* fake = [FakeEncoder new];
            id<MTLRenderCommandEncoder> enc = (id<MTLRenderCommandEncoder>)fake;
            id<MTLBuffer> stage = stages[pass & 1];
            eng_encode_state_reset();
            EngRecord r; memset(&r, 0, sizeof r);
            r.kind = ENG_REC_DRAW; r.vertex_count = 3; r.stride = 16;
            for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) r.samp[u] = -1;
            for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) r.vsamp[u] = -1;
            MTLScissorRect sc = { 0 }; int have_sc = 0;
            const int draws = 1 + (int)rnd(30);
            for (int d = 0; d < draws; d++, n++) {
                /* Mostly repeats, as a material run is. */
                if (!d || rnd(4) == 0) r.pipeline = 1 + rnd(3);
                r.depth_bias = (float)(d % 3); r.depth_slope = (float)(d % 2);
                if (!d || rnd(5) == 0) r.stencil_ref = rnd(2);
                if (!d || rnd(6) == 0) { r.vp[0] = (float)rnd(2); r.vp[1] = 0; r.vp[2] = 64.0f + (float)rnd(2); r.vp[3] = 32; }
                if (rnd(5) == 0) { r.sc[0] = rnd(3); r.sc[1] = 0; r.sc[2] = rnd(3) * 8u; r.sc[3] = 16; }
                r.vb_off = rnd(3) * 256u;
                r.vs_cb_bytes = rnd(4) ? 16u : 0u; r.vs_cb_off = 1024u + rnd(2) * 256u;
                r.ps_cb_bytes = rnd(4) ? 16u : 0u; r.ps_cb_off = 4096u + rnd(2) * 256u;
                if (rnd(3) == 0) { const u32 u = rnd(RSX_BE_MAX_TEXTURES); r.tex[u] = rnd(2) ? tex[rnd(4)] : 0; r.samp[u] = (int)rnd(4) - 1; }
                if (rnd(8) == 0) { const u32 u = rnd(RSX_BE_MAX_VERTEX_TEXTURES); r.vtex[u] = rnd(2) ? tex[rnd(4)] : 0; r.vsamp[u] = (int)rnd(4) - 1; }
                if (r.sc[2] && r.sc[3]) { sc = (MTLScissorRect){ r.sc[0], r.sc[1], r.sc[2], r.sc[3] }; have_sc = 1; }
                eng_encode_draw(enc, &r, stage);
                expect_record(fake, &r, stage, &sc, have_sc, n);
                /* What sending everything would have cost. */
                unfiltered += 6 + 2 + 1 + (r.vs_cb_bytes != 0) + (r.ps_cb_bytes != 0) + (r.sc[2] && r.sc[3]) +
                              2 * RSX_BE_MAX_TEXTURES + 2 * RSX_BE_MAX_VERTEX_TEXTURES;
            }
            total_calls += fake->calls; total_draws += fake->draws;
        }
        CHECK(total_draws == (unsigned)n, "every record was drawn (%u of %d)", total_draws, n);
        CHECK(total_calls * 2 < unfiltered, "repeated state is skipped (%u binding calls, %u unfiltered)", total_calls, unfiltered);
        printf("test_metal_encode_filter: %d draws, %u binding calls instead of %u, %d failure(s)\n",
               n, total_calls, unfiltered, fails);
    }
    return fails ? 1 : 0;
}
