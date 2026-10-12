# Extended system-module unload

`cellSysmoduleUnloadModuleEx` (`0xFD0E1CAC`) takes one guest `u16` module ID.
Its NID is independently reproduced by `compute_nid` in the SDK. The entry
point now resolves through the normal generated HLE table and changes the
existing module tracker through `cellSysmoduleUnloadModule`.

The Ex policy accepts 51 IDs in the internal namespace `0xF000..0xF056`.
Disallowed IDs return `CELL_SYSMODULE_ERROR_UNKNOWN` (`0x80012002`) without
changing state. A permitted loaded module becomes unloaded and returns
`CELL_OK`. A permitted already-unloaded module returns
`CELL_SYSMODULE_ERROR_UNLOADED` (`0x80012003`). Core `0x0044` and internal
`0xF044` remain separate tracker entries. The one `u16` argument uses the low
16 bits of the PPU argument register.

The shared Ex allowlist includes `0xF020`, `0xF024`, and `0xF03E` for unloading.
Loading those IDs still returns UNKNOWN because the process-PARAM.SFO permission
checks are unsupported. Unloading requires no load-time permission check.
All other existing LoadModuleEx behavior, including idempotent HLE loads, is
preserved by the shared policy helper.

The observed native stop was an unload of internal `0xF044` (NP EULA), rather
than a loader with argc/argv or output parameters. The caller queried loaded
state, set only r3 to `0xF044`, then called its extended unload import. Registers
r4 and onward held unrelated prior values; the HLE must not interpret them as
pointers. This API performs no guest-memory output writes.

This is bounded HLE state tracking. It does not unload a firmware PRX, execute
module finalizers, manage dependency reference counts, free a module image,
change memory-container selection, or claim that every tracked module API is
implemented. The existing tracker and its concurrency behavior are preserved.
Native training progress after this import needs a new game run.

Primary references inspected 2026-10-12:

- [RPCS3 cellSysmodule.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSysmodule.cpp),
  `cellSysmoduleUnloadModuleEx`, `cellSysmoduleUnloadModuleInternal`,
  `LOADABLE_INTERNAL_MODULES_EX`, and its REG_FUNC registration establish the
  symbol, integer argument, accepted-ID policy, and unload operation.
  Its module table maps `0xF044` to `external/libsysutil_np_eula.sprx`.
- [RPCS3 cellSysmodule.h](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSysmodule.h)
  establishes UNKNOWN and UNLOADED error values.
- Local SDK `tools/nid_database.py`, `compute_nid("cellSysmoduleUnloadModuleEx")`
  evaluates to `0xFD0E1CAC`; no manual NID alias is needed.

Run `python tools/test_sysmodule_unload_module_ex.py`. The asset-free fixture
compiles the production sysmodule implementation, actual sys_memory dependency,
normal generated NID table, and production PPU HLE bridge. Its 406 checks cover
query/load/unload transitions, already-unloaded errors, every internal ID,
disallowed loaded IDs retaining their state, namespace separation, u16 ABI,
and unused argument registers with zero, inaccessible addresses, and UINT64_MAX.
Guest-memory canaries allow only the bridge's ABI-required TOC linkage write.

The regression was RED before implementation because `ps3_hle_has(0xFD0E1CAC)`
failed against the generated eight-export table. It is GREEN with nine exports.
The three existing sysmodule regression scripts also pass. A fresh Release
runtime build and CTest run pass (`sync_stress`, `fiber_switch`, 2/2 tests).

A broader exploratory tools sweep was stopped once the scoped checks and
runtime build were available. It was not a complete green suite. These
unrelated fixtures aborted at Metal-device initialization (`s_dev`) in this
execution environment: `test_macos_window_pad`, `test_metal_checked_completion`,
`test_metal_first_nonblack`, `test_metal_frame_rate`,
`test_metal_texture_binding_capture`, `test_native_osk` (native macOS portion),
and `test_rsx_b8_component_remap`. `test_metal_draw_diagnostics` and
`test_metal_present_diagnostics` also reported SIGABRT; their Python wrappers
captured the child stderr, so the logs do not expose the precise assertion.
No baseline comparison was performed, so these are verification limitations,
not attributed regressions. Native window/graphics behavior is not validated
by this change's synthetic dispatch fixture.
