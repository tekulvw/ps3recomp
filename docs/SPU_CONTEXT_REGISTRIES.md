# Per-context DMA and reservation ownership

The SPURS HLE retains a context for each workload/lane across scheduling quanta.
That count can exceed the physical SPU count even when only one policy executes
at a time. The previous global eight-entry MFC table assigned all later contexts
one shared engine; the coherence table omitted later reservation holders. Real
MAG v1.0 initialized workloads 8–10 after both limits were exhausted.

Both registries now grow with live context ownership instead of a numeric cap.
MFC nodes keep stable engine addresses and private queue/tag state. An existing
registry lock guards all lookup, insertion and removal. Coherence nodes retain
all potential reservation holders under the existing lock-line lock, including
contexts paused between policy quanta. Allocation failure prints a diagnostic and
aborts: sharing DMA state or silently omitting invalidation is not a usable
fallback. Registry allocation occurs only when a context first uses that service.

A context has one execution owner. It may be paused and resumed without losing
its engine, stalled-list state or registration. Its owner must stop accessing it
before release/reset/free. Transient lifted/interpreted jobs and the taskset-policy
probe now release MFC state alongside existing coherence unregister calls.
Persistent policy contexts release both before the existing fresh-context reset.
A completed lifted thread run now unregisters coherence before releasing its MFC
engine. The existing Windows thread-exit safety net removes matching coherence and MFC
nodes through separate calls, without nesting their locks. POSIX thread-exit
stack discovery is not added here; normal transient cleanup applies on both hosts.

The fix adds a locked linear search for MFC channel lookup; this favors clear
ownership over introducing per-context caches or a new synchronization scheme.
It does not prove full scheduler conformance. Inherited global workload/lane
caching, workload removal/finalization, raw-SPU restart, and stopping threads
before group destruction still need separate lifecycle review. The existing
unlocked coherence bitmap fast-path reads are unchanged. This branch does not
claim that exceeding eight contexts caused MAG workload 4's missing-receiver
halt: that workload was resolved before the overflow warnings. Missing
registration does provably lose same-value and ABA invalidations that snapshot
polling cannot reconstruct.

`python3 tools/test_spu_context_registries.py` runs POSIX C fixtures. They cover
12 concurrent MFC owners, private queue/tag state, actual stalled-list DMA and
per-context acknowledgement, 20 reservation holders, same-value/ABA stores,
unregister/re-register, range cleanup, real transient helper cleanup and explicit
allocation failure. The existing SPU-to-SPU lock-line regression also runs.
Production code uses the existing platform lock wrappers and standard allocation;
Windows/Linux execution has not been verified locally.
