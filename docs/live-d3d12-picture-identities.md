# Immutable pictures in the live D3D12 consumer

The existing picture registry owns immutable copied bytes and transfers references
from producer to descriptor to queued command to texture binding. The shared draw
engine consumes those references; the live D3D12 engine currently loses command
addresses and uploads mutable guest bytes. Reusing a guest allocation can therefore
change a picture that an earlier command still owns.

Reuse the one existing registry. Add a live method entry that carries the FIFO data
address, retain one transferred token per bound texture unit, and resolve the token
after the offset/format pair is known. A token selects copied picture bytes before
surface/depth aliases or guest-memory fallback. Texture cache keys include the token;
GPU resources keep their existing checked-fence retirement rules. Missing, mismatched,
or interrupted picture bindings latch failure and refuse checked completion. Ordinary
textures and the legacy method API retain their existing behavior.

Implementation sequence:

1. Add a synthetic GPU fixture that queues two immutable pictures at the same guest
   address before consuming either, poisons guest memory, and checks separate outputs.
   Watch the current renderer fail the pixel checks.
2. Carry data addresses through GCM and live dispatch; share registry range/pending
   queries without duplicating the registry. Resolve/release bindings and reject a
   texture change after packets inside BEGIN.
3. Include tokens in texture-cache identity and read only retained immutable bytes
   for those uploads. Preserve existing resource retirement. Release binding tokens
   on shutdown, including failed initialization/operation.
4. Cover missing/mismatched command identities, same-frame cache separation, stale
   surface aliases, pending-offset replacement, partial groups and normal texture
   regression. Run the actual GPU checks plus the SDK CMake suite.

This change supplies consumer ownership only. It does not alter media decoding,
timing, completion events, or title-specific state.
