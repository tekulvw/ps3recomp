/* What a Vulkan engine present costs the CPU, and that a direct present shows
 * the same frame a read-back present does.
 *
 * Each frame clears a 1280x720 target, draws a number of blended full-screen
 * quads into it and presents it; the fixture times the present call alone.
 * Run once per present mode: the runner sets PS3RECOMP_VK_PRESENT_READBACK=1
 * for the read-back run. Headless unless the runner asks for the window
 * (PS3RECOMP_VK_WINDOW=1). Writes the final target's bytes to argv[2]; the
 * runner requires both modes to match. No guest memory or game data.
 * python3 tools/test_vulkan_present_cost.py */
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

static const char* VS =
    "cbuffer VPConst : register(b0) { float4 vc[4]; };\n"
    "struct VSIn { float4 a0 : ATTR0; float4 a1 : ATTR1; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : TEXCOORD0; };\n"
    "VSOut main(VSIn i) { VSOut o; o.pos = float4(i.a0.xy, 0.5, 1.0); o.col = i.a1; return o; }\n";
static const char* PS =
    "struct VSOut { float4 pos : SV_Position; float4 col : TEXCOORD0; };\n"
    "float4 main(VSOut i) : SV_Target0 { return i.col; }\n";

enum { W = 1280, H = 720 };

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }

int main(int argc, char** argv)
{
    if (argc != 5 && argc != 6) { fprintf(stderr, "usage: fixture LABEL OUT FRAMES DRAWS_PER_FRAME [FRAME_MS]\n"); return 2; }
    const u32 frames = (u32)strtoul(argv[3], NULL, 0), draws = (u32)strtoul(argv[4], NULL, 0);
    /* Optional pacing: each frame takes at least FRAME_MS, as a title well
     * below the display rate does, so a window's vsync does not set the cost. */
    const double frame_ms = argc == 6 ? atof(argv[5]) : 0.0;
    setenv("PS3RECOMP_RSX_ENGINE", "dispatch", 1);
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 1);
    if (!getenv("PS3RECOMP_VK_WINDOW")) setenv("PS3RECOMP_VK_WINDOW", "0", 1);
    assert(rsx_vulkan_backend_init(W, H, "present cost synthetic") == 0 && s_vk.eng_active);
    CHECK(s_g.on, "guest programs (glslang) available");
    if (!s_g.on) { rsx_vulkan_backend_shutdown(); return 1; }
    const int readback = getenv("PS3RECOMP_VK_PRESENT_READBACK") && !strcmp(getenv("PS3RECOMP_VK_PRESENT_READBACK"), "1");
    const int threaded = getenv("PS3RECOMP_RSX_PRESENT_THREAD") && !strcmp(getenv("PS3RECOMP_RSX_PRESENT_THREAD"), "1");
    CHECK(!threaded == !s_vpt.pt, "the present thread runs exactly when asked for");

    u32 rt = vk_eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, W, H, NULL, 0);
    rsx_be_render_state rs; memset(&rs, 0, sizeof rs);
    rs.color_mask = 0xFFFFFFFFu;
    rs.blend_enable = 1; rs.sf_rgb = rs.sf_a = 0x0302; rs.df_rgb = rs.df_a = 0x0303; rs.eq_rgb = rs.eq_a = 0x8006;
    rsx_vertex_layout_plan lp; memset(&lp, 0, sizeof lp);
    lp.mask = 3; lp.stride = 32; lp.count = 2;
    const u32 pipe = vk_eng_pipeline_create(NULL, VS, PS, &rs, &lp, 32, RSX_BE_FMT_R8G8B8A8, 1);
    CHECK(rt && pipe, "target and pipeline");

    double* cost = calloc(frames, sizeof *cost);
    static float v[6 * 8];
    const float p[6][2] = { {-1,-1},{1,-1},{-1,1},{1,-1},{1,1},{-1,1} };
    const double t0 = now_ms();
    for (u32 f = 0; f < frames; f++) {
        const double frame_start = now_ms();
        const float clear[4] = { (float)(f % 3) / 2.0f, 0.25f, 0.5f, 1.0f };
        vk_eng_clear_color(NULL, rt, clear);
        vk_eng_bind_targets(NULL, &rt, 1, 0);
        vk_eng_bind_pipeline(NULL, pipe);
        vk_eng_set_viewport(NULL, 0, 0, W, H);
        vk_eng_set_scissor(NULL, 0, 0, W, H);
        for (u32 d = 0; d < draws; d++) {
            const float c[4] = { (float)((f + d) % 7) / 6.0f, (float)(d % 5) / 4.0f, 0.125f, 0.0625f };
            for (int i = 0; i < 6; i++) { v[i*8] = p[i][0]; v[i*8+1] = p[i][1]; v[i*8+2] = 0; v[i*8+3] = 1; memcpy(v + i*8 + 4, c, 16); }
            vk_eng_draw(NULL, RSX_TOPOLOGY_TRIANGLES, v, 6, 32, NULL, 0);
        }
        const double s = now_ms();
        vk_eng_present(NULL, rt);
        cost[f] = now_ms() - s;
        while (now_ms() - frame_start < frame_ms) {
            struct timespec ts = { 0, 200000 };
            nanosleep(&ts, NULL);
        }
    }
    CHECK(vk_eng_submit_and_wait_checked(NULL, RSX_BE_FLUSH_GUEST_REFERENCE) == 1, "checked flush");
    const double total = now_ms() - t0;

    static u8 out[W * H * 4];
    vk_eng_readback(NULL, rt, 0, 0, W, H, out, W * 4);
    const u8* c = out + ((size_t)(H / 2) * W + W / 2) * 4;
    const u32 centre = ((u32)c[0] << 16) | ((u32)c[1] << 8) | c[2];
    if (threaded) {
        /* Every frame staged; each one then shown or dropped for a newer one,
         * never waited for. Only the present thread presents. */
        rsx_present_thread_stats st;
        for (int i = 0; i < 10000; i++) {
            rsx_present_thread_get_stats(s_vpt.pt, &st);
            if (st.presented + st.dropped + st.show_failed >= frames) break;
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
        }
        CHECK(st.staged == frames && st.stage_failed == 0, "every present staged");
        CHECK(st.presented + st.dropped == frames && st.show_failed == 0, "every staged frame shown or dropped");
        CHECK(st.presented >= 1 && s_vk.presented == st.presented, "the present thread presented");
        CHECK(s_vk.present_direct == 0 && s_vk.present_readback_count == 0, "no present on the presenting thread");
    } else
    CHECK(s_vk.presented == frames, "every present counted");
#ifdef VK_PRESENT_SLOTS
    if (threaded) {
    } else if (readback) {
        CHECK(s_vk.present_readback_count == frames && s_vk.present_direct == 0, "every present read back");
        CHECK((rsx_vulkan_backend_presented_center() & 0xFFFFFFu) == centre, "read-back present kept the centre pixel");
    } else {
        CHECK(s_vk.present_direct == frames && s_vk.present_readback_count == 0, "every present direct");
        CHECK(rsx_vulkan_backend_presented_center() == 0, "a direct present keeps no centre pixel");
    }
#else
    (void)readback;
    CHECK((rsx_vulkan_backend_presented_center() & 0xFFFFFFu) == centre, "present kept the centre pixel");
#endif
    FILE* fo = fopen(argv[2], "wb");
    assert(fo && fwrite(out, 1, sizeof out, fo) == sizeof out); fclose(fo);

    qsort(cost, frames, sizeof *cost, cmp);
    double sum = 0; for (u32 f = 0; f < frames; f++) sum += cost[f];
    printf("%s: %u frames x %u draws at %ux%u, present CPU median %.1f us, p90 %.1f us, mean %.1f us; "
           "loop %.2f ms/frame (paced %.1f ms)\n", argv[1], frames, draws, W, H, cost[frames / 2] * 1e3,
           cost[frames * 9 / 10] * 1e3, sum / frames * 1e3, total / frames, frame_ms);
    free(cost);
    vk_eng_release(NULL, rt);
    vk_eng_pipeline_release(NULL, pipe);
    rsx_vulkan_backend_shutdown();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
