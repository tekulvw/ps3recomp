# Design note: encoding backend commands on a second thread

Status: proposal, not implemented. It frames upstream candidate A3 ("a decoupled RSX consumer") for the
register-file draw engine, and is meant to be read before any code for it is written.

## Problem

One thread does all RSX work. The frame clock wakes, walks the FIFO
(`gcm_rsx_process_fifo_pass`), dispatches every method into the register file, and at each
`SET_BEGIN_END(0)` the draw engine derives the draw and drives the backend vtable
(`rsx_draw_backend`). The backend then encodes the draw on the same thread:

- Vulkan records `vkCmd*` into the open batch.
- Metal stores an `EngRecord` and replays all records into encoders at commit.

In MAG on Linux this thread is the frame-rate limiter: 97% busy and 49 ms per frame, while the SPU
workers sit at about 74%. On the Mac it is 91% busy. The engine-side work (FIFO decode, vertex
fetch, pipeline and texture derivation) and the backend-side work (descriptor writes, `vkCmd*`, the
driver, Metal replay) are serial, although only the first half has to follow the FIFO.

The per-draw cost reductions (#49 to #57, the delta-driven engine) come first. This note is for what
remains after them: if the walker is still the limiter, it buys parallelism with the SPU workers'
headroom instead of reducing CPU.

## Split point

Split at the backend vtable:

- The *walker thread* keeps everything it does today up to and including the engine: FIFO decode,
  the register file, labels and reports, the deferred-report queue, early release, parks and holes,
  vertex fetch, the pipeline key and texture cache decisions.
- An *encoder thread* owns the backend: every Vulkan or Metal object, the command buffers, and the
  queue submissions the engine causes.

The walker turns each backend call into an operation in a single-producer, single-consumer ring.
The encoder applies the operations in ring order, calling the real backend functions unchanged.

This boundary is already the one the engine is written against: the engine never touches a host API
object, only handles and plain arrays. That makes it the cheapest place to cut. No GCM semantics
move: everything the guest can observe is still decided on the walker thread, in FIFO order.

A split earlier, at FIFO decode versus the engine, would also move the engine work, but the engine
reads guest memory (vertices, indices, textures, programs) that the guest may overwrite as soon as
early release has published the label after the draw. Copy-at-record would then have to happen on
the walker anyway, which is most of the engine. Not proposed.

## Operations and their payloads

Most vtable calls are fire-and-forget and become ring records:

- the binds (`bind_targets`, `bind_pipeline`, `bind_textures`, `bind_vertex_textures`);
- the dynamic state (`set_viewport`, `set_scissor`, `set_stencil_ref`, `set_depth_bounds`);
- `draw`, the clears, `texture_upload`, `color_target_write`, `present`;
- every `*_release`.

Payloads that are pointers to engine-owned buffers (vertices, indices, constant blocks, upload
rows) are copied into the ring's byte arena when recorded. The engine reuses those buffers on the
next draw (`dc.verts`, `g.indices`, `g.vp_cb`, `g.fp_cb`, `g.tex_staging`), so the copy is required.
Today the backend makes the same copy into its staging; with the split the walker copies into the
ring and the encoder copies from the ring into backend staging.

The second copy can later be removed by letting the walker reserve backend staging directly. That
makes the arena multi-threaded and is not part of the first step. With #51 the 8 KB vertex-constant
block is already sent only when it changed (`rebind_vs_constants` becomes a one-word record). The
remaining per-draw payload is a few KB.

## Calls that return a value

| Call | Treatment |
| --- | --- |
| `texture_create`, `texture_create_3d`, `color_target_create`, `depth_target_create`, `surface_view`, `color_snapshot`, `depth_snapshot`, `depth_z24_texture`, `depth_z24_view`, `pipeline_create` | **Proxy handles.** The walker allocates the handle from its own table and records a "create into proxy *h*" operation. The encoder maps proxy to backend handle in a table only it writes; every later record carries proxy handles. A creation that fails on the encoder marks the proxy failed, and the next checked point reports it as a checked failure, as a backend failure does today. `pipeline_create` keeps its synchronous "0 = cannot build" contract only through the drain below, or the engine accepts an asynchronous failure (the draw is then dropped on the encoder). That needs its own decision; start with the drain. |
| `submit_async` | **Proxy tickets.** The walker returns a ticket number it allocates and records "submit with ticket *t*". `completion_status(t)` reports pending until the encoder has submitted *t* and its backend ticket has completed, and failure if either failed. Early release does not wait on tickets, so this keeps the deferred-report queue's semantics: records are verified at completion and capacity waits stay where they are. |
| `readback`, `depth_readback`, `depth_store_z24`, `capture_texture_rgba8`, `submit_and_wait`, `submit_and_wait_checked` | **Drain.** The walker records the call with a completion slot and waits for the encoder to reach it. These are the points where the CPU needs a GPU result now (guest-memory depth and colour stores, `NV3089` reads, checked references without early sync). They already wait for the GPU, so the extra wait for the ring is small. |
| `init`, `shutdown` | Drain, and start or stop the encoder. |

## Ordering rules

1. **One ring, one order.** The encoder applies operations exactly in walker order. Every
   GPU-ordering argument the backends rely on today (each operation after everything before it on
   one queue) holds unchanged.
2. **Labels and reports stay on the walker.** Early release publishes at FIFO position, deferred
   records are verified through proxy tickets, and the frames-ahead bound counts proxy tickets.
   Nothing guest-visible is decided on the encoder.
3. **Inputs are copied at record time,** now into the ring. This keeps the precondition
   `RSX_EARLY_LABEL_RELEASE.md` states for early release.
4. **Outputs reach guest memory only at drain points,** which wait for the encoder and then the GPU,
   exactly as today's synchronous readbacks wait for the GPU.
5. **Holes and parks are untouched.** They are FIFO-walk concepts and stay on the walker thread.
6. **Other queue users.** The present thread, the native owner's reserved queue order and the
   Vulkan hold all synchronise on the backend's own locks today, and the encoder becomes the only
   thread calling into the backend for the engine. The native boundary (`native_boundary` /
   `vk_bat_flush` at the native seam) becomes a drain so that native work still queues behind
   exactly the original work recorded before it.

## Backpressure and bounds

The ring is bounded in records and in arena bytes. A full ring makes the walker wait, which is
counted as a new `PS3RECOMP_RSX_FIFO_BUSY` category, so the cost is visible.

How far the walker may run ahead of the GPU is still bounded by the frames-ahead rule and the
deferred-report capacity, both counted in proxy tickets. The ring adds at most its own depth.

## Per backend

- **Vulkan:** the batched recorder (`PS3RECOMP_VK_BATCHED_ENGINE=1`) already turns every operation
  into recording plus deferred submission. It moves to the encoder thread as is. Per-operation mode
  stays single-threaded and does not use the ring.
- **Metal:** the engine records `EngRecord`s and replays them at commit. The simplest split moves
  the commit (`eng_encode_and_commit_mode`: replay, encode, commit) to the encoder, handing over the
  record array and staging buffer at each commit point. That is a smaller change than routing every
  vtable call through the ring, and it moves the replay (most of the Metal backend's per-draw cost).
- **D3D12:** parked; it would follow Vulkan.

## Switch, tests, rollout

- **Switch:** `PS3RECOMP_RSX_ENCODE_THREAD=1`, default off, read at engine init. With it off, the
  vtable is called directly as today.
- **Tests:** synthetic tests in the existing fixtures.
  - Vulkan: `test_vulkan_batched_engine` gains an encode-thread mode that must read back
    byte-identical to per-operation submission under synchronization validation.
  - Engine: `test_rsx_draw_engine` and the GCM walker fixtures run with a ring-backed stub backend
    and must see the same calls in the same order, plus drains at the documented points.
  - Proxy handle and ticket failure paths get injected-failure tests.
- **Rollout:** Metal commit handover first (smallest change, the Mac is 91% busy), then Vulkan
  batched. A lane A/B and a visual check on each host before any default change.

## What it does not do

- It does not reduce CPU time; it moves the backend half off the walker. Expected benefit is bounded
  by the backend share after #49 to #57: estimate 5 to 10 ms per frame on Linux and 4 to 6 ms on the
  Mac. Measure the share with the per-function profile after those land, before building this.
- It does not parallelise FIFO decode, which is sequential by definition.
