# Loaded guest library exports

`sys_prx_register_library` now publishes the exports of a library already in guest memory. It does not create a module, copy an image, execute a start routine, or answer success without registering the table.

The accepted guest descriptor has the established 0x1c layout: size at byte 0, version at +2, attributes at +4, function/variable/TLS counts at +6/+8/+10, and 32-bit big-endian name/NID/address pointers at +16/+20/+24. A zero size uses the same 0x1c layout. Named export libraries require attribute bit 0. The function entries precede variable entries in both arrays. Function addresses identify eight-byte OPDs; variable addresses identify storage whose extent is not provided by this ABI. TLS exports and extended descriptor layouts return `CELL_ENOSYS` with a diagnostic.

This format is corroborated by the SDK's existing `PRXExportEntry` parser in `tools/elf_parser.py` and the primary RPCS3 loader's [registration boundary](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/lv2/sys_prx.cpp) and [function/variable export handling](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/PPUModule.cpp). The runtime implementation is independent code using those format and ABI observations.

The host API in `runtime/prx/prx_loader.h` accepts a stable owner identity, library name, typed `prx_loaded_export` array, and explicit guest address-space size. It copies host metadata after validating the complete batch. Owner and library name conflicts, duplicate `(kind, NID)` pairs, zero or out-of-range addresses, and unaligned OPDs fail. Repeating the identical owner/name/batch is idempotent. A guest descriptor's address supplies its owner identity; unregister removes only that owner.

Storage grows dynamically up to documented safety limits: 65,536 total exports, 4,096 libraries, and 127 characters per library name. Allocation, limit, and validation failures leave the registry unchanged. A read/write lock covers publication, lookup, count, and removal. A reader sees the complete batch or no batch; it cannot see a partially appended table.

`prx_resolve_library_export(name, nid, kind)` distinguishes functions from globals. The existing `prx_resolve_export(nid)` remains the unqualified function-dispatch seam and returns only function OPDs, using the most recently registered library that supplies the function. Removing that library exposes the prior binding. Variable imports must use the typed library lookup and retain their own relocation semantics. TLS relocation is not implemented by this change.

The existing two-word `prx_export` and `prx_module` layouts remain unchanged. `prx_load_module` treats that legacy export array as function OPDs; callers with variable exports publish a separate typed batch. Module loading returns failure when its export batch cannot be published. As before, callers must stop on load failure; the loader does not undo image writes or host function registrations already performed.

The real strict policies `PS3_HLE_UNRESOLVED=halt` in `runtime/ppu/ppu_hle.cpp` and `PS3_SYSCALL_UNRESOLVED=halt` in `runtime/ppu/ppu_loader.cpp` are unchanged from the source base. Missing exports still reach the selected strict failure policy; registration adds no fallback success path.

Run the synthetic registry, guest descriptor, allocation/capacity, compatibility, generated-NID, and concurrency fixture with:

```sh
python3 tools/test_loaded_prx_libraries.py
```

The fixture has no game, firmware, graphics, network, or account inputs. `--sanitize` additionally requests AddressSanitizer and UndefinedBehaviorSanitizer when the host can execute those runtimes. Sanitizer startup is a separate host capability; normal fixture success is not sanitizer evidence.
