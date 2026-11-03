# Early label release (opt-in)

With `PS3RECOMP_RSX_DEFERRED_REPORTS=1`, label releases and reports publish
into guest memory only after the backend command buffer that follows them
completes. A guest that polls a label for progress then waits for everything
queued on the GPU ahead of that buffer, including work recorded by other
producers on the same queue.

Two default-off switches relax this on the draw-engine deferred path. They
are refused, with a message, under `PS3RECOMP_GCM_LIVE_REPORT_BATCH=1` or
without deferred reports.

| Variable | Effect |
| --- | --- |
| `PS3RECOMP_RSX_EARLY_LABEL_RELEASE=1` | `NV406E` semaphore release (`0x6C`) and `NV4097` texture-read and backend releases (`0x1D70`, `0x1D74`) write their label at the FIFO position of the method. `GET_REPORT` (`0x1800`) does too. The value encodings are unchanged. |
| `PS3RECOMP_RSX_EARLY_FIFO_SYNC=1` | Requires the variable above. A checked `SET_REFERENCE` (`PS3RECOMP_GCM_CHECKED_REF=1`) commits the work recorded so far without waiting and queues its value at once. `NV406E` acquire and the `NV0039` copy keep their report ordering but no longer wait for the GPU. |

## Why this is safe for this renderer

The deferral protects two kinds of guest memory: memory the GPU may still read,
and memory the GPU may still write. On the draw-engine path neither depends on
completion.

- **Inputs.** Vertex, index and constant bytes are copied into backend staging
  when a draw is recorded. Texture bytes are decoded and uploaded when they are
  bound. A changed texture gets a fresh backend texture, and in-flight command
  buffers retain the old one. No backend resource aliases guest memory: the
  Metal backend creates no no-copy buffers.
- **Outputs.** GPU results reach guest memory only through explicit synchronous
  readbacks issued at their own stream position: surface or depth to guest
  memory, and `NV3089` reads. Each readback waits for the GPU itself. Reports
  carry no measured GPU result: the value is fixed per type, and the timestamp
  is host time.

So a label, report or reference published when the FIFO reaches it promises
nothing that completion would add. Later GPU work stays ordered behind earlier
work because both are submitted in order on the same queue.

## What still waits

- **Records behind an unpublished record.** A record is applied early only
  when every earlier queued record has already been applied, so guest-visible
  order never changes.
- **The diagnostic ring-progress word.** The word named by
  `rsx_ring_progress_observer_set` stays completion-gated, because its observer
  treats publication as a GPU receipt.
- **Synchronous readbacks**, and every API-level barrier: IO mapping, init,
  terminate, flip with wait label, and set-FIFO.
- **Completion checks.** Each early record stays queued until its genuine
  completion. It is never applied twice. If the backend fails, publication
  latches off and checked references freeze, as before. Values that were
  already published cannot be withdrawn; the failure is reported on stderr.
- **Capacity.** A full queue still waits for every record.

## Tests

- `python3 tools/test_rsx_deferred_reports.py` covers the queue: stream-order
  application, gating behind an unapplied record, capacity, receipts and sticky
  failure.
- `python3 tools/test_gcm_early_label_release.py` runs the real FIFO walker. It
  checks every release form, the report record, ordering behind an observed
  word, checked `SET_REFERENCE` with and without early FIFO sync, the acquire
  barrier, and a failure after early publication.

## Attributing the remaining FIFO waits

The draw engine's own waits pass the report barrier before they commit and
wait: surface or depth readback into guest memory, and synchronous flushes.
While a critical trace is active, that barrier records a kind-19 event. Its
reason is outside the source-line range, so it reads as a cause rather than a
line:

| Reason | Wait that follows |
| --- | --- |
| `0x10000` | An engine-initiated readback or flush |
| `0x10039` | The pre-copy flush of an `NV0039` copy |
| `0x10050` | A checked backend completion (`SET_REFERENCE`, non-deferred report) |

Barriers that record their own event, such as `gcm_reports_sync_barrier`,
suppress the nested one. On a single in-order queue, any of these waits also
waits for every command buffer queued ahead of it, including work submitted
by an external renderer on that queue.

## GPU-only depth reads (`PS3RECOMP_RSX_GPU_DEPTH_VIEW=1`)

A texture fetch that reads a depth target's bytes as linear `A8R8G8B8` or
`D8R8G8B8` normally stores the depth into guest memory as Z24S8 words first:
the engine commits, waits for the GPU, converts and copies, and then builds
the texture. On a single queue that wait also covers every command buffer
queued ahead of it, so a pass that samples a depth target another renderer
just wrote stalls the FIFO until that renderer's GPU work finishes.

With this switch, which is read when the engine initializes and is
independent of the label switches, the engine asks the backend for a
`depth_z24_view` instead. The backend appends the conversion and a copy into
a fresh texture to its next submission, after every draw recorded so far.
The engine submits that asynchronously and binds the view. The texels are
exactly those the store would have produced; `tools/test_metal_depth_z24.py`
compares them for both formats and a spread of crossbars, and checks stream
order. The view is reused while the depth target is unchanged.

Not covered:

- `Z16`, mipmapped, cube or volume fetches, a pitch other than the zeta's,
  and rectangles larger than the written clip. These keep the store.
- Guest memory is not written. The target stays `guest_dirty`, so any later
  consumer that needs the bytes, such as another format or a transfer, still
  gets the synchronous store. A guest CPU or SPU read of that memory, which
  the engine cannot observe, no longer sees the depth the store used to leave
  there as a side effect.

**Backends.** Metal and Vulkan implement the view (Vulkan: see below). The
engine prints a message when the switch is set and the backend lacks the
operation, and keeps the store. The D3D12 path uses a separate engine (live
draw) and is not affected.

## Bounded work in flight

Without the GPU waits that used to pace it, the FIFO consumer could get
arbitrarily far ahead of the GPU. Three bounds keep that finite.

| Bound | Default | Behavior |
| --- | --- | --- |
| `PS3RECOMP_RSX_MAX_FRAMES_AHEAD` | `3` | Applies with early label release. At each FIFO flip, the frame's recorded work is committed without waiting and its ticket is kept. The flip waits until the frame that many flips back has genuinely completed. `0` means unbounded; at most 15. |
| `PS3RECOMP_METAL_MAX_INFLIGHT_STAGE_MB` | `512` | An asynchronous commit first waits for the oldest tracked buffer while staging handed to unfinished buffers exceeds the limit. `0` disables the byte bound. |
| Tracked buffers | 64 | A 65th outstanding buffer now waits for the oldest instead of latching a checked failure. |

GPU depth views come from a pool of four pairs (words buffer and texture). A
pair is reused once the engine has released its view and every commit up to
the one that retired it has completed.

`PS3RECOMP_RSX_INFLIGHT_LOG=1` prints, every two seconds:

```
[rsx inflight gcm] frames=… bound=3 waits=… wait_ms=… max_wait_ms=… reports_pending=… gated=… max_pending=… applied_early=…
[rsx inflight metal] commits=… done=… tracked=… fences=… stage_inflight=…MB peak=…MB limit=512MB stage_waits=… wait_ms=… cap_waits=… views=… reused=… metal_allocated=…MB
```

Either line is also printed, at most every two seconds, whenever its bound
actually waits. `metal_allocated` is the device's `currentAllocatedSize`. If
it stays flat while the process grows, the growth is not GPU resources.

## Vulkan

The register-file engine's Vulkan backend (`rsx_vulkan_backend.c`, stage E5)
provides what the switches above need. Before E5 it had no completion tickets
and no checked flush, so deferred reports never deferred on Vulkan, checked
`SET_REFERENCE` could not certify, and both early switches had nothing to act
on.

| Operation | Vulkan |
| --- | --- |
| `submit_async` / `completion_status` | A ticket is a fence submitted, with no command buffer, after everything so far. Every drawing operation is still one submission the CPU waits for, so no recorded work is left unsubmitted. `vkQueueSubmit` signals the fence only after all earlier work on the queue has completed, so it covers this backend, the native renderer and the present alike. Tickets come from a ring of 64 fences retired strictly in order; a full ring waits for its oldest. |
| `submit_and_wait_checked` | A ticket, waited for. Any failed submission, fence wait or recording call latches: from then on tickets report -1 and the checked flush 0. |
| `depth_store_z24` + `depth_z24_texture` | Behind `PS3RECOMP_VK_GPU_Z24=1` (default off; the engine keeps `depth_readback` plus its CPU conversion). Depth aspect to a buffer as floats, `libs/video/vulkan/rsx_vk_depth_z24.comp` to D24S8 words in place (integer-exact rounding, the CPU path's clamp and NaN rule), a copy to a cached host buffer, then the rows into guest memory. The alias texture is filled from the device-side words. |
| `depth_z24_view` | The same conversion and a buffer-to-image copy into a fresh `R8G8B8A8` image, recorded into a command buffer of its own and submitted without a wait as a ticket. The image view's component mapping composes the A8R8G8B8/D8R8G8B8 decode's channel rotation with the unit's crossbar. |

Depth formats other than `D32_SFLOAT` and `D32_SFLOAT_S8_UINT` are refused
with one message, and the engine takes its fallback.

**Audit against the two facts above, for Vulkan.**

- **Inputs are copied when recorded.** The engine path's draw copies its vertex
  and index bytes into backend buffers and waits for the draw; uploads go
  through staging; textures are decoded at bind time. Nothing imports guest
  memory: no `VK_EXT_external_memory_host`, no mapped guest pointers. Guest
  memory is read only by the legacy vtable path, which also decodes at bind.
- **Outputs reach guest memory only through synchronous readbacks** (`readback`,
  `depth_readback`, `depth_store_z24`), each waiting for its own fence.
  Retiring a ticket advances counters, frees backend-private buffers and
  destroys released images; it writes no guest memory. The backend has no
  completion callbacks.
- **Resources stay alive until completion.** Synchronous work is waited for
  before anything it used is released. A view's words buffer is retired with
  its ticket. A released view, or the depth target an unfinished view still
  reads, goes on a retire list until its ticket completes.
- **One queue, explicit dependencies.** Engine, present and native work are
  all submitted on the same `VkQueue` under one mutex. Engine operations
  start with barriers that order them after every earlier submission. Each E5
  command buffer starts and ends with a full memory dependency, so ordering
  never depends on what other submissions record themselves. Native passes
  order their imported images against all earlier queue work.
- **Native boundary.** A pending ticket is accepted as the ordered boundary
  before native work, as Metal's asynchronous boundary is. Before E5 the
  latest submission was always a completed synchronous one.

**Per-operation waits, and batched recording.** By default, Vulkan drawing
stays synchronous: every draw, clear and upload waits for its fence, and so
for everything queued before it, native Main included. With the switches on,
labels, reports and references stop waiting, but the FIFO consumer still
waits at the next engine draw after native work has been queued.

`PS3RECOMP_VK_BATCHED_ENGINE=1` (stage E6, default off, read at engine init)
removes that wait.

- **Recording.** Operations are recorded into one command buffer per batch.
  Each operation starts with a full dependency on everything before it, so
  GPU order is exactly the per-operation order.
- **When a batch is submitted.**
  - At a ticket, the checked flush, or `submit_and_wait`.
  - At a CPU read (readback, depth readback, the z24 store). These also wait.
  - At the native boundary and at every present. These do not wait.
  - When the batch reaches `PS3RECOMP_VK_BATCH_MAX_OPS` operations (default
    2048), or its arena or descriptor pool is full.
- **Memory.** Vertices, indices, constants and descriptor sets come from the
  batch's own host-visible arena and descriptor pool.
  `PS3RECOMP_VK_BATCH_SLOTS` batches can be in flight (default 16, at most 32).
  A slot is reused only after its ticket completes, and a full ring waits for
  its oldest batch.
- **Lifetime.** Images, buffers, framebuffers and pipelines that are released
  while a batch may still use them are destroyed when the last ticket that
  could reference them retires.
- **Guest memory.** Unchanged. Inputs are copied when recorded, and every
  result the CPU reads is still waited for at its stream position.

`python3 tools/test_vulkan_batched_engine.py [--timing]` checks it. It runs one
synthetic operation stream twice, per operation and batched, under validation
with synchronization validation:

- the two runs must read back identical bytes;
- behind about 300 ms of foreign GPU work on the queue, 64 batched draws and a
  ticket must return while that work is still running, where the
  per-operation path waits for it.

It needs glslang, because the engine translates its HLSL.

**Reserved queue order for native work.** On Metal, the native renderer can
fix its submission's place in the queue at the native boundary: it enqueues
an empty command buffer, then encodes and commits it later from its worker.
The FIFO consumer goes on in the meantime, and its later commits queue behind
that buffer.

A Vulkan queue has no such slot. A timeline semaphore that later original
submissions wait on cannot stand in for one either: the native signal would be
submitted after those waits on the same queue. Mesa, for one, processes a
queue's submissions in order, so a wait whose signal has not been submitted
yet blocks every later submission, and that deadlocks.

`rsx_vulkan_backend_queue_order` (`rsx_native_owner.h`) keeps the order on the
CPU instead.

- `reserve`, called under the native lease, starts a hold. From then on, every
  original queue submission is recorded in order, with its fence, instead of
  reaching the queue: engine batches, tickets, presents, depth views.
- `submit`, called once from the native worker without the lease, submits the
  native batch, then the held submissions in order, and ends the hold.
  `submit` with no batch abandons the reservation and releases the held work
  the same way.
- While a hold is active:
  - a wait on a held fence first waits for the release, and a status read of
    one reports it as pending;
  - swapchain presents and device-idle waits also wait for the release;
  - the native lease reports -4, so no other native submission can overtake
    held work.
- In effect, an original operation that has to wait for the GPU waits for the
  native worker's encode as well. With batched recording, only CPU reads and
  explicit waits do; recording itself continues.

The native Vulkan backend uses it behind `MAG_RENDER_VULKAN_RESERVED_COMMIT=1`.

`tools/check_native_owner_vulkan.py` checks it in both recording modes:

- a clear recorded after a reservation lands after a native clear submitted
  150 ms later from another thread;
- the readback waited for that native submission;
- an abandoned reservation releases the held clear;
- the native lease is refused while the hold is active.

`PS3RECOMP_RSX_INFLIGHT_LOG=1` also prints:

```
[rsx inflight vulkan] tickets=… completed=… outstanding=… cap_waits=… views=… view_buffers_reused=… stores=… deferred_destroys=… pending_destroys=… failed=…
[rsx inflight vulkan batch] batches=… ops=… ops_per_batch=… slot_waits=… cpu_waits=… retired=… pending_retire=… arena_grows=…
```

Device test: `python3 tools/test_vulkan_depth_z24.py [--require-hardware
--vendor-id … --device-id …] [--exhaustive]`. It runs headless under the
Khronos validation layer, and any VUID fails it. It covers:

- the conversion against the CPU conversion, bit for bit;
- the store against `depth_readback` plus the CPU conversion;
- alias and view texels against the decoded upload, for both formats and
  eight crossbars;
- the view's stream order;
- the ticket ring, the checked flush, and deferred destruction;
- with the switch unset, the store and alias declining.

CI runs it on lavapipe in `frontend-vulkan.yml`.
