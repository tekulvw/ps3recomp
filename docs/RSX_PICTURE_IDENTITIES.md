# Immutable texture picture identities

This optional producer boundary snapshots pixels under monotonically unique
64-bit tokens. A producer owns the initial reference. A descriptor owns a
reference until replacement or explicit unbind; an emitted FIFO offset word
owns another until the serialized consumer transfers it to a texture binding.
The binding retains the token while hashing/uploading. Cached GPU textures are
independent copies keyed by token, and existing backend retirement preserves
objects used by recorded draws.

The producer must capture **the actual selected picture**, before guest memory
can be recycled. Bind that token when constructing its descriptor. Capture the
descriptor token for the precise offset payload word before exposing the FIFO
command. Descriptor EA or guest picture EA alone is never a generation key.
The branch supplies generic APIs, not title-specific capture hooks.

The serialized engine carries payload word EA with `rsx_draw_engine_method_at`.
OFFSET becomes pending until FORMAT supplies the actual memory location, or a
draw uses the existing format. Sidecars include both location and offset.
Identity overrides stale surface/depth aliases. Missing identities and any
ordinary texture access overlapping registered picture ranges fail rather
than read mutable guest bytes. Vertex or interior aliases are unsupported.
Replacing a captured unresolved OFFSET, or a picture binding during buffered
BEGIN/DRAW work, fails explicitly rather than dropping its ownership.

Bounds are 32 snapshots, 128 MiB total pixels, 64 MiB per picture, 256 descriptor
aliases, 4096 pending command words and 256 registered ranges. Capacity failure
latches; checked rendering cannot certify reclamation after that failure.
Resolved producer references and known descriptor unbinds may release safely;
uncertain pending commands remain quarantined until process restart. Reset
requires no outstanding owner and never resets the token serial.

An unlocked pointer from `rsx_picture_data` is borrowed while the caller owns
a retained token; never release that ownership concurrently with its read.
All engine methods and lifecycle operations require the existing serialized
FIFO/render consumer. Producer/descriptor/command registry operations are
synchronized separately and must not invoke guest callbacks under its lock.

Validation exercises old queued and suppressed future descriptors across
same-address reuse, location collisions, unknown identity rejection, stale
surface priority, superseded/partial offset failure, concurrent lifetime
operations and snapshot/alias/command bounds. Timed playback, actual guest
allocator retirement and title-specific capture coverage require separate
native evidence.
