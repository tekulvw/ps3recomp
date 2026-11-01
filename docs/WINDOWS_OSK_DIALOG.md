# Native Windows text input

Windows builds enable `PS3RECOMP_NATIVE_OSK_WINDOWS` by default. Set the CMake
option OFF to retain the previous fallback. Apple and portable test selection
remain unchanged. A host without a registered native window returns unavailable.

The null/live window and legacy D3D12 backends register their HWND and pump a
modeless native text dialog on its owning thread. Guest calls queue copied UTF-16
requests; UI threads never dereference guest memory or deliver guest callbacks.
Native OK/Cancel publishes a host result. The existing guest CheckCallback pump
delivers LOADED/FINISHED/UNLOADED, and the normal guest Unload copies bounded
big-endian UTF-16 results. Abort and stale generations cannot submit another UI.

`PS3RECOMP_OSK_INITIAL_TEXT` only prefills the first eligible non-password field.
It requires ordinary user confirmation. Password controls are secure, and text
is not logged. Guest pad payloads are neutral while OSK owns input; host device
state and report freshness remain intact. Focus-gated mailbox holds are canceled
before neutralization, and do not resume when the dialog closes.

`tools/test_native_osk_windows.py --runtime-library <static-runtime.lib>` drives
real NID/sysutil dispatch and actual native controls on a fresh private Desktop;
it never switches the user's desktop. It checks nonblocking owner-thread UI,
Unicode messages/results/prefill, limits, secure Cancel, abort, parent closure,
worker callers and stale generations. Optional native test control still routes
only through the real OK button; it is off unless `OSK_NATIVE_CONTROL_FILE` is set.
