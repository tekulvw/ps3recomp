# Callback-completed Sail futures

This implements only `cellSailFutureInitialize`, `Set`, `Get` and `Finalize` for guest effective addresses, independently of player Boot or decoding. Initialization does not complete a future. Only an explicit `Set` publishes a signed result; `Get` on a completed future returns success and writes that exact 32-bit result in big-endian guest order. Repeated Gets retain the result.

The record is 24 bytes, aligned to eight bytes. Initialize clears only that validated span. The known ABI has mutex ID at +0, condition ID at +4, flags at +8, signed result at +12 and user parameter at +16. Mutex IDs, condition IDs and private flag values are not fabricated: completion and lifecycle live in a locked host table. Set writes only result +12. Finalize removes the host entry and does not erase guest bytes; reinitialization clears the record and starts pending. Sail termination invalidates all host future entries.

## Supported and unsupported behavior

Pending Get returns `CELL_SAIL_ERROR_NOT_AVAILABLE` (0x80610708) for every timeout value, without writing its output. This is an explicit implementation limitation, not a claim that it matches the SDK's pending wait/timeout semantics. There is no wait, forced completion, Reset or IsDone implementation.

Invalid null/alignment/VM-range arguments return INVALID_ARG (0x80610701). Unknown/finalized handles, duplicate initialization and a second Set return INVALID_STATE (0x80610702). These are conservative lifecycle validation choices; SDK behavior for every misuse has not been independently observed. The bounded implementation stores 64 concurrent futures and returns MEMORY (0x806107F0) without guest writes on exhaustion. Get does not return the operation result as its own status: the signed operation result is written through pResult even if negative.

Range checks use 64-bit arithmetic, respect `ppu_vm_size`, and treat zero size as the runtime's full 4-GiB sentinel. They validate VM bounds, not individual mapped-page permissions. The host lock protects lifecycle, completion/result publication and result copy. No host mutex identifiers are exposed as guest IDs.

## Evidence

[RPCS3's public Sail header](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.h) identifies the 24-byte layout and four-argument notification callback. [Its Sail API declarations](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.cpp) identify Set's signed result and Get's 64-bit timeout/output arguments; those implementations are TODOs and do not establish private flags or wait behavior.

The motivating title initializes a stack future, passes its address as Boot userParam, and its guest Boot-completion callback invokes FutureSet(arg1, signed arg0). It then calls Get(timeout=0) and Finalize. This patch supports the completed-before-Get case once the independent Boot callback roundtrip is correct. It neither implements that callback delivery nor fabricates its completion.

## Validation

`python3 tools/test_sail_future.py` generates NID registration from the actual source and links it with the real PPU HLE dispatcher. The fixture exercises callback-shaped Set dispatch, preservation of a negative result, output/record canaries, pending error without writes, lifecycle/reinitialization/termination, table capacity, guest range overflow including the full-VM sentinel, and 2,000 concurrent lifecycles. The callback fixture is synthetic and contains no game code or assets. Windows explicitly skips this standalone POSIX-toolchain fixture.
