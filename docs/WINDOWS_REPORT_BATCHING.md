# Checked report batching for the Windows LIVE renderer

The LIVE D3D12 provider can use the existing ordered GCM report/label queue
while retaining synchronous command-list completion. Enable both
`PS3RECOMP_RSX_DEFERRED_REPORTS=1` and
`PS3RECOMP_GCM_LIVE_REPORT_BATCH=1` before initializing the consumer and
starting guest or renderer threads. Without this selection, the existing
immediate LIVE path remains in effect.

The queue's default batch is 32 pending report/label records, with capacity
1024. `PS3RECOMP_RSX_DEFERRED_REPORT_BATCH` retains its existing override.
Dependencies and FIFO exhaustion can issue a smaller batch; the consumer
never waits for a batch to fill. These are completion-record counts, not
draw counts. Literal reference publication and its pacing remain unchanged.

Each LIVE issue performs the original checked flush, wait and reset. Only
successful completion returns an actual fence value, bound to a physical
renderer lifetime. The queue then publishes records in order through its
existing semantics. This provider does not introduce additional inflight
allocators or change report values or publication-time timestamps.

Initialization registers the existing reentrant consumer lease, dependency
barriers and VM guards. High-level renderer effects hold the lease across
the barrier and the entire operation, acquiring it before renderer locks.
Low-level issue, status and flush functions do not invoke the barrier.
This distinction prevents recursive report issue and lock inversion.

Report admission validates the original primitive position. Invalid,
failed, future-fence or previous-lifetime work cannot become successful
publication after later commands. Texture staging reserves the whole
upload before recording copies, and pressure completion must preserve
the draw state needed after reset.

## Synthetic validation

With `PS3RECOMP_BUILD_TESTS=ON`, Windows CTest includes the CPU-only
`gcm_live_report_batch` fixture. It uses the configured C compiler and
synthetic GCM commands; no game inputs are required.

Real D3D12 adapter, upload-pressure and ownership checks are explicit:

```text
python tools/test_live_draw_report_batch.py --runtime-library <path-to-MT-ps3recomp_runtime.lib>
```

Run this from an initialized Windows native compiler environment using the
same runtime linkage as the library. The fixture requires a working D3D12
device. Test results must distinguish queue batching, actual submissions
and completion correctness from application frame-rate measurements.
