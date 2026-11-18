/* The FIFO walker's per-word path after the lean-header change (cellGcmSys.c):
 * checked REF read once per pass and again after a stand-down; the per-word
 * diagnostics behind one armed word; the checked-REF packet mapping test by
 * IO page instead of by word.
 *
 * One synthetic FIFO, walked several ways, must give the draw engine and the
 * legacy method layer the same calls in the same order every time:
 *   1. nothing armed, checked REF off (the production form);
 *   2. GCM_WORD_STATS armed (the full per-word form), and GCM_FIFO_OBSERVE
 *      armed, which must still emit its observations;
 *   3. checked REF on, with an unknown word mid-stream: the mode stands down
 *      and the rest of the stream -- including a SET_REFERENCE the mode would
 *      have rejected as malformed -- walks as without it;
 *   4. checked REF on with the stream's page-crossing packet pointing into an
 *      unmapped page: the mode fails the packet ("unmapped-packet") before the
 *      walker enters it, exactly as the word-by-word test did.
 *
 * tools/test_gcm_fifo_lean.py builds this (and the listed mutants, which must
 * fail it). The fixture includes the walker's translation unit by -I order,
 * so a mutated copy can stand in for libs/video/cellGcmSys.c. */
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
#define UNKNOWN_IO 0x00000F00u           /* an unknown word, then a 2-word SET_REFERENCE */

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
static void iow(u32 io, u32 v) { gw32(gcm_io2ea(io), v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}
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

/* Ordinary packets, a long one (200 words, all in page 0), a JUMP to a
 * second segment holding an unknown word followed by a 2-word SET_REFERENCE
 * (malformed under checked REF), then a JUMP to a packet across the page
 * boundary and a park. */
static u32 build(void)
{
    u32 io = 0;
    io = packet(io, 0, 0, 0x1A00u, 8, 0x100);
    io = packet(io, 1, 0, 0x1F00u, 6, 0x200);
    io = packet(io, 0, 0, 0x019Cu, 5, 0x300);
    io = packet(io, 0, 1, 0x1A80u, 2, 0x500);
    io = packet(io, 0, 0, 0x1D6Cu, 1, 0x700);
    io = packet(io, 1, 0, 0x1C00u, 200, 0x800);
    iow(io, 0x20000000u | CROSS_IO); io += 4;
    u32 x = packet(CROSS_IO, 0, 0, 0x1680u, 10, 0xB00);   /* 0xFFFF0..0x100018 */
    x = packet(x, 1, 0, 0x1E40u, 3, 0xC00);               /* entirely in page 1 */
    iow(x, 0x20000000u | UNKNOWN_IO);
    u32 u = UNKNOWN_IO;
    iow(u, 0x60000000u); u += 4;                      /* unknown word: type 3 is no command */
    u = packet(u, 0, 0, 0x0050u, 2, 0x900);           /* SET_REFERENCE, count 2 */
    u = packet(u, 0, 0, 0x0A00u, 3, 0xA00);
    iow(u, 0x20000000u | u);                          /* park at put */
    return u;
}

static void reset(void)
{
    g_n = 0;
    s_fifo_getoff = 0;
    s_fifo_diag = -1;
    s_walk_fast = -1; s_walk_diag = -1;
    atomic_store(&s_checked_ref_off, 0);
    atomic_store(&s_checked_ref_failed, 0);
    memset(&s_fifo_observer, 0, sizeof s_fifo_observer);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    vm_write32(ctrl + 4, 0);
}
static void walk(void) { cellGcm_rsx_process_fifo(); }

static int same_calls(const call* ref, u32 n_ref, const char* what)
{
    int same = g_n == n_ref;
    for (u32 i = 0; same && i < g_n && i < LOG_MAX; i++)
        if (memcmp(&g_log[i], &ref[i], sizeof g_log[i])) {
            printf("  call %u: kind %u method 0x%04X arg 0x%X ea 0x%08X, reference kind %u method 0x%04X arg 0x%X ea 0x%08X\n",
                   i, g_log[i].kind, g_log[i].method, g_log[i].arg, g_log[i].ea,
                   ref[i].kind, ref[i].method, ref[i].arg, ref[i].ea);
            same = 0;
        }
    if (g_n != n_ref) printf("  %u calls, reference %u\n", g_n, n_ref);
    check(same, what);
    return same;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    unsetenv("PS3RECOMP_GCM_CHECKED_REF");
    for (const char* const* n = gcm_fifo_diag_env; *n; ++n) unsetenv(*n);

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    s_ea_address_table[1] = (u16)(FAR_PAGE >> 20);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
    const u32 put = build();
    vm_write32(ctrl + 0, put);

    /* 1. The production form. */
    reset(); walk();
    static call ref[LOG_MAX];
    const u32 n_ref = g_n;
    memcpy(ref, g_log, sizeof ref);
    check(s_fifo_diag == 0, "nothing armed: the diagnostics word resolves to 0");
    check(s_fifo_getoff == put, "the plain walk reaches the park");
    check(n_ref > 230 && n_ref <= LOG_MAX, "the stream exercises the per-word path");
    int ref_seen = 0;
    for (u32 i = 0; i < n_ref; i++) ref_seen |= ref[i].kind == 1 && ref[i].method == 0x50u;
    check(ref_seen, "the 2-word SET_REFERENCE reaches the legacy layer when nothing checks it");

    /* 2. Armed: the full form, same calls; the observer still observes. */
    setenv("GCM_WORD_STATS", "1", 1);
    reset(); walk();
    check(s_fifo_diag == 1, "GCM_WORD_STATS arms the diagnostics word");
    same_calls(ref, n_ref, "the armed form makes the same calls");
    check(s_fifo_getoff == put, "and reaches the park");
    unsetenv("GCM_WORD_STATS");
    setenv("GCM_FIFO_OBSERVE", "0x0-0x400", 1);
    reset(); walk();
    check(s_fifo_diag == 1 && s_fifo_observer.enabled && s_fifo_observer.emitted >= 4,
          "GCM_FIFO_OBSERVE arms the word and the observer emits its observations");
    same_calls(ref, n_ref, "the observed form makes the same calls");
    unsetenv("GCM_FIFO_OBSERVE");
    reset(); walk();
    check(s_fifo_diag == 0 && !s_fifo_observer.enabled && s_fifo_observer.emitted == 0,
          "unarmed again: no observations");

    /* 3. Checked REF stands down at the unknown word and the rest walks as
     *    without it. */
    setenv("PS3RECOMP_GCM_CHECKED_REF", "1", 1);
    reset(); walk();
    check(atomic_load(&s_checked_ref_off) == 1, "checked REF stood down at the unknown word");
    check(atomic_load(&s_checked_ref_failed) == 0, "and did not fail the malformed SET_REFERENCE after it");
    same_calls(ref, n_ref, "the checked walk makes the same calls once it has stood down");
    check(s_fifo_getoff == put, "and reaches the park");

    /* 4. Checked REF rejects a packet into an unmapped page before entering it. */
    s_ea_address_table[1] = 0xFFFFu;
    reset(); walk();
    check(atomic_load(&s_checked_ref_failed) == 1, "a packet crossing into an unmapped page fails checked REF");
    check(s_fifo_getoff == CROSS_IO, "at the packet's header, before any of it was dispatched");
    int crossed = 0;
    for (u32 i = 0; i < g_n && i < LOG_MAX; i++) crossed |= g_log[i].kind == 3 && g_log[i].method == 0x1680u;
    check(!crossed, "none of the rejected packet was dispatched");
    /* Without checked REF the same stream stops at the unmapped word. */
    unsetenv("PS3RECOMP_GCM_CHECKED_REF");
    reset(); walk();
    check(atomic_load(&s_checked_ref_failed) == 0, "unchecked, the same stream does not fail the mode");
    s_ea_address_table[1] = (u16)(FAR_PAGE >> 20);

    printf("[gcm_fifo_lean] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
