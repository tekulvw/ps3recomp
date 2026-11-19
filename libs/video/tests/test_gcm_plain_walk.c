/* The walker's plain-method path (PS3RECOMP_GCM_FAST_WALK=1, cellGcmSys.c)
 * against the general per-word path, and its packet runs
 * (rsx_draw_engine_methods_at, PS3RECOMP_GCM_FAST_WALK_RUNS) against both.
 *
 * One synthetic FIFO, walked three times: by the general path, by the plain
 * path one word at a time, and by the plain path handing whole packets over
 * as runs. The draw engine must receive the same methods, arguments and data
 * addresses in the same order, and the legacy method layer the same calls,
 * every way. The stream has incrementing and non-incrementing packets, a
 * packet whose methods with effects of their own (SET_REFERENCE, report and
 * label DMA contexts) sit between plain ones, packets that end just before,
 * on and just after such a method, a subchannel 1 packet, a 2D packet, a
 * CALL/RET, a long packet, and a packet that crosses a 1 MB IO page whose
 * second page maps somewhere unrelated -- the plain path steps the data
 * address within a page and must look the next page up, and a run must end
 * at the page boundary.
 *
 * python3 tools/test_gcm_plain_walk.py [--mutants]
 */
#include "cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
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

/* Every call either layer receives, in order. */
enum { LOG_MAX = 1024 };
typedef struct { u32 kind, method, arg, ea; } call;
static call g_log[LOG_MAX];
static u32 g_n;
static void note(u32 kind, u32 method, u32 arg, u32 ea)
{
    if (g_n < LOG_MAX) g_log[g_n] = (call){ kind, method, arg, ea };
    g_n++;
}
int  rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    note(1, method, data, 0);
    if (method == 0x50u) g_rsx_last_reference = data;
    return 0;
}
int  rsx_draw_engine_enabled(void) { return 1; }
void rsx_draw_engine_method(u32 method, u32 arg) { note(2, method, arg, 0); }
void rsx_draw_engine_method_at(u32 method, u32 arg, u32 data_ea) { note(3, method, arg, data_ea); }
/* The runs the walker hands over: each one as its words (the engine's own
 * run entry applies them one by one too), plus where each run started and
 * how long it was. */
enum { RUNS_MAX = 256 };
static struct { u32 ea, count; } g_runs[RUNS_MAX];
static u32 g_n_runs;
void rsx_draw_engine_methods_at(u32 subch, u32 method, int incrementing, const u32* words, u32 count, u32 data_ea)
{
    if (g_n_runs < RUNS_MAX) { g_runs[g_n_runs].ea = data_ea; g_runs[g_n_runs].count = count; }
    g_n_runs++;
    for (u32 i = 0; i < count; i++) {
        const u32 w = words[i];
        const u32 arg = (w >> 24) | ((w >> 8) & 0xFF00u) | ((w << 8) & 0xFF0000u) | (w << 24);
        note(3, (subch << 13) | (incrementing ? method + i * 4u : method), arg, data_ea + i * 4u);
    }
}
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

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00200000u            /* two 1 MB IO pages */
#define CMD_SIZE  0x00010000u
#define FAR_PAGE  0x00900000u            /* where IO page 1 is remapped */
#define CROSS_IO  0x000FFFF0u            /* a packet across the page boundary */
#define CALLEE_IO 0x00008000u
#define LONG_WORDS 300u

static uint32_t g_heap = 0x00400000u;
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
/* Guest address of an IO offset under the test's (remapped) table. */
static u32 io_ea(u32 io) { return gcm_io2ea(io); }
static void iow(u32 io, u32 v) { gw32(io_ea(io), v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* A packet header: incrementing (ni = 0) or not, subchannel, method, count. */
static u32 hdr(int ni, u32 subch, u32 method, u32 count)
{
    return (ni ? 0x40000000u : 0u) | (count << 18) | (subch << 13) | method;
}
static u32 packet(u32 io, int ni, u32 subch, u32 method, u32 count, u32 seed)
{
    iow(io, hdr(ni, subch, method, count));
    for (u32 i = 0; i < count; i++) iow(io + 4 + i * 4, seed + i);
    return io + 4 + count * 4;
}

static u32 build(void)
{
    u32 io = 0;
    io = packet(io, 0, 0, 0x1A00u, 8, 0x100);       /* texture unit registers   */
    io = packet(io, 1, 0, 0x1F00u, 6, 0x200);       /* NI: one constant window  */
    io = packet(io, 0, 0, 0x019Cu, 5, 0x300);       /* 0x19C 0x1A0 [0x1A4 0x1A8] 0x1AC */
    io = packet(io, 0, 0, 0x0050u, 1, 0x400);       /* SET_REFERENCE             */
    io = packet(io, 0, 1, 0x1A80u, 2, 0x500);       /* 3D state on subchannel 1  */
    io = packet(io, 0, 2, 0x0300u, 1, 0x600);       /* a 2D method               */
    io = packet(io, 0, 0, 0x1D6Cu, 1, 0x700);       /* label offset, then plain  */
    io = packet(io, 0, 0, 0x0A00u, 3, 0x800);
    /* Around the methods with effects of their own: a packet ending just
     * before one, one ending on one, one starting just after one, one
     * starting on one, and a non-incrementing packet of one. */
    io = packet(io, 0, 0, 0x1D60u, 3, 0xC00);       /* 0x1D60 0x1D64 0x1D68      */
    io = packet(io, 0, 0, 0x1D64u, 3, 0xD00);       /* 0x1D64 0x1D68 [0x1D6C]    */
    io = packet(io, 0, 0, 0x1D78u, 2, 0xE00);       /* 0x1D78 0x1D7C             */
    io = packet(io, 0, 0, 0x1D74u, 2, 0xF00);       /* [0x1D74] 0x1D78           */
    io = packet(io, 0, 0, 0x004Cu, 2, 0x1000);      /* 0x4C [0x50]               */
    io = packet(io, 1, 0, 0x1D70u, 3, 0x1100);      /* NI: three label values    */
    io = packet(io, 1, 0, 0x1A00u, 3, 0x1200);      /* NI: plain                 */
    io = packet(io, 0, 0, 0x0000u, 0, 0x1300);      /* an empty packet           */
    io = packet(io, 0, 0, 0x0B00u, LONG_WORDS, 0x2000);   /* a long packet       */
    iow(io, CALLEE_IO | 2u); io += 4;               /* CALL                      */
    {   u32 c = packet(CALLEE_IO, 0, 0, 0x1740u, 4, 0x900);
        iow(c, 0x00020000u); }                      /* RET                       */
    iow(io, 0x20000000u | CROSS_IO); io += 4;       /* JUMP to the page crossing */
    u32 x = packet(CROSS_IO, 0, 0, 0x1680u, 10, 0xA00);   /* 0xFFFF0..0x100018 */
    x = packet(x, 1, 0, 0x1E40u, 3, 0xB00);
    iow(x, 0x20000000u | x);                        /* park at put               */
    return x;
}

static void walk(int fast, int runs)
{
    g_n = 0; g_n_runs = 0;
    s_walk_fast = fast; s_walk_diag = -1; s_walk_runs = runs;
    s_walk_plain_words = 0; s_walk_plain_runs = 0;
    s_fifo_getoff = 0;
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    vm_write32(ctrl + 4, 0);
    cellGcm_rsx_process_fifo();
}

static int same_log(const call* ref, u32 n_ref, const char* form)
{
    int same = g_n == n_ref;
    if (!same) printf("  %s: %u calls, general %u\n", form, g_n, n_ref);
    for (u32 i = 0; same && i < g_n && i < LOG_MAX; i++)
        if (memcmp(&g_log[i], &ref[i], sizeof g_log[i])) {
            printf("  call %u: %s kind %u method 0x%04X arg 0x%X ea 0x%08X, general kind %u method 0x%04X arg 0x%X ea 0x%08X\n",
                   i, form, g_log[i].kind, g_log[i].method, g_log[i].arg, g_log[i].ea,
                   ref[i].kind, ref[i].method, ref[i].arg, ref[i].ea);
            same = 0;
        }
    return same;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    /* IO page 1 maps somewhere unrelated to page 0's neighbour. */
    s_ea_address_table[1] = (u16)(FAR_PAGE >> 20);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    const u32 put = build();
    vm_write32(ctrl + 0, put);

    walk(0, 1);
    static call slow[LOG_MAX];
    const u32 n_slow = g_n;
    memcpy(slow, g_log, sizeof slow);
    check(s_walk_plain_words == 0 && g_n_runs == 0, "the general path takes every word when the switch is off");
    check(s_fifo_getoff == put, "the general walk reaches the park");
    check(n_slow > 30 && n_slow <= LOG_MAX, "the stream fits the log");

    walk(1, 0);
    check(s_walk_plain_words > 20 && g_n_runs == 0, "the plain path takes the ordinary methods one at a time with runs off");
    check(s_fifo_getoff == put, "the plain walk reaches the park");
    check(same_log(slow, n_slow, "plain"), "and makes the same calls, in the same order, with the same data addresses");
    const unsigned long long plain_words = s_walk_plain_words;

    /* The page-crossing packet's words after the boundary came from the
     * remapped page. */
    int crossed = 0;
    for (u32 i = 0; i < g_n && i < LOG_MAX; i++)
        if (g_log[i].kind == 3 && g_log[i].method == 0x1680u + 4u * 3u)
            crossed = g_log[i].ea == FAR_PAGE && g_log[i].arg == 0xA03u;
    check(crossed, "the first word past a page boundary is read through the next page's mapping");
    /* Methods with effects of their own still reach the legacy layer. */
    int ref = 0;
    for (u32 i = 0; i < g_n && i < LOG_MAX; i++) ref |= g_log[i].kind == 1 && g_log[i].method == 0x50u;
    check(ref, "SET_REFERENCE still takes the general path");

    walk(1, 1);
    check(s_fifo_getoff == put, "the plain walk with runs reaches the park");
    check(same_log(slow, n_slow, "runs"), "runs make the same calls, in the same order, with the same data addresses");
    check(s_walk_plain_words == plain_words, "runs cover exactly the words the per-word plain path took");
    check(g_n_runs == s_walk_plain_runs && g_n_runs >= 8, "the plain packets went over as runs");
    /* The packets around the special methods: the ones that touch one are
     * walked word by word (their plain words still count), the others are
     * one run each. */
    int run_1d60 = 0, run_1d78 = 0, run_1a00_ni = 0, run_long = 0, word_1d64 = 1, word_1d74 = 1, word_4c = 1, word_1d70 = 1;
    int cross_runs = 0, cross_first_ok = 0, cross_second_ok = 0;
    for (u32 r = 0; r < g_n_runs && r < RUNS_MAX; r++) {
        const u32 ea = g_runs[r].ea, n = g_runs[r].count;
        const u32 w0 = vm_read32(ea);
        if (n == 3 && w0 == 0xC00u) run_1d60 = 1;
        if (n == 2 && w0 == 0xE00u) run_1d78 = 1;
        if (n == 3 && w0 == 0x1200u) run_1a00_ni = 1;
        if (n == LONG_WORDS && w0 == 0x2000u) run_long = 1;
        if (w0 == 0xD00u || w0 == 0xF00u || w0 == 0x1000u || w0 == 0x1100u) {
            if (w0 == 0xD00u) word_1d64 = 0;
            if (w0 == 0xF00u) word_1d74 = 0;
            if (w0 == 0x1000u) word_4c = 0;
            if (w0 == 0x1100u) word_1d70 = 0;
        }
        if (w0 >= 0xA00u && w0 < 0xA0Au) {
            cross_runs++;
            if (w0 == 0xA00u) cross_first_ok = n == 3 && ((ea + n * 4u) & 0xFFFFFu) == 0;
            if (w0 == 0xA03u) cross_second_ok = n == 7 && ea == FAR_PAGE;
        }
    }
    check(run_1d60 && run_1d78 && run_1a00_ni && run_long,
          "packets that end before, start after, or never touch a special method are one run each");
    check(word_1d64 && word_1d74 && word_4c && word_1d70,
          "packets whose range holds a special method are walked word by word");
    check(cross_runs == 2 && cross_first_ok && cross_second_ok,
          "a packet across an IO page boundary is two runs, split at the boundary, the second through the next page's mapping");

    /* A per-word diagnostic turns the plain path off, runs included. */
    setenv("GCM_METHOD_HIST", "60", 1);
    walk(1, 1);
    check(s_walk_plain_words == 0 && g_n_runs == 0 && g_n == n_slow, "a per-word diagnostic keeps the general path");
    unsetenv("GCM_METHOD_HIST");

    printf("[gcm_plain_walk] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
