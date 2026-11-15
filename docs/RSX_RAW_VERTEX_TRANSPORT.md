# Design note: vertex data in host formats instead of float4

Status: proposal, not implemented. This is the next large per-draw lever after the delta-driven engine
work (#51 to #53). It is meant to be read before code is written.

## Problem

For every draw, the register-file engine:

1. turns the index buffer into a reference list;
2. collapses repeated references (`rsx_vertex_remap_build`);
3. decodes every attribute of every unique vertex into a float4 (`rsx_vertex_fetch_many`);
4. writes a u32 index list for the host.

So a triangle-list draw of 30 vertices with four attributes uploads 30 × 64 bytes plus 54 × 4 bytes
of indices. In MAG on Linux, steps 2 and 3 together cost about 7 ms per frame of the thread that
limits the frame rate. The engine expanded to float4 because every backend binds one fixed vertex
layout, sixteen float4 slots, and the programs read float4 inputs.

## Proposal: host-format streams for plain draws

A draw qualifies when all of the following hold:

- its topology needs no rebuild (a triangle, line or point list, no restart index);
- every attribute the vertex program reads is a plain array (`frequency` 0 or 1);
- every such attribute has one of the formats the host's vertex fetch can convert itself:
  - FLOAT 1 to 4;
  - HALF 2 or 4 (HALF 1 and 3 need padding, so those draws keep the float4 path);
  - UNORM8 and UINT8 4;
  - SNORM16 and SINT16 1 to 4;
- it is not an inline array and has no immediate vertices.

For such a draw the engine does this instead:

- Find the index range `[lo, hi]` once (the remap's dense path already computes it).
- For each attribute, copy the bytes of elements `lo..hi` with the big-endian components swapped to
  host order into the backend's staging, with its stride kept. This is a byte-swapping copy, with no
  float conversion, and it vectorises.
- Copy the indices as host-endian u16 (or u32) minus `lo`, without the remap and the u32 rebuild.
- Hand the backend a per-draw vertex description: per attribute, its host format, offset, stride
  and buffer slot.

The backend binds the streams with that description and lets the GPU's fixed-function vertex fetch
convert to float, as Vulkan (`VkVertexInputAttributeDescription.format`), Metal
(`MTLVertexDescriptor`) and D3D12 (input layout) all do natively. The vertex shader is unchanged:
its inputs are still float4, filled by the hardware as the formats define, and missing components
default to (0, 0, 0, 1), as the CPU path does.

Draws that don't qualify keep today's path unchanged.

## Exactness

Every format converts identically except one. SNORM16 -32768:

- the CPU path computes -32768 / 32767 = -1.0000305;
- Vulkan, Metal and D3D12 SNORM clamp it to -1.0.

Either accept that difference, which is invisible, or bind SNORM16 as SSCALED16 and have the
vertex-program prologue multiply by 1/32767. The second needs a per-attribute scale in the program
key. Proposal: accept it, and say so in the code and tests.

## Pipeline identity

The vertex input state is pipeline state on Vulkan and D3D12 and part of the render pipeline
descriptor on Metal. The attributes' formats and strides join the pipeline key, which today hashes
only the layout mask and the float4 stride. In MAG, a material run shares its vertex format, so the
number of pipelines grows by the number of distinct (program, format set) pairs. That should be
small, and the engine's pipeline table reports it. `VK_EXT_vertex_input_dynamic_state` can remove
this where it is present; it is optional.

## What it buys and what it keeps

- **Removed per qualifying draw:**
  - the remap, since the dense range is the remap;
  - the float4 decode;
  - the u32 index rebuild;
  - 2 to 4 times the upload bytes and GPU vertex bandwidth.
- **Expected:** 5 to 7 ms per frame on Linux and 2 to 3 ms on the Mac. This is the review's G3
  estimate; measure it.
- **Kept:** inputs are still copied at record time. That is the precondition for early label
  release (`RSX_EARLY_LABEL_RELEASE.md`): no backend resource aliases guest memory. Binding RSX
  local memory directly is a separate, later question.

## Steps

1. Engine:
   - the qualification test;
   - the byte-swapping stream copy;
   - u16 indices;
   - a `rsx_be_vertex_input` description passed with `draw`, through a new optional backend entry
     point, so a backend without it keeps getting float4.
2. Vulkan:
   - the vertex input state from the description, in the pipeline variant key;
   - u16 index type.
3. Metal: the vertex descriptor from the description.
4. Rollout: behind `PS3RECOMP_RSX_HOST_VERTEX_FORMATS=1`, default off, until lane A/Bs and a visual
   check pass on each host.
5. Tests:
   - `test_vertex_fetch_plan` gains a check that the host-format stream, converted by a reference
     decoder of each host format, equals `rsx_vertex_fetch_many` (SNORM16 -32768 excepted, by rule);
   - `test_vulkan_batched_engine` gains draws through the new path that must read back identical
     to float4 draws of the same vertices.
