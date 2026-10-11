# Absolute quadword addresses in runtime-generated SPU code

The SMC microstep decoder used `imm16 << 4` for LQA and STQA. Absolute
quadword addresses use `(imm16 << 2) & 0x3fff0`. The incorrect scale could load
a different vector or store into unrelated local-store data, including a guest
stack. This change corrects only that expression; relative and base-displacement
quadword instructions, dispatch, interrupts and stops are unchanged.

The existing full interpreter computes the correct address. Independent primary
references are RPCS3's [LQA/STQA interpreter definitions](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/SPUInterpreter.cpp)
and [spu_ls_target](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/SPUOpcodes.h#L43-L46).
Both absolute operations call the latter with PC zero.

## Reproduction and verification

Run `python3 tools/test_spu_microstep_absolute_quadword.py`. It compiles the real
SPU dispatcher, microstep decoder and interpreter, uses a synthetic registered
continuation, and fails if unrelated host service boundaries are reached. Each
child run has a ten-second timeout. No title binary, firmware, assets or snapshots
are needed or included.

The regression covers both operations at eleven immediate values, including
zero, low alignment bits, signed/high-bit boundaries and `0xffff`. It compares
all register bytes and all 256 KiB of local store against the full interpreter;
a separate literal oracle checks each interpreted operation. Canaries at the
previously computed wrong address detect stray writes. The exact opcode pair
`0x20ffca49` / `0x30ffca49` addresses register 73 at LS `0x3fe50`; the old decoder
instead used LS `0x3f940`.

A second test constructs a generic 76-register save/restore stub at LS `0x3fec0`,
using STQA/LQA for registers 0 through 75 at LS `0x3f9c0` through `0x3fe70`, and
`bi r79` to a synthetic continuation. Every vector has four distinct word values.
It verifies each saved vector, each restored vector, unsaved registers and every
surrounding local-store byte. Checking memory is essential: wrong-address saves
and matching wrong-address loads can restore registers while corrupting data.

On original SDK base `5244db92669b54831c6d3ebb7a96d58c4dbfa6df`, the fixture
completed with 105 failed checks out of 239. With only the address-scale fix,
all 239 checks pass. All 18 `tools/test_*.py` scripts also pass with `CI=1`,
and an independent reviewer reran the focused fixture and reviewed the source,
tests and evidence.

## Runtime evidence and limits

The witness was a private renderer workload snapshot from retail MAG v1.0,
engine ELF SHA-256
`0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c`.
The captured renderer local store SHA-256 was
`3c0ef6a12d52754ebb424f836896bcc7f1038e034b11b4e5fdc4b65f69721bca`.
The snapshot had PC `0x3ff78`, r79 preferred word `0xffffff00`, and a generated
upper-local-store restore stub ending in `bi r79`. Offline replay of those exact
LS/GPR bytes through the real interpreter executed 512 instructions, eight tail
branches and 61 distinct PCs, cycling in `0x3ff00` through `0x3fff0`. No channel or
host service operation was involved in this replay.

The WWS policy saves r76–r79 through statically lifted operations, constructs a
76-register save/restore stub, sets r79 to its continuation, and executes that
stub through the dynamic adapter. The captured r79 equals its saved value at
LS `0x3feb0`, rather than the normal stub continuation. A policy allocation-mask
helper can legitimately produce this value; the snapshot does not establish the
last instruction that wrote it or the complete chain that entered the loop.

The instruction discrepancy and wrong-address memory corruption are reproduced
independently. Their contribution to the title's renderer stall or later audio
assertion still requires a native rerun. This fix does not claim rendering or
complete title startup, and it does not force any guest register or bypass a
condition.
