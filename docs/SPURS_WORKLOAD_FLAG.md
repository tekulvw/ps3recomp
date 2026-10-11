# SPURS workload flag guest ABI

`cellSpursGetWorkloadFlag` (NID `C765B995`) writes a four-byte big-endian guest
pointer to the existing 16-byte object at `spurs + 0x60`. Its scalar is at
`spurs + 0x6C`; returning that scalar address would violate the ABI. Both arguments
are guest addresses, even though the C declaration uses pointer types.

The getter checks null arguments and 128-byte SPURS alignment. It does not require
an HLE registry entry, reset the flag, or impose output alignment. This follows
[RPCS3's getter](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSpurs.cpp#L2642).
The host implementation additionally rejects addresses wider than 32 bits, missing
VM backing, and ranges outside the configured VM with policy-module `FAULT`.
It bounds the 16-byte flag object and four-byte output using widened arithmetic;
`ppu_vm_size == 0` means the runtime backs the full 32-bit address space.

Initialization now leaves the object's twelve reserved bytes zero, sets the BE32
flag at `+0x6C` to `FFFFFFFF`, and sets the separate receiver byte at `+0x77` to
`FF`. Previously the initializer wrote eight FF bytes to the reserved portion and
left the scalar and receiver zero. Layout comes from
[RPCS3's structures](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSpurs.h#L278)
and its SPURS initializer.

## Receiver support remains separate

This getter does not implement workload-flag scheduling. RPCS3's
`_cellSpursWorkloadFlagReceiver` validates the workload and exception state, updates
receiver ownership atomically, rejects an occupied receiver with `BUSY` and an
incorrect releasing owner with `PERM`, and resets the scalar when changing
ownership. Its scheduler recognizes a zero flag for the selected receiver.
The existing ps3recomp poller only considers ready counts and signals; receiver
selection and consumption require separate synchronization and scheduling work.
No receiver is selected and no readiness is manufactured by this change.

## Validation

Run `python3 tools/test_spurs_workload_flag.py` with the project's Python 3.10+
toolchain. The POSIX fixture executes the real initializer and getter with real
guest bytes; only thread creation is suppressed. It checks initialization, exact
BE32 output and adjacent sentinels, aliasing to the original flag, a second
instance without registration, null/alignment errors, wide and out-of-bounds
addresses, exact-end boundaries, the full-space convention, and generated NID
registration. The initial regression failed on the old scalar initialization
before production code changed. The fixture does not establish receiver
scheduling or GPU semaphore behavior.
