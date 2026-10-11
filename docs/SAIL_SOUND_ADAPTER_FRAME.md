# Sail sound-adapter empty polling

The sound adapter now records initialization by guest effective address.
GetFrame returns `CELL_SAIL_SYNC_NO_FRAME` (zero) for a valid initialized adapter
and writes exactly 24 zero bytes: buffer, session ID, tag, status and 64-bit PTS.
No decoded buffer, samples, playback event or callback is fabricated.

## Evidence

The [RPCS3 Sail header](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.h)
defines `CellSailSoundFrameInfo` with four 32-bit fields followed by a 64-bit PTS.
Its synchronization flags distinguish NO_FRAME=0 from frame-bearing values
starting at 0x100. The
[RPCS3 implementation](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.cpp)
provides the three-argument GetFrame signature `(self, u32 samples, info)`,
initialization validation and a samples>2048 rejection. Its incomplete GetFrame
returns zero without initializing the output; this implementation clears the
empty output so callers do not consume stale tags. Zero samples remains accepted:
no evidence establishes a positive lower bound.

Original MAG v1.0 ELF instructions were rechecked from SHA256
`0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c`.
At `003CFD50..6C`, retail passes its adapter at object+0x480, requested samples
and a stack output at SP+0x88 to stub `0152A374` (NID `7EB8D6B5`).
The signed comparison at `003CFD74` branches to `003CFF80` for values <=255,
skipping frame consumption. That path still reads the tag at SP+0x90.
The native no-Sail diagnostic reached this call with 256 samples; that diagnostic
setting is not made a runtime default by this change.

## Scope and policies

- Duplicate initialization and polling/finalizing an uninitialized adapter return
  exact Sail INVALID_STATE `80610702`; invalid guest spans and samples>2048
  return INVALID_ARG `80610701`. Error paths leave output untouched.
- Initialize retains the existing 0x100-byte guest clearing behavior, but validates
  the whole span before touching it. Its callback arguments remain unused; the
  backend does not call makeup, cleanup or format-change functions.
- Finalize removes the host record; Term clears the table. Finalize/reinitialize
  and slot reuse are local empty-backend lifecycle policies, not a claim that
  RPCS3's incomplete registered-adapter lifecycle is a complete firmware model.
- The 16-entry table is a bounded host limit, not a documented firmware capacity;
  exhaustion returns MEMORY `806107F0` without modifying the guest object.
  A shared C11 atomic lock serializes this adapter table's lifecycle and polling.
- Player attachment, preferred formats, synchronization, full guest adapter
  layout, decoding and playback are unchanged. No new unsupported APIs are
  silently advertised; SoundAdapterUpdateAvSync is outside this patch.
- New paths use accurate local error values instead of changing the historical
  aliases used by unrelated Sail APIs. Bounds arithmetic widens before addition;
  the existing VM convention uses ppu_vm_size=0 for a full 4 GiB guest space.

## Verification

`python3 tools/test_sail_sound_adapter.py` compiles real Sail, generated NID
registration and the actual PPU HLE bridge. It first failed because GetFrame was
absent. The 62 checks cover empty status, exact output and surrounding canaries,
initialization/finalization/Term, two independent adapters, zero and maximum sample
counts, overflowed sample counts and guest spans, full-table failure and reuse.
The complete `tools/test_*.py` suite also passes before commit.
