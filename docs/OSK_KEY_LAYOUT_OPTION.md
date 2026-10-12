# OSK key-layout configuration

`cellOskDialogSetKeyLayoutOption(u32 option)` is registered under NID `B53C54FA`.
The retail MAG caller sets r3 to3 at122BCC and calls the import stub at15291F4,
returning to122BD4. Its other registers are leftover arguments, not pointers
owned by this single-argument API.

The option selects the ten-key panel (bit0), full keyboard panel (bit1), or both.
The new handler validates a nonempty subset of these bits, retains the setting
atomically, and accepts configuration before a dialog is loaded. Invalid masks
return the published OSK parameter error8002B504 without changing the setting.
The source defaults to the ten-key panel. No guest output, input text or dialog
completion is synthesized by this handler.

Reference semantics: RPCS3's original implementation and constants at
https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellOskDialog.cpp
and cellOskDialog.h. This implements configuration only: the existing SDK's
headless OSK lifecycle and text-entry stub are still separate limitations.

`python3 tools/test_osk_key_layout.py` compiles the original source, generated
NID registration and real PPU HLE dispatcher. It verifies the exact import,
valid/invalid masks, retained-state integrity, the one-u32 register ABI,
pre-load use, no guest writes beyond the normal HLE TOC spill, and register
preservation. The original source failed because the import was absent.
