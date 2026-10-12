# Native macOS keyboard pad

The native Metal window accepts keyboard input for virtual controller port zero.
Click the game window, then use physical **X** (or **Z**) for PS3 Cross,
**Return** (or keypad Enter) for START, and the **arrow keys** for the D-pad.
The D-pad also drives the left analog stick, centered when released.

This fills an unoccupied SDL game-controller port; a real controller takes
priority. Keys release when the view or window loses focus, on window close,
or when a Command shortcut begins. Unbound ordinary keys are consumed rather
than passed to Cocoa's default rejection beep. Application Command shortcuts
remain in the standard responder path. No global keyboard monitor is installed.
Set `PAD_NO_KEYBOARD=1` to disable the keyboard fallback and
`PAD_KEYBOARD_TRACE=1` to log button-mask changes at the cellPad polling boundary.

AppKit event handlers update atomic state on the main thread; cellPad reads it
on guest polling threads without calling AppKit. Pressure-enabled reports
retain the existing digital-button pressure synthesis and big-endian layout.

`python3 tools/test_macos_window_pad.py` builds the production Metal window,
native input view and real cellPad implementation. It exercises Cross/START,
simultaneous and repeated presses, key release, pressure data, D-pad analog
values, real window focus switching, close, Command exclusion and opt-out.
The original native content view fails the first-responder assertion before
any synthetic key is delivered. This test requires macOS, a GUI session,
Metal and SDL2. It opens brief test windows.
