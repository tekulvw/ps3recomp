# Vulkan texture uploads

The draw engine uploads a texture when its cache does not hold it (a first use, or a use after
the 1024-entry cache evicted it) and when a cached texture's guest bytes change. Every upload runs
on the FIFO walker thread, so whatever the host API charges for one comes straight out of the
frame. The Vulkan backend used to pay for each upload with:

- a new `VkImage` with its own `vkAllocateMemory` for every new texture and every changed one,
  plus a `vkFreeMemory` when the old one was released;
- one pooled host staging buffer per mip level and face, taken from a 32-entry pool that runs dry
  while batches are in flight, so most levels allocated and mapped a new host buffer and freed one
  later;
- a full dependency and an image barrier per level.

Driver allocations and frees are the expensive part, and they cost more on some drivers than
others.

## What it does now

| Mechanism | What changes | Off switch (read once) |
|---|---|---|
| In-place re-upload | The engine asks the backend whether a changed texture's handle takes new contents (`texture_reusable` in `rsx_draw_engine.h`). Vulkan says yes for an image `texture_create` made with the same format, size, levels, faces and crossbar. The engine then decodes into the image it has, with no create or release. Every upload is recorded on the backend's one queue after a full dependency on all earlier work, so draws recorded before it still read the old texels and later draws read the new ones. Images the native owner may hold are excluded. | `PS3RECOMP_RSX_TEXTURE_IN_PLACE=0` (engine init) |
| Texture heap | A texture of up to 16 MiB takes a first-fit range of a 64 MiB device-local block instead of its own allocation. The range is returned when the image is destroyed, which happens only after no submitted work can use it. Freed ranges merge with their neighbours. Up to 32 blocks are kept. Further empty blocks beyond one are freed. A larger texture, a full heap or a failed block allocation falls back to its own allocation. Blocks hold only optimal-tiling images. | `PS3RECOMP_VK_TEXTURE_HEAP=0` (device creation) |
| Arena staging | Under batched recording, a level of up to 4 MiB is staged in the open batch's arena, which is reused when its slot comes round again. Larger levels and the per-operation path take a pooled buffer as before. | `PS3RECOMP_VK_TEXTURE_STAGE_ARENA=0` (batched init) |
| Level chaining | A texture's levels and faces arrive one upload each, back to back. The first records the full dependency and the image barrier. A following upload into another level or face of the same image, with nothing recorded in between, records only its copy. A second write to the same subresource, or any other operation in between, takes the full path. | `PS3RECOMP_VK_TEXTURE_LEVEL_CHAIN=0` (batched init) |

Pixels are unchanged. The same bytes are decoded and copied into the same subresources, and only
where the image and its staging live, and how many barriers order them, differ.
`tools/test_vulkan_batched_engine.py` checks this. It covers multi-level in-place re-uploads, a
level sampled on its own, and an upload larger than the arena limit. Per-operation recording and
every batched mode must read back identical bytes, clean under the validation layer with
synchronization validation. The fixture also checks the heap's range list (fit, alignment,
merging, a full list) and that each path is the one taken.
`libs/video/tests/test_rsx_draw_engine.c` ("texture re-upload in place") checks the engine side:
it asks only on a content change, uses the answer, and honours the switch.

## Counters

Build stats version 2 (`docs/RSX_BUILD_STATS.md`) counts uploads as new or changed (and in
place), evictions, uploads by size, and the time uploads spent waiting for a Vulkan batch slot.
Before version 2 that wait was folded into upload time.
