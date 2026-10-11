# Sail graphics adapter polling without a decoder

The media stub already initializes graphics adapters but previously omitted
`cellSailGraphicsAdapterGetFrame` (NID `0x0247C69E`). With strict unresolved-HLE
handling, polling a video adapter stopped execution instead of describing the
backend's actual state.

An initialized adapter now returns `CELL_SAIL_SYNC_NO_FRAME` (zero), clears exactly
the 24-byte guest frame-info record, and supplies no decoded buffer. The result is
a synchronization flag value: frame-bearing statuses start at `0x100`. Zero does
not mean a frame was decoded. Errors leave the caller's record unchanged.

Initialize/Finalize track at most 16 live adapters in host state; duplicate init,
unknown/finalized polls, and invalid guest ranges fail explicitly. Term discards
those records. The private registry is serialized across HLE callers. This is the
current stub lifecycle, not a complete implementation of player attachment,
callback allocation, or firmware's internal adapter representation. Existing
player state changes and callbacks are untouched.

`UpdateAvSync` accepts the adapter, tag, and 64-bit time, but returns
`0x80610708` (`CELL_SAIL_ERROR_NOT_AVAILABLE`) for an initialized adapter. This is
an explicit policy for an unsupported timing backend; it does not acknowledge a
frame's presentation or claim to reproduce firmware's timing algorithm. Its
three argument slots were verified in a retail caller. RPCS3 currently provides
an untyped TODO for this entry, so its full firmware contract remains unverified.
No decoder, fabricated pixels, EOS event, or playback bypass is introduced.

The new paths use ABI error numbers locally because the pre-existing public
Sail error aliases are inconsistent. Correcting all older APIs is separate work.

Primary reference, inspected 2026-10-11:

- [RPCS3 cellSail.h](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.h):
  `CellSailGraphicsFrameInfo` is four 32-bit values plus a 64-bit PTS;
  synchronization flags include NO_FRAME=0 and REPEATED=0x100; error values include
  INVALID_ARG=0x80610701, INVALID_STATE=0x80610702, NOT_AVAILABLE=0x80610708,
  MEMORY=0x806107F0.
- [RPCS3 cellSail.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.cpp):
  GetFrame takes an adapter and output record, but remains a TODO. Its existing
  Initialize/Finalize code is incomplete and is not treated as proof of full
  playback or attachment semantics.

Run `python tools/test_sail_graphics_adapter.py`. The fixture builds actual
`cellSail.c`, the generated NID registration unit, and the real PPU HLE dispatcher.
It checks registration, raw guest address marshalling, the no-frame result and
24-byte output canaries, lifecycle/reinitialization/isolation, explicit unsupported
synchronization, null and overflow ranges, and the last complete VM record. No
proprietary media or title binary is required. Compilation and execution have
separate time limits. Windows explicitly skips the POSIX standalone bridge fixture.
