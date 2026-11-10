# Vulkan encode queue

`PS3RECOMP_RSX_ENCODE_THREAD=1` moves the Vulkan register-file engine's backend
recording onto a separate thread. The switch is off by default. Metal uses the
same switch for its existing recorded-batch handoff.

The FIFO walker remains the sole producer. It copies draw arguments and other
temporary buffers into a bounded queue before returning to the engine. The
consumer calls the Vulkan backend in stream order. Creation and initial uploads
of new objects execute on the caller under the backend lock. Readbacks, depth
snapshots, checked waits and other operations requiring an answer drain earlier
queued work first. A color snapshot allocates its object immediately and queues
the copy at its original stream position.

Labels, reports and completion decisions stay with the FIFO walker. Queue
submission tickets map to actual backend tickets; they do not claim completion
until the backend reports it. Host split presents run in queue order, with at
most one preceding present outstanding. Their queue positions belong to the
current backend lifetime and reset on backend reinitialization.

The queue is not enabled alongside an extracted native owner or the explicit
render frontend, which have their own synchronization and queue ownership.
`PS3RECOMP_RSX_INFLIGHT_LOG=1` prints queue counts and wait durations. These are
diagnostics and do not establish frame rate.

`test_encode_queue` exercises a synthetic recording backend without a window or
GPU. `python3 tools/test_vulkan_encode_thread.py` compares direct and threaded
rendering in per-operation and batched modes under Vulkan synchronization
validation. It also exercises backend shutdown and restart in one process.
Actual game performance and windowed presentation require separate platform
tests before considering a default change.
