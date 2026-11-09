/* The Vulkan engine backend driven through a synthetic stream of the
 * operations the register-file engine issues -- target creation and seeding,
 * clears, texture creation, uploads and in-place re-uploads, colour feedback
 * snapshots, depth-tested and blended draws with per-draw constants, pipeline
 * and texture release mid-stream, completion tickets, a GPU depth view, a
 * present and readbacks -- headless under the Khronos validation layer.
 *
 * Run once with each recording mode (argv[1] "sync", "batched", or "reuse":
 * batched with PS3RECOMP_VK_DESCRIPTOR_REUSE=1, the constant blocks bound with
 * dynamic offsets and descriptor sets reused across draws). Each run
 * writes its readback bytes to argv[2]; the runner requires the two files to
 * be identical, so batched recording is checked against the per-operation
 * submissions, pixel for pixel. No window, guest memory or game data.
 * python3 tools/test_vulkan_batched_engine.py */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "../rsx_vulkan_backend.c"
#include <assert.h>
#include <time.h>
u8* vm_base;u32 ppu_vm_size,ppu_hle_inject_base;
u32 cellGcmResolveLocated(int local,u32 offset){(void)local;(void)offset;assert(!"fixture has no guest memory");return 0;}
u32 cellGcmResolveIO(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
u32 gcm_io_offset_to_ea(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
void vm_write32(uint32_t a,uint32_t v){(void)a;(void)v;assert(!"fixture cannot publish guest memory");}
void ps3_ms(const char* key){(void)key;}
unsigned long long ps3_ms_now(void){return 0;}
void cellGcm_replay_display_buffers_to_engine(void){}
u32 cellGcm_flip_request_count(void){return 0;}
void cellGcm_fifo_kick_notify(void){}
rsx_frontend_device* rsx_frontend_vulkan_create(const rsx_frontend_vulkan_owner* owner){(void)owner;assert(!"legacy frontend must not be created");return NULL;}
void rsx_frontend_close(rsx_frontend_device* device){assert(!device);}

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } } while (0)

/* Two float4 slots: position (xy) and colour; the pixel stage mixes the
 * colour with unit 0 by PSConstants. */
static const char* VS =
    "cbuffer VPConst : register(b0) { float4 vc[4]; };\n"
    "struct VSIn { float4 a0 : ATTR0; float4 a1 : ATTR1; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : TEXCOORD0; float4 uv : TEXCOORD1; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = float4(i.a0.xy * vc[0].xy + vc[0].zw, vc[1].x, 1.0);\n"
    "  o.col = i.a1; o.uv = float4(i.a0.xy * 0.5 + 0.5, 0, 0); return o; }\n";
static const char* PS =
    "cbuffer PSConstants : register(b1) { float4 pc[2]; };\n"
    "Texture2D rsx_tex[16] : register(t0);\n"
    "SamplerState rsx_samp[16] : register(s0);\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : TEXCOORD0; float4 uv : TEXCOORD1; };\n"
    "float4 main(VSOut i) : SV_Target0 { return i.col * pc[0] + rsx_tex[0].Sample(rsx_samp[0], i.uv.xy) * pc[1]; }\n";

enum { TW = 96, TH = 64 };

static u32 pipeline_for(int blend, int depth, rsx_be_format format)
{
    rsx_be_render_state rs; memset(&rs, 0, sizeof rs);
    rs.color_mask = 0xFFFFFFFFu;
    rs.blend_enable = (u32)blend; rs.sf_rgb = rs.sf_a = 0x0302; rs.df_rgb = rs.df_a = 0x0303;
    rs.eq_rgb = rs.eq_a = 0x8006;
    rs.depth_test = (u32)depth; rs.depth_write = (u32)depth; rs.depth_func = 0x0203;  /* LEQUAL */
    rsx_vertex_layout_plan lp; memset(&lp, 0, sizeof lp);
    lp.mask = 3; lp.stride = 32; lp.count = 2;
    return vk_eng_pipeline_create(NULL, VS, PS, &rs, &lp, 32, format, 1);
}
static u32 pipeline(int blend, int depth) { return pipeline_for(blend, depth, RSX_BE_FMT_R8G8B8A8); }

static void quad(float* v, float x0, float y0, float x1, float y1, const float c[4])
{
    const float p[6][2] = { {x0,y0},{x1,y0},{x0,y1},{x1,y0},{x1,y1},{x0,y1} };
    for (int i = 0; i < 6; i++) {
        float* o = v + i * 8;
        o[0] = p[i][0]; o[1] = p[i][1]; o[2] = 0; o[3] = 1;
        memcpy(o + 4, c, 16);
    }
}

static void texels(u8* t, u32 w, u32 h, u32 seed)
{
    for (u32 i = 0; i < w * h; i++) {
        const u32 v = (i + seed) * 2654435761u;
        t[4*i] = (u8)v; t[4*i+1] = (u8)(v >> 8); t[4*i+2] = (u8)(v >> 16); t[4*i+3] = 0xFF;
    }
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* Foreign GPU work on the same queue, standing in for a native pass: `n`
 * conversion dispatches over a large buffer, submitted without a wait. */
static struct { VkCommandPool pool; VkCommandBuffer cb; VkFence fence; vk_buffer buf; VkDescriptorPool dp; VkDescriptorSet set; } B;
static void busy_submit(u32 n)
{
    enum { BW = 4096, BH = 4096 };
    if (!B.pool) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = s_vk.queue_family };
        assert(pvkCreateCommandPool(s_vk.device, &pci, NULL, &B.pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = B.pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        assert(pvkAllocateCommandBuffers(s_vk.device, &cai, &B.cb) == VK_SUCCESS);
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        assert(pvkCreateFence(s_vk.device, &fci, NULL, &B.fence) == VK_SUCCESS);
        assert(vk_e5_pipeline() == 0 && vk_e5_device_buffer(&B.buf, (VkDeviceSize)BW * BH * 4u) == 0);
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
        VkDescriptorPoolCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                           .poolSizeCount = 1, .pPoolSizes = &ps };
        assert(pvkCreateDescriptorPool(s_vk.device, &dci, NULL, &B.dp) == VK_SUCCESS);
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = B.dp,
                                            .descriptorSetCount = 1, .pSetLayouts = &s_e5.set_layout };
        assert(pvkAllocateDescriptorSets(s_vk.device, &dai, &B.set) == VK_SUCCESS);
        VkDescriptorBufferInfo bi = { B.buf.buf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = B.set, .dstBinding = 0,
                                    .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
        pvkUpdateDescriptorSets(s_vk.device, 1, &wr, 0, NULL);
    }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    assert(pvkResetCommandBuffer(B.cb, 0) == VK_SUCCESS && pvkBeginCommandBuffer(B.cb, &bi) == VK_SUCCESS);
    vk_cmd_after_everything(B.cb);
    pvkCmdBindPipeline(B.cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_e5.pipe);
    pvkCmdBindDescriptorSets(B.cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_e5.layout, 0, 1, &B.set, 0, NULL);
    const u32 ext[2] = { BW, BH };
    pvkCmdPushConstants(B.cb, s_e5.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, ext);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    for (u32 i = 0; i < n; i++) {
        pvkCmdDispatch(B.cb, BW / 8, BH / 8, 1);
        pvkCmdPipelineBarrier(B.cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    vk_cmd_after_everything(B.cb);
    assert(pvkEndCommandBuffer(B.cb) == VK_SUCCESS && pvkResetFences(s_vk.device, 1, &B.fence) == VK_SUCCESS);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &B.cb };
    pthread_mutex_lock(&s_vk.queue_mu);
    assert(pvkQueueSubmit(s_vk.queue, 1, &si, B.fence) == VK_SUCCESS);
    pthread_mutex_unlock(&s_vk.queue_mu);
}
static void busy_shutdown(void)
{
    if (!B.pool) return;
    pvkWaitForFences(s_vk.device, 1, &B.fence, VK_TRUE, UINT64_MAX);
    pvkDestroyDescriptorPool(s_vk.device, B.dp, NULL); vk_destroy_buffer_now(&B.buf);
    pvkDestroyFence(s_vk.device, B.fence, NULL); pvkDestroyCommandPool(s_vk.device, B.pool, NULL);
}

int main(int argc, char** argv)
{
    if (argc != 4) { fprintf(stderr, "usage: fixture sync|batched|reuse OUT DRAWS\n"); return 2; }
    const int reuse = !strcmp(argv[1], "reuse");
    const int batched = reuse || !strcmp(argv[1], "batched");
    if (reuse) setenv("PS3RECOMP_VK_DESCRIPTOR_REUSE", "1", 1); else unsetenv("PS3RECOMP_VK_DESCRIPTOR_REUSE");
    const u32 draws = (u32)strtoul(argv[3], NULL, 0);
    setenv("PS3RECOMP_RSX_ENGINE", "dispatch", 1);
    setenv("PS3RECOMP_VK_WINDOW", "0", 1);
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 1);
    if (batched) setenv("PS3RECOMP_VK_BATCHED_ENGINE", "1", 1); else unsetenv("PS3RECOMP_VK_BATCHED_ENGINE");
    assert(rsx_vulkan_backend_init(TW, TH, "batched engine synthetic") == 0 && s_vk.eng_active);
    CHECK(s_bat_on == batched, "recording mode as requested");
    CHECK(s_bat.desc_reuse == reuse, "descriptor reuse as requested");
    CHECK(s_g.on, "guest programs (glslang) available");
    if (!s_g.on) { rsx_vulkan_backend_shutdown(); return 1; }

    u8 seed[TW * TH * 4]; texels(seed, TW, TH, 7);
    u32 rt = vk_eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, TW, TH, seed, TW * 4);
    u32 rt2 = vk_eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, TW, TH, NULL, 0);
    u32 z = vk_eng_depth_target_create(NULL, TW, TH);
    u32 p_blend = pipeline(1, 0), p_depth = pipeline(0, 1);
    CHECK(rt && rt2 && z && p_blend && p_depth, "targets and pipelines");
    /* Float colour targets a title reduces into (1x1 RGBA32F and R32F). */
    {
        const rsx_be_format floats[2] = { RSX_BE_FMT_R32G32B32A32F, RSX_BE_FMT_R32F };
        for (int k = 0; k < 2; k++) {
            u32 bpp = 0;
            const u32 ft = vk_eng_color_target_create(NULL, floats[k], 1, 1, NULL, 0);
            const u32 fp = pipeline_for(1, 0, floats[k]);
            CHECK(ft && fp, "float colour target and blended pipeline");
            CHECK(vk_eng_render_pass(vk_eng_format(floats[k], &bpp), 1) != VK_NULL_HANDLE, "float render pass");
            if (ft) vk_eng_release(NULL, ft);
        }
    }
    u8 tex[16 * 16 * 4]; texels(tex, 16, 16, 1);
    u32 t0 = vk_eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 16, 16, 1, 1, 0xAAE4u, 0x85u);
    vk_eng_texture_upload(NULL, t0, 0, 0, 16, 16, tex, 16 * 4, 16);
    rsx_be_sampler_desc samp; memset(&samp, 0, sizeof samp);
    samp.wrap_s = samp.wrap_t = samp.wrap_r = 1; samp.max_lod = 0;
    rsx_be_sampler_desc samps[16]; for (int i = 0; i < 16; i++) samps[i] = samp;
    const float clear[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
    vk_eng_clear_depth_stencil(NULL, z, RSX_BE_CLEAR_DEPTH, 1.0f, 0);

    static float verts[6 * 8];
    u32 tickets = 0, views = 0;
    const double t_start = now_ms();
    for (u32 i = 0; i < draws; i++) {
        if (i % 97 == 0) vk_eng_clear_color(NULL, i % 2 ? rt : rt2, clear);
        if (i % 41 == 13) {                         /* in-place re-upload: later draws see it */
            texels(tex, 16, 16, i);
            vk_eng_texture_upload(NULL, t0, 0, 0, 16, 16, tex, 16 * 4, 16);
        }
        if (i % 173 == 50) {                        /* release a sampled texture, make another */
            vk_eng_release(NULL, t0);
            t0 = vk_eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 16, 16, 1, 1, 0xAAE4u, 0x85u);
            texels(tex, 16, 16, i + 3);
            vk_eng_texture_upload(NULL, t0, 0, 0, 16, 16, tex, 16 * 4, 16);
        }
        if (i % 211 == 100) {                       /* release a pipeline that recorded draws use */
            vk_eng_pipeline_release(NULL, p_blend);
            p_blend = pipeline(1, 0);
        }
        u32 target = (i % 5 == 4) ? rt2 : rt;
        u32 bound_tex = t0, snap = 0;
        if (i % 61 == 30) {                         /* colour feedback: sample a copy of the target */
            snap = vk_eng_color_snapshot(NULL, target);
            if (snap) bound_tex = snap;
        }
        vk_eng_bind_targets(NULL, &target, 1, z);
        vk_eng_bind_pipeline(NULL, (i % 3) ? p_blend : p_depth);
        const float vc[16] = { 0.25f, 0.25f, ((float)(i % 13) / 6.5f) - 0.9f, ((float)(i % 7) / 3.5f) - 0.9f,
                               (float)((i * 37) % 100) / 100.0f, 0, 0, 0 };
        vk_eng_bind_vs_constants(NULL, vc, sizeof vc);
        const float pc[8] = { 1, 1, 1, 0.5f + 0.5f * (float)(i % 2), 0.3f, 0.3f, 0.3f, 0.25f };
        vk_eng_bind_ps_constants(NULL, pc, sizeof pc);
        vk_eng_bind_textures(NULL, &bound_tex, samps, 1u);
        vk_eng_set_viewport(NULL, 0, 0, TW, TH);
        vk_eng_set_scissor(NULL, 0, 0, TW, TH);
        const float col[4] = { (float)(i % 17) / 16.0f, (float)(i % 11) / 10.0f, (float)(i % 5) / 4.0f, 0.6f };
        quad(verts, -1, -1, 1, 1, col);
        vk_eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, verts, 6, 32, NULL, 0);
        if (snap) vk_eng_release(NULL, snap);
        if (i % 50 == 49) {                          /* a ticket mid-stream, as deferred reports take */
            const u64 t = vk_eng_submit_async(NULL, RSX_BE_FLUSH_GUEST_REFERENCE);
            CHECK(t != 0, "ticket");
            tickets++;
            if (i % 200 == 199) CHECK(vk_eng_completion_status(NULL, t, 1) == 1, "ticket completes");
        }
        if (i % 300 == 150) {                        /* GPU depth view of the depth so far */
            const u32 v = vk_eng_depth_z24_view(NULL, z, TW, TH, 0xAAE4u, 0xA5u);
            CHECK(v && vk_eng_submit_async(NULL, RSX_BE_FLUSH_GUEST_REFERENCE), "depth view");
            vk_eng_release(NULL, v);
            views++;
        }
        if (i % 257 == 128) vk_eng_present(NULL, rt);
    }
    CHECK(vk_eng_submit_and_wait_checked(NULL, RSX_BE_FLUSH_GUEST_REFERENCE) == 1, "checked flush");
    const double t_ms = now_ms() - t_start;

    /* Behind foreign GPU work on the queue: per-operation submissions wait for
     * it at the first draw; batched recording returns while it still runs.
     * The job is sized from a measured probe to about 300 ms. */
    {
        busy_submit(1); pvkWaitForFences(s_vk.device, 1, &B.fence, VK_TRUE, UINT64_MAX);   /* warm-up */
        double start = now_ms(); busy_submit(4); pvkWaitForFences(s_vk.device, 1, &B.fence, VK_TRUE, UINT64_MAX);
        const double probe = now_ms() - start;
        u32 n = probe > 0.0 ? (u32)(300.0 * 4.0 / probe) : 4000u;
        if (n < 4) n = 4;
        if (n > 20000) n = 20000;
        start = now_ms();
        busy_submit(n);
        u32 target = rt;
        for (u32 i = 0; i < 64; i++) {
            vk_eng_bind_targets(NULL, &target, 1, z);
            vk_eng_bind_pipeline(NULL, p_blend);
            vk_eng_bind_textures(NULL, &t0, samps, 1u);
            const float col[4] = { 1, 0, 0, 0.01f };
            quad(verts, -1, -1, -0.5f, -0.5f, col);
            vk_eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, verts, 6, 32, NULL, 0);
        }
        const u64 t = vk_eng_submit_async(NULL, RSX_BE_FLUSH_GUEST_REFERENCE);
        const double returned = now_ms() - start;
        const VkResult still = pvkGetFenceStatus(s_vk.device, B.fence);
        CHECK(t && vk_eng_completion_status(NULL, t, 1) == 1, "ticket behind foreign work completes");
        const double done = now_ms() - start;
        printf("%s: 64 draws + ticket behind a %.0f ms foreign GPU job returned after %.1f ms (job %s then; all done at %.1f ms)\n",
               argv[1], probe * n / 4.0, returned, still == VK_NOT_READY ? "still running" : "finished", done);
        if (batched) CHECK(still == VK_NOT_READY, "batched recording does not wait for foreign queue work");
        else CHECK(still == VK_SUCCESS, "per-operation submissions wait for foreign queue work");
    }

    /* Everything the CPU reads: both targets and the depth. */
    static u8 out[2 * TW * TH * 4 + TW * TH * 4];
    vk_eng_readback(NULL, rt, 0, 0, TW, TH, out, TW * 4);
    vk_eng_readback(NULL, rt2, 0, 0, TW, TH, out + TW * TH * 4, TW * 4);
    float* d = (float*)(out + 2 * TW * TH * 4);
    CHECK(vk_eng_depth_readback(NULL, z, 0, 0, TW, TH, d, TW * 4) == 0, "depth readback");
    int lit = 0;
    for (u32 i = 0; i < TW * TH * 4; i += 4) lit |= out[i] != out[0] || out[i + 1] != out[1];
    CHECK(lit, "draws landed");
    FILE* f = fopen(argv[2], "wb");
    assert(f && fwrite(out, 1, sizeof out, f) == sizeof out); fclose(f);

    /* A released colour target's image is pooled and a same-sized request
     * takes it back: the new target still starts transparent black, and the
     * release leaves every other target's framebuffer usable. */
    {
        enum { PW = 24, PH = 16 };
        static u8 seedp[PW * PH * 4]; texels(seedp, PW, PH, 11);
        u32 a = vk_eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, PW, PH, seedp, PW * 4);
        u32 keep = vk_eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, PW, PH, NULL, 0);
        CHECK(a && keep, "pool: two targets");
        const float red[4] = { 1, 0, 0, 1 };
        u32 both[2] = { a, keep };
        for (int k = 0; k < 2; k++) {
            vk_eng_bind_targets(NULL, &both[k], 1, 0);
            vk_eng_bind_pipeline(NULL, p_blend);
            vk_eng_bind_textures(NULL, &t0, samps, 1u);
            const float vcp[16] = { 1, 1, 0, 0, 0.5f };
            vk_eng_bind_vs_constants(NULL, vcp, sizeof vcp);
            const float pcp[8] = { 1, 1, 1, 1, 0, 0, 0, 0 };
            vk_eng_bind_ps_constants(NULL, pcp, sizeof pcp);
            vk_eng_set_viewport(NULL, 0, 0, PW, PH); vk_eng_set_scissor(NULL, 0, 0, PW, PH);
            quad(verts, -1, -1, 1, 1, red);
            vk_eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, verts, 6, 32, NULL, 0);
        }
        CHECK(vk_eng_submit_and_wait_checked(NULL, RSX_BE_FLUSH_GUEST_REFERENCE) == 1, "pool: draws flushed");
        vk_eng_release(NULL, a);
        CHECK(vk_eng_submit_and_wait_checked(NULL, RSX_BE_FLUSH_GUEST_REFERENCE) == 1, "pool: release retired");
        vk_eng_completion_status(NULL, s_e5.issued, 1);
        if (s_bat_on) vk_bat_reap();
        const u32 pooled_before = s_img_pool_n;
        u32 b = vk_eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, PW, PH, NULL, 0);
        CHECK(b && pooled_before >= 1 && s_img_pool_n == pooled_before - 1, "pool: the released image was taken back");
        static u8 px[PW * PH * 4];
        memset(px, 0xAA, sizeof px);
        vk_eng_readback(NULL, b, 0, 0, PW, PH, px, PW * 4);
        int nonzero = 0; for (u32 i = 0; i < sizeof px; i++) nonzero |= px[i];
        CHECK(!nonzero, "pool: a reused target starts transparent black");
        /* The kept target's framebuffer survived the release: draw again. */
        const float green[4] = { 0, 1, 0, 1 };
        vk_eng_bind_targets(NULL, &keep, 1, 0);
        vk_eng_bind_pipeline(NULL, p_blend);
        quad(verts, -1, -1, 1, 1, green);
        vk_eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, verts, 6, 32, NULL, 0);
        CHECK(vk_eng_submit_and_wait_checked(NULL, RSX_BE_FLUSH_GUEST_REFERENCE) == 1, "pool: draw after a release");
        u8 one[4] = { 0 };
        vk_eng_readback(NULL, keep, PW / 2, PH / 2, 1, 1, one, 4);
        CHECK(one[1] > one[0], "pool: the kept target took the later draw");
        vk_eng_release(NULL, b); vk_eng_release(NULL, keep);
    }

    /* A unit the program samples with nothing bound reads transparent black,
     * the Metal backend's placeholder, for a 2D unit and a cube unit alike. A
     * coloured "not bound" texture showed through in a title's material that
     * reads a unit it never binds. */
    {
        static const char* PSU =
            "Texture2D   rsx_tex0 : register(t0);\n"
            "Texture2D   rsx_tex1 : register(t1);\n"
            "TextureCube rsx_tex2 : register(t2);\n"
            "Texture2D   rsx_tex3 : register(t3);\n"
            "SamplerState rsx_samp[16] : register(s0);\n"
            "struct VSOut { float4 pos : SV_Position; float4 col : TEXCOORD0; float4 uv : TEXCOORD1; };\n"
            "float4 main(VSOut i) : SV_Target0 { return rsx_tex3.Sample(rsx_samp[3], i.uv.xy)\n"
            "  + rsx_tex2.Sample(rsx_samp[2], float3(i.uv.xy * 2.0 - 1.0, 1.0)); }\n";
        rsx_be_render_state rs; memset(&rs, 0, sizeof rs);
        rs.color_mask = 0xFFFFFFFFu;
        rsx_vertex_layout_plan lp; memset(&lp, 0, sizeof lp);
        lp.mask = 3; lp.stride = 32; lp.count = 2;
        const u32 pu = vk_eng_pipeline_create(NULL, VS, PSU, &rs, &lp, 32, RSX_BE_FMT_R8G8B8A8, 1);
        CHECK(pu, "pipeline sampling unbound 2D and cube units");
        if (pu) {
            u32 target = rt2;
            vk_eng_bind_targets(NULL, &target, 1, 0);
            vk_eng_bind_pipeline(NULL, pu);
            vk_eng_bind_textures(NULL, &t0, samps, 0u);
            const float vc0[16] = { 1, 1, 0, 0, 0.5f };
            vk_eng_bind_vs_constants(NULL, vc0, sizeof vc0);
            vk_eng_set_viewport(NULL, 0, 0, TW, TH);
            vk_eng_set_scissor(NULL, 0, 0, TW, TH);
            const float white[4] = { 1, 1, 1, 1 };
            quad(verts, -1, -1, 1, 1, white);
            vk_eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, verts, 6, 32, NULL, 0);
            CHECK(vk_eng_submit_and_wait_checked(NULL, RSX_BE_FLUSH_GUEST_REFERENCE) == 1, "unbound-unit draw flushed");
            u8 px[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
            vk_eng_readback(NULL, rt2, TW / 2, TH / 2, 1, 1, px, 4);
            printf("%s: unbound units sample %02X %02X %02X %02X\n", argv[1], px[0], px[1], px[2], px[3]);
            CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 0, "unbound 2D and cube units read transparent black");
            vk_eng_pipeline_release(NULL, pu);
        }
    }
    if (reuse) {
        printf("reuse: %llu descriptor sets written, %llu reused\n",
               (unsigned long long)s_bat.sets_written, (unsigned long long)s_bat.sets_reused);
        CHECK(s_bat.sets_reused > draws / 4, "reuse: draws with the same textures share a set");
        CHECK(s_bat.sets_written > 0, "reuse: and the others get their own");
    } else {
        CHECK(s_bat.sets_reused == 0, "no set reuse unless asked");
    }
    CHECK(!vk_e5_failed(), "no submission failed");
    if (batched)
        printf("batched: %llu batches for %u draws (%.1f ops/batch), %llu slot waits, %llu CPU waits, %llu retired, %u pending\n",
               (unsigned long long)s_bat.batches, draws,
               s_bat.batches ? (double)s_bat.batch_ops / (double)s_bat.batches : 0.0,
               (unsigned long long)s_bat.slot_waits, (unsigned long long)s_bat.waits_for_cpu,
               (unsigned long long)s_bat.retired, s_bat.nret);
    printf("%s: %u draws, %u tickets, %u views in %.1f ms (%.1f us/draw)\n", argv[1], draws, tickets, views, t_ms,
           draws ? t_ms * 1000.0 / draws : 0.0);
    vk_eng_release(NULL, t0); vk_eng_release(NULL, rt); vk_eng_release(NULL, rt2); vk_eng_release(NULL, z);
    busy_shutdown();
    rsx_vulkan_backend_shutdown();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
