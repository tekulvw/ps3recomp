# Sail boot completion parameter

`cellSailPlayerBoot` now forwards its complete 64-bit `userParam` as callback
`arg1` for `PLAYER_CALL_COMPLETED / BOOT` (event major 2, minor 1).
Previously the runtime discarded this parameter and always delivered zero.
The callback's registered player argument remains the first argument. State
notifications continue to receive zero for `arg1`, and the existing sequence
remains transition state, call completion, settled state. Other player calls
continue to use zero completion parameters.

The motivating retail MAG v1.0 image has SHA256
`0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c`.
Original instructions independently rechecked on 2026-10-11 establish:

- `003C7AF4` forms a stack future at `r1+0xB8`; `003C7B14` supplies it in r4
  before Boot at `003C7B18` (import stub `0152A3F4`).
- Callback `003CDB20` receives the packed event in r4, result in r5 and call
  parameter in r6. Its major-2/minor-1 path at `003CDC68` moves low32(r6) to
  r3, sign-extends r5 to r4, and calls FutureSet at `003CDC70`
  (stub `0152A3D4`). Zeroing the parameter loses the actual completion target.

[RPCS3's cellSail implementation](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.cpp)
corroborates the typed Boot parameter and callback/event shape. Its current
Boot implementation also sends zero for this parameter; it is incomplete and
is **not** evidence of the corrected echo behavior. The original retail
caller and callback establish the required parameter relationship here.

The runtime still delivers its existing synchronous completion. This change
neither completes a future directly nor implements decoding, playback, new
events or broader asynchronous lifecycle semantics.

`python3 tools/test_sail_boot_callback.py` compiles actual `cellSail.c`, generated
NID registration, and the real PPU HLE dispatcher. Forty checks cover full-width,
zero and guest-pointer tokens, event order and all callback argument positions,
two players, repeated calls, Stop/Close notification preservation, a destroyed
player, an absent callback and an absent dispatch hook. The regression failed
on the lost completion parameter before the source fix; all 18 SDK regression
scripts pass afterward.
