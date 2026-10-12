# Checked native reference completion

Launch with `PS3RECOMP_GCM_CHECKED_REF=1` to require a normally consumed, complete
SET_REFERENCE packet and a checked renderer completion before admitting a guest
REF value. `PS3RECOMP_GCM_CHECKED_REF_LOG=1` logs monotonic host receipts separately
from repeated/wrapped guest values. This launch configuration is immutable for a
run. A failure freezes certification until process restart; it is not repaired
by clearing a trace setting or an apparently successful later buffer.

The appended `submit_and_wait_checked(user,reason)` backend callback returns1
only after successful completion. The engine refuses uninitialized/unsupported
backends, partial BEGIN/END, dropped batches and resource failures. The original
void flush remains compatible but is never a certificate. Call renderer lifecycle
operations and checked flush on the serialized renderer consumer, not a guest
callback worker. Backend replacement/teardown must happen with that consumer idle.

Metal validates device/queue, staging, command buffer, encoders and required
resources, waits an ordered same-queue buffer and checks Completed/no error. It
tracks prior asynchronous buffers and checks their statuses directly: completion
handler delivery timing cannot hide an earlier failure. Resource, encoding,
record-cap and GPU failures stay latched until teardown. Successful submission
retains the existing queued texture-handle protection.

Strict FIFO resynchronization/rescue, incomplete packets beyond submitted PUT,
unmapped packets, malformed references, unknown words and queue overflow freeze
REF. The producer and poll/ticker publishers share the publication lock. Strict
mode disables the legacy title-ring rescue and GET-equals-PUT diagnostic. Other
backends do not supply checked certificates until they implement this contract.

`rsx_draw_engine_mark_guest_dirty(ea,bytes)` publishes a CPU-write generation with
release/acquire ordering; cache work stays on the renderer. It conservatively
rehashes cached guest textures even within the same presentation epoch and gives
changed bytes a fresh backend texture. It is neither a memory-reuse certificate
nor a picture-identity mechanism. A stale unread address command can still mean
an older picture: its caller must prove exclusion or carry immutable identity.

Tests run real FIFO and draw engine fixtures; Metal includes a real queue/clear
pixel check and narrow dependency failure injection. They cover held completion,
repeated/wrapped guest references, incomplete PUT packets, rescues/overflow,
partial groups, batch drops, color/depth/MRT allocation failure, same-epoch cache
refresh and earlier GPU failure without relying on completion handlers. Native
allocator reuse and movie playback remain integration acceptance work.
