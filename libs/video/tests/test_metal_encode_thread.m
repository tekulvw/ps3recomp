/* PS3RECOMP_RSX_ENCODE_THREAD on a real Metal device, headless: the engine
 * drives the Metal backend through 64 draws, each a different colour into its
 * own 8x8 cell, with a clear part-way, completion tickets between draws, and
 * the vertex data rewritten after each draw. Recording with the encode thread
 * must read back the same bytes as recording without it. Then the backend's
 * own rules: a released target or pipeline keeps its slot until the encode
 * thread has finished the batches recorded before the release. Synthetic
 * inputs only; no window. */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"
float cellAvconfExt_display_gamma(void) { return 1.0f; }   /* identity present gamma */
u8* vm_base; u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < ppu_vm_size ? offset : 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
int cellGcm_native_consumer_enabled(void) { return 0; }
void cellGcm_fifo_kick_notify(void) {}
void cellGcm_rsx_consumer_begin(void) {}
void cellGcm_rsx_consumer_end(void) {}
void rsx_ring_progress_metal_committed(uint64_t ticket, uint64_t records) { (void)ticket; (void)records; }
void rsx_ring_progress_metal_completed(uint64_t ticket, uint64_t status) { (void)ticket; (void)status; }

static unsigned checks, failures;
#define T(x, label) do { checks++; if (!(x)) { failures++; if (failures < 20) fprintf(stderr, "FAIL %s\n", label); } } while (0)
static void method(u32 a, u32 v) { rsx_draw_engine_method(a, v); }
static void be_float(u8* p, float f) { u32 v; memcpy(&v, &f, 4); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

enum { W = 64, H = 64, CELLS = 64, VTX = 0x10000 };

/* One triangle over the whole target; position, colour and a spare
 * attribute, 48 bytes a vertex. */
static void write_vertices(float r, float g, float b)
{
    static const float pos[3][2] = { { -1, -1 }, { 3, -1 }, { -1, 3 } };
    for (u32 v = 0; v < 3; v++) {
        const float f[12] = { pos[v][0], pos[v][1], 0.5f, 1, r, g, b, 1, 0, 0, 0, 1 };
        for (u32 i = 0; i < 12; i++) be_float(vm_base + VTX + v * 48 + i * 4, f[i]);
    }
}

static void draw(void) { method(0x1808, RSX_PRIMITIVE_TRIANGLES); method(0x1814, 2u << 24); method(0x1808, 0); }

/* The frame: returns the readback. */
static void frame(u8* out, int async_every, u64* tickets, u32* nticket)
{
    T(rsx_draw_engine_init(W, H) == 0, "initialization");
    method(0x194, 0xFEED0000); method(0x210, 0x40000); method(0x20C, W * 4);
    method(0x200, (u32)W << 16); method(0x204, (u32)H << 16); method(0x208, 0x148);
    method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80000000u | VTX);
    method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80000000u | (VTX + 16));
    method(0x1760, 2u | (4u << 4) | (48u << 8)); method(0x16A0, 0x80000000u | (VTX + 32));
    u8 vp[48];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    method(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0xB80 + i * 4, w); }
    method(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
    method(0x8E4, 0x60002); method(0x1D60, 64);
    method(0xA6C, 0x207); method(0xA70, 0); method(0xA74, 0);   /* no depth test or write */
    method(0x1D90, 0xFF000000u); method(0x1D94, 0xF3);           /* opaque black, depth and stencil */
    *nticket = 0;
    for (u32 c = 0; c < CELLS; c++) {
        if (c == CELLS / 2) {
            /* Everything drawn so far goes grey: the clear must land after
             * those draws and before the next. */
            method(0x8C0, (u32)W << 16); method(0x8C4, (u32)H << 16);
            method(0x1D90, 0xFF808080u); method(0x1D94, 0xF0);
        }
        const u32 x = (c % 8u) * 8u, y = (c / 8u) * 8u;
        method(0x8C0, x | (8u << 16)); method(0x8C4, y | (8u << 16));
        write_vertices((float)(c % 4u) / 3.0f, (float)((c / 4u) % 4u) / 3.0f, (float)(c / 16u) / 3.0f);
        draw();
        /* The guest reuses the bytes at once; the draw already copied them.
         * A quarter, a half and three quarters are colours no cell has. */
        write_vertices(0.25f, 0.5f, 0.75f);
        if (async_every && c % (u32)async_every == 0) {
            const u64 t = rsx_draw_engine_submit_async();
            T(t != 0, "completion ticket");
            if (t && *nticket < 64) tickets[(*nticket)++] = t;
        }
    }
    T(s_engine_backend.submit_and_wait_checked(NULL, 0), "GPU completion");
    for (u32 i = 0; i < *nticket; i++)
        T(rsx_draw_engine_completion_status(tickets[i], 1) == 1, "every ticket completes");
    s_engine_backend.readback(NULL, g.surfaces[0].handle, 0, 0, W, H, out, W * 4);
    T(atomic_load(&s_eng_dropped) == 0 && !atomic_load(&s_eng_checked_failed), "no dropped records or backend failure");
}

/* The backend's slot rules with the encode thread running. */
static void lifetimes(void)
{
    setenv("PS3RECOMP_RSX_ENCODE_THREAD", "1", 1);
    T(eng_init(NULL, 16, 16) == 0, "backend initialization");
    T(eng_enc_on(), "encode thread running");
    const float red[4] = { 1, 0, 0, 1 };
    const u32 a = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 16, 16, NULL, 0);
    T(a != 0, "target");
    eng_clear_color(NULL, a, red);
    u64 ticket = 0;
    T(eng_enc_handoff(1, &ticket) && ticket, "records handed over with a ticket");
    /* Released while that batch may still be encoding, and recorded against
     * in the batch now open: the slot stays until that batch is done too. */
    eng_clear_color(NULL, a, red);
    eng_obj_release(NULL, a);
    T(s_eng_obj_retired[a - 1] && eng_obj(a) != nil, "released target keeps its slot while recorded");
    const u32 b = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 16, 16, NULL, 0);
    T(b && b != a, "a new target does not take the released slot");
    eng_enc_drain();
    eng_collect_retired_objects();
    T(eng_obj(a) != nil, "the open batch still holds it after a drain");
    T(eng_enc_handoff(0, NULL), "hand over the open batch");
    eng_enc_drain();
    eng_collect_retired_objects();
    T(eng_obj(a) == nil, "recycled once every batch recorded before the release is encoded");
    T(eng_completion_status(NULL, ticket, 1) == 1, "the ticket completes");
    eng_obj_release(NULL, b);
    eng_shutdown(NULL);
    T(!eng_enc_on(), "encode thread stopped at shutdown");
    unsetenv("PS3RECOMP_RSX_ENCODE_THREAD");
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("SKIP: no Metal device"); return 0; }
        s_queue = [s_dev newCommandQueue]; s_headless = 1; s_guest_shaders = 1;
        vm_base = calloc(1, 1u << 20); ppu_vm_size = 1u << 20;
        rsx_draw_engine_set_backend(&s_engine_backend);
        static u8 direct[W * H * 4], threaded[W * H * 4], every[W * H * 4];
        u64 tickets[64]; u32 n = 0;

        unsetenv("PS3RECOMP_RSX_ENCODE_THREAD");
        frame(direct, 4, tickets, &n);
        T(!eng_enc_on(), "no encode thread unless asked");
        rsx_draw_engine_shutdown();

        setenv("PS3RECOMP_RSX_ENCODE_THREAD", "1", 1);
        atomic_store(&s_eng_checked_failed, 0);
        frame(threaded, 4, tickets, &n);
        const unsigned long long jobs = s_encq.jobs, records = s_encq.records;
        T(eng_enc_on() && jobs >= CELLS / 4 && records >= CELLS, "draws encoded on the encode thread");
        /* A pipeline released while its batch is open keeps its state
         * objects until that batch is encoded. */
        T(s_eng_pipe_count && s_eng_pipe[0].pso, "the frame made a pipeline");
        if (s_eng_pipe_count && s_eng_pipe[0].pso) {
            eng_pipeline_release(NULL, 1);
            T(s_eng_pipe[0].pso != nil, "released pipeline kept while the batch is open");
            T(eng_enc_handoff(0, NULL), "hand over");
            eng_enc_drain();
            eng_collect_retired_objects();
            T(s_eng_pipe[0].pso == nil, "dropped once encoded");
        }
        rsx_draw_engine_shutdown();
        T(!eng_enc_on(), "stopped with the engine");

        /* A ticket after every draw: more jobs than the queue holds, so the
         * walker waits for room. */
        atomic_store(&s_eng_checked_failed, 0);
        frame(every, 1, tickets, &n);
        T(s_encq.jobs > jobs, "one job per ticket");
        rsx_draw_engine_shutdown();
        unsetenv("PS3RECOMP_RSX_ENCODE_THREAD");

        u32 diff = 0, distinct = 0;
        for (u32 i = 0; i < W * H * 4; i++) diff += direct[i] != threaded[i] || direct[i] != every[i];
        T(diff == 0, "the encode thread reads back the same bytes");
        /* The frame is what it should be: the first half grey, the second
         * half one colour per cell, and not the colour written after each draw. */
        u32 seen[CELLS]; u32 nseen = 0; int grey = 1, stale = 0;
        for (u32 c = 0; c < CELLS; c++) {
            const u32 x = (c % 8u) * 8u + 4u, y = (c / 8u) * 8u + 4u;
            u32 px; memcpy(&px, direct + (y * W + x) * 4, 4);
            if (c < CELLS / 2) grey &= (px & 0x00FFFFFFu) == 0x808080u;
            else {
                int found = 0;
                for (u32 k = 0; k < nseen; k++) found |= seen[k] == px;
                if (!found) seen[nseen++] = px;
                /* Cell colours are thirds: 0, 85, 170 or 255 a channel. */
                const u8* b = (const u8*)&px;
                for (u32 k = 0; k < 3; k++) stale |= b[k] != 0 && b[k] != 85 && b[k] != 170 && b[k] != 255;
            }
        }
        distinct = nseen;
        T(grey, "the clear lands after the first half of the draws");
        T(distinct == CELLS / 2, "one colour per cell after the clear");
        T(!stale, "no draw read the vertex bytes written after it");

        lifetimes();

        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
        printf("test_metal_encode_thread: checks=%u failures=%u diff=%u distinct=%u jobs=%llu records=%llu\n",
               checks, failures, diff, distinct, (unsigned long long)s_encq.jobs, (unsigned long long)s_encq.records);
        return failures ? 1 : 0;
    }
}
