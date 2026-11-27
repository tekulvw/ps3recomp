# Depth targets read as textures: from the GPU, not guest memory

This spec describes how the register-file draw engine serves a texture fetch that names the guest
address of one of its depth targets. It covers when the engine samples a GPU copy, when it falls
back to guest memory, and what keeps the guest-visible bytes correct.

## What the engine already tracks

- **Depth targets.** One entry per guest zeta address, holding:
  - the host depth image (location, offset, size, pitch and zeta format);
  - `depth_serial`, the order of the last write or clear;
  - `had_write`: a depth-writing draw ran since the last clear;
  - `guest_dirty`: the host image changed since guest memory last got a copy.
- **Colour targets.** The same, with `color_serial`. A fetch is served from the depth when the
  depth's serial is newer than the colour's at the same address.
- **Recorded guest writes** (`rsx_draw_engine_mark_guest_dirty`). These are transfers and DMA the
  runtime reports. The texture cache re-checks only the entries they overlap.
- **Store on demand.** When a consumer needs the guest bytes of a `guest_dirty` target, the
  engine stores the host image into guest memory in the target's own layout. It does this through
  a GPU readback, or through a GPU Z24 conversion when the backend has one. The consumer can be a
  texture in a format the engine has no GPU path for, or a transfer.

## The rule

A texture fetch of a depth target as DEPTH24_D8 is served from the backend's depth snapshot
(`depth_snapshot`, a GPU copy of the host depth) when all of these hold:

1. **The host depth defines the target's contents.** Either:
   - a depth-writing draw ran since the last clear (`had_write`); or
   - the guest cleared the depth since guest memory last got a copy (`cleared_since_store`,
     which implies `guest_dirty`).

   A target the guest never cleared or drew into falls back. Its host image holds only the
   engine's initial clear, while guest memory may hold bytes the CPU or SPU wrote.
2. **The target is not the currently bound zeta.** A fetch of the bound depth is a feedback
   loop. It keeps the store path, which samples a copy taken at bind time.
3. **The backend has `depth_snapshot`.** Vulkan and Metal do; D3D12 does not.

Before this change, the snapshot required `had_write`. A target only cleared each frame then
went through guest memory on every read: a readback, a store, and a decode and upload of the
whole surface. That happened even though the store copied exactly the host depth.

## What the guest sees

The store path writes the host depth to guest memory and decodes those bytes. The snapshot
samples the same host depth on the GPU. The one difference is that the store quantizes depth to
24 bits, while the snapshot keeps the host's 32-bit float. That difference already exists for
drawn targets.

When the snapshot serves a fetch, the target stays `guest_dirty`. So any later consumer that
needs the guest bytes (another format, a transfer, a colour read of the same address) still
gets the synchronous store, as with the existing GPU depth view.

## Invalidation

- **A clear** marks the snapshot stale, as before.
- **A depth-writing draw now marks the snapshot stale too.** Before, a snapshot taken after one
  draw was not refreshed by later draws until the next clear. With clear-only snapshots, a
  snapshot can predate the first draw, so every write invalidates it. The next fetch copies
  again.
- **Recorded guest writes** (transfers, DMA) never change the host depth image, so they don't
  touch the snapshot. A transfer that reads the target's guest bytes first triggers the store
  above.

## Synchronisation

The backend's snapshot is a GPU copy recorded in stream order. On Vulkan it is an operation of
its own: it ends any open render pass, sits after a full dependency on everything recorded
before it, and copies into a separate image. Draws that sample the copy never sample the
attachment itself, so later depth writes can't race them. Feedback loops (rule 2) are excluded
anyway.

## Off switch

`PS3RECOMP_RSX_DEPTH_SNAPSHOT_CLEARED=0`, read at engine init. It restores the old rule (snapshot
only after a depth-writing draw) and the old invalidation (clears only).

## Why not every reinterpreting read

The general version would serve any fetch of any render surface from the GPU, whatever format
it is read as, with GPU conversion passes. It would keep surfaces resident instead of storing
them to guest memory, and store them back only when something else reads that memory.

To be correct, that version has to know when the guest CPU or an SPU reads or writes the bytes
of a surface the engine is keeping on the GPU. The runtime only sees writes it is told about
(`rsx_draw_engine_mark_guest_dirty`: transfers and DMA that go through it). It doesn't track
guest CPU loads and stores to arbitrary memory: those are lifted PPU code writing guest memory
directly, and there are no page protections or write barriers on render-target ranges.

Without that tracking, a surface kept only on the GPU could be read stale by the CPU, or
overwritten by the CPU without the engine noticing. The store-on-demand rule avoids this today
by putting the bytes in guest memory whenever a guest-visible consumer is in the engine's view.
The focused rule above stays within what the engine can observe.
