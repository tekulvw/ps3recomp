# Correct ILH halfword replication in the SPU interpreter

Base: `5244db92669b54831c6d3ebb7a96d58c4dbfa6df`.
Branch: `fix/spu-interpreter-ilh`.

The interpreter grouped `SPU_ilh` with `SPU_il` and replicated its immediate as
four 32-bit words. ILH instead replicates the unsigned 16-bit immediate into
eight halfwords. For example, `ilh $80,0xBEEF` must produce four words of
`BEEFBEEF`, not `0000BEEF`.

The fix gives ILH its own switch case using the existing `spu_ilh` helper.
IL, ILHU, decode tables, control flow, stop behavior, and generated lifts are
unchanged. The existing helper already uses `spu_splat_u16`; the independent
[RPCS3 ILH implementation](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/SPUInterpreter.cpp#L1596-L1599)
also replicates 16-bit lanes, while its IL uses signed 32-bit replication and
its ILHU shifts into the upper half of each word.

## Native evidence motivating the regression

A retained MAG engine local-store dump from
`retail-visible-spu-handoff-20261011` identifies a BRB audio job on workload 11,
SPURS instance `0x01D39500`, queue data `0x02001C00`. It is separate from the
renderer workload 6 whose completion marker was also being investigated.

- Retail engine ELF SHA-256:
  `0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c`.
- Local-store dump SHA-256:
  `0bb74cdbdd3b52d18f23ff195071eb384672e3891b7181ddc08873f5203b6fe1`.
- The entire initialized module at LS `[0x4000,0x26A00)` is byte-identical to
  the engine ELF slice at file offset `0x019EB200`, guest EA `0x019FB200`.
  Its `0x22A00` bytes hash to
  `7593f9d82d2d8e6d7d488d8d8a711fddfb84b99da88e20de41b10d374de85c3f`.
- LS `0x1EEE0`, original word `0x41DF77D0`, is `ilh $80,0xBEEF`.
- Actual speaker bytes at LS `0x3CC08` contain `BEEFBEEF`; the captured
  comparison operand r65 also contains `BEEFBEEF`. Captured r80 instead
  contains `0000BEEF` in all four words.
- `ceq` at LS `0x1EEF8` therefore falsely rejects the valid cookie.
  The guest assertion at `brb_pluginpack_main_ps3_wwsjm-spu.cpp:233` reports
  `pspk->cookie == brbSpeakers::ChocolateCookie` and bad speakers data.
- The assertion executes original `stopd` word `0x28000000` at LS `0x1EBA8`.
  The inherited zero-stop-code adapter path then reports an unresolved branch
  at the next PC `0x1EBAC`. That diagnostic does not mean the guest branched to
  a missing function there.

The source defect and captured comparison establish the false audio assertion.
They do not establish that correcting ILH resolves the renderer's wait or
produces a rendered frame. No native title execution is part of this branch's
validation, and no game bytes or captured memory are included in the branch.

## Regression and validation

Run `python3 tools/test_spu_interpreter_ilh.py` on a POSIX C toolchain.
It compiles the real interpreter and runs synthetic immediate-load bytes,
including exact instruction word `41DF77D0`. For immediates `0000`, `0001`,
`7FFF`, `8000`, `BEEF`, and `FFFF`, it verifies all 16 result bytes against
literal independently derived vectors and against the existing lifted helper.
IL and ILHU are controls for sign extension and upper-half placement. The
neighboring register must remain unchanged. External channel hooks abort if
unexpectedly called; these instructions require no channels or guest services.

On the unmodified baseline, 6 of the 19 vector checks fail, all ILH cases;
all IL and ILHU controls pass. With the one-opcode correction, all 19 pass.
The runner also compiles and runs the existing interpreter self-test covering
arithmetic, local-store writes, a backward loop, and a drain stop address.
Each child has a five-second timeout and temporary working directory, with
behavior-changing diagnostic environment variables cleared.

The complete 18-script tool suite, including the new runner, passes with the
change (`CI=1`, every `tools/test_*.py`).
