/* PS3RECOMP_GCM_SPU_PUT_KICK=1 (cellGcmSys.c): an SPU transfer that writes the
 * control register's put word wakes the FIFO walker; transfers that miss the
 * word, and every transfer with the switch off, wake nothing.
 *
 * python3 tools/test_gcm_spu_put_kick.py
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
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_draw_engine_method_at(u32 method, u32 arg, u32 data_ea) { (void)method; (void)arg; (void)data_ea; }
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

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const u32 put = GCM_CONTROL_GUEST_ADDR;   /* the control register's first word */
    for (int on = 0; on <= 1; on++) {
        s_spu_put_kick = -1;
        if (on) setenv("PS3RECOMP_GCM_SPU_PUT_KICK", "1", 1); else unsetenv("PS3RECOMP_GCM_SPU_PUT_KICK");
        const unsigned long long k0 = atomic_load(&s_spu_put_kicks);
        gcm_note_spu_put(put + 4u, 4);           /* get, right after put */
        gcm_note_spu_put(put - 16u, 16);         /* ends right before put */
        gcm_note_spu_put(put, 0);                /* empty */
        check(atomic_load(&s_spu_put_kicks) == k0,
              on ? "on: transfers that miss the put word wake nothing" : "off: transfers that miss the put word wake nothing");
        gcm_note_spu_put(put, 4);                /* exactly put */
        gcm_note_spu_put(put - 112u, 128);       /* a line that covers it */
        check(atomic_load(&s_spu_put_kicks) == k0 + (on ? 2u : 0u),
              on ? "on: each transfer covering the put word wakes the walker" : "off: a write of the put word wakes nothing");
    }
    printf("[gcm_spu_put_kick] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
