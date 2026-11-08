/* SDL game-window keyboard fallback for controller port zero.
 *
 * The window-pump thread publishes one atomic mask of the bound physical keys
 * (SDL scancodes, independent of the active text layout) after each event
 * poll; cellPad reads that mask on guest polling threads and never calls SDL
 * for the keyboard. The layout matches the macOS window pad: X/Z/C/V cross/
 * square/circle/triangle, Return or keypad Enter START, Space SELECT, Q/E
 * L1/R1, R/T L2/R2, F/G L3/R3, arrows the d-pad, W/A/S/D the left stick and
 * I/J/K/L the right stick (full deflection, opposing keys cancel).
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

static const struct { SDL_Scancode code; uint16_t button; } s_map[] = {
    {SDL_SCANCODE_X, CELL_PAD_CTRL_CROSS}, {SDL_SCANCODE_Z, CELL_PAD_CTRL_SQUARE},
    {SDL_SCANCODE_C, CELL_PAD_CTRL_CIRCLE}, {SDL_SCANCODE_V, CELL_PAD_CTRL_TRIANGLE},
    {SDL_SCANCODE_RETURN, CELL_PAD_CTRL_START}, {SDL_SCANCODE_KP_ENTER, CELL_PAD_CTRL_START},
    {SDL_SCANCODE_SPACE, CELL_PAD_CTRL_SELECT},
    {SDL_SCANCODE_Q, CELL_PAD_CTRL_L1}, {SDL_SCANCODE_E, CELL_PAD_CTRL_R1},
    {SDL_SCANCODE_R, CELL_PAD_CTRL_L2}, {SDL_SCANCODE_T, CELL_PAD_CTRL_R2},
    {SDL_SCANCODE_F, CELL_PAD_CTRL_L3}, {SDL_SCANCODE_G, CELL_PAD_CTRL_R3},
    {SDL_SCANCODE_LEFT, CELL_PAD_CTRL_LEFT}, {SDL_SCANCODE_RIGHT, CELL_PAD_CTRL_RIGHT},
    {SDL_SCANCODE_DOWN, CELL_PAD_CTRL_DOWN}, {SDL_SCANCODE_UP, CELL_PAD_CTRL_UP},
    /* Stick keys have no digital-button alias. */
    {SDL_SCANCODE_W, 0}, {SDL_SCANCODE_S, 0}, {SDL_SCANCODE_A, 0}, {SDL_SCANCODE_D, 0},
    {SDL_SCANCODE_I, 0}, {SDL_SCANCODE_K, 0}, {SDL_SCANCODE_J, 0}, {SDL_SCANCODE_L, 0},
};
#define KEY_COUNT (sizeof(s_map) / sizeof(s_map[0]))
_Static_assert(KEY_COUNT <= 32, "physical key mask capacity");

static _Atomic(uint32_t) s_keys;  /* published: bound keys that count */
static _Atomic(int) s_window;     /* published: a game window owns the port */
static uint32_t s_blocked;        /* pump thread: held across a focus/owner change */

static uint32_t slot(SDL_Scancode code)
{
    for (unsigned i = 0; i < KEY_COUNT; i++)
        if (s_map[i].code == code) return 1u << i;
    return 0;
}

void ps3recomp_pad_sdl_keyboard_publish(const uint8_t* scancodes, int count, int window, int focused,
                                        int suppressed)
{
    atomic_store(&s_window, window != 0);
    uint32_t held = 0;
    for (unsigned i = 0; scancodes && i < KEY_COUNT; i++)
        if ((int)s_map[i].code < count && scancodes[s_map[i].code]) held |= 1u << i;
    if (!focused || suppressed) {
        s_blocked = held;
        atomic_store(&s_keys, 0);
        return;
    }
    s_blocked &= held;            /* a release ends the block for that key */
    atomic_store(&s_keys, held & ~s_blocked);
}

void ps3recomp_pad_sdl_keyboard_sample(void* window, int suppressed)
{
    if (!window) { ps3recomp_pad_sdl_keyboard_publish(NULL, 0, 0, 0, 1); return; }
    int count = 0;
    const Uint8* state = SDL_GetKeyboardState(&count);
    ps3recomp_pad_sdl_keyboard_publish(state, count, 1, SDL_GetKeyboardFocus() == (SDL_Window*)window,
                                       suppressed);
}

static uint8_t axis(uint32_t keys, SDL_Scancode negative, SDL_Scancode positive)
{
    int direction = !!(keys & slot(positive)) - !!(keys & slot(negative));
    return direction < 0 ? 0 : direction > 0 ? 255 : 128;
}

PS3RecompSDLKeyboardPadState ps3recomp_pad_sdl_keyboard_state(void)
{
    PS3RecompSDLKeyboardPadState state = {0, 128, 128, 128, 128, 0};
    state.window = (uint8_t)atomic_load(&s_window);
    uint32_t keys = atomic_load(&s_keys);   /* one snapshot for buttons and sticks */
    for (unsigned i = 0; i < KEY_COUNT; i++)
        if (keys & (1u << i)) state.buttons |= s_map[i].button;
    state.analog_lx = axis(keys, SDL_SCANCODE_A, SDL_SCANCODE_D);
    state.analog_ly = axis(keys, SDL_SCANCODE_W, SDL_SCANCODE_S);
    state.analog_rx = axis(keys, SDL_SCANCODE_J, SDL_SCANCODE_L);
    state.analog_ry = axis(keys, SDL_SCANCODE_I, SDL_SCANCODE_K);
    return state;
}
#endif
