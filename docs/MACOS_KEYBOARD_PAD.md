# Native macOS keyboard pad

The native Metal window accepts physical keyboard input for virtual controller
port zero. Click the game window and use this layout:

| Keys | PS3 control |
| --- | --- |
| **W / S / A / D** | **Right stick** up / down / left / right |
| **I / K / J / L** | **Left stick** up / down / left / right |
| **Arrow keys** | D-pad only |
| **X / Z / C / V** | Cross / Square / Circle / Triangle |
| **Return** or **keypad Enter** | START |
| **Space** | SELECT |
| **Q / E** | L1 / R1 |
| **R / T** | L2 / R2 |
| **F / G** | L3 / R3 |

Stick directions produce full deflection (0 or 255), with 128 for neutral.
Both sticks work independently and allow diagonals. Opposing directions on the
same axis cancel to neutral; releasing one restores the other held direction.
The arrow keys do not drive either analog stick. Codes follow the physical
key positions of a US keyboard, independently of the active text layout.

This fills an unoccupied SDL game-controller port; a real controller takes
priority. Keys release when the view or window loses focus, on window close,
or when a Command shortcut begins. Unbound ordinary keys are consumed rather
than passed to Cocoa's default rejection beep. Application Command shortcuts
remain in the standard responder path. No global keyboard monitor is installed.
Set `PAD_NO_KEYBOARD=1` to disable the keyboard fallback and
`PAD_KEYBOARD_TRACE=1` to log button-mask changes at the cellPad polling boundary
(the trace reports digital buttons, not stick movement).

AppKit event handlers update atomic physical-key state on the main thread.
cellPad reads one snapshot for all buttons and both sticks on guest polling
threads without calling AppKit. Pressure-enabled reports retain the existing
digital-button pressure synthesis and big-endian layout. The Windows keyboard
fallback keeps its existing layout.

`python3 tools/test_macos_keyboard_layout.py` checks synthetic native key events
through the production view handlers and real big-endian cellPad reports. It
covers all directions and buttons, two simultaneous sticks, opposing keys and
release, digital pressure, repeated presses, the Return/keypad Enter alias,
focus clear and ignored unfocused presses, SDL controller priority, and opt-out.
It requires macOS and SDL2 and creates no application or window; its focus
source is a test double, while event handling and guest pad polling are real.
The expanded layout regression failed against base
`210db40fa489fc9315abd8f17f83e2ce85ec1ffc` with 35 missing-layout assertions
before implementation; its layout, controller and opt-out modes pass afterward.

`python3 tools/test_macos_window_pad.py` builds and runs the production Metal
window, native input view and real cellPad implementation. It checks native
first-responder routing, real window focus switching, close, Command exclusion,
pressure data and controller arbitration. This test requires a GUI session,
Metal and SDL2 and opens brief test windows. Use `--build-only` to compile the
fixture without launching or activating windows. The original native content
view fails the first-responder assertion before any synthetic key is delivered.
