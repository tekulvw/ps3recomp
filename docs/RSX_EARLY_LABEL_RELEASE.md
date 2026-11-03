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
