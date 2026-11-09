/* The walker's plain-method path (PS3RECOMP_GCM_FAST_WALK=1, cellGcmSys.c)
 * against the general per-word path.
 *
 * One synthetic FIFO, walked twice: once by the general path and once with
 * the plain-method path on. The draw engine must receive the same methods,
 * arguments and data addresses in the same order, and the legacy method layer
 * the same calls, either way. The stream has incrementing and
 * non-incrementing packets, a packet whose methods with effects of their own
 * (SET_REFERENCE, report and label DMA contexts) sit between plain ones, a
 * subchannel 1 packet, a 2D packet, a CALL/RET, and a packet that crosses a
 * 1 MB IO page whose second page maps somewhere unrelated -- the plain path
 * steps the data address within a page and must look the next page up.
 *
 * python3 tools/test_gcm_plain_walk.py
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
    iow(io, CALLEE_IO | 2u); io += 4;               /* CALL                      */
    {   u32 c = packet(CALLEE_IO, 0, 0, 0x1740u, 4, 0x900);
        iow(c, 0x00020000u); }                      /* RET                       */
    iow(io, 0x20000000u | CROSS_IO); io += 4;       /* JUMP to the page crossing */
    u32 x = packet(CROSS_IO, 0, 0, 0x1680u, 10, 0xA00);   /* 0xFFFF0..0x100018 */
    x = packet(x, 1, 0, 0x1E40u, 3, 0xB00);
    iow(x, 0x20000000u | x);                        /* park at put               */
    return x;
}

static void walk(int fast)
{
    g_n = 0;
    s_walk_fast = fast; s_walk_diag = -1;
    s_walk_plain_words = 0;
    s_fifo_getoff = 0;
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    vm_write32(ctrl + 4, 0);
    cellGcm_rsx_process_fifo();
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

    walk(0);
    static call slow[LOG_MAX];
    const u32 n_slow = g_n;
    memcpy(slow, g_log, sizeof slow);
    check(s_walk_plain_words == 0, "the general path takes every word when the switch is off");
    check(s_fifo_getoff == put, "the general walk reaches the park");

    walk(1);
    check(s_walk_plain_words > 20, "the plain path takes the ordinary methods");
    check(s_fifo_getoff == put, "the plain walk reaches the park");
    check(g_n == n_slow && n_slow > 30 && n_slow <= LOG_MAX, "both walks make the same number of calls");
    int same = g_n == n_slow;
    for (u32 i = 0; same && i < g_n && i < LOG_MAX; i++)
        if (memcmp(&g_log[i], &slow[i], sizeof g_log[i])) {
            printf("  call %u: plain kind %u method 0x%04X arg 0x%X ea 0x%08X, general kind %u method 0x%04X arg 0x%X ea 0x%08X\n",
                   i, g_log[i].kind, g_log[i].method, g_log[i].arg, g_log[i].ea,
                   slow[i].kind, slow[i].method, slow[i].arg, slow[i].ea);
            same = 0;
        }
    check(same, "and the same calls, in the same order, with the same data addresses");

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

    /* A per-word diagnostic turns the plain path off. */
    setenv("GCM_METHOD_HIST", "60", 1);
    walk(1);
    check(s_walk_plain_words == 0 && g_n == n_slow, "a per-word diagnostic keeps the general path");
    unsetenv("GCM_METHOD_HIST");

    printf("[gcm_plain_walk] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
