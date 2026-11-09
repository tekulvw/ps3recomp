# Opt-in PPU lift passes: registers in locals, inline stack accesses

`tools/ppu_promote.py` rewrites lifted PPU functions so that the guest registers
each one touches live in C locals instead of the escaped `ppu_context`. The
promoted registers are:

- the GPRs and FPRs;
- CR, XER, LR and CTR.

Vector registers, FPSCR, VSCR and the reservation fields stay in the context.

Both passes are off by default, and the lifter's default output does not change.

## Why

Every guest load and store in a lift calls an out-of-line accessor, and so does
every call. Neither is visible to the compiler. So the compiler has to:

- store every register result back to `ctx`;
- load every operand from `ctx` again after each accessor or call.

A record compare becomes a read-modify-write of `ctx->cr` in memory. With
locals, those values stay in host registers across the accessors.

## Where the context is the truth

The context is written back and reloaded only at sync points:

- a lifted call, with its trampoline drain;
- `ps3_indirect_call`, `lv2_syscall` and `ps3_hle_call`;
- every `return`, including tail trampolines and conditional returns.

Two analyses choose the work at each point:

- A forward may-dirty analysis picks the registers to write back: only those
  whose local may differ from the context.
- A backward liveness analysis picks the registers to reload after a call: only
  those that may be read later, counting the later write-backs as reads.

Guest loads and stores do not touch the context, and neither do the
reservation helpers, so none of them is a sync point. The pass does not
reorder anything that executes: the calls and the guest loads and stores keep
the order of the input text.

The pass leaves a function exactly as it is in these cases:

- it uses `ctx` in any way the pass does not model, such as a hook, a trace
  object or an unknown helper taking `ctx`;
- it contains a preprocessor line;
- it uses nonlocal-return machinery;
- it indexes a register by anything but a literal.

## Inline stack accesses (`--inline-stack`)

This pass rewrites r1-relative guest loads and stores to inline helpers. They
use the gate of the runtime's fast accessors:

- the read or store diagnostics are off;
- the address is in [0x10000, 0xE0000000) and inside the mapping;
- for a store, no dynamic store watch is armed and the line is not marked in
  the coherence bitmap.

Anything else takes the out-of-line accessor.

The diagnostic flags, `vm_base` and the bitmap pointers are sampled into a local
at entry and after every call. A watch that another thread arms therefore takes
effect at this thread's next call. The armed flag and the line's bitmap bit are
read on every store.

## Using it

- Lifter: `--promote-regs [--inline-stack]`. Chunks are still cut by the plain
  line count, so every function stays in the file the plain lift puts it in.
- Already generated directory, after any splices and hooks:
  `python3 tools/ppu_promote.py SRC OUT [--inline-stack]`.
  - The tool skips the functions that `provenance.json` names as spliced or
    hooked, and every body it rejects.
  - It records a `transforms` entry and updates the `outputs` digests.

## Known difference

A float loaded with `lfs` and stored again with `stfs` through a promoted local
may keep a signaling NaN's payload bits. The plain lift's host conversion quiets
them. PPC hardware keeps them, so the promoted result is closer to the
architecture. Every other observable difference is a bug.

## Tests

`python3 tools/test_ppu_promote.py` lifts synthetic instruction sequences with
the real lifter and runs the plain and promoted code on the same seeded states.
The sequences cover:

- frames, calls and trampolines;
- conditional indirect calls and system calls;
- counted loops and carry chains;
- FP;
- reservations;
- CR logic;
- stack traffic.

The test compares the guest access log, the context at every call, and the
final context and memory. It runs with and without `--inline-stack`. It also
checks that three mutated promotions are caught:

- a dropped write-back;
- a dropped reload;
- a conditional call treated as always taken.
