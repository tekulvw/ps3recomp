# Linux keyboard pad

With the Vulkan backend's SDL2 game window (`PS3RECOMP_VK_WINDOW=1`), the
keyboard drives virtual controller port zero while that window has keyboard
focus and no game controller occupies the port. The layout is the macOS window
pad's (`libs/input/pad_macos.m`), by physical key position (SDL scancodes,
independent of the active text layout):

| Keys | PS3 control |
| --- | --- |
| **W / S / A / D** | analog_ly / analog_lx (the left-stick report fields) |
| **I / K / J / L** | analog_ry / analog_rx (the right-stick report fields) |
| **Arrow keys** | D-pad only |
| **X / Z / C / V** | Cross / Square / Circle / Triangle |
| **Return** or **keypad Enter** | START |
| **Space** | SELECT |
| **Q / E** | L1 / R1 |
| **R / T** | L2 / R2 |
| **F / G** | L3 / R3 |

Stick keys give full deflection (0 or 255) and opposing keys cancel to 128.
Escape keeps its existing meaning (it closes the game window).

The window's event-pump thread publishes one atomic snapshot of the bound keys
after each poll; cellPad reads it on guest threads and never calls SDL for the
keyboard. Keys count only while the game window is focused and no OSK dialog
owns the keyboard; a key still held when focus returns or a dialog closes (for
example the Return that accepted the dialog) is ignored until it is released.
Without a game window port zero is left exactly as before. Set
`PAD_NO_KEYBOARD=1` to disable it and `PAD_KEYBOARD_TRACE=1` to log button-mask
changes. The CMake option `PS3RECOMP_PAD_SDL_KEYBOARD` (ON) builds it.

`python3 tools/test_pad_sdl_keyboard.py` drives synthetic scancode snapshots
through the production publish step and real big-endian `cellPadGetData`
reports: every button and stick key, aliases, simultaneous and opposing
sticks, pressure, focus, the held-key latch, dialog ownership, controller
priority, window loss, the no-window case and the opt-out, plus a real SDL
window sampled under the dummy video driver.
