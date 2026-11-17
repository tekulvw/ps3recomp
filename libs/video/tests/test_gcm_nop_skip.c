/* PS3RECOMP_GCM_NOP_SKIP=1 (cellGcmSys.c): runs of zero words in the FIFO
 * stepped over in one go, against the general per-word walk. One synthetic
 * FIFO with NOP runs of several lengths between packets, a run across a 1 MB
 * IO page whose next page maps somewhere unrelated, put inside a run, and an
 * armed native handoff inside a run. The engine and legacy layers must get
 * the same calls in the same order and the walk must end at the same offset.
 *
 * python3 tools/test_gcm_nop_skip.py
 */
#include "../cellGcmSys.c"
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
enum { LOG_MAX = 512 };
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


/* A run of n zero words at io. */
static u32 zeros(u32 io, u32 n) { for (u32 i = 0; i < n; i++) iow(io + 4 * i, 0); return io + 4 * n; }

static u32 g_handoff_io;
static u32 build(void)
{
    u32 io = 0;
    io = packet(io, 0, 0, 0x1A00u, 4, 0x100);
    io = zeros(io, 1);                              /* a lone NOP                 */
    io = packet(io, 1, 0, 0x1F00u, 3, 0x200);
    io = zeros(io, 2);                              /* an even run                */
    io = packet(io, 0, 0, 0x0050u, 1, 0x300);
    io = zeros(io, 37);                             /* an odd run                 */
    io = packet(io, 0, 1, 0x1A80u, 2, 0x400);
    io = zeros(io, 9);
    g_handoff_io = io + 4 * 6;                      /* an armed handoff in a run  */
    io = zeros(io, 20);
    io = packet(io, 0, 0, 0x0A00u, 3, 0x500);
    iow(io, 0x20000000u | (CROSS_IO - 0x40u)); io += 4;   /* JUMP before the page end */
    u32 x = zeros(CROSS_IO - 0x40u, 0x40u / 4u + 6u);     /* zeros across the page    */
    x = packet(x, 0, 0, 0x1680u, 2, 0x600);
    x = zeros(x, 300);
    x = packet(x, 1, 0, 0x1E40u, 3, 0x700);
    iow(x, 0x20000000u | x);                        /* park at put                */
    return x;
}

static void walk(int skip, u32 from, u32 put)
{
    g_n = 0;
    s_nop_skip = skip; s_nop_skipped = s_nop_runs = 0;
    s_walk_fast = 0; s_walk_diag = -1;
    s_fifo_getoff = from;
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    vm_write32(ctrl + 0, put);
    vm_write32(ctrl + 4, from);
    cellGcm_rsx_process_fifo();
}
static int same_calls(const call* ref, u32 n_ref)
{
    if (g_n != n_ref) return 0;
    for (u32 i = 0; i < g_n && i < LOG_MAX; i++)
        if (memcmp(&g_log[i], &ref[i], sizeof g_log[i])) {
            printf("  call %u: skip kind %u method 0x%04X arg 0x%X, general kind %u method 0x%04X arg 0x%X\n",
                   i, g_log[i].kind, g_log[i].method, g_log[i].arg, ref[i].kind, ref[i].method, ref[i].arg);
            return 0;
        }
    return 1;
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
    s_ea_address_table[1] = (u16)(FAR_PAGE >> 20);  /* page 1 maps elsewhere */
    const u32 put = build();
    static call slow[LOG_MAX];

    /* 1. The whole stream: the same calls, the same end, fewer steps. */
    walk(0, 0, put);
    const u32 n_slow = g_n; memcpy(slow, g_log, sizeof slow);
    const u32 ring_slow = s_last_ring_off;
    check(s_fifo_getoff == put && n_slow > 15, "the general walk reaches the park");
    check(s_nop_skipped == 0, "off, nothing is stepped over");
    walk(1, 0, put);
    check(s_fifo_getoff == put, "the skipping walk reaches the park");
    check(same_calls(slow, n_slow), "and makes the same calls in the same order");
    check(s_nop_skipped == 1 + 2 + 37 + 9 + 20 + 22 + 300 && s_nop_runs >= 7 && s_nop_runs <= 9,
          "every NOP is stepped over, in about one step per run");
    check(s_last_ring_off == ring_slow, "the ring position is where the general walk leaves it");

    /* 2. The run across the page end: the memory after page 0 is all zero,
     * so a run that read on past the page end would have stepped over the
     * 0x1680 packet in the remapped page and missed its calls (1 caught it). */
    {   int zero_after = 1;
        for (u32 i = 0; i < 64; i++) zero_after &= vm_read32(IO_ADDR + 0x00100000u + 4 * i) == 0;
        int got = 0;
        for (u32 i = 0; i < g_n && i < LOG_MAX; i++) got |= g_log[i].method == 0x1680u && g_log[i].arg == 0x600u;
        check(zero_after && got, "a run stops at the IO page end"); }

    /* 3. Put inside a run: the walk stops exactly at put. */
    const u32 mid = 4u * (4 + 1 + 1 + 1 + 3 + 2 + 1 + 1) + 4u * 13u;   /* inside the 37 */
    walk(0, 0, mid); const u32 n_mid = g_n; memcpy(slow, g_log, sizeof slow);
    const u32 slow_end = s_fifo_getoff;
    walk(1, 0, mid);
    check(slow_end == mid && s_fifo_getoff == mid && same_calls(slow, n_mid), "a run stops at put");

    /* 4. An armed native handoff inside a run: the walk stops on its header,
     * as the general walk does, so the per-word check there still runs. */
    s_native_handoffs[0].phase = 1;
    s_native_handoffs[0].vm_base_identity = (uintptr_t)vm_base;
    s_native_handoffs[0].source.header_ea = io_ea(g_handoff_io);
    atomic_store(&s_native_handoff_count, 1u);
    walk(0, 0, put); const u32 n_h = g_n; memcpy(slow, g_log, sizeof slow);
    const u32 slow_h = s_fifo_getoff;
    walk(1, 0, put);
    check(slow_h == g_handoff_io && s_fifo_getoff == g_handoff_io && same_calls(slow, n_h),
          "a run stops on an armed native handoff's header");
    s_native_handoffs[0].phase = 0;
    atomic_store(&s_native_handoff_count, 0u);

    /* 5. The pass budget bounds a run. */
    walk(0, 0, put); memcpy(slow, g_log, sizeof slow);
    {
        /* (budget is per pass and large; check a run never reads past put
         * when put is the last zero of a run) */
        const u32 last = g_handoff_io - 4u * 6u + 4u * 19u;   /* the last of the 20 */
        walk(0, 0, last); const u32 n_l = g_n; memcpy(slow, g_log, sizeof slow);
        walk(1, 0, last);
        check(s_fifo_getoff == last && same_calls(slow, n_l), "a run ending one word before put stops there");
    }

    /* 6. A per-word diagnostic turns it off. */
    s_nop_skip = -1; setenv("PS3RECOMP_GCM_NOP_SKIP", "1", 1); setenv("GCM_WORD_STATS", "60", 1);
    check(!gcm_nop_skip_on(), "GCM_WORD_STATS keeps every word walked");
    s_nop_skip = -1; unsetenv("GCM_WORD_STATS");
    check(gcm_nop_skip_on(), "PS3RECOMP_GCM_NOP_SKIP=1 turns it on");
    s_nop_skip = -1; unsetenv("PS3RECOMP_GCM_NOP_SKIP");
    check(!gcm_nop_skip_on(), "and it is off by default");

    printf("[gcm_nop_skip] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
