# PPU count-register state

CTR is a 64-bit architectural register. `mtctr` and `mtspr 9` preserve the
complete source GPR, and `mfctr` reads all of it. Guest code can use CTR as a
64-bit temporary between these instructions, including while constructing
command words. Truncating it on assignment silently removes high flags.

Counted branches decrement the complete unsigned register modulo 2^64. For
this lifter's 64-bit PPU execution model, `bdnz` and `bdz` test that complete
result. A zero initial value wraps to UINT64_MAX; a count of 0x100000001
becomes 0x100000000, not zero. Ordinary small loop counts behave as before.

Guest branch-address resolution remains separate. The existing jump-table
switch and indirect dispatcher narrow guest addresses where they resolve
32-bit PS3 code addresses; storing data in CTR does not perform that narrowing.

The architectural references are PowerPC User Instruction Set Architecture,
Version 2.02, section 2.3.3 (Count Register), section 2.4.1 (Branch Instructions),
and section 3.3.13 (Move To/From System Register Instructions).

`python tools/test_ppu_ctr64.py` compiles the lifter's actual emitted statements
and checks register round trips, command-flag preservation, counted-branch
borrow/wrap, and the unchanged address-dispatch boundary. It requires a C++
compiler; on Windows it uses the same Visual Studio discovery as the existing
conformance suite. The test uses synthetic values and no game data.
