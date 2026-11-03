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

**Backends.** Metal implements the view. Vulkan has neither `depth_store_z24`
nor the view, so its depth reads keep the CPU readback path. The engine prints
a message when the switch is set and the backend lacks the operation. A Vulkan
implementation needs the conversion as a compute shader writing a buffer,
then a buffer-to-image copy into an `R8G8B8A8` image whose view carries the
same component swizzle, with both recorded into the engine's current command
stream before later draws. The D3D12 path uses a separate engine (live draw)
and is not affected.

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
