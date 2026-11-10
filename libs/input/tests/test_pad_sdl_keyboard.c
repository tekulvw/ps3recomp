/* SDL game-window keyboard on pad port 0 (Linux): synthetic SDL scancode
 * snapshots through the production publish step and real big-endian
 * cellPadGetData reports. Checks the layout (same as the macOS window pad),
 * both sticks, opposing keys, focus, a dialog owning the keyboard and keys
 * held across it, controller priority, window loss, the headless no-window
 * case and PAD_NO_KEYBOARD. Mode "sdl" also samples a real SDL window.
 *
 * Build: see tools/test_pad_sdl_keyboard.py. */
#include "../cellPad.c"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

#ifndef PAD_HAS_SDL_KEYBOARD
#error "build with -DPS3RECOMP_PAD_SDL_KEYBOARD=1 on a non-Apple, non-Windows host"
#endif

/* A controller double: none attached unless the test plugs one in. */
static int s_fake_gc, s_attached;
void SDL_GameControllerUpdate(void) {}
SDL_bool SDL_IsGameController(int i) { (void)i; return SDL_FALSE; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController* gc) { return gc == (SDL_GameController*)&s_fake_gc && s_attached; }
Uint8 SDL_GameControllerGetButton(SDL_GameController* gc, SDL_GameControllerButton b) { (void)gc; return b == SDL_CONTROLLER_BUTTON_B; }
Sint16 SDL_GameControllerGetAxis(SDL_GameController* gc, SDL_GameControllerAxis a) { (void)gc; (void)a; return 0; }
void SDL_GameControllerClose(SDL_GameController* gc) { (void)gc; }
int SDL_GameControllerRumble(SDL_GameController* gc, Uint16 lo, Uint16 hi, Uint32 ms) { (void)gc; (void)lo; (void)hi; (void)ms; return 0; }

static int s_failures;
static Uint8 keys[SDL_NUM_SCANCODES];

static u16 word(unsigned i) { u16 v; memcpy(&v, vm_base + 0x200 + 4 + 2 * i, 2); return (u16)((v >> 8) | (v << 8)); }

static void report(const char* what, u16 buttons, unsigned lx, unsigned ly, unsigned rx, unsigned ry)
{
    usleep(6000);   /* past PAD_POLL_INTERVAL_MS, so the report is a fresh poll */
    assert(cellPadGetData(0, (CellPadData*)(uintptr_t)0x200) == CELL_OK);
    u16 got = (u16)(word(CELL_PAD_BTN_OFFSET_DIGITAL1) | (word(CELL_PAD_BTN_OFFSET_DIGITAL2) << 8));
    unsigned glx = word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X), gly = word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y);
    unsigned grx = word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X), gry = word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y);
    if (got != buttons || glx != lx || gly != ly || grx != rx || gry != ry) {
        printf("FAIL %s: buttons 0x%04X (want 0x%04X) left %u,%u (want %u,%u) right %u,%u (want %u,%u)\n",
               what, got, buttons, glx, gly, lx, ly, grx, gry, rx, ry);
        s_failures++;
    }
}

static void publish(int window, int focused, int suppressed)
{
    ps3recomp_pad_sdl_keyboard_publish(keys, SDL_NUM_SCANCODES, window, focused, suppressed);
}

static void press(SDL_Scancode code, int down) { keys[code] = (Uint8)down; publish(1, 1, 0); }

static void setup(void)
{
    vm_base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);
    s_pad_initialized = 1;
    s_max_connect = 1;
    memset(s_host_state, 0, sizeof s_host_state);
}

static void test_layout(void)
{
    /* Before any window publishes, port 0 is exactly what the backend made it:
     * a controller-less port is released each poll, buttons up and sticks
     * centred (pad_host_release). */
    report("headless port untouched", 0, 128, 128, 128, 128);
    publish(1, 1, 0);
    report("window, no keys: neutral", 0, 128, 128, 128, 128);

    static const struct { SDL_Scancode code; u16 button; } buttons[] = {
        {SDL_SCANCODE_X, CELL_PAD_CTRL_CROSS}, {SDL_SCANCODE_Z, CELL_PAD_CTRL_SQUARE},
        {SDL_SCANCODE_C, CELL_PAD_CTRL_CIRCLE}, {SDL_SCANCODE_V, CELL_PAD_CTRL_TRIANGLE},
        {SDL_SCANCODE_RETURN, CELL_PAD_CTRL_START}, {SDL_SCANCODE_KP_ENTER, CELL_PAD_CTRL_START},
        {SDL_SCANCODE_SPACE, CELL_PAD_CTRL_SELECT},
        {SDL_SCANCODE_Q, CELL_PAD_CTRL_L1}, {SDL_SCANCODE_E, CELL_PAD_CTRL_R1},
        {SDL_SCANCODE_R, CELL_PAD_CTRL_L2}, {SDL_SCANCODE_T, CELL_PAD_CTRL_R2},
        {SDL_SCANCODE_F, CELL_PAD_CTRL_L3}, {SDL_SCANCODE_G, CELL_PAD_CTRL_R3},
        {SDL_SCANCODE_UP, CELL_PAD_CTRL_UP}, {SDL_SCANCODE_DOWN, CELL_PAD_CTRL_DOWN},
        {SDL_SCANCODE_LEFT, CELL_PAD_CTRL_LEFT}, {SDL_SCANCODE_RIGHT, CELL_PAD_CTRL_RIGHT},
    };
    for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; i++) {
        press(buttons[i].code, 1);
        report(SDL_GetScancodeName(buttons[i].code), buttons[i].button, 128, 128, 128, 128);
        press(buttons[i].code, 0);
        report("button released", 0, 128, 128, 128, 128);
    }
    press(SDL_SCANCODE_RETURN, 1); press(SDL_SCANCODE_KP_ENTER, 1); press(SDL_SCANCODE_RETURN, 0);
    report("START alias still held", CELL_PAD_CTRL_START, 128, 128, 128, 128);
    press(SDL_SCANCODE_KP_ENTER, 0);

    static const struct { SDL_Scancode code; unsigned lx, ly, rx, ry; } sticks[] = {
        {SDL_SCANCODE_W, 128, 0, 128, 128}, {SDL_SCANCODE_S, 128, 255, 128, 128},
        {SDL_SCANCODE_A, 0, 128, 128, 128}, {SDL_SCANCODE_D, 255, 128, 128, 128},
        {SDL_SCANCODE_I, 128, 128, 128, 0}, {SDL_SCANCODE_K, 128, 128, 128, 255},
        {SDL_SCANCODE_J, 128, 128, 0, 128}, {SDL_SCANCODE_L, 128, 128, 255, 128},
    };
    for (unsigned i = 0; i < sizeof sticks / sizeof sticks[0]; i++) {
        press(sticks[i].code, 1);
        report(SDL_GetScancodeName(sticks[i].code), 0, sticks[i].lx, sticks[i].ly, sticks[i].rx, sticks[i].ry);
        press(sticks[i].code, 0);
    }
    press(SDL_SCANCODE_W, 1); press(SDL_SCANCODE_D, 1); press(SDL_SCANCODE_K, 1); press(SDL_SCANCODE_J, 1);
    report("independent simultaneous sticks", 0, 255, 0, 0, 255);
    press(SDL_SCANCODE_S, 1); press(SDL_SCANCODE_A, 1); press(SDL_SCANCODE_I, 1); press(SDL_SCANCODE_L, 1);
    report("opposing directions neutral", 0, 128, 128, 128, 128);
    memset(keys, 0, sizeof keys); publish(1, 1, 0);
    press(SDL_SCANCODE_ESCAPE, 1); press(SDL_SCANCODE_B, 1);
    report("unbound keys ignored", 0, 128, 128, 128, 128);
    memset(keys, 0, sizeof keys); publish(1, 1, 0);

    /* Pressure-enabled reports synthesize full pressure for held buttons. */
    s_port_setting[0] = CELL_PAD_SETTING_PRESS_ON;
    press(SDL_SCANCODE_X, 1);
    report("pressure report", CELL_PAD_CTRL_CROSS, 128, 128, 128, 128);
    assert(word(CELL_PAD_BTN_OFFSET_PRESS_CROSS) == 255);
    press(SDL_SCANCODE_X, 0);
    s_port_setting[0] = 0;
    printf("layout: buttons, both sticks, opposing keys, aliases, pressure\n");
}

static void test_focus_and_dialog(void)
{
    keys[SDL_SCANCODE_X] = 1;
    publish(1, 0, 0);
    report("unfocused window ignores keys", 0, 128, 128, 128, 128);
    publish(1, 1, 0);
    report("focus regained with key held: ignored until released", 0, 128, 128, 128, 128);
    keys[SDL_SCANCODE_X] = 0; publish(1, 1, 0);
    press(SDL_SCANCODE_X, 1);
    report("pressed again after release", CELL_PAD_CTRL_CROSS, 128, 128, 128, 128);
    press(SDL_SCANCODE_X, 0);

    /* A dialog owns the keyboard: nothing reaches the pad, and the Return that
     * accepted it is still held when the dialog goes away. */
    keys[SDL_SCANCODE_W] = 1; keys[SDL_SCANCODE_RETURN] = 1;
    publish(1, 1, 1);
    report("dialog owns the keyboard", 0, 128, 128, 128, 128);
    publish(1, 1, 0);
    report("keys held across the dialog stay ignored", 0, 128, 128, 128, 128);
    keys[SDL_SCANCODE_RETURN] = 0; publish(1, 1, 0);
    report("one released, the other still blocked", 0, 128, 128, 128, 128);
    keys[SDL_SCANCODE_W] = 0; publish(1, 1, 0);
    press(SDL_SCANCODE_RETURN, 1);
    report("fresh press after the dialog", CELL_PAD_CTRL_START, 128, 128, 128, 128);
    press(SDL_SCANCODE_RETURN, 0);
    printf("focus and dialog ownership: unfocused, held-key latch, dialog suppression\n");
}

static void test_controller_and_window_loss(void)
{
    s_sdl_controllers[0] = (SDL_GameController*)&s_fake_gc; s_attached = 1;
    press(SDL_SCANCODE_X, 1);
    report("physical controller wins port 0", CELL_PAD_CTRL_CIRCLE, 127, 127, 127, 127);
    s_attached = 0;
    report("controller unplugged: keyboard fills the port", CELL_PAD_CTRL_CROSS, 128, 128, 128, 128);

    /* The window goes away with X held: released once, then left alone. */
    ps3recomp_pad_sdl_keyboard_sample(NULL, 0);
    report("window loss releases held keys", 0, 128, 128, 128, 128);
    /* No window: keys no longer reach the port, which stays as the backend
     * left it (a controller-less port is released every poll). */
    keys[SDL_SCANCODE_X] = 1;
    ps3recomp_pad_sdl_keyboard_publish(keys, SDL_NUM_SCANCODES, 0, 1, 0);
    report("no window: keys leave the port alone", 0, 128, 128, 128, 128);
    memset(keys, 0, sizeof keys);
    printf("controller priority and window loss\n");
}

static void test_real_sdl_window(void)
{
    assert(SDL_InitSubSystem(SDL_INIT_VIDEO) == 0);
    SDL_Window* window = SDL_CreateWindow("pad", 0, 0, 64, 64, SDL_WINDOW_SHOWN);
    assert(window);
    SDL_PumpEvents();
    ps3recomp_pad_sdl_keyboard_sample(window, 0);
    PS3RecompSDLKeyboardPadState state = ps3recomp_pad_sdl_keyboard_state();
    assert(state.window == 1 && state.buttons == 0 && state.analog_lx == 128 && state.analog_ry == 128);
    report("real SDL window sampled: neutral", 0, 128, 128, 128, 128);
    ps3recomp_pad_sdl_keyboard_sample(NULL, 0);
    assert(ps3recomp_pad_sdl_keyboard_state().window == 0);
    printf("real SDL window (%s driver) sampled on its pump thread\n", SDL_GetCurrentVideoDriver());
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep FAIL lines if an assert aborts */
    const char* mode = argc > 1 ? argv[1] : "layout";
    setup();
    if (!strcmp(mode, "off")) {
        setenv("PAD_NO_KEYBOARD", "1", 1);
        keys[SDL_SCANCODE_X] = 1; publish(1, 1, 0);
        report("PAD_NO_KEYBOARD leaves port 0 alone", 0, 128, 128, 128, 128);
        printf("PAD_NO_KEYBOARD opt-out\n");
    } else if (!strcmp(mode, "sdl")) {
        test_real_sdl_window();
    } else {
        test_layout();
        test_focus_and_dialog();
        test_controller_and_window_loss();
    }
    if (s_failures) { printf("%d failure(s)\n", s_failures); return 1; }
    printf("test_pad_sdl_keyboard %s: ok\n", mode);
    return 0;
}
