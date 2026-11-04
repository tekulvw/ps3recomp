/* Flip pacing (cellGcmSys.c, PS3RECOMP_GCM_FLIP_PACING).
 *
 * fixed (variable unset): an executed flip completes only on the beat
 * (cellGcmTickFlip); a drain, a status poll or a wait completes nothing.
 * adaptive: an executed flip completes at max(execution, last completion +
 * one period), on whichever poll comes first, and never faster than one per
 * period; the beat itself is only a poll then. Unexecuted (unflushed) flips
 * never complete, and the vblank count and vblank pending bit stay on the
 * ticker in both modes.
 *
 * Built and run by tools/test_gcm_flip_pacing.py: once per mode.
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
/* The rest of the walker's world: no graphics backend, no lifted program. */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
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



#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00400000u
#define CMD_SIZE  0x00080000u
#define TITLE_CB  0x012D6A78u      /* a title function: its own buffer-full callback */
#define LABEL     0x42u
#define VALUE     7u

static uint32_t g_heap = 0x00800000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { vm_write32(ea, v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}


static uint64_t g_now;
static uint64_t fake_clock(void) { return g_now; }
#define MS 1000000ull

/* Queue one stream flip on the title context, flush it, and drain it. */
static void flip_and_drain(u32 ctx, int flush)
{
    (void)_cellGcmSetFlipCommandWithWaitLabel((void*)(uintptr_t)ctx, 1, LABEL, VALUE);
    if (flush) {
        vm_write32(GCM_CONTROL_GUEST_ADDR + 0, gcm_ea2io(vm_read32(ctx + 8)));
        cellGcm_rsx_process_fifo();
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const int adaptive = argc > 1 && !strcmp(argv[1], "adaptive");
    if (adaptive) setenv("PS3RECOMP_GCM_FLIP_PACING", "adaptive", 1);
    else unsetenv("PS3RECOMP_GCM_FLIP_PACING");
    s_flip_clock = fake_clock;
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    s_display_buffer_set[1] = 1;
    const u32 ctx = host_alloc(0x18, 16), buf = IO_ADDR + 0x200000u;
    vm_write32(ctx + 0x0, buf);
    vm_write32(ctx + 0x4, buf + 0x30000u);
    vm_write32(ctx + 0x8, buf);
    vm_write32(ctx + 0xC, ctx + 0x10);
    vm_write32(ctx + 0x10, TITLE_CB);
    vm_write32(ctx + 0x14, 0);
    s_fifo_getoff = gcm_ea2io(buf);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, s_fifo_getoff);
    vm_write32(GCM_LABEL_GUEST_BASE + LABEL * GCM_LABEL_STRIDE, 0);
    (void)GCM_PENDING_TAKE();
    long p;

    /* 1: first flip, executed by the drain at t = 0. */
    g_now = 0;
    cellGcmResetFlipStatus();
    flip_and_drain(ctx, 1);
    p = GCM_PENDING_TAKE();
    if (adaptive) check(p == 2, "adaptive: the first executed flip completes right after the drain");
    else check(p == 0, "fixed: the drain alone completes nothing");
    if (!adaptive) {
        cellGcmTickFlip();
        p = GCM_PENDING_TAKE();
        check(p == 2, "fixed: the beat completes the executed flip");
    }
    GCM_PENDING_SET(p);   /* the checks took the pending bits; hand them back to the pump */
    ppu_gcm_pump();
    check(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE, "the flip status is DONE after delivery");

    /* 2: the next frame's flip executes 5 ms later: within the period. */
    g_now = 5 * MS;
    cellGcmResetFlipStatus();
    flip_and_drain(ctx, 1);
    cellGcmTickFlip();
    (void)cellGcmGetFlipStatus();
    p = GCM_PENDING_TAKE();
    if (adaptive) check(!(p & 2), "adaptive: no completion before one period has passed");
    else check(p == 2, "fixed: the beat completes it whenever it comes");
    if (adaptive) {
        g_now = 16 * MS;
        (void)cellGcmGetFlipStatus();
        check(!(GCM_PENDING_TAKE() & 2), "adaptive: still none at 16.0 ms");
        g_now = 16666667ull;
        (void)cellGcmGetFlipStatus();
        p = GCM_PENDING_TAKE();
        check(p == 2, "adaptive: a status poll completes it at exactly one period");
    }
    GCM_PENDING_SET(p);
    ppu_gcm_pump();
    check(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE, "and it is delivered DONE");

    /* 3: a late frame: the flip executes 40 ms after the last completion and
     * completes at once in adaptive mode, without waiting for a beat. */
    g_now += 40 * MS;
    cellGcmResetFlipStatus();
    flip_and_drain(ctx, 1);
    p = GCM_PENDING_TAKE();
    if (adaptive) check(p == 2, "adaptive: a late flip completes as soon as it is executed");
    else check(p == 0, "fixed: a late flip still waits for the beat");
    if (!adaptive) { cellGcmTickFlip(); check(GCM_PENDING_TAKE() == 2, "fixed: which completes it"); }
    ppu_gcm_pump();

    /* 4: a flip written but not flushed never completes, however long. */
    g_now += 100 * MS;
    cellGcmResetFlipStatus();
    flip_and_drain(ctx, 0);
    cellGcm_rsx_process_fifo();
    cellGcmTickFlip();
    (void)cellGcmGetFlipStatus();
    check(!(GCM_PENDING_TAKE() & 2), "an unflushed flip does not complete");

    /* 5: the vblank beat is untouched: count and pending bit every tick. */
    const u32 v0 = s_vblank_count;
    cellGcmTickVBlank(); cellGcm_request_tick();
    p = GCM_PENDING_TAKE();
    check(s_vblank_count == v0 + 2 && (p & 1), "vblanks advance on the ticker in both modes");

    printf("[gcm_flip_pacing %s] %s (%d failed)\n", adaptive ? "adaptive" : "fixed", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}

void rsx_live_draw_method_at(u32 method,u32 arg,u32 data_ea) { (void)data_ea;rsx_live_draw_method(method,arg); }
