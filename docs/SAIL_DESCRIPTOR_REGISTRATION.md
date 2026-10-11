# Sail descriptor registration and count

`cellSailPlayerGetDescriptorCount` returns the number of descriptors actually
registered with an initialized player. Creation alone does not register one.
The existing host descriptor table now tracks ownership and registration:
Create produces an unregistered record, Add registers a valid same-player
record once, and Destroy rejects registered records. Invalid owners and handles
return `80610701`; duplicate registration and registered destruction return
`80610702`. Exhausting the existing 16-record host table returns `806107F0`.
The descriptor handle remains the existing index, including valid handle zero;
this change does not pretend those handles are firmware-allocated guest objects.

## Evidence

[RPCS3 cellSail](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.cpp)
separates creation from registration, counts registered descriptors, and rejects
destruction of a registered descriptor. Its removal implementation does not
write the apparent output argument and has incomplete indexing/lifetime code.
It is not adequate evidence for removal ordering, outputs or firmware capacity.
The existing 16-record capacity here is a host runtime limit, not a claimed SDK
limit.

The retail MAG v1.0 ELF SHA256 is
`0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c`.
Original instruction words were rechecked independently: GetDescriptorCount
at `003CCE30` calls `0152A2F4`; `003CCE38..40` tests negative/zero results.
Zero leads to URI construction and CreateDescriptor at `003CD6B4`, descriptor
configuration and AddDescriptor at `003CD6D8`. A positive count skips creation.
Both paths then call OpenStream at `003CCE4C`. The import inventory contains
no RemoveDescriptor (`9897FBD1`). A later cleanup calls DestroyDescriptor at
`003D297C`. This evidence does not establish automatic stream-driven removal;
no completion or removal is synthesized.

## Deliberate bounds and limitations

- RemoveDescriptor now explicitly returns `80610708` (NOT_AVAILABLE) for a
  live player, without output writes or changing registration. Its apparent
  output argument remains unverified; the old successful no-op was misleading.
  Normal release of registered descriptors remains unavailable until removal or
  stream lifecycle semantics are implemented.
- Local player cleanup policy rejects PlayerDestroy while any owned descriptor
  is registered. On success it releases that player's unregistered host records
  before removing the player, preventing orphan records after guest-EA reuse.
  This is a bounded HLE policy, not full firmware lifecycle semantics.
- Create validates player membership, the aligned four-byte output span and
  bytes of the existing bounded URI copy before allocating a record. Failure
  leaves the output unchanged. Null URI and the existing 511-byte prefix limit
  are retained; media decoding, URI parsing and database creation are unchanged.
- New descriptor paths use accurate local errors rather than changing historical
  error aliases for unrelated APIs. Existing synchronous player registry access
  remains; this patch adds no guarantee for concurrent API calls.
- Existing descriptor getter signatures, guest object layout, stream lifetime,
  PlayerInitialize2/PlayerFinalize and descriptor Term behavior, decoder
  availability and playback behavior remain outside
  this change. Accurate registration does not imply a playable stream.

## Regression

`python3 tools/test_sail_descriptor_registration.py` compiles real Sail,
generated NID registration and the PPU HLE bridge. The initial regression failed
because GetDescriptorCount was absent. The 77 checks cover legitimate zero count,
created-versus-registered count, zero-index handles, ownership, duplicate Add,
registered Destroy rejection, unsupported Remove preserving state/output, missing
players, unregistered destruction, player destroy/reuse, invalid output/URI spans,
full-table behavior and unchanged outputs on failure.
