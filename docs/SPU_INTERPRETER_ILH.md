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

## Motivation and scope

The defect was observed while investigating an interpreted comparison of a
replicated halfword cookie. A valid cookie can be rejected when ILH places the
immediate in only the low half of each word. The instruction semantics and the
synthetic regression below establish that defect independently of any title's
memory layout.

The fixture does not establish native title progression, completed jobs, audio
output, or rendering. No original title bytes or captured memory are included
in this branch.

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
