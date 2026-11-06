/* D3D12 register-file engine backend on a real adapter, headless, with the
 * debug layer when it is installed (any error message fails the run):
 *  - clear, then a decompiler-shaped pipeline (ATTRn inputs, VPConst at b0,
 *    PSConstants at b1) draws a full-target triangle whose colour comes from
 *    both constant blocks; the readback is exact;
 *  - an indexed draw, a scissor and a viewport restrict where it lands;
 *  - MRT: two targets of one size and format both take the draw;
 *  - depth: a nearer draw wins, a farther one is rejected, then a cleared
 *    depth accepts it again;
 *  - texture sampling with the TEXTURE_CONTROL1 crossbar (a channel rotation)
 *    and a surface view of a colour target;
 *  - color_target_write lands rows at an offset;
 *  - released objects are retired on the fence and their handles reused;
 *  - every refusal is counted, not silently skipped.
 * No window, no swapchain, no guest memory and no game data.
 * python3 tools/test_d3d12_engine_backend.py */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../rsx_d3d12_backend.c"
#include <assert.h>
#include <math.h>

/* The fixture has no guest: none of these may be reached on its paths. */
u8* vm_base; u32 ppu_vm_size, ppu_hle_inject_base;
static void no_guest(const char* what) { fprintf(stderr, "FAIL fixture reached guest-side %s\n", what); exit(1); }
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index) { (void)index; no_guest("display buffers"); return NULL; }
u32 cellGcmGetCurrentDisplayBufferId(void) { no_guest("display buffer id"); return 0; }
unsigned cellGcm_flip_request_count(void) { no_guest("flip count"); return 0; }
u32 cellGcmResolveOffset(u32 offset) { (void)offset; no_guest("offset"); return 0; }
u32 cellGcmResolveLocated(int location, u32 offset) { (void)location; (void)offset; no_guest("located offset"); return 0; }
u32 cellGcmResolveIO(u32 offset) { (void)offset; no_guest("IO offset"); return 0; }
int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; no_guest("display offset"); return 0; }
u32 rsx_find_vram_upload(u32 size) { (void)size; no_guest("vram upload"); return 0; }
void rsx_reset_upload_claims(void) { no_guest("upload claims"); }
void cellGcm_replay_display_buffers_to_engine(void) {}
u32 gcm_io_offset_to_ea(u32 offset) { (void)offset; no_guest("IO map"); return 0; }
void vm_write32(uint32_t a, uint32_t v) { (void)a; (void)v; no_guest("memory write"); }
void ps3_ms(const char* key) { (void)key; }
unsigned long long ps3_ms_now(void) { return 0; }
struct spu_context;
uint32_t spurs_policy_service_identity(const struct spu_context* c) { (void)c; return 0; }
/* The owner seam takes the guest consumer lease around original work; the
 * fixture has no guest, so the lease is a counter. */
static int consumer_depth;
void cellGcm_rsx_consumer_begin(void) { consumer_depth++; }
void cellGcm_rsx_consumer_end(void) { consumer_depth--; }

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } \
                            else fprintf(stderr, "ok   %s\n", name); } while (0)

static const char VS[] =
    "struct VSInput { float4 a0:ATTR0; float4 a8:ATTR8; };\n"
    "struct VSOutput { float4 pos:SV_Position; float4 col0:COLOR0; float4 tc0:TEXCOORD0; };\n"
    "cbuffer VPConst : register(b0) { float4 vp_c[512]; float4 vp_posscale; float4 vp_posoffset; };\n"
    "VSOutput main(VSInput input) { VSOutput o;\n"
    "  o.pos = float4(input.a0.xyz * vp_posscale.xyz + input.a0.w * vp_posoffset.xyz, input.a0.w);\n"
    "  o.col0 = vp_c[1];\n"
    "  o.tc0 = input.a8; return o; }\n";
static const char PS[] =
    "struct PSInput { float4 pos:SV_Position; float4 col0:COLOR0; float4 tc0:TEXCOORD0; };\n"
    "cbuffer PSConstants : register(b1) { float4 fp_c[2]; };\n"
    "float4 main(PSInput input) : SV_TARGET { return input.col0 * fp_c[0]; }\n";
static const char PS_TEX[] =
    "struct PSInput { float4 pos:SV_Position; float4 col0:COLOR0; float4 tc0:TEXCOORD0; };\n"
    "Texture2D    rsx_tex[16] : register(t0);\n"
    "SamplerState rsx_samp[16] : register(s0);\n"
    "float4 main(PSInput input) : SV_TARGET { return rsx_tex[0].Sample(rsx_samp[0], input.tc0.xy); }\n";
static const char PS_MRT[] =
    "struct PSInput { float4 pos:SV_Position; float4 col0:COLOR0; float4 tc0:TEXCOORD0; };\n"
    "struct PSOutput { float4 c0:SV_Target0; float4 c1:SV_Target1; };\n"
    "PSOutput main(PSInput input) { PSOutput o; o.c0 = input.col0; o.c1 = input.col0.bgra; return o; }\n";

/* Two attribute slots (ATTR0 position, ATTR8 texcoord), one float4 each. */
typedef struct { float pos[4]; float tc[4]; } vtx;
static const vtx FULL[3] = { { { -1, -1, 0.5f, 1 }, { 0, 1, 0, 0 } }, { { -1, 3, 0.5f, 1 }, { 0, -1, 0, 0 } },
                             { { 3, -1, 0.5f, 1 }, { 2, 1, 0, 0 } } };

static rsx_be_render_state plain_state(void)
{
    rsx_be_render_state rs; memset(&rs, 0, sizeof rs);
    rs.color_mask = 0x01010101u; rs.depth_func = 0x0203u; /* LEQUAL */
    rs.s_func = 0x0207u; rs.s_func_mask = 0xFF; rs.s_write_mask = 0xFF;
    rs.s_fail = rs.s_zfail = rs.s_zpass = 0x1E00u;
    return rs;
}
static rsx_vertex_layout_plan layout2(void)
{
    rsx_vertex_layout_plan l; memset(&l, 0, sizeof l);
    l.count = 2; l.attrs[0] = 0; l.attrs[1] = 8; l.stride = sizeof(vtx);
    return l;
}
static u32 px(const rsx_draw_backend* b, u32 rt, u32 x, u32 y)
{
    u32 v = 0; b->readback(b->user, rt, x, y, 1, 1, &v, 4); return v;
}
static void draw_full(const rsx_draw_backend* b, u32 pipe)
{
    b->bind_pipeline(b->user, pipe);
    b->draw(b->user, RSX_TOPOLOGY_TRIANGLES, FULL, 3, sizeof(vtx), NULL, 0);
}

static int open_device(void)
{
    const char* v = getenv("D3D12_ENGINE_TEST_VALIDATION");
    if (v && *v == '1') {
        ID3D12Debug* dbg = NULL;
        if (FAILED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&dbg))) { fprintf(stderr, "SKIP: no debug layer\n"); return 2; }
        dbg->lpVtbl->EnableDebugLayer(dbg); dbg->lpVtbl->Release(dbg);
    }
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&s_d3d.device))) {
        fprintf(stderr, "SKIP: no D3D12 device\n"); return 2;
    }
    D3D12_COMMAND_QUEUE_DESC qd; memset(&qd, 0, sizeof qd); qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommandQueue(s_d3d.device, &qd, &IID_ID3D12CommandQueue, (void**)&s_d3d.cmd_queue)))
        return 2;
    return 0;
}
static int validation_errors(void)
{
    ID3D12InfoQueue* iq = NULL; int errors = 0;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) || !iq) return 0;
    const UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0; iq->lpVtbl->GetMessage(iq, i, NULL, &len);
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
        if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            fprintf(stderr, "[validation error] %s\n", m->pDescription); errors++;
        }
        free(m);
    }
    iq->lpVtbl->Release(iq);
    return errors;
}


/* A hidden window and flip-model swap chain on the test queue, so the
 * engine's present path runs for real. Returns 0 when none can be made (for
 * example a non-interactive session); the present checks then skip. */
static LRESULT CALLBACK test_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
static int open_swap_chain(u32 w, u32 h)
{
    WNDCLASSW wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = test_wndproc; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"e12_present_fixture"; RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"e12", WS_OVERLAPPEDWINDOW, 0, 0, (int)w, (int)h, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 0;
    IDXGIFactory4* f = NULL;
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void**)&f))) return 0;
    DXGI_SWAP_CHAIN_DESC1 d; memset(&d, 0, sizeof d);
    d.Width = w; d.Height = h; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = FRAME_COUNT; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = NULL;
    const HRESULT hr = f->lpVtbl->CreateSwapChainForHwnd(f, (IUnknown*)s_d3d.cmd_queue, hwnd, &d, NULL, NULL, &sc1);
    f->lpVtbl->Release(f);
    if (FAILED(hr) || !sc1) { fprintf(stderr, "     no swap chain here (0x%08lX)\n", (long)hr); return 0; }
    sc1->lpVtbl->QueryInterface(sc1, &IID_IDXGISwapChain3, (void**)&s_d3d.swap_chain); sc1->lpVtbl->Release(sc1);
    D3D12_DESCRIPTOR_HEAP_DESC hd; memset(&hd, 0, sizeof hd); hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = FRAME_COUNT;
    if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap))) return 0;
    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE c; s_d3d.rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_d3d.rtv_heap, &c);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (FAILED(s_d3d.swap_chain->lpVtbl->GetBuffer(s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]))) return 0;
        D3D12_CPU_DESCRIPTOR_HANDLE ci = c; ci.ptr += (SIZE_T)i * s_d3d.rtv_descriptor_size;
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_d3d.render_targets[i], NULL, ci);
    }
    s_d3d.width = w; s_d3d.height = h;
    return 1;
}
/* The native worker of the queue-order check: submits its reserved batch a
 * little later, while the engine thread waits on work held behind it. */
typedef struct { rsx_native_queue_order* qo; u64 token; const rsx_native_d3d12_submit* si; int rc; } order_job;
static DWORD WINAPI order_submit_later(void* p)
{
    order_job* job = (order_job*)p;
    Sleep(150);
    job->rc = job->qo->submit(job->qo->userdata, job->qo->lifetime, job->token, job->si, NULL);
    return 0;
}
int main(void)
{
    const int open = open_device();
    if (open) return open;
    InitializeCriticalSection(&s_e12.lock);
    const rsx_draw_backend* b = &s_d3d12_engine_backend;
    CHECK(b->init(b->user, 64, 64) == 0, "init");
    if (!s_e12.ready) return 1;

    const u32 rt = b->color_target_create(b->user, RSX_BE_FMT_R8G8B8A8, 64, 64, NULL, 0);
    CHECK(rt != 0, "colour target");
    const float red[4] = { 1, 0, 0, 1 };
    b->clear_color(b->user, rt, red);
    CHECK(px(b, rt, 5, 5) == 0xFF0000FFu, "clear reads back R,G,B,A");

    rsx_be_render_state rs = plain_state();
    rsx_vertex_layout_plan lay = layout2();
    const u32 pipe = b->pipeline_create(b->user, VS, PS, &rs, &lay, sizeof(vtx), RSX_BE_FMT_R8G8B8A8, 1);
    CHECK(pipe != 0, "pipeline from decompiler-shaped HLSL (VPConst with the viewport epilogue)");
    /* The engine's block: 512 transform constants, then the viewport epilogue
     * (scale, offset) the decompiled program's tail applies. */
    static float vc[514 * 4]; float fc[8] = { 0 };
    vc[512 * 4 + 0] = vc[512 * 4 + 1] = vc[512 * 4 + 2] = 1.0f;   /* vp_posscale; vp_posoffset stays 0 */
    vc[4] = 0.0f; vc[5] = 1.0f; vc[6] = 0.5f; vc[7] = 1.0f;      /* vp_c[1] */
    fc[0] = 1.0f; fc[1] = 1.0f; fc[2] = 1.0f; fc[3] = 1.0f;      /* fp_c[0] */
    b->bind_vs_constants(b->user, vc, sizeof vc);
    b->bind_ps_constants(b->user, fc, sizeof fc);
    b->bind_textures(b->user, NULL, NULL, 0);
    b->bind_vertex_textures(b->user, NULL, NULL, 0);
    b->bind_targets(b->user, &rt, 1, 0);
    draw_full(b, pipe);
    const u32 green = px(b, rt, 32, 32);
    CHECK((green & 0xFF) == 0 && ((green >> 8) & 0xFF) == 0xFF && ((green >> 16) & 0xFF) >= 0x7F &&
          ((green >> 16) & 0xFF) <= 0x80 && (green >> 24) == 0xFF, "draw colour from both constant blocks");

    /* Scissor: only the left half. */
    b->clear_color(b->user, rt, red);
    b->set_scissor(b->user, 0, 0, 32, 64);
    fc[0] = 0.0f;                                                  /* zero red channel */
    b->bind_ps_constants(b->user, fc, sizeof fc);
    draw_full(b, pipe);
    CHECK(px(b, rt, 10, 10) != 0xFF0000FFu && px(b, rt, 50, 10) == 0xFF0000FFu, "scissor bounds the draw");
    b->set_scissor(b->user, 0, 0, 64, 64);

    /* Indexed draw. */
    static const u32 idx[3] = { 2, 1, 0 };
    b->clear_color(b->user, rt, red);
    b->bind_pipeline(b->user, pipe);
    b->draw(b->user, RSX_TOPOLOGY_TRIANGLES, FULL, 3, sizeof(vtx), idx, 3);
    CHECK(px(b, rt, 32, 32) != 0xFF0000FFu, "indexed draw");

    /* Depth: draws at z 0.5; a target cleared to 0.25 rejects them under LEQUAL. */
    rsx_be_render_state rsd = plain_state(); rsd.depth_test = 1; rsd.depth_write = 1;
    const u32 pdepth = b->pipeline_create(b->user, VS, PS, &rsd, &lay, sizeof(vtx), RSX_BE_FMT_R8G8B8A8, 1);
    const u32 z = b->depth_target_create(b->user, 64, 64);
    CHECK(pdepth && z, "depth pipeline and target");
    b->clear_color(b->user, rt, red);
    b->clear_depth_stencil(b->user, z, RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 0.25f, 0);
    b->bind_targets(b->user, &rt, 1, z);
    draw_full(b, pdepth);
    CHECK(px(b, rt, 32, 32) == 0xFF0000FFu, "farther draw rejected by depth");
    b->clear_depth_stencil(b->user, z, RSX_BE_CLEAR_DEPTH, 1.0f, 0);
    draw_full(b, pdepth);
    CHECK(px(b, rt, 32, 32) != 0xFF0000FFu, "cleared depth accepts it");
    /* Depth readback: the cleared 1.0 outside the draw, the draw's 0.5 inside. */
    CHECK(b->depth_readback != NULL, "depth readback present");
    {
        float dv[4 * 3]; memset(dv, 0, sizeof dv);
        CHECK(b->depth_readback(b->user, z, 30, 31, 4, 3, dv, 4 * sizeof(float)) == 0, "depth readback succeeds");
        int all_half = 1; for (int i = 0; i < 12; i++) if (dv[i] < 0.499f || dv[i] > 0.501f) all_half = 0;
        CHECK(all_half, "depth readback reads the drawn depth 0.5 in a sub-rectangle");
        b->clear_depth_stencil(b->user, z, RSX_BE_CLEAR_DEPTH, 0.75f, 0);
        float corner = 0; CHECK(b->depth_readback(b->user, z, 63, 63, 1, 1, &corner, 4) == 0 && corner == 0.75f,
                                "depth readback sees a later clear (0.75)");
        CHECK(b->depth_readback(b->user, z, 60, 60, 8, 8, dv, 32) != 0, "out-of-bounds depth readback refused");
        draw_full(b, pdepth);   /* the target is still usable for depth after a readback */
        CHECK(b->depth_readback(b->user, z, 32, 32, 1, 1, &corner, 4) == 0 && corner > 0.499f && corner < 0.501f,
              "depth test and write still work after readback");
    }
    const u32 snap = b->depth_snapshot(b->user, z, 64, 64);
    CHECK(snap != 0, "depth snapshot");
    b->texture_release(b->user, snap);

    /* MRT. */
    const u32 rt2 = b->color_target_create(b->user, RSX_BE_FMT_R8G8B8A8, 64, 64, NULL, 0);
    const u32 pmrt = b->pipeline_create(b->user, VS, PS_MRT, &rs, &lay, sizeof(vtx), RSX_BE_FMT_R8G8B8A8, 2);
    const u32 pair[2] = { rt, rt2 };
    vc[4] = 1.0f; vc[5] = 0.0f; vc[6] = 0.0f; vc[7] = 1.0f;       /* red in c0, blue in c1 */
    b->bind_vs_constants(b->user, vc, sizeof vc);
    b->bind_targets(b->user, pair, 2, 0);
    draw_full(b, pmrt);
    CHECK(px(b, rt, 8, 8) == 0xFF0000FFu && px(b, rt2, 8, 8) == 0xFFFF0000u, "MRT writes both targets");

    /* Texture with a crossbar, sampled point-wise. Texel bytes R,G,B,A = 10,20,30,40. */
    u8 texel[4 * 4 * 4];
    for (int i = 0; i < 16; i++) { texel[4 * i] = 10; texel[4 * i + 1] = 20; texel[4 * i + 2] = 30; texel[4 * i + 3] = 40; }
    const u32 identity = 0xE4u;   /* TEXTURE_CONTROL1 for an identity A8R8G8B8 crossbar (see rsx_texture_layout.h) */
    const u32 tex = b->texture_create(b->user, RSX_BE_FMT_R8G8B8A8, 4, 4, 1, 1, identity, 0x85u);
    CHECK(tex != 0, "texture");
    b->texture_upload(b->user, tex, 0, 0, 4, 4, texel, 16, 4);
    const u32 ptex = b->pipeline_create(b->user, VS, PS_TEX, &rs, &lay, sizeof(vtx), RSX_BE_FMT_R8G8B8A8, 1);
    rsx_be_sampler_desc sd[RSX_BE_MAX_TEXTURES]; memset(sd, 0, sizeof sd);
    for (int u = 0; u < RSX_BE_MAX_TEXTURES; u++) { sd[u].wrap_s = sd[u].wrap_t = sd[u].wrap_r = 1; sd[u].max_lod = 0; }
    u32 units[RSX_BE_MAX_TEXTURES] = { tex };
    b->bind_targets(b->user, &rt, 1, 0);
    b->bind_textures(b->user, units, sd, 1u);
    draw_full(b, ptex);
    {
        u8 expect[4]; u8 sel[4];
        rsx_texture_component_remap(identity, 0x85u & 0x9Fu, sel);   /* A,R,G,B order */
        const u8 src[6] = { 10, 20, 30, 40, 0, 255 };
        expect[0] = src[sel[1]]; expect[1] = src[sel[2]]; expect[2] = src[sel[3]]; expect[3] = src[sel[0]];
        const u32 got = px(b, rt, 32, 32);
        const u32 want = expect[0] | (expect[1] << 8) | (expect[2] << 16) | ((u32)expect[3] << 24);
        if (got != want) fprintf(stderr, "     texel %08X want %08X\n", got, want);
        CHECK(got == want, "texture sampled through the crossbar mapping");
    }
    /* A surface view samples the colour target rt2 (blue). */
    const u32 view = b->surface_view(b->user, rt2, identity, 0x85u);
    CHECK(view != 0 && b->surface_view(b->user, rt2, identity, 0x85u) == view, "surface view, cached");
    units[0] = view;
    b->bind_textures(b->user, units, sd, 1u);
    draw_full(b, ptex);
    CHECK(px(b, rt, 32, 32) != 0xFF0000FFu, "surface view sampled");
    /* 0xAAE4: every output channel takes its remapped input (the low byte alone
     * selects constants), so these checks see the sampled colour itself. */
    const u32 pass_through = 0xAAE4u;
    const u32 live = b->surface_view(b->user, rt2, pass_through, 0x85u);
    units[0] = live;
    b->bind_textures(b->user, units, sd, 1u);
    draw_full(b, ptex);
    const u32 blue_sample = px(b, rt, 32, 32);
    CHECK(blue_sample != 0 && blue_sample != 0xFF0000FFu, "pass-through view samples the target's colour");

    /* color_snapshot: an immutable copy taken now, unaffected by later writes,
     * sampled through a view, and sampled while rendering into its source. */
    CHECK(b->color_snapshot != NULL, "colour snapshot present");
    const u32 snapc = b->color_snapshot(b->user, rt2);
    CHECK(snapc != 0, "colour snapshot");
    const float lime[4] = { 0, 1, 0, 1 };
    b->clear_color(b->user, rt2, lime);
    const u32 sview = b->surface_view(b->user, snapc, pass_through, 0x85u);
    CHECK(sview != 0, "view of a colour snapshot");
    units[0] = sview;
    b->bind_targets(b->user, &rt, 1, 0);
    b->bind_textures(b->user, units, sd, 1u);
    draw_full(b, ptex);
    CHECK(px(b, rt, 32, 32) == blue_sample, "snapshot keeps the contents at the time of the copy");
    units[0] = live;   /* the live view of rt2 sees the later clear */
    b->bind_textures(b->user, units, sd, 1u);
    draw_full(b, ptex);
    const u32 lime_sample = px(b, rt, 32, 32);
    CHECK(lime_sample != blue_sample, "live view sees the later write");
    {   /* feedback: render into rt2 while sampling a snapshot of rt2 */
        const u32 fb = b->color_snapshot(b->user, rt2);
        const u32 fbv = fb ? b->surface_view(b->user, fb, pass_through, 0x85u) : 0;
        CHECK(fb && fbv, "snapshot of a target about to be rendered");
        units[0] = fbv;
        b->bind_targets(b->user, &rt2, 1, 0);
        b->bind_textures(b->user, units, sd, 1u);
        draw_full(b, ptex);
        CHECK(px(b, rt2, 32, 32) == lime_sample, "draw into the target while sampling its snapshot");
        b->texture_release(b->user, fb);
        b->bind_targets(b->user, &rt, 1, 0);
    }
    b->texture_release(b->user, snapc);
    CHECK(s_e12o[sview].kind == E12_FREE, "releasing a snapshot releases its views");

    /* Depth as D24S8 words on the GPU. Left half drawn at 0.5, right half
     * cleared to 0.75: depth_store_z24 must equal depth_readback plus the
     * engine's conversion, and a z24 view must sample like a texture made
     * from the decoded words. */
    {
        const u32 zz = b->depth_target_create(b->user, 64, 64);
        b->clear_depth_stencil(b->user, zz, RSX_BE_CLEAR_DEPTH, 0.75f, 0);   /* LEQUAL lets 0.5 through */
        b->bind_targets(b->user, &rt, 1, zz);
        b->set_scissor(b->user, 0, 0, 32, 64);
        draw_full(b, pdepth);
        b->set_scissor(b->user, 0, 0, 64, 64);
        CHECK(b->depth_store_z24 && b->depth_z24_view, "z24 store and view present");
        static float df[64 * 64]; static u8 words[64 * 64 * 4], expect_words[64 * 64 * 4];
        CHECK(b->depth_readback(b->user, zz, 0, 0, 64, 64, df, 64 * 4) == 0, "readback for the z24 reference");
        for (int i = 0; i < 64 * 64; i++) {
            float d = df[i]; d = d > 0.0f ? d : 0.0f; d = d < 1.0f ? d : 1.0f;
            const u32 v = (u32)((double)d * 16777215.0 + 0.5);
            expect_words[4 * i] = (u8)(v >> 16); expect_words[4 * i + 1] = (u8)(v >> 8);
            expect_words[4 * i + 2] = (u8)v; expect_words[4 * i + 3] = 0;
        }
        memset(words, 0xA5, sizeof words);
        CHECK(b->depth_store_z24(b->user, zz, 64, 64, words, 64 * 4) == 0, "z24 store succeeds");
        CHECK(memcmp(words, expect_words, sizeof words) == 0, "z24 store equals readback plus the CPU conversion");
        CHECK(expect_words[0] == 0x80 && expect_words[4 * 63] == 0xBF, "reference holds both depths (0.5 and 0.75)");
        u8 part[10 * 3 * 4]; memset(part, 0xA5, sizeof part);
        CHECK(b->depth_store_z24(b->user, zz, 3, 3, part, 40) == 0 && memcmp(part, expect_words, 12) == 0 &&
              part[12] == 0xA5, "z24 store honours w, h and the destination pitch");
        /* View vs a texture of the decoded words (A,R,G,B = word bytes 0..3 -> RGBA8 R,G,B,A = 1,2,3,0). */
        const u32 zv = b->depth_z24_view(b->user, zz, 64, 64, 0xAAE4u, 0x85u);
        CHECK(zv != 0, "z24 view");
        static u8 decoded[64 * 64 * 4];
        for (int i = 0; i < 64 * 64; i++) {
            decoded[4 * i] = expect_words[4 * i + 1]; decoded[4 * i + 1] = expect_words[4 * i + 2];
            decoded[4 * i + 2] = expect_words[4 * i + 3]; decoded[4 * i + 3] = expect_words[4 * i];
        }
        const u32 ref = b->texture_create(b->user, RSX_BE_FMT_R8G8B8A8, 64, 64, 1, 1, 0xAAE4u, 0x85u);
        b->texture_upload(b->user, ref, 0, 0, 64, 64, decoded, 64 * 4, 64);
        b->bind_targets(b->user, &rt, 1, 0);
        u32 zunits[RSX_BE_MAX_TEXTURES] = { zv };
        b->bind_textures(b->user, zunits, sd, 1u); draw_full(b, ptex);
        const u32 left_v = px(b, rt, 8, 8), right_v = px(b, rt, 56, 8);
        zunits[0] = ref; b->bind_textures(b->user, zunits, sd, 1u); draw_full(b, ptex);
        const u32 left_r = px(b, rt, 8, 8), right_r = px(b, rt, 56, 8);
        if (left_v != left_r || right_v != right_r || left_v == right_v)
            fprintf(stderr, "     z24 view %08X %08X, texture %08X %08X\n", left_v, right_v, left_r, right_r);
        CHECK(left_v == left_r && right_v == right_r && left_v != right_v, "z24 view samples like the decoded texture");
        b->texture_release(b->user, zv); b->texture_release(b->user, ref);
        /* -0.0 (which saturate() can return) stores as 0, not as a huge
         * exponent. A clear may normalise -0.0, so the depth plane is filled
         * with the exact bit pattern 0x80000000 by a buffer copy. */
        {
            e12_obj* zo = e12_get(zz, E12_DEPTH);
            const u32 pitch = 256;   /* 64 floats, already 256-byte aligned */
            D3D12_RESOURCE_DESC bd; memset(&bd, 0, sizeof bd);
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (u64)pitch * 64; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* up = e12_committed(D3D12_HEAP_TYPE_UPLOAD, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL);
            u32* mp = NULL; D3D12_RANGE none = { 0, 0 };
            CHECK(zo && up && SUCCEEDED(up->lpVtbl->Map(up, 0, &none, (void**)&mp)), "negative-zero source buffer");
            for (u32 i = 0; i < 64u * 64u; i++) mp[i] = 0x80000000u;
            up->lpVtbl->Unmap(up, 0, NULL);
            e12_begin();
            e12_transition(zo, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dl, sl; memset(&dl, 0, sizeof dl); memset(&sl, 0, sizeof sl);
            dl.pResource = zo->res; dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
            sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sl.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_TYPELESS; sl.PlacedFootprint.Footprint.Width = 64;
            sl.PlacedFootprint.Footprint.Height = 64; sl.PlacedFootprint.Footprint.Depth = 1; sl.PlacedFootprint.Footprint.RowPitch = pitch;
            s_e12.cl->lpVtbl->CopyTextureRegion(s_e12.cl, &dl, 0, 0, 0, &sl, NULL);
            e12_transition(zo, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
            up->lpVtbl->Release(up);
            float nz = 1.0f; u32 nzb = 0;
            CHECK(b->depth_readback(b->user, zz, 0, 0, 1, 1, &nz, 4) == 0, "negative-zero readback");
            memcpy(&nzb, &nz, 4);
            CHECK(nzb == 0x80000000u, "the depth plane holds -0.0 exactly");
        }
        memset(words, 0xA5, 16);
        CHECK(b->depth_store_z24(b->user, zz, 2, 2, words, 8) == 0 &&
              !words[0] && !words[1] && !words[2] && !words[3] && !words[12] && !words[15], "-0.0 depth stores as zero");
        b->depth_target_release(b->user, zz);
    }

    /* Depth bounds: left half of the depth at 0.5, right half at 0.75; with
     * bounds [0.4, 0.6] a full-screen draw only lands on the left half. */
    {
        CHECK(b->set_depth_bounds != NULL, "depth bounds entry point present");
        const u32 zb = b->depth_target_create(b->user, 64, 64);
        b->clear_depth_stencil(b->user, zb, RSX_BE_CLEAR_DEPTH, 0.75f, 0);
        b->bind_targets(b->user, &rt, 1, zb);
        b->set_scissor(b->user, 0, 0, 32, 64);
        draw_full(b, pdepth);
        b->set_scissor(b->user, 0, 0, 64, 64);
        rsx_be_render_state rsb = plain_state(); rsb.depth_bounds_test = 1;
        const u32 pbounds = b->pipeline_create(b->user, VS, PS, &rsb, &lay, sizeof(vtx), RSX_BE_FMT_R8G8B8A8, 1);
        CHECK(pbounds != 0, "depth-bounds pipeline");
        b->clear_color(b->user, rt, red);
        b->set_depth_bounds(b->user, 0.4f, 0.6f);
        draw_full(b, pbounds);
        const u32 l = px(b, rt, 8, 8), r = px(b, rt, 56, 8);
        CHECK(l != 0xFF0000FFu && r == 0xFF0000FFu, "bounds [0.4, 0.6] pass 0.5 and reject 0.75");
        b->clear_color(b->user, rt, red);
        b->set_depth_bounds(b->user, 0.0f, 1.0f);
        draw_full(b, pbounds);
        CHECK(px(b, rt, 8, 8) != 0xFF0000FFu && px(b, rt, 56, 8) != 0xFF0000FFu, "bounds [0, 1] pass both");
        b->clear_color(b->user, rt, red);
        b->set_depth_bounds(b->user, NAN, 0.6f);   /* NaN opens its end: [0, 0.6] */
        draw_full(b, pbounds);
        CHECK(px(b, rt, 8, 8) != 0xFF0000FFu && px(b, rt, 56, 8) == 0xFF0000FFu, "NaN minimum opens the range");
        b->clear_color(b->user, rt, red);
        b->set_depth_bounds(b->user, 0.4f, NAN);   /* [0.4, 1] */
        draw_full(b, pbounds);
        CHECK(px(b, rt, 8, 8) != 0xFF0000FFu && px(b, rt, 56, 8) != 0xFF0000FFu, "NaN maximum opens the range");
        b->clear_color(b->user, rt, red);
        b->clear_depth_stencil(b->user, zb, RSX_BE_CLEAR_DEPTH, 1.0f, 0);   /* depth exactly 1 everywhere */
        const u64 skipped = s_e12.bounds_empty_draws;
        b->set_depth_bounds(b->user, 0.7f, 0.3f);  /* inverted: nothing passes, not even depth 1 */
        draw_full(b, pbounds);
        CHECK(px(b, rt, 8, 8) == 0xFF0000FFu && px(b, rt, 56, 8) == 0xFF0000FFu &&
              s_e12.bounds_empty_draws == skipped + 1, "inverted range draws nothing");
        b->set_depth_bounds(b->user, 0.0f, 1.0f);
        CHECK(s_e12.count[E12_X_DEPTH_BOUNDS_UNSUPPORTED] == 0, "no depth-bounds fallback counted");
        b->pipeline_release(b->user, pbounds);
        b->bind_targets(b->user, &rt, 1, 0);
        b->depth_target_release(b->user, zb);
    }

    /* color_target_write at an offset. */
    u8 rows[2 * 2 * 4]; for (int i = 0; i < 4; i++) { rows[4 * i] = 1; rows[4 * i + 1] = 2; rows[4 * i + 2] = 3; rows[4 * i + 3] = 4; }
    b->color_target_write(b->user, rt, 10, 20, 2, 2, rows, 8);
    CHECK(px(b, rt, 11, 21) == 0x04030201u, "colour target write at an offset");

    /* Refusals are counted. */
    const u64 before = s_e12.count[E12_X_DRAW_TARGETS];
    b->bind_targets(b->user, pair, 2, 0);
    draw_full(b, pipe);                      /* one-target pipeline, two targets */
    CHECK(s_e12.count[E12_X_DRAW_TARGETS] == before + 1, "mismatched target count refused and counted");

    /* Release and reuse: handles come back; resources retire on the fence. */
    b->texture_release(b->user, tex);
    b->color_target_release(b->user, rt2);
    const u32 again = b->color_target_create(b->user, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
    CHECK(again != 0, "handle reuse after release");
    b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    CHECK(s_e12.retire_n == 0, "retired resources released after the fence");
    CHECK(b->submit_and_wait_checked(b->user, RSX_BE_FLUSH_GUEST_REFERENCE) == 1, "checked flush");

    /* Async submission: a ticket per submission, genuine completion status,
     * resources retained until the GPU passes it, allocators reused. */
    CHECK(b->submit_async && b->completion_status, "async entry points present");
    CHECK(b->completion_status(b->user, 0, 0) == -1, "ticket 0 is unknown");
    CHECK(b->completion_status(b->user, s_e12.fence_value + 100, 0) == -1, "future ticket is unknown");
    const float blue[4] = { 0, 0, 1, 1 };
    b->bind_targets(b->user, &rt, 1, 0);
    b->clear_color(b->user, rt, blue);
    const u32 doomed = b->texture_create(b->user, RSX_BE_FMT_R8G8B8A8, 4, 4, 1, 1, identity, 0x85u);
    b->texture_release(b->user, doomed);     /* released while the submission is pending */
    const u64 t1 = b->submit_async(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    CHECK(t1 != 0, "async ticket");
    CHECK(s_e12.retire_n >= 1, "released texture retained until the ticket completes");
    CHECK(b->completion_status(b->user, t1, 1) == 1, "waited ticket completes");
    CHECK(s_e12.retire_n == 0, "retired after completion");
    CHECK(px(b, rt, 3, 3) == 0xFFFF0000u, "async work visible after completion");
    u64 last = 0, first = 0; int polled_ok = 1;
    for (int i = 0; i < 3 * ENG12_ALLOCATORS; i++) {
        draw_full(b, pipe);
        const u64 t = b->submit_async(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        if (!first) first = t;
        if (!t || t <= last) polled_ok = 0;
        const int st = b->completion_status(b->user, t, 0);
        if (st != 0 && st != 1) polled_ok = 0;
        last = t;
    }
    CHECK(polled_ok, "tickets increase and poll as pending or complete");
    CHECK(b->completion_status(b->user, last, 1) == 1, "last of many async submissions completes");
    CHECK(b->completion_status(b->user, first, 0) == 1, "earlier ticket complete once a later one is");
    CHECK(s_e12.count[E12_X_ASYNC_REFUSED] == 0, "no async refusal");
    CHECK(b->submit_and_wait_checked(b->user, RSX_BE_FLUSH_GUEST_REFERENCE) == 1, "checked flush after async");
    /* The shader-visible heap holds a whole frame of descriptor tables: 4000
     * draws (20 entries each, more than the old 65536-entry heap) need no wait. */
    CHECK(s_e12.srv_gpu_cap == ENG12_SRV_GPU_HEAP_MAX, "large shader-visible heap");
    {
        const u64 before = s_e12.waits[E12_W_SRV_TABLES];
        b->bind_targets(b->user, &rt, 1, 0);
        for (int i = 0; i < 4000; i++) draw_full(b, pipe);
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        CHECK(s_e12.waits[E12_W_SRV_TABLES] == before, "4000 draws without a descriptor-ring wait");
    }
    /* Fenced rings: shrink both rings so many async batches must wrap them.
     * Space returns as the GPU passes each batch's fence; no full wait is
     * needed, and the drawing stays correct. */
    {
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        const u64 keep_srv = s_e12.srv_ring.size, keep_up = s_e12.upload_ring.size;
        s_e12.srv_ring.size = 2000;            /* 100 draws of 20 descriptors */
        s_e12.upload_ring.size = 4u << 20;     /* a few batches of constants and vertices */
        const u64 flushes = s_e12.flushes;
        b->bind_targets(b->user, &rt, 1, 0);
        int ok = 1;
        for (int batch = 0; batch < 40; batch++) {
            for (int i = 0; i < 60; i++) draw_full(b, pipe);
            if (!b->submit_async(b->user, RSX_BE_FLUSH_GUEST_REFERENCE)) ok = 0;
        }
        CHECK(ok, "40 async batches over small rings");
        if (s_e12.flushes != flushes) fprintf(stderr, "     full waits %llu, up head %llu tail %llu segs %u\n",
            (unsigned long long)(s_e12.flushes - flushes), (unsigned long long)s_e12.upload_ring.head,
            (unsigned long long)s_e12.upload_ring.tail, s_e12.upload_ring.nseg);
        CHECK(s_e12.flushes == flushes, "rings wrapped without a full GPU wait");
        CHECK(s_e12.srv_ring.first + s_e12.srv_ring.nseg > 1 || s_e12.waits[E12_W_SRV_TABLES] > 0,
              "the descriptor ring did wrap");
        CHECK(s_e12.srv_ring.head <= s_e12.srv_ring.size && s_e12.upload_ring.head <= s_e12.upload_ring.size,
              "ring heads stay inside the rings");
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        CHECK(px(b, rt, 32, 32) != 0xFF0000FFu, "drawing through wrapped rings is correct");
        s_e12.srv_ring.size = keep_srv; s_e12.upload_ring.size = keep_up;
    }
    /* Rings barely larger than one draw: every draw must make room first,
     * and that wait or flush must happen before any of its allocations. */
    {
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        const u64 keep_srv = s_e12.srv_ring.size, keep_up = s_e12.upload_ring.size;
        s_e12.srv_ring.size = 30; s_e12.upload_ring.size = 24u << 10;
        const u64 rw = s_e12.waits[E12_W_UPLOAD_RING] + s_e12.waits[E12_W_SRV_TABLES];
        b->clear_color(b->user, rt, red);
        b->bind_targets(b->user, &rt, 1, 0);
        for (int i = 0; i < 20; i++) draw_full(b, pipe);
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        CHECK(s_e12.waits[E12_W_UPLOAD_RING] + s_e12.waits[E12_W_SRV_TABLES] > rw, "tiny rings make draws wait");
        CHECK(px(b, rt, 32, 32) != 0xFF0000FFu && !s_e12.failed, "draws that waited for ring space are intact");
        s_e12.srv_ring.size = keep_srv; s_e12.upload_ring.size = keep_up;
    }
    /* Present: by default every present waits; with async present (opt-in)
     * presents only wait when the GPU is a whole swap chain behind. */
    if (open_swap_chain(64, 64)) {
        b->bind_targets(b->user, &rt, 1, 0);
        const u64 w0 = s_e12.waits[E12_W_PRESENT], p0 = s_e12.presents;
        for (int i = 0; i < 6; i++) { draw_full(b, pipe); b->present(b->user, rt); }
        CHECK(s_e12.presents == p0 + 6 && s_e12.waits[E12_W_PRESENT] == w0 + 6, "default present waits every frame");
        s_e12.async_present = 1;
        const u64 w1 = s_e12.waits[E12_W_PRESENT], f1 = s_e12.flushes, fv = s_e12.fence_value;
        for (int i = 0; i < 30; i++) { for (int k = 0; k < 50; k++) draw_full(b, pipe); b->present(b->user, rt); }
        CHECK(s_e12.fence_value == fv + 30, "async present submits once per frame");
        CHECK(s_e12.waits[E12_W_PRESENT] - w1 < 30, "async present does not wait every frame");
        CHECK(s_e12.backbuffer_fence[0] || s_e12.backbuffer_fence[1], "back buffers carry their frame's fence");
        (void)f1;
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        CHECK(px(b, rt, 32, 32) != 0xFF0000FFu && !s_e12.failed, "frames after async presents are intact");
        s_e12.async_present = 0;
    } else fprintf(stderr, "SKIP present checks: no swap chain in this session\n");
    CHECK(s_e12.waits[E12_W_ENGINE_CHECKED] >= 2 && s_e12.waits[E12_W_COLOR_READBACK] > 0 &&
          s_e12.waits[E12_W_DEPTH_READBACK] > 0, "waits are counted by site");

    /* Native-owner seam (PS3RECOMP_RSX_NATIVE_OWNER=1): off by default; when on,
     * a native consumer gets the device and queue only at a clean boundary,
     * after retain, for exactly one lease, and its own work on the queue does
     * not disturb the engine. */
    {
        rsx_native_owner o; memset(&o, 0, sizeof o); o.abi_version = 1; o.struct_size = sizeof o;
        CHECK(rsx_d3d12_backend_borrow_owner(&o) == -2, "owner seam off by default");
        atomic_store(&s_e12_owner_setting, 1);
        s_e12.active = 1;
        CHECK(rsx_d3d12_backend_borrow_owner(&o) == 0 && o.backend == RSX_NATIVE_OWNER_D3D12 &&
              o.device == (void*)s_d3d.device && o.queue == (void*)s_d3d.cmd_queue && o.lifetime != 0,
              "borrow names the D3D12 device and queue");
        rsx_native_owner_v2 o2; memset(&o2, 0, sizeof o2); o2.abi_version = 2; o2.struct_size = sizeof o2;
        o2.base.abi_version = 1; o2.base.struct_size = sizeof o2.base;
        CHECK(rsx_d3d12_backend_borrow_owner_v2(&o2) == 0 && o2.base.lifetime == o.lifetime &&
              (o2.enabled_features & RSX_NATIVE_OWNER_FEATURE_DEPTH_BOUNDS) == (s_e12.depth_bounds_ok ? RSX_NATIVE_OWNER_FEATURE_DEPTH_BOUNDS : 0u),
              "v2 reports the enabled depth-bounds feature");
        CHECK(o.try_begin(o.userdata, o.lifetime) == -3, "no lease before retain");
        CHECK(o.retain(o.userdata, o.lifetime) == 0, "retain");
        b->bind_targets(b->user, &rt, 1, 0);
        draw_full(b, pipe);                                    /* recorded, not submitted */
        CHECK(o.try_begin(o.userdata, o.lifetime) == -4, "no lease while engine work is unsubmitted");
        b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        CHECK(consumer_depth == 0, "consumer lease balanced around original work");
        CHECK(o.try_begin(o.userdata, o.lifetime) == 0, "lease at a clean boundary");
        {   /* native work on the borrowed queue: its own target, list, allocator and fence */
            ID3D12Device* dv = (ID3D12Device*)o.device; ID3D12CommandQueue* q = (ID3D12CommandQueue*)o.queue;
            D3D12_RESOURCE_DESC td = e12_tex_desc(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            ID3D12Resource* nt = e12_committed(D3D12_HEAP_TYPE_DEFAULT, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
            ID3D12DescriptorHeap* nh = NULL; D3D12_DESCRIPTOR_HEAP_DESC hd; memset(&hd, 0, sizeof hd);
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
            ID3D12CommandAllocator* na = NULL; ID3D12GraphicsCommandList* nl = NULL; ID3D12Fence* nf = NULL;
            const int made = nt && SUCCEEDED(dv->lpVtbl->CreateDescriptorHeap(dv, &hd, &IID_ID3D12DescriptorHeap, (void**)&nh)) &&
                SUCCEEDED(dv->lpVtbl->CreateCommandAllocator(dv, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&na)) &&
                SUCCEEDED(dv->lpVtbl->CreateCommandList(dv, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, na, NULL, &IID_ID3D12GraphicsCommandList, (void**)&nl)) &&
                SUCCEEDED(dv->lpVtbl->CreateFence(dv, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&nf));
            CHECK(made, "native resources on the borrowed device");
            if (made) {
                D3D12_CPU_DESCRIPTOR_HANDLE c; nh->lpVtbl->GetCPUDescriptorHandleForHeapStart(nh, &c);
                dv->lpVtbl->CreateRenderTargetView(dv, nt, NULL, c);
                const float green[4] = { 0, 1, 0, 1 };
                nl->lpVtbl->ClearRenderTargetView(nl, c, green, 0, NULL);
                nl->lpVtbl->Close(nl);
                ID3D12CommandList* lists[1] = { (ID3D12CommandList*)nl };
                q->lpVtbl->ExecuteCommandLists(q, 1, lists);
                q->lpVtbl->Signal(q, nf, 1);
            }
            o.end(o.userdata, o.lifetime);
            if (made) {   /* the native consumer drains its own fence before releasing its resources */
                HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
                nf->lpVtbl->SetEventOnCompletion(nf, 1, ev); WaitForSingleObject(ev, 5000); CloseHandle(ev);
                CHECK(nf->lpVtbl->GetCompletedValue(nf) == 1, "native submission completed");
            }
            if (nf) nf->lpVtbl->Release(nf); if (nl) nl->lpVtbl->Release(nl); if (na) na->lpVtbl->Release(na);
            if (nh) nh->lpVtbl->Release(nh); if (nt) nt->lpVtbl->Release(nt);
        }
        CHECK(o.try_begin(o.userdata, o.lifetime) == 0, "a second lease at the same boundary");
        o.end(o.userdata, o.lifetime);
        b->clear_color(b->user, rt, red);
        b->bind_targets(b->user, &rt, 1, 0);
        draw_full(b, pipe);
        CHECK(px(b, rt, 32, 32) != 0xFF0000FFu && !s_e12_owner.failed && !s_e12.failed, "engine continues after native leases");
        /* Image leases on the engine's own targets: borrow names the resource
         * and its contract state, the boundary moves it there and submits,
         * native work draws into it, and the engine sees that work after it.
         * A retained image outlives the engine freeing the target but no
         * longer validates; a reused handle is a new generation. */
        {
            const u32 dz = b->depth_target_create(b->user, 64, 64);
            rsx_native_image ci, zi, bad;
            memset(&ci, 0, sizeof ci); ci.abi_version = RSX_NATIVE_IMAGE_ABI; ci.struct_size = sizeof ci; zi = ci; bad = ci;
            const rsx_native_d3d12_image* cp = NULL; const rsx_native_d3d12_image* zp = NULL;
            CHECK(rsx_d3d12_backend_borrow_image(o.lifetime, rt, &ci) == 0 && (cp = (const rsx_native_d3d12_image*)ci.image) &&
                  ci.backend == RSX_NATIVE_OWNER_D3D12 && ci.format == RSX_NATIVE_IMAGE_RGBA8 && ci.width == 64 &&
                  ci.height == 64 && ci.resource_id == rt && ci.resource_generation && cp->resource == (void*)s_e12o[rt].res &&
                  cp->abi_version == RSX_NATIVE_D3D12_IMAGE_ABI && cp->acquire_state == D3D12_RESOURCE_STATE_RENDER_TARGET &&
                  cp->handback_state == D3D12_RESOURCE_STATE_RENDER_TARGET,
                  "borrow a colour target: its resource and the RENDER_TARGET contract");
            CHECK(dz && rsx_d3d12_backend_borrow_image(o.lifetime, dz, &zi) == 0 && (zp = (const rsx_native_d3d12_image*)zi.image) &&
                  zi.format == RSX_NATIVE_IMAGE_D32S8 && zp->acquire_state == D3D12_RESOURCE_STATE_DEPTH_WRITE &&
                  zp->handback_state == D3D12_RESOURCE_STATE_DEPTH_WRITE, "borrow a depth target: the DEPTH_WRITE contract");
            CHECK(rsx_d3d12_backend_borrow_image(o.lifetime, pipe, &bad) == -3 &&
                  rsx_d3d12_backend_borrow_image(o.lifetime + 1, rt, &bad) == -3 &&
                  rsx_d3d12_backend_borrow_image(o.lifetime, 0, &bad) == -3, "borrow refuses non-targets and a stale lifetime");
            CHECK(ci.retain(ci.userdata, ci.owner_lifetime, ci.resource_id, ci.resource_generation) == 0 &&
                  zi.retain(zi.userdata, zi.owner_lifetime, zi.resource_id, zi.resource_generation) == 0, "retain both images");
            CHECK(ci.validate(ci.userdata, ci.owner_lifetime, ci.resource_id, ci.resource_generation) == -4,
                  "validate only under the native lease");
            (void)px(b, rt, 1, 1);   /* the engine leaves the target as a copy source */
            u64 ticket = 0;
            CHECK(rsx_d3d12_backend_native_boundary(&ticket) == -3 && ticket == 0,
                  "the public boundary asks the common engine (inactive in this fixture)");
            CHECK(e12_native_boundary(0, &ticket) == 0 && ticket && ticket == s_e12.fence_value && !s_e12.recording &&
                  s_e12o[rt].state == D3D12_RESOURCE_STATE_RENDER_TARGET && s_e12o[dz].state == D3D12_RESOURCE_STATE_DEPTH_WRITE,
                  "boundary submits with borrowed images in their contract state");
            CHECK(o.try_begin(o.userdata, o.lifetime) == 0, "lease after the boundary");
            CHECK(ci.validate(ci.userdata, ci.owner_lifetime, ci.resource_id, ci.resource_generation) == 0 &&
                  zi.validate(zi.userdata, zi.owner_lifetime, zi.resource_id, zi.resource_generation) == 0,
                  "both images validate under the lease");
            CHECK(rsx_d3d12_backend_validate_image_planes(ci.userdata, ci.owner_lifetime, ci.resource_id, ci.resource_generation, 1) == -3,
                  "clear-plane evidence comes from the common engine (inactive here)");
            ID3D12Device* dv = (ID3D12Device*)o.device; ID3D12CommandQueue* q = (ID3D12CommandQueue*)o.queue;
            ID3D12DescriptorHeap *rh = NULL, *dh = NULL; ID3D12CommandAllocator* na = NULL;
            ID3D12GraphicsCommandList* nl = NULL; ID3D12Fence* nf = NULL;
            D3D12_DESCRIPTOR_HEAP_DESC rd; memset(&rd, 0, sizeof rd); rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rd.NumDescriptors = 1;
            D3D12_DESCRIPTOR_HEAP_DESC dd = rd; dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            const int made = cp && zp &&
                SUCCEEDED(dv->lpVtbl->CreateDescriptorHeap(dv, &rd, &IID_ID3D12DescriptorHeap, (void**)&rh)) &&
                SUCCEEDED(dv->lpVtbl->CreateDescriptorHeap(dv, &dd, &IID_ID3D12DescriptorHeap, (void**)&dh)) &&
                SUCCEEDED(dv->lpVtbl->CreateCommandAllocator(dv, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&na)) &&
                SUCCEEDED(dv->lpVtbl->CreateCommandList(dv, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, na, NULL, &IID_ID3D12GraphicsCommandList, (void**)&nl)) &&
                SUCCEEDED(dv->lpVtbl->CreateFence(dv, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&nf));
            CHECK(made, "native views and list on the borrowed device");
            if (made) {   /* acquired and handed back in the contract states: no barriers needed */
                D3D12_CPU_DESCRIPTOR_HANDLE rv, zv;
                rh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rh, &rv); dh->lpVtbl->GetCPUDescriptorHandleForHeapStart(dh, &zv);
                dv->lpVtbl->CreateRenderTargetView(dv, (ID3D12Resource*)cp->resource, NULL, rv);
                D3D12_DEPTH_STENCIL_VIEW_DESC zd; memset(&zd, 0, sizeof zd);
                zd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; zd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dv->lpVtbl->CreateDepthStencilView(dv, (ID3D12Resource*)zp->resource, &zd, zv);
                const float native_blue[4] = { 0, 0, 1, 1 };
                nl->lpVtbl->ClearRenderTargetView(nl, rv, native_blue, 0, NULL);
                nl->lpVtbl->ClearDepthStencilView(nl, zv, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, NULL);
                nl->lpVtbl->Close(nl);
                ID3D12CommandList* lists[1] = { (ID3D12CommandList*)nl };
                q->lpVtbl->ExecuteCommandLists(q, 1, lists);
                q->lpVtbl->Signal(q, nf, 1);
            }
            o.end(o.userdata, o.lifetime);
            float zval = 0;
            CHECK(made && px(b, rt, 7, 9) == 0xFFFF0000u, "engine reads the native colour work queued after the boundary");
            CHECK(made && b->depth_readback(b->user, dz, 3, 4, 1, 1, &zval, 4) == 0 && zval == 0.25f,
                  "engine reads the native depth work queued after the boundary");
            if (made) {
                HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
                nf->lpVtbl->SetEventOnCompletion(nf, 1, ev); WaitForSingleObject(ev, 5000); CloseHandle(ev);
            }
            if (nf) nf->lpVtbl->Release(nf); if (nl) nl->lpVtbl->Release(nl); if (na) na->lpVtbl->Release(na);
            if (dh) dh->lpVtbl->Release(dh); if (rh) rh->lpVtbl->Release(rh);
            ID3D12Resource* kept = cp ? (ID3D12Resource*)cp->resource : NULL;
            b->color_target_release(b->user, rt);
            b->submit_and_wait(b->user, RSX_BE_FLUSH_GUEST_REFERENCE);   /* the engine's fenced retirement runs */
            ULONG refs = 0;
            if (kept) { refs = kept->lpVtbl->AddRef(kept); kept->lpVtbl->Release(kept); }
            CHECK(refs == 2, "a retained image keeps the resource after the engine frees the target");
            CHECK(e12_native_boundary(0, &ticket) == 0 && o.try_begin(o.userdata, o.lifetime) == 0 &&
                  ci.validate(ci.userdata, ci.owner_lifetime, ci.resource_id, ci.resource_generation) == -3 &&
                  zi.validate(zi.userdata, zi.owner_lifetime, zi.resource_id, zi.resource_generation) == 0,
                  "a freed target no longer validates; the other still does");
            o.end(o.userdata, o.lifetime);
            CHECK(ci.retain(ci.userdata, ci.owner_lifetime, ci.resource_id, ci.resource_generation) == -3,
                  "a freed target cannot be retained again");
            const u32 rt2 = b->color_target_create(b->user, RSX_BE_FMT_R8G8B8A8, 64, 64, NULL, 0);
            rsx_native_image c2 = bad;
            CHECK(rt2 && rsx_d3d12_backend_borrow_image(o.lifetime, rt2, &c2) == 0 && c2.resource_generation != ci.resource_generation &&
                  (rt2 != rt || ci.retain(ci.userdata, ci.owner_lifetime, rt2, ci.resource_generation) == -3),
                  "a new target in a reused handle is a new generation");
            ci.release(ci.userdata, ci.owner_lifetime, ci.resource_id, ci.resource_generation);
            zi.release(zi.userdata, zi.owner_lifetime, zi.resource_id, zi.resource_generation);
            CHECK(s_e12_owner.retained == 1 && !e12_image_find(ci.resource_id, ci.resource_generation),
                  "image releases balance and the freed target's record goes");
            CHECK(s_e12_image_stats.borrows == 3 && s_e12_image_stats.retains == 2 && s_e12_image_stats.boundaries == 2 &&
                  s_e12_image_stats.transitions >= 1, "image leases counted");
            b->color_target_release(b->user, rt2); b->depth_target_release(b->user, dz);
        }
        /* Reserved queue order: engine work submitted after a reservation
         * reaches the queue only after the native batch, so a native clear
         * recorded first and submitted last still runs before the engine's
         * clear. A wait on held work blocks until the native submit, made
         * here from another thread; abandoning releases the hold the same way. */
        {
            rsx_native_queue_order qo; memset(&qo, 0, sizeof qo);
            qo.abi_version = RSX_NATIVE_QUEUE_ORDER_ABI; qo.struct_size = sizeof qo;
            CHECK(rsx_d3d12_backend_queue_order(&qo) == 0 && qo.lifetime == o.lifetime && qo.reserve && qo.submit,
                  "queue order available with the owner seam");
            u64 token = 0, other = 0, ticket = 0;
            CHECK(qo.reserve(qo.userdata, qo.lifetime, &token) == -4 && !token, "reserve only under the native lease");
            const u32 rq = b->color_target_create(b->user, RSX_BE_FMT_R8G8B8A8, 64, 64, NULL, 0);
            rsx_native_image qi; memset(&qi, 0, sizeof qi); qi.abi_version = RSX_NATIVE_IMAGE_ABI; qi.struct_size = sizeof qi;
            CHECK(rq && rsx_d3d12_backend_borrow_image(o.lifetime, rq, &qi) == 0 &&
                  qi.retain(qi.userdata, qi.owner_lifetime, qi.resource_id, qi.resource_generation) == 0, "borrow and retain a target");
            CHECK(e12_native_boundary(0, &ticket) == 0 && o.try_begin(o.userdata, o.lifetime) == 0 &&
                  qo.reserve(qo.userdata, qo.lifetime, &token) == 0 && token, "reserve under the lease at the boundary");
            CHECK(qo.reserve(qo.userdata, qo.lifetime, &other) == -4, "one reservation at a time");
            ID3D12Device* dv = (ID3D12Device*)o.device;
            ID3D12DescriptorHeap* rh = NULL; ID3D12CommandAllocator* na = NULL; ID3D12GraphicsCommandList* nl = NULL; ID3D12Fence* nf = NULL;
            D3D12_DESCRIPTOR_HEAP_DESC rd; memset(&rd, 0, sizeof rd); rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rd.NumDescriptors = 1;
            const int made = qi.image &&
                SUCCEEDED(dv->lpVtbl->CreateDescriptorHeap(dv, &rd, &IID_ID3D12DescriptorHeap, (void**)&rh)) &&
                SUCCEEDED(dv->lpVtbl->CreateCommandAllocator(dv, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&na)) &&
                SUCCEEDED(dv->lpVtbl->CreateCommandList(dv, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, na, NULL, &IID_ID3D12GraphicsCommandList, (void**)&nl)) &&
                SUCCEEDED(dv->lpVtbl->CreateFence(dv, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&nf));
            CHECK(made, "native list for the reserved place");
            if (made) {
                D3D12_CPU_DESCRIPTOR_HANDLE rv; rh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rh, &rv);
                dv->lpVtbl->CreateRenderTargetView(dv, (ID3D12Resource*)((const rsx_native_d3d12_image*)qi.image)->resource, NULL, rv);
                const float native_blue[4] = { 0, 0, 1, 1 };
                nl->lpVtbl->ClearRenderTargetView(nl, rv, native_blue, 0, NULL);
                nl->lpVtbl->Close(nl);
            }
            o.end(o.userdata, o.lifetime);
            CHECK(o.try_begin(o.userdata, o.lifetime) == -4, "no lease while a reservation holds the queue");
            const float engine_red[4] = { 1, 0, 0, 1 };
            b->clear_color(b->user, rq, engine_red);
            const u64 held = b->submit_async(b->user, 0);
            CHECK(held && b->completion_status(b->user, held, 0) == 0 && s_e12_hold.n >= 1 && s_e12_hold.queued < held,
                  "engine work after the reservation is held, not queued");
            static void* lists[1]; lists[0] = nl;
            static rsx_native_d3d12_submit si;
            si = (rsx_native_d3d12_submit){ .abi_version = RSX_NATIVE_D3D12_SUBMIT_ABI, .struct_size = sizeof si,
                                            .lists = lists, .list_count = made ? 1u : 0u, .signal_fence = nf, .signal_value = 1 };
            order_job job = { &qo, token, &si, 99 };
            HANDLE worker = CreateThread(NULL, 0, order_submit_later, &job, 0, NULL);
            CHECK(worker && b->completion_status(b->user, held, 1) == 1, "a wait on held work returns once the native batch is submitted");
            if (worker) { WaitForSingleObject(worker, 10000); CloseHandle(worker); }
            CHECK(job.rc == 0 && !s_e12_hold.active && (!nf || nf->lpVtbl->GetCompletedValue(nf) == 1), "native submit from another thread");
            CHECK(made && px(b, rq, 10, 10) == 0xFF0000FFu, "the native batch ran before the engine work held behind it");
            CHECK(e12_native_boundary(0, &ticket) == 0 && o.try_begin(o.userdata, o.lifetime) == 0 &&
                  qo.reserve(qo.userdata, qo.lifetime, &token) == 0, "a second reservation");
            o.end(o.userdata, o.lifetime);
            b->clear_color(b->user, rq, engine_red);
            const u64 held2 = b->submit_async(b->user, 0);
            CHECK(qo.submit(qo.userdata, qo.lifetime, token + 1, NULL, NULL) == -3 && s_e12_hold.active, "a wrong token releases nothing");
            CHECK(qo.submit(qo.userdata, qo.lifetime, token, NULL, NULL) == 0 && !s_e12_hold.active &&
                  b->completion_status(b->user, held2, 1) == 1, "abandoning releases the held work");
            CHECK(qo.submit(qo.userdata, qo.lifetime, token, NULL, NULL) == -3, "a reservation is submitted once");
            CHECK(s_e12_hold.reservations == 2 && s_e12_hold.abandoned == 1 && s_e12_hold.blocked_waits >= 1 &&
                  s_e12_hold.failures == 0 && s_e12_hold.held_submits >= 2, "queue order counted");
            if (nf) nf->lpVtbl->Release(nf); if (nl) nl->lpVtbl->Release(nl); if (na) na->lpVtbl->Release(na);
            if (rh) rh->lpVtbl->Release(rh);
            qi.release(qi.userdata, qi.owner_lifetime, qi.resource_id, qi.resource_generation);
            b->color_target_release(b->user, rq);
        }
        o.release(o.userdata, o.lifetime);
        CHECK(o.try_begin(o.userdata, o.lifetime) == -3, "release ends lease rights");
        CHECK(s_e12_owner_stats.grants == 6 && s_e12_owner_stats.refused_dirty + s_e12_owner_stats.refused_busy >= 1,
              "lease grants and refusals counted");
        atomic_store(&s_e12_owner_setting, 0);
    }
    b->shutdown(b->user);
    const int verrors = validation_errors();
    CHECK(verrors == 0, "no debug-layer errors");
    fprintf(stderr, "%s (%d/%d)\n", failures ? "FAILED" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
