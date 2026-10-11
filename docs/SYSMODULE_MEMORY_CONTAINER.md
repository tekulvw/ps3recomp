# System-module memory-container selection

`cellSysmoduleSetMemcontainer` (`0xA193143C`) takes one guest `u32` container ID.
The existing HLE module loader omitted this import. Selection now validates the
real runtime container record, not an invented ID or successful allocation.

The accepted default-memory sentinel is `0xFFFFFFFF`. Otherwise the container
must exist and have at least `0x60000` available bytes. Availability is total
minus used; an inconsistent used value above total fails. Invalid, destroyed,
out-of-range, or undersized containers return `0x80012004` and clear the previous
selection. Initialize and Finalize remain no-ops.

The selected ID is retained as host loader metadata and appears in module-load
logs. HLE modules are already compiled into the host, so this selection does not
allocate a PRX image or charge fabricated bytes. The host-only
`ps3_sysmodule_memcontainer()` accessor reports the retained request; it does
not keep a container alive. Destruction and slot reuse may invalidate or replace
its referent. Any future actual PRX allocation must validate the ID again and
perform real accounting. This change does not implement that allocator.

Container creation, destruction, availability queries, size snapshots, and
startup reset share the existing allocator mutex. The selection itself is atomic.
This provides a coherent availability snapshot, not a lifetime reservation.
Existing memory-container syscall argument/output conventions are unchanged.

Primary references, inspected 2026-10-11:

- [RPCS3 cellSysmodule.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSysmodule.cpp):
  SetMemcontainer accepts the invalid-ID sentinel as default memory; otherwise
  checks container existence and a minimum of `0x60000` available bytes. Rejection
  clears the selection. Its initialization/finalization entry points are no-ops.
- [RPCS3 cellSysmodule.h](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSysmodule.h):
  `CELL_SYSMODULE_ERROR_INVALID_MEMCONTAINER` is `0x80012004`.

Run `python tools/test_sysmodule_memory_container.py`. The bounded POSIX fixture
compiles actual sys_memory and cellSysmodule implementations, generated NID
registration, and the real PPU HLE bridge. It checks real create/destroy/reuse,
the exact minimum boundary, availability after use, underflow rejection,
selection clearing/default memory, no allocation side effects, and concurrent
container writers/readers. It requires no title or firmware assets. Windows
explicitly skips the standalone POSIX HLE bridge fixture.
