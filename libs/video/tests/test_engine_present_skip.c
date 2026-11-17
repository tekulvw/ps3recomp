/* PS3RECOMP_RSX_PRESENT_SKIP_UNCHANGED: a present of the surface the previous
 * present showed, with no colour write into it since, is skipped; every other
 * present reaches the backend. Off, every present does. Synthetic engine and
 * stub backend; no title data.
 *
 * python3 tools/test_engine_present_skip.py */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main

static int presents_for(void (*step)(void))
{
    const int before = stub.n_present;
    step();
    return stub.n_present - before;
}
static void host_present(void) { rsx_draw_engine_present(); }
static void five_presents(void) { for (int i = 0; i < 5; i++) rsx_draw_engine_present(); }
static void draw_then_present(void) { draw_triangle(); rsx_draw_engine_present(); }
static void clear_then_present(void) { m(M_CLEAR_BUFFERS, 0xF0u); rsx_draw_engine_present(); }
static void guest_write_then_present(void)
{
    memset(&g_written, 0, sizeof g_written);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, 0x40000u + 3u * 1024u, 1024u, 4u, 4u);
    CHECK(g_written.n == 1, "(the guest write reached the shown surface)");
    rsx_draw_engine_present();
}
static void other_surface_then_present(void)
{
    /* Draw into another target: the displayed surface is unchanged. */
    m(M_COLOR_A_OFFSET, 0x60000u); draw_triangle(); m(M_COLOR_A_OFFSET, 0x40000u);
    rsx_draw_engine_present_buffer(0);
}
static void flip_to_other_buffer(void) { rsx_draw_engine_present_buffer(1); }

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;

    /* Off: every present reaches the backend. */
    unsetenv("PS3RECOMP_RSX_PRESENT_SKIP_UNCHANGED");
    engine_up();
    draw_triangle();
    CHECK(presents_for(five_presents) == 5, "off: five presents of an unchanged frame are five presents");
    engine_down();

    setenv("PS3RECOMP_RSX_PRESENT_SKIP_UNCHANGED", "1", 1);
    engine_up();
    m(M_COLOR_A_PITCH, 1024u);                 /* 256 pixels of A8R8G8B8 */
    rsx_draw_engine_set_display_buffer(0, RSX_LOCATION_LOCAL, 0x40000, 1024, 256, 256);
    rsx_draw_engine_set_display_buffer(1, RSX_LOCATION_LOCAL, 0x60000, 1024, 256, 256);
    draw_triangle();
    CHECK(presents_for(host_present) == 1, "on: the first present of a frame is shown");
    CHECK(presents_for(five_presents) == 0, "repeat presents of the same image are skipped");
    CHECK(presents_for(draw_then_present) == 1, "a draw into the shown surface makes the next present show it");
    CHECK(presents_for(clear_then_present) == 1, "so does a clear");
    CHECK(presents_for(host_present) == 0, "and again nothing changed");
    CHECK(presents_for(guest_write_then_present) == 1, "guest pixels written into a colour target are shown");
    CHECK(presents_for(other_surface_then_present) == 0,
          "drawing into another target leaves the shown one unchanged (buffer 0 is the surface just shown)");
    CHECK(presents_for(flip_to_other_buffer) == 1, "a flip to the other buffer shows it");
    const u32 shown = presented_surface;
    CHECK(presents_for(flip_to_other_buffer) == 0 && presented_surface == shown, "and repeating it is skipped");
    engine_down();
    unsetenv("PS3RECOMP_RSX_PRESENT_SKIP_UNCHANGED");

    printf("[engine_present_skip] %s (%d failed)\n", g_failures ? "FAIL" : "PASS", g_failures);
    free(vm_base);
    return g_failures ? 1 : 0;
}
