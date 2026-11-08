/* SDL game-window keyboard fallback for controller port zero (Linux and other
 * non-Apple, non-Windows hosts). Same physical layout as the macOS window pad. */
#ifndef PS3RECOMP_PAD_SDL_KEYBOARD_H
#define PS3RECOMP_PAD_SDL_KEYBOARD_H
#include <stdint.h>
#if !defined(_WIN32) && !defined(__APPLE__) && defined(PS3RECOMP_PAD_SDL_KEYBOARD)
#define PAD_HAS_SDL_KEYBOARD 1
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint16_t buttons;
    uint8_t analog_lx, analog_ly, analog_rx, analog_ry;
    uint8_t window;   /* a game window is publishing; 0 leaves the port alone */
} PS3RecompSDLKeyboardPadState;
/* Window-pump thread, after its events are polled: take one snapshot of the
 * SDL keyboard. Keys count only while `window` has keyboard focus and nothing
 * else (the OSK dialog) owns the keyboard. NULL releases everything. */
void ps3recomp_pad_sdl_keyboard_sample(void* sdl_window, int suppressed);
/* The same step on an explicit SDL scancode state (indexed by SDL_Scancode);
 * window=0 withdraws the keyboard from the port. */
void ps3recomp_pad_sdl_keyboard_publish(const uint8_t* scancodes, int count, int window, int focused,
                                        int suppressed);
/* Any thread: the last published snapshot. */
PS3RecompSDLKeyboardPadState ps3recomp_pad_sdl_keyboard_state(void);
#ifdef __cplusplus
}
#endif
#endif
#endif
