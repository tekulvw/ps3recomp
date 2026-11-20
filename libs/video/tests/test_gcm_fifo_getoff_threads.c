/* The walker's position seen from other threads (cellGcmSys.c).
 *
 * The FIFO pass keeps its position in a local and stores it to s_fifo_getoff
 * at control flow, flips, semaphores/references, resyncs and the end of the
 * pass. Two readers on other threads depend on that: the ring-recycle wait
 * (cellGcm_fifo_recycle polls it for progress until the walker takes the
 * backward JUMP it wrote) and the flip-into-ring test. This fixture runs one
 * long pass on a walker thread while
 *   1. a poller thread samples s_fifo_getoff: the samples must be
 *      non-decreasing (the stream ascends), several distinct values must
 *      appear before the pass ends (publication at the JUMPs between
 *      segments, not only at the end), and the final value must be put;
 *   2. the real cellGcm_fifo_recycle runs on the second thread while the
 *      walker is inside the ring: it must return well inside its 2 s bound
 *      with the backward jump counted, i.e. it saw the walker move.
 *
 * python3 tools/test_gcm_fifo_getoff_threads.py [--mutants] */
#include "cellGcmSys.c"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_draw_engine_enabled(void) { return 1; }
static volatile u32 g_words;
/* Scenario 2 slows the engine for words inside the ring (EAs below
 * g_slow_below), so the pass is still in the ring when the recycle lands. */
static volatile int g_slow;
static void slow_word(void) { if (g_slow) { volatile int k = 0; while (k < 40) k++; } }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; g_words++; slow_word(); }
void rsx_draw_engine_method_at(u32 method, u32 arg, u32 data_ea) { (void)method; (void)arg; (void)data_ea; g_words++; slow_word(); }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
void rsx_live_draw_method_at(u32 method,u32 arg,u32 data_ea) { (void)data_ea;rsx_live_draw_method(method,arg); }

#define IO_ADDR    0x00100000u
#define IO_SIZE    0x00800000u            /* eight 1 MB IO pages */
#define CMD_SIZE   0x00180000u            /* the context ring: 1.5 MB at IO 0 */
#define SEGMENTS   3u
#define SEG_BASE   0x00400000u            /* the segment stream: IO pages 4.. */
#define SEG_BYTES  0x00100000u            /* one segment per IO page */
#define SEG_PACKETS 600u                  /* 600 x 201 words = ~120k words a segment */
#define RING_PACKETS 1900u                /* 1900 x 804 B = 1.46 MB of the ring */

static uint32_t g_heap = 0x01000000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }
static void iow(u32 io, u32 v) { gw32(gcm_io2ea(io), v); }
static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}
static u32 packet(u32 io, u32 method, u32 count, u32 seed)
{
    iow(io, (count << 18) | method);
    for (u32 i = 0; i < count; i++) iow(io + 4 + i * 4, seed + i);
    return io + 4 + count * 4;
}
/* SEGMENTS segments of plain packets, each ending in a JUMP to the next; the
 * last parks at `put`. Returns put. */
static u32 build(void)
{
    u32 io = 0;
    for (u32 s = 0; s < SEGMENTS; s++) {
        io = SEG_BASE + s * SEG_BYTES;
        for (u32 k = 0; k < SEG_PACKETS; k++) io = packet(io, 0x1A00u, 200, 0x100 + k);
        if (s + 1 < SEGMENTS) iow(io, 0x20000000u | (SEG_BASE + (s + 1) * SEG_BYTES));
    }
    iow(io, 0x20000000u | io);                        /* park at put */
    return io;
}
static u32 g_put;
static void* walker(void* arg) { (void)arg; cellGcm_rsx_process_fifo(); return NULL; }
static volatile int g_stop;
/* As the host's frame clock: pass after pass until told to stop. */
static void* walker_loop(void* arg)
{
    (void)arg;
    while (!g_stop) { cellGcm_rsx_process_fifo(); struct timespec d = { 0, 200 * 1000 }; nanosleep(&d, NULL); }
    return NULL;
}

static u32 g_samples[1 << 16];
static u32 g_n_samples;
static volatile int g_walker_done;
static int g_monotone = 1;
static void* poller(void* arg)
{
    (void)arg;
    u32 last = 0;
    while (!g_walker_done) {
        const u32 v = __atomic_load_n(&s_fifo_getoff, __ATOMIC_RELAXED);
        if (v != last) {
            if (v < last) g_monotone = 0;
            if (g_n_samples < (1u << 16)) g_samples[g_n_samples] = v;
            g_n_samples++;
            last = v;
        }
    }
    return NULL;
}

/* Scenario 2: the title's buffer-full callback, while the walker is in the ring. */
static u32 g_ctx_ea;
static double g_recycle_ms;
static u32 g_back_jumps_after;
static u32 g_ring_io, g_cur_io;
static void* recycler(void* arg)
{
    (void)arg;
    /* Wait until the walker is inside the ring -- which it can only learn
     * from the position the JUMP into the ring published. */
    while (__atomic_load_n(&s_fifo_getoff, __ATOMIC_RELAXED) >= SEG_BASE) {}
    g_slow = 1;   /* from here the ring's words run slowly: the recycle lands mid-pass */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    cellGcm_fifo_recycle(g_ctx_ea);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_recycle_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    g_back_jumps_after = s_fifo_back_jumps;
    g_stop = 1;
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    unsetenv("PS3RECOMP_GCM_CHECKED_REF");
    setenv("PS3RECOMP_GCM_FAST_WALK", "1", 1);

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    g_put = build();
    vm_write32(ctrl + 0, g_put);
    s_fifo_getoff = SEG_BASE;

    /* 1. One long pass, sampled from another thread. */
    pthread_t tw, tp;
    pthread_create(&tp, NULL, poller, NULL);
    pthread_create(&tw, NULL, walker, NULL);
    pthread_join(tw, NULL);
    g_walker_done = 1;
    pthread_join(tp, NULL);
    check(s_fifo_getoff == g_put, "after the pass s_fifo_getoff is put");
    check(g_words >= SEGMENTS * SEG_PACKETS * 200u, "every data word reached the engine");
    check(g_monotone, "the position another thread samples never goes backwards");
    printf("  %u distinct positions sampled during the pass\n", g_n_samples);
    check(g_n_samples >= SEGMENTS, "the position is published at the JUMPs between segments, not only at the end");

    /* 2. The ring-recycle wait sees the walker move and the backward jump land.
     * The context's ring: begin..end at IO 0; `current` is where the title's
     * write head is. Fill it, start a pass over it, and recycle from the other
     * thread while the walker is inside. */
    const u32 ctx = ctx_out;   /* cellGcmSetupContext wrote the context EA here */
    g_ctx_ea = vm_read32(ctx);
    const u32 begin = vm_read32(g_ctx_ea + 0x0);
    u32 io = gcm_ea2io(begin);
    check(io != 0xFFFFFFFFu, "the context ring is in IO space");
    g_ring_io = io;
    /* Most of the ring is filled; the recycle writes its JUMP at `current`. */
    u32 cur_io = io;
    for (u32 k = 0; k < RING_PACKETS; k++) cur_io = packet(cur_io, 0x1A00u, 200, 0x900 + k);
    iow(cur_io, 0x20000000u | cur_io);                         /* park at the write head */
    g_cur_io = cur_io;
    vm_write32(g_ctx_ea + 0x8, gcm_io2ea(cur_io));             /* current */
    vm_write32(ctrl + 0, cur_io);                              /* put */
    s_fifo_getoff = io;
    const u32 back0 = s_fifo_back_jumps;
    /* Make the pass long enough for the recycler to land inside it: prepend
     * the big stream by jumping into the ring at its end. */
    iow(g_put, 0x20000000u | io);                              /* the old park now jumps into the ring */
    s_fifo_getoff = SEG_BASE;
    pthread_t tr;
    pthread_create(&tr, NULL, recycler, NULL);
    pthread_create(&tw, NULL, walker_loop, NULL);
    pthread_join(tw, NULL);
    pthread_join(tr, NULL);
    printf("  recycle returned in %.1f ms, back jumps %u -> %u\n", g_recycle_ms, back0, g_back_jumps_after);
    check(g_back_jumps_after > back0, "the recycle wait saw the walker take its backward JUMP");
    check(g_recycle_ms < 1500.0, "and returned well inside its 2 s no-progress bound");

    printf("[gcm_fifo_getoff_threads] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
