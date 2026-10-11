# Sail allocator argument and callback lifetime

The local PAMF DescriptorOpen path must use real guest allocation callbacks after the initializing function returns. The initializer now preserves its observed third argument, and PlayerInitialize2 retains the callback identities and argument in player-owned host state. The public eight-byte `{callbacksEA,pArg}` layout remains unchanged. No media events, frames, or synthetic allocations are introduced.

## Evidence

Original MAG v1.0 `mage_g.elf` SHA-256 `0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c` establishes:

- InitSail entry `0x3C7794` allocates a 0x3C0-byte stack frame; return `0x3C781C` restores it.
- `0x3C7914` sets r25 to the movie object. `0x3C799C/0x3C79B8` sets r3 to movie+0x74, the allocator record.
- `0x3C79BC` sets r4 to SP+0x78, a temporary callback table; `0x3C79C4` explicitly sets r5 to the movie object.
- `0x3C79E0/E4` writes alloc/free OPDs to SP+0x78/+0x7C; `0x3C79F4` calls MemAllocatorInitialize, NID346EBBA3.
- `0x3C7A24` calls PlayerInitialize2 with this allocator before the stack table leaves scope.
- Using the function's TOC `0x01B35B20`, callback OPDs resolve to `0x01AE9198` (allocation code `0x003CE224`) and `0x01AE9150` (free code `0x003BFD90`). Both consume r5 as the allocation size or memory pointer. These particular MAG callbacks ignore the context argument.

The native failing capture recorded allocator01FB97D4, callbacks0FEF78B8, alloc0, free01B25B30 at DescriptorOpen. The callback-table EA equals the expired InitSail stack location, not stable code storage. The previous implementation reread that table on every Open.

[RPCS3 cellSail.h](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.h) supplies the eight-byte allocator/table layouts and allocation/free callback signatures `(pArg,boundary,size/memory)`. [RPCS3 cellSail.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.cpp) has a two-argument initializer and copies the allocator record by value into the player. That incomplete implementation does not establish the third argument or deep callback lifetime. The retail register setup independently establishes the third argument. Snapshotting OPDs at PlayerInitialize2 is this backend's bounded lifetime policy, not a claim about firmware's internal storage layout.

## Behavior and limits

MemAllocatorInitialize validates both eight-byte guest ranges before writing callbacksEA and pArg. PlayerInitialize2 validates a supplied allocator, callback-table range, and both OPD ranges/nonzero entries before mutating player memory or registering it. It snapshots callback identities plus pArg; the table and allocator record may then be reused. DescriptorOpen revalidates the retained guest OPDs before calling them; OPD memory itself must remain valid. Each successful allocation retains its free callback and argument until release.

Null allocator preserves the existing registration-only player behavior. It cannot supply storage for DescriptorOpen, which fails explicitly. A reused player slot initializes all new allocator fields, preventing stale callback reuse. The existing descriptor-operation gate serializes this capture with descriptor operations; this change does not solve unrelated legacy player capacity, duplicate-initialization, or guest-memory concurrency limitations.

## Regression

The actual generated HLE fixtures first reproduced both failures: the initializer dropped nonzero r5, and Open failed after callback-table memory was overwritten. The revised fixtures verify exact public layout/canary, nonzero callback argument, retained allocation/free identity after overwriting table and allocator, malformed allocator/table/OPD bounds with no partial player initialization, real PAMF Open, descriptor cleanup, and player-slot reuse without an allocator. No game assets are committed; the Open runner accepts an optional read-only local PAMF path.

Verification completed: 13 allocator layout HLE checks, 70 DescriptorOpen HLE checks with synthetic and actual THX media, and all 21 source `tools/test_*.py` scripts pass. Independent source/test review found no actionable issue in the observed subset.
