# Linux and Windows keyboard pad

With the Vulkan backend's SDL2 game window on Linux (`PS3RECOMP_VK_WINDOW=1`),
and with the game window on Windows, the keyboard drives virtual controller
port zero while that window has keyboard focus. Both hosts use one layout, `libs/input/pad_keyboard_layout.h`, which
is also the macOS window pad's (`libs/input/pad_macos.m`). Keys are physical
positions (USB HID usage IDs, as printed on a US keyboard), independent of the
active text layout:

| Keys | PS3 control |
| --- | --- |
| **W / S / A / D** | Left stick up / down / left / right (analog_ly / analog_lx) |
| **I / K / J / L** | Right stick up / down / left / right (analog_ry / analog_rx) |
| **Arrow keys** | D-pad only |
| **X / Z / C / V** | Cross / Square / Circle / Triangle |
| **Return** or **keypad Enter** | START |
| **Space** | SELECT |
| **Q / E** | L1 / R1 |
| **R / T** | L2 / R2 |
| **F / G** | L3 / R3 |

Stick keys give full deflection (0 or 255) and opposing keys cancel to 128.

## With a controller

A controller on port zero does not turn the keyboard off; the two merge, on
Linux, Windows and macOS alike (`pad_keyboard_merge` in `cellPad.c`). The
keyboard's buttons add to the controller's. A stick follows the keyboard while
any of its keys is held, opposing keys included (they hold it centred), and
returns to the controller's stick when they are released. With nothing held the
controller's report is exactly its own, pressure included; a keyboard button
reads full pressure in pressure-enabled reports. Without a controller the
keyboard is the port, as before.
On Linux, Escape keeps its existing meaning (it closes the game window).

## Linux

The window's event-pump thread publishes one atomic snapshot of the bound keys
after each poll; cellPad reads it on guest threads and never calls SDL for the
keyboard. Keys count only while the game window is focused and no OSK dialog
owns the keyboard; a key still held when focus returns or a dialog closes (for
example the Return that accepted the dialog) is ignored until it is released.
Without a game window port zero is left exactly as before. Set
`PAD_NO_KEYBOARD=1` to disable it and `PAD_KEYBOARD_TRACE=1` to log button-mask
changes. The CMake option `PS3RECOMP_PAD_SDL_KEYBOARD` (ON) builds it.

## Windows

The controller backend (XInput, or SDL2 with `PS3RECOMP_PAD_USE_SDL2`) reads
port zero first, and the keyboard merges into what it read. This matters on Windows in particular: an XInput device is often present
without anyone holding it (a virtual pad from Steam Input or remote-desktop and
streaming software, or an adapter that stays plugged in), and it used to turn
the keyboard off entirely. Keys are read with
`GetAsyncKeyState` while a window of this process is in the foreground and the
native OSK dialog does not own input. Each bound position is resolved to the
virtual key it produces under the foreground window's keyboard layout (letters
through their PC/AT scan code; Return, Space and the arrows are the same
everywhere), again whenever that layout changes, so an AZERTY keyboard plays
with the same physical keys. Keypad Enter shares Return's virtual key. Focus
loss releases everything, and, as on Linux, a key still held when focus returns
or the dialog closes is ignored until it is released. Without a controller,
port zero is reported connected whether or not a key is held, as before.
`PAD_NO_KEYBOARD=1` and `PAD_KEYBOARD_TRACE=1` work as on Linux.

This replaces the earlier Windows-only layout (Z/X/A/S face buttons, Q/W
shoulders, 1/2 triggers, Tab SELECT and a left stick that mirrored the d-pad).

## Tests

`python3 tools/test_pad_sdl_keyboard.py` drives synthetic scancode snapshots
through the production publish step and real big-endian `cellPadGetData`
reports: every button and stick key, aliases, simultaneous and opposing
sticks, pressure, focus, the held-key latch, dialog ownership, merging into a
controller (buttons, held and opposing stick keys, focus, release), window loss, the no-window case and the opt-out, plus a real SDL
window sampled under the dummy video driver.

`python3 tools/test_pad_file_focus_windows.py` (Windows, clang-cl) builds the
real Windows `cellPad.c` with deterministic focus, XInput, held-key and
keyboard-layout providers. Its keyboard modes cover every button and stick key,
both sticks, opposing keys, chords, the retired Windows-only keys, arrows not
moving a stick, foreign and missing foreground, the held-key latch across focus
and across the native dialog, digital pressure, merging into an XInput
controller (buttons, each stick, opposing keys, pressure, focus, release) and
unplugging, physical positions under an AZERTY layout and a layout switch, and
`PAD_NO_KEYBOARD` with and without a controller.
