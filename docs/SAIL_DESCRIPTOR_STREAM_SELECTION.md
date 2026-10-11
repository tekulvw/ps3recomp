# Sail descriptor stream selection

This branch adds the bounded PAMF metadata-selection form exercised by MAG retail v1.0. It depends on DescriptorOpen (`b367841`) and its PAMF reader and allocator dependencies. It does not start playback, decode media, supply frames, or generate events.

## Evidence and supported form

[RPCS3 cellSail.h](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.h) defines ES audio as 0, video as 1, and user data as 2. [RPCS3 cellSail.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.cpp) names SetEs and DestroyDatabase but leaves both as unimplemented functions without argument declarations. It cannot establish their full contracts.

Original MAG retail `mage_g.elf`, SHA-256 `0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c`, supplies the observed argument evidence:

- `0x3C9AE0`: SetEs NID `0D0C2F0C` receives descriptor, category 0 (audio), zero, one, and a 128-byte PAMF reader at SP+0x4B8. Immediately beforehand, PAMF selects audio category 21; THX's absolute audio stream index is 1.
- `0x3C9B38`: SetEs receives descriptor, category 1 (video), zero, one, and the same reader, now selected to video category 20 / absolute index 0.
- `0x3C9B54` and `0x3D294C`: DestroyDatabase NID `28336E89` receives descriptor in r3 and reader in r4. It is not GetMediaInfo (NID `BD1635F4`).

The third argument is represented locally as a destination slot, not a source index: it stays zero for both source indices. The fourth argument is deliberately named `unknownMode`; its firmware meaning has not been established. Only slot zero and mode one are supported. This is an observed compatibility subset, not a claim about the complete firmware API. Neither RPCS3 nor the available prototype DWARF supplied a formal SetEs prototype.

## State and validation

An open PAMF descriptor accepts only audio/video selections backed by its own resident header. The implementation first checks the complete guest reader range and alignment, then invokes the real PAMF reader validation/query to validate the nested tables, selected pointer, and index. The raw stream coding byte must match the requested category. Each successful call snapshots the complete reader and 48-byte stream metadata into the descriptor's separate audio/video records. The snapshots contain pointers only to the descriptor-owned header, never the borrowed caller reader. The current decoder-less backend retains this metadata; no existing playback consumer is claimed.

DestroyDatabase validates the observed reader but neither frees nor erases caller memory. Selected metadata survives the caller reusing that memory. Destroying the descriptor or terminating Sail clears snapshots along with the owned header; reuse of a descriptor slot starts with neither stream selected. Existing descriptor-registration and player-cleanup policies remain unchanged.

The local conservative error policy returns `0x80610701` for invalid handles, reader ranges, mismatched headers or selected categories, and out-of-enum categories; `0x80610702` for an unopened descriptor or descriptor callback reentry; and `0x80610708` for user data, nonzero destination slots, modes other than one, and unsupported containers. These bounded error choices are implementation policy, not undocumented firmware behavior claims. Existing guest-memory concurrency limitations are unchanged: descriptor operations serialize, but another guest thread may still modify memory concurrently.

## Validation

`tools/test_sail_descriptor_stream_selection.py` compiles actual Sail/PAMF code and generated NID registration, then calls through the real PPU HLE bridge. It first failed because SetEs was absent. The regression covers audio index 1 then video index 0 using the same temporary reader, independent retained snapshots, stack reuse after DestroyDatabase, no guest writes/allocation/events, forged index/pointer, wrong descriptor header, high/wrapped/end-of-VM pointers, unsupported forms, and descriptor release/reuse. A test-only translation unit exposes retained host snapshots without adding production accessors. Unexpected guest callbacks abort the fixture.

The runner accepts an optional local PAMF path to validate the same flow using an actual file; no game data is committed. The default synthetic fixture is PAMF0041 with MPEG-2 video and ATRAC audio.

Verification: 72 HLE checks pass with the synthetic fixture and actual THX media; all 22 `tools/test_*.py` scripts pass. The first full run exposed missing link dependencies in two older isolated fixtures; their unreachable PAMF query stubs now abort if unexpectedly called, matching their existing initializer stubs. The real media fixtures link actual PAMF throughout.
