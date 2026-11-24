/* cellPad's SDL2 GameController backend against real SDL2 with synthetic
 * controllers: SDL's virtual joystick API stands in for a DualSense, a
 * DualShock 4 and an Xbox pad. No physical controller, window or game file is
 * used; tools/test_pad_sdl_controllers.py builds and runs it on every host,
 * Windows included (with PS3RECOMP_PAD_USE_SDL2).
 *
 * Covered: the PlayStation face buttons land on their own PS3 buttons (Cross on
 * Cross, Circle on Circle), sticks and triggers, rumble reaching the
 * controller, hot-plug (ports keep their controller when another leaves, a new
 * one takes the lowest free port, none appears twice), the focus rule
 * (neutral without focus, held buttons wait for a release, the
 * SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS opt-out) and the event queue staying
 * empty when no SDL window drains it. */
static int s_focused = 1;
#define PAD_SDL_FOCUS_OVERRIDE() s_focused
#ifndef PS3RECOMP_PAD_USE_SDL2
#define PS3RECOMP_PAD_USE_SDL2 1
#endif
#include "../cellPad.c"
#undef NDEBUG
#include <assert.h>

static uint8_t s_guest[0x10000];
uint8_t* vm_base = s_guest;
uint32_t ppu_vm_size = sizeof s_guest;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
#if defined(__APPLE__) && !TARGET_OS_IPHONE
PS3RecompMacOSPadState ps3recomp_pad_macos_state(void)
{PS3RecompMacOSPadState s={0,128,128,128,128};return s;}
#endif
#ifdef OSK_NATIVE_HAS_WIN32_HOST
int osk_native_win32_input_active(void) { return 0; }
#endif
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
u32 cellGcm_flip_request_count(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

#if SDL_VERSION_ATLEAST(2, 24, 0)   /* virtual controllers with rumble */

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* A synthetic controller: what the backend asked of its motors. */
typedef struct { int calls; Uint16 lo, hi; } Motors;

/* SDL 2 takes 0 from a virtual Rumble callback as success; SDL 3 under
 * sdl2-compat reads the same int as its bool, so there it must be 1. Which
 * one this SDL wants is probed once (rumble_probe). */
static int s_rumble_success = 0;

static int rumble(void* userdata, Uint16 lo, Uint16 hi)
{
    Motors* m = (Motors*)userdata;
    m->calls++; m->lo = lo; m->hi = hi;
    return s_rumble_success;
}

static void rumble_probe(int device, Motors* m)
{
    SDL_GameController* gc = SDL_GameControllerOpen(device);
    CHECK(gc);
    if (SDL_GameControllerRumble(gc, 1, 1, 0) != 0) {
        s_rumble_success = 1;
        CHECK(SDL_GameControllerRumble(gc, 2, 2, 0) == 0);
    }
    CHECK(SDL_GameControllerRumble(gc, 0, 0, 0) == 0);
    SDL_GameControllerClose(gc);
    memset(m, 0, sizeof *m);
}

static SDL_JoystickID attach(const char* name, Uint16 vendor, Uint16 product, Motors* motors)
{
    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.name = name;
    desc.userdata = motors;
    desc.Rumble = rumble;
    const int device = SDL_JoystickAttachVirtualEx(&desc);
    CHECK(device >= 0);
    rumble_probe(device, motors);
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
    CHECK(id >= 0);
    return id;
}

static SDL_Joystick* joystick_on(int port)
{
    CHECK(s_sdl_controllers[port]);
    return SDL_GameControllerGetJoystick(s_sdl_controllers[port]);
}

static void press(int port, SDL_GameControllerButton b, int down)
{
    CHECK(SDL_JoystickSetVirtualButton(joystick_on(port), b, (Uint8)down) == 0);
}

static void axis(int port, SDL_GameControllerAxis a, Sint16 v)
{
    CHECK(SDL_JoystickSetVirtualAxis(joystick_on(port), a, v) == 0);
}

/* A virtual controller's triggers span the whole axis range: at rest they
 * are at its minimum. */
#define TRIGGER_REST (-32768)

static void release_axes(int port)
{
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
        axis(port, (SDL_GameControllerAxis)a,
             a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? TRIGGER_REST : 0);
}

static void poll(void) { pad_poll_sdl2(); }

static void test_playstation_layout(void)
{
    poll();
    CHECK(s_host_state[0].connected);
    /* SDL 2 names a virtual Sony pad by its IDs; SDL 3 under sdl2-compat calls every
     * virtual pad virtual. Either way the buttons below are the test. */
    const SDL_GameControllerType type = SDL_GameControllerGetType(s_sdl_controllers[0]);
    CHECK(type == SDL_CONTROLLER_TYPE_PS5 || type == SDL_CONTROLLER_TYPE_VIRTUAL);
    CHECK(s_host_state[0].buttons == 0);

    /* Each face button lands on the PS3 button in the same place. */
    static const struct { SDL_GameControllerButton sdl; u16 cell; } faces[] = {
        { SDL_CONTROLLER_BUTTON_A, CELL_PAD_CTRL_CROSS },
        { SDL_CONTROLLER_BUTTON_B, CELL_PAD_CTRL_CIRCLE },
        { SDL_CONTROLLER_BUTTON_X, CELL_PAD_CTRL_SQUARE },
        { SDL_CONTROLLER_BUTTON_Y, CELL_PAD_CTRL_TRIANGLE },
        { SDL_CONTROLLER_BUTTON_BACK, CELL_PAD_CTRL_SELECT },
        { SDL_CONTROLLER_BUTTON_START, CELL_PAD_CTRL_START },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, CELL_PAD_CTRL_L3 },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK, CELL_PAD_CTRL_R3 },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, CELL_PAD_CTRL_L1 },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, CELL_PAD_CTRL_R1 },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, CELL_PAD_CTRL_UP },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, CELL_PAD_CTRL_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, CELL_PAD_CTRL_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, CELL_PAD_CTRL_RIGHT },
    };
    for (size_t i = 0; i < sizeof faces / sizeof faces[0]; i++) {
        press(0, faces[i].sdl, 1);
        poll();
        CHECK(s_host_state[0].buttons == faces[i].cell);
        press(0, faces[i].sdl, 0);
        poll();
        CHECK(s_host_state[0].buttons == 0);
    }
    press(0, SDL_CONTROLLER_BUTTON_A, 1);
    poll();
    CHECK(s_host_state[0].press_cross == 255 && s_host_state[0].press_circle == 0);
    press(0, SDL_CONTROLLER_BUTTON_A, 0);
    /* The PS button and the touchpad click are not PS3 pad buttons. */
    press(0, SDL_CONTROLLER_BUTTON_GUIDE, 1);
    press(0, SDL_CONTROLLER_BUTTON_TOUCHPAD, 1);
    poll();
    CHECK(s_host_state[0].buttons == 0);
    press(0, SDL_CONTROLLER_BUTTON_GUIDE, 0);
    press(0, SDL_CONTROLLER_BUTTON_TOUCHPAD, 0);

    /* Sticks: SDL's right/down-positive axes, PS3's 0..255 (down = 255). */
    axis(0, SDL_CONTROLLER_AXIS_LEFTX, 32767);
    axis(0, SDL_CONTROLLER_AXIS_LEFTY, -32768);
    axis(0, SDL_CONTROLLER_AXIS_RIGHTX, -32768);
    axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 32767);
    axis(0, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    axis(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -30000);   /* barely pulled */
    poll();
    CHECK(s_host_state[0].analog_lx == 255 && s_host_state[0].analog_ly == 0);
    CHECK(s_host_state[0].analog_rx == 0 && s_host_state[0].analog_ry == 255);
    CHECK(s_host_state[0].trigger_l2 == 255 && (s_host_state[0].buttons & CELL_PAD_CTRL_L2));
    const Sint16 r2 = SDL_GameControllerGetAxis(s_sdl_controllers[0], SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    CHECK(r2 > 0 && r2 <= 3000);
    CHECK(s_host_state[0].trigger_r2 == pad_sdl_trigger_to_u8(r2) && !(s_host_state[0].buttons & CELL_PAD_CTRL_R2));
    release_axes(0);
    poll();
    CHECK(s_host_state[0].buttons == 0 && s_host_state[0].trigger_l2 == 0);
}

static void set_act(u32 port, u8 small, u8 large)
{
    CellPadActParam* p = (CellPadActParam*)(vm_base + 0x100);
    p->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] = small;
    p->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] = large;
    CHECK(cellPadSetActDirect(port, (CellPadActParam*)(uintptr_t)0x100) == CELL_OK);
}

static void test_rumble(Motors* m)
{
    set_act(0, 1, 128);
    CHECK(m->calls == 1 && m->lo == 128 * 257 && m->hi == 65535);
    /* Renewed from the poll (SDL passes an unchanged level on only when it
     * must, so the renewal shows in the backend's own clock). */
    s_rumble[0].sent_ms -= PAD_RUMBLE_RENEW_MS;
    poll();
    CHECK(pad_now_ms() - s_rumble[0].sent_ms < PAD_RUMBLE_RENEW_MS);
    CHECK(m->lo == 128 * 257 && m->hi == 65535);
    set_act(0, 0, 0);
    CHECK(m->lo == 0 && m->hi == 0 && s_rumble[0].sent_ms == 0);
}

static int port_of_id(SDL_JoystickID id)
{
    int found = -1;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_sdl_controllers[i] || s_sdl_instance[i] != id) continue;
        CHECK(found < 0);   /* never on two ports */
        CHECK(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_sdl_controllers[i])) == id);
        found = i;
    }
    return found;
}

static void test_hot_plug(SDL_JoystickID dualsense)
{
    Motors ds4_motors = {0}, xbox_motors = {0};
    const SDL_JoystickID ds4 = attach("Synthetic DualShock 4", 0x054C, 0x09CC, &ds4_motors);
    poll();
    CHECK(port_of_id(dualsense) == 0 && port_of_id(ds4) == 1);
    CHECK(s_host_state[1].connected);
    CHECK(SDL_GameControllerGetType(s_sdl_controllers[1]) == SDL_CONTROLLER_TYPE_PS4 ||
          SDL_GameControllerGetType(s_sdl_controllers[1]) == SDL_CONTROLLER_TYPE_VIRTUAL);
    CHECK(SDL_GameControllerGetPlayerIndex(s_sdl_controllers[1]) == 1);
    press(1, SDL_CONTROLLER_BUTTON_B, 1);
    poll();
    CHECK(s_host_state[1].buttons == CELL_PAD_CTRL_CIRCLE && s_host_state[0].buttons == 0);

    /* A held rumble on a port that loses its controller is dropped. */
    set_act(0, 1, 0);
    CHECK(SDL_JoystickDetachVirtual(0) == 0);   /* the DualSense: device 0 */
    poll();
    CHECK(!s_host_state[0].connected && s_sdl_controllers[0] == NULL && s_rumble[0].sent_ms == 0);
    /* The DualShock 4 is device 0 now; it keeps port 1, and is not opened again. */
    CHECK(port_of_id(ds4) == 1 && s_host_state[1].connected);
    CHECK(s_host_state[1].buttons == CELL_PAD_CTRL_CIRCLE);
    poll();
    CHECK(s_sdl_controllers[0] == NULL);

    /* A new controller takes the lowest free port. */
    const SDL_JoystickID xbox = attach("Synthetic Xbox pad", 0x045E, 0x0B12, &xbox_motors);
    poll();
    CHECK(port_of_id(xbox) == 0 && port_of_id(ds4) == 1);
    CHECK(SDL_GameControllerGetPlayerIndex(s_sdl_controllers[0]) == 0);
    press(0, SDL_CONTROLLER_BUTTON_A, 1);
    poll();
    CHECK(s_host_state[0].buttons == CELL_PAD_CTRL_CROSS && s_host_state[1].buttons == CELL_PAD_CTRL_CIRCLE);
    set_act(0, 0, 255);
    CHECK(xbox_motors.calls == 1 && xbox_motors.lo == 65535 && xbox_motors.hi == 0 && ds4_motors.calls == 0);
    set_act(0, 0, 0);
    press(0, SDL_CONTROLLER_BUTTON_A, 0);
    press(1, SDL_CONTROLLER_BUTTON_B, 0);
    poll();
}

static void test_focus(void)
{
    /* Unfocused: a connected controller at rest. */
    press(0, SDL_CONTROLLER_BUTTON_A, 1);
    axis(0, SDL_CONTROLLER_AXIS_LEFTX, 32767);
    axis(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    s_focused = 0;
    poll();
    CHECK(s_host_state[0].connected && s_host_state[0].buttons == 0);
    CHECK(s_host_state[0].analog_lx == 128 && s_host_state[0].trigger_r2 == 0);
    CHECK(s_host_state[0].press_cross == 0);

    /* Focus returns with Cross and R2 still held: they wait for a release;
     * the sticks are live again at once. */
    s_focused = 1;
    poll();
    CHECK(s_host_state[0].buttons == 0 && s_host_state[0].trigger_r2 == 0);
    CHECK(s_host_state[0].analog_lx == 255);
    press(0, SDL_CONTROLLER_BUTTON_A, 0);
    axis(0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, TRIGGER_REST);
    poll();
    CHECK(s_host_state[0].buttons == 0);
    press(0, SDL_CONTROLLER_BUTTON_A, 1);
    poll();
    CHECK(s_host_state[0].buttons == CELL_PAD_CTRL_CROSS);

    /* SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1: read without focus too. */
    CHECK(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"));
    s_focused = 0;
    poll();
    CHECK(s_host_state[0].buttons == CELL_PAD_CTRL_CROSS && s_host_state[0].analog_lx == 255);
    CHECK(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0"));
    poll();
    CHECK(s_host_state[0].buttons == 0);
    s_focused = 1;
    press(0, SDL_CONTROLLER_BUTTON_A, 0);
    release_axes(0);
    poll();
}

/* PS3RECOMP_PAD_CLAIM_ON_INPUT=1: an idle controller (here a keypad's
 * XInput interface, connected first) never takes port 0 from the one that
 * is played with; each takes a port at its first use. */
static void test_claim_on_input(void)
{
    pad_shutdown_backend();
    while (SDL_NumJoysticks() > 0) CHECK(SDL_JoystickDetachVirtual(0) == 0);
    poll();
    s_sdl_claim_on_input = 1;
    pad_init_backend();

    Motors keypad_motors = {0}, pad_motors = {0};
    const SDL_JoystickID keypad = attach("Synthetic keypad XInput interface", 0x045E, 0x028E, &keypad_motors);
    const SDL_JoystickID dualsense = attach("Synthetic DualSense", 0x054C, 0x0CE6, &pad_motors);
    poll();
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) CHECK(!s_host_state[i].connected);
    CHECK(pad_sdl_waiting_of(keypad) >= 0 && pad_sdl_waiting_of(dualsense) >= 0);

    SDL_Joystick* keypad_js = SDL_JoystickFromInstanceID(keypad);
    SDL_Joystick* pad_js = SDL_JoystickFromInstanceID(dualsense);
    CHECK(keypad_js && pad_js);
    /* Stick noise is not use. */
    CHECK(SDL_JoystickSetVirtualAxis(keypad_js, SDL_CONTROLLER_AXIS_LEFTX, 3000) == 0);
    poll();
    CHECK(port_of_id(keypad) < 0);

    /* Not while another application is in front. */
    CHECK(SDL_JoystickSetVirtualButton(pad_js, SDL_CONTROLLER_BUTTON_B, 1) == 0);
    s_focused = 0;
    poll();
    CHECK(port_of_id(dualsense) < 0);
    s_focused = 1;
    poll();
    /* The press that claims port 0 is reported at once. */
    CHECK(port_of_id(dualsense) == 0 && pad_sdl_waiting_of(dualsense) < 0);
    CHECK(s_host_state[0].connected && s_host_state[0].buttons == CELL_PAD_CTRL_CIRCLE);
    CHECK(SDL_GameControllerGetPlayerIndex(s_sdl_controllers[0]) == 0);
    CHECK(port_of_id(keypad) < 0 && !s_host_state[1].connected);
    set_act(0, 0, 255);
    CHECK(pad_motors.lo == 65535 && keypad_motors.calls == 0);
    set_act(0, 0, 0);

    /* A waiting controller that leaves is closed; ports are unchanged. */
    CHECK(SDL_JoystickDetachVirtual(0) == 0);   /* the keypad: device 0 */
    poll();
    CHECK(pad_sdl_waiting_of(keypad) < 0 && port_of_id(dualsense) == 0);
    CHECK(SDL_JoystickSetVirtualButton(pad_js, SDL_CONTROLLER_BUTTON_B, 0) == 0);
    poll();
    s_sdl_claim_on_input = 0;
}

#endif

int main(void)
{
#if !SDL_VERSION_ATLEAST(2, 24, 0)
    printf("SKIP: SDL %d.%d.%d has no SDL_JoystickAttachVirtualEx (2.24+)\n",
           SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    return 0;
#else
    /* Only the synthetic controllers: no host controller driver. */
    static const char* const off[] = {
        "SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_WGI", "SDL_XINPUT_ENABLED",
        "SDL_DIRECTINPUT_ENABLED", "SDL_JOYSTICK_GAMEINPUT", "SDL_JOYSTICK_MFI", "SDL_JOYSTICK_IOKIT",
        "SDL_JOYSTICK_LINUX_CLASSIC", "SDL_JOYSTICK_DEVICE",
    };
    for (size_t i = 0; i < sizeof off / sizeof off[0]; i++) SDL_SetHint(off[i], "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");

    s_pad_initialized = 1;
    s_max_connect = 4;
    pad_init_backend();
    CHECK(SDL_WasInit(SDL_INIT_GAMECONTROLLER));
    if (SDL_NumJoysticks() != 0) {
        printf("SKIP: %d host controller(s) still visible\n", SDL_NumJoysticks());
        return 0;
    }

    Motors dualsense_motors = {0};
    const SDL_JoystickID dualsense = attach("Synthetic DualSense", 0x054C, 0x0CE6, &dualsense_motors);
    test_playstation_layout();
    CHECK(port_of_id(dualsense) == 0);
    test_rumble(&dualsense_motors);
    test_hot_plug(dualsense);
    test_focus();
    test_claim_on_input();

    /* Nothing drains SDL's event queue here (no SDL window): the backend
     * must not let controller events pile up in it. */
    SDL_PumpEvents();
    poll();
    CHECK(SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_JOYAXISMOTION, SDL_FINGERDOWN - 1) == 0);

    pad_shutdown_backend();
    CHECK(s_sdl_controllers[0] == NULL && s_sdl_controllers[1] == NULL);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    printf("test_pad_sdl_controllers: ok\n");
    return 0;
#endif
}
