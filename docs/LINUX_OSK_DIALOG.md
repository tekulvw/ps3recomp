# Native Linux text input

Builds that are neither Apple nor Windows enable `PS3RECOMP_NATIVE_OSK_SDL` by
default. Set the CMake option OFF to retain the previous fallback. Apple, Windows
and the portable test selection are unchanged.

The Vulkan backend's SDL2 game window (`PS3RECOMP_VK_WINDOW=1`) registers itself
with the dialog host from the thread that created it, which is also the thread
that pumps its SDL events. That thread alone creates, draws, feeds and destroys
the dialog: a small separate SDL window with the guest's prompt, its initial
text, OK and Cancel. Guest calls only queue copied UTF-16 requests; the guest's
own CheckCallback pump delivers LOADED/FINISHED/UNLOADED and Unload copies the
bounded big-endian UTF-16 result, as on macOS and Windows.

Enter or OK accepts; Escape, Cancel or closing the dialog window cancels. Text
comes from SDL text input, so keyboard layouts and input methods work; IME
composition is shown but committed only when the IME commits it. Backspace
deletes one character and Ctrl+V (or Shift+Insert) pastes. The guest's length
limit is enforced in UTF-16 code units as text is entered, and a password panel
is drawn masked. The dialog draws printable ASCII with an embedded public-domain
8x8 font (`libs/misc/osk_font8x8.h`) and shows other characters as `?`; the text
returned to the guest is the real text. While a dialog is open it owns the
keyboard: key and text events from the game window are routed to it too, so a
compositor that keeps focus on the game window still lets the user type, and
Escape does not reach the game window's quit handling.

Without a registered window (no display, SDL video failed, or the game window
is not enabled) requests go to the headless mailbox host when
`PS3RECOMP_NATIVE_OSK_HEADLESS` is also built; otherwise the OSK reports
unavailable and says why once on stderr. `OSK_NATIVE_CONTROL_FILE` confirms an
open SDL dialog through the same path as its OK button.

`python3 tools/test_native_osk_sdl.py` builds the host with and without the
headless fallback and runs real SDL windows under the dummy and offscreen video
drivers, plus a private Xvfb server when one is installed. It checks the retail
lifecycle on a separate guest thread, owner-thread-only UI, UTF-16 results,
limits, surrogate pairs, cancel paths, masking, IME composition, prohibited
spaces, mailbox confirmation, abort and window teardown.
