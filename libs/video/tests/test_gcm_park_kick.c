/* PS3RECOMP_GCM_PARK_KICK=1 (cellGcmSys.c): an SPU transfer that lands on the
 * self-JUMP the walker parked on wakes the walker; anything else does not.
 *
 * The walker runs a method, then parks on a jump-to-self (a reserved hole).
 * A transfer elsewhere, or one that ends just before the park, must not take
 * the park; one that covers it takes it exactly once; the next pass, which
 * runs past the filled hole, starts unarmed. With the switch off nothing is
 * armed at all.
 *
 * python3 tools/test_gcm_park_kick.py
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
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00010000u

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

/* An incrementing subchannel 0 packet of `count` arguments seed, seed+1, ... */
static u32 packet(u32 io, int ni, u32 subch, u32 method, u32 count, u32 seed)
{
    iow(io, (ni ? 0x40000000u : 0u) | (count << 18) | (subch << 13) | method);
    for (u32 i = 0; i < count; i++) iow(io + 4 + i * 4, seed + i);
    return io + 4 + count * 4;
}

static void walk(void)
{
    g_n = 0;
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
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    for (int on = 0; on <= 1; on++) {
        s_park_kick = -1;
        if (on) setenv("PS3RECOMP_GCM_PARK_KICK", "1", 1); else unsetenv("PS3RECOMP_GCM_PARK_KICK");
        atomic_store(&s_park_wait_ea, 0u);
        const unsigned long long kicks0 = atomic_load(&s_park_kicks);
        /* A method, then a hole of two 128-byte lines parked at its start;
         * after the hole, another method; put past it. */
        const u32 base = on ? 0x2000u : 0x1000u;
        u32 io = packet(base, 0, 0, 0x1A00u, 1, 0x10);
        const u32 hole = (io + 127u) & ~127u;
        for (u32 a = io; a < hole; a += 4) iow(a, 0);           /* NOPs up to the line */
        iow(hole, 0x20000000u | hole);                         /* park */
        iow(hole + 128u, 0x20000000u | (hole + 128u));
        const u32 after = packet(hole + 256u, 0, 0, 0x1A04u, 1, 0x20);
        s_fifo_getoff = base;
        vm_write32(ctrl + 4, base);
        vm_write32(ctrl + 0, after);
        walk();
        const u32 park_ea = io_ea(hole);
        check(s_fifo_getoff == hole, on ? "on: the walker parks at the hole" : "off: the walker parks at the hole");
        if (!on) {
            check(atomic_load(&s_park_wait_ea) == 0u, "off: nothing is armed");
            gcm_note_spu_put(park_ea, 128);
            check(atomic_load(&s_park_kicks) == kicks0, "off: a fill of the park kicks nothing");
            continue;
        }
        check(atomic_load(&s_park_wait_ea) == park_ea, "on: the park's address is armed");
        gcm_note_spu_put(park_ea + 128u, 128);                 /* the next line */
        gcm_note_spu_put(park_ea - 128u, 128);                 /* ends right before it */
        check(atomic_load(&s_park_kicks) == kicks0 && atomic_load(&s_park_wait_ea) == park_ea,
              "on: transfers that miss the park do not take it");
        /* The SPU fills the hole: NOPs, the second line's park last-but-one,
         * the first word last. */
        for (u32 a = hole + 4u; a < hole + 256u; a += 4) iow(a, 0);
        gcm_note_spu_put(park_ea + 4u, 252);
        check(atomic_load(&s_park_kicks) == kicks0, "on: the rest of the hole does not take it");
        iow(hole, 0);
        gcm_note_spu_put(park_ea, 4);
        check(atomic_load(&s_park_kicks) == kicks0 + 1 && atomic_load(&s_park_wait_ea) == 0u,
              "on: the write that lands on the park takes it, once");
        gcm_note_spu_put(park_ea, 4);
        check(atomic_load(&s_park_kicks) == kicks0 + 1, "on: a second write finds nothing armed");
        walk();
        check(s_fifo_getoff == after && atomic_load(&s_park_wait_ea) == 0u,
              "on: the next pass runs past the filled hole and leaves nothing armed");
        int saw = 0;
        for (u32 i = 0; i < g_n && i < LOG_MAX; i++) saw |= g_log[i].method == 0x1A04u && g_log[i].arg == 0x20u;
        check(saw, "on: and runs the method after it");
    }
    printf("[gcm_park_kick] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
