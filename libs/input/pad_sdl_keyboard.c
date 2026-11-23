/* SDL game-window keyboard fallback for controller port zero.
 *
 * The window-pump thread publishes one atomic mask of the bound physical keys
 * (SDL scancodes, independent of the active text layout) after each event
 * poll; cellPad reads that mask on guest polling threads and never calls SDL
 * for the keyboard. The layout, the held-key latch and the stick arithmetic
 * are pad_keyboard_layout.h's, shared with the Windows game window.
 *
 * Keys count only while the game window has keyboard focus and no dialog owns
 * the keyboard. A key still held when either stops being true (for example
 * Return that accepted the OSK dialog) stays ignored until it is released, so
 * it cannot leak into the game as a press. */
#include "pad_sdl_keyboard.h"
#ifdef PAD_HAS_SDL_KEYBOARD
#include <SDL2/SDL.h>
#include <stdatomic.h>
#include "cellPad.h"
#include "pad_keyboard_layout.h"

/* SDL scancodes are USB HID usage IDs, so the shared layout indexes SDL's
 * keyboard state directly. */
_Static_assert(SDL_SCANCODE_A == PAD_KEY_A && SDL_SCANCODE_C == PAD_KEY_C && SDL_SCANCODE_D == PAD_KEY_D &&
               SDL_SCANCODE_E == PAD_KEY_E && SDL_SCANCODE_F == PAD_KEY_F && SDL_SCANCODE_G == PAD_KEY_G &&
               SDL_SCANCODE_I == PAD_KEY_I && SDL_SCANCODE_J == PAD_KEY_J && SDL_SCANCODE_K == PAD_KEY_K &&
               SDL_SCANCODE_L == PAD_KEY_L && SDL_SCANCODE_Q == PAD_KEY_Q && SDL_SCANCODE_R == PAD_KEY_R &&
               SDL_SCANCODE_S == PAD_KEY_S && SDL_SCANCODE_T == PAD_KEY_T && SDL_SCANCODE_V == PAD_KEY_V &&
               SDL_SCANCODE_W == PAD_KEY_W && SDL_SCANCODE_X == PAD_KEY_X && SDL_SCANCODE_Z == PAD_KEY_Z &&
               SDL_SCANCODE_RETURN == PAD_KEY_RETURN && SDL_SCANCODE_SPACE == PAD_KEY_SPACE &&
               SDL_SCANCODE_RIGHT == PAD_KEY_RIGHT && SDL_SCANCODE_LEFT == PAD_KEY_LEFT &&
               SDL_SCANCODE_DOWN == PAD_KEY_DOWN && SDL_SCANCODE_UP == PAD_KEY_UP &&
               SDL_SCANCODE_KP_ENTER == PAD_KEY_KP_ENTER,
               "SDL scancodes are HID usage IDs");

static _Atomic(uint32_t) s_keys;  /* published: bound keys that count */
static _Atomic(int) s_window;     /* published: a game window owns the port */
static uint32_t s_blocked;        /* pump thread: held across a focus/owner change */

void ps3recomp_pad_sdl_keyboard_publish(const uint8_t* scancodes, int count, int window, int focused,
                                        int suppressed)
{
    atomic_store(&s_window, window != 0);
    uint32_t held = 0;
    for (unsigned i = 0; scancodes && i < PAD_KEYBOARD_BINDING_COUNT; i++)
        if ((int)pad_keyboard_bindings[i].usage < count && scancodes[pad_keyboard_bindings[i].usage])
            held |= 1u << i;
    atomic_store(&s_keys, pad_keyboard_latch(&s_blocked, held, focused && !suppressed));
}

void ps3recomp_pad_sdl_keyboard_sample(void* window, int suppressed)
{
    if (!window) { ps3recomp_pad_sdl_keyboard_publish(NULL, 0, 0, 0, 1); return; }
    int count = 0;
    const Uint8* state = SDL_GetKeyboardState(&count);
    ps3recomp_pad_sdl_keyboard_publish(state, count, 1, SDL_GetKeyboardFocus() == (SDL_Window*)window,
                                       suppressed);
}

PS3RecompSDLKeyboardPadState ps3recomp_pad_sdl_keyboard_state(void)
{
    /* One snapshot for buttons and sticks. */
    PadKeyboardReport report = pad_keyboard_report(atomic_load(&s_keys));
    PS3RecompSDLKeyboardPadState state = {report.buttons, report.analog_lx, report.analog_ly,
                                          report.analog_rx, report.analog_ry, 0};
    state.window = (uint8_t)atomic_load(&s_window);
    return state;
}
#endif
