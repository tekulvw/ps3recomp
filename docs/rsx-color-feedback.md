# Ordered colour-target feedback

A texture unit can name the same colour allocation the current draw writes.
The shared engine formerly excluded the live attachment from surface binding
and uploaded its guest-memory backing instead. GPU-only writes need not have
updated that backing. The old path therefore sampled stale pixels.

The optional backend `color_snapshot` callback now supplies a unique immutable
copy for each referenced owned colour attachment, shared by texture units in
that draw. Allocation identity and newer depth content retain priority. The
copy receives the sampling unit's existing component-remap view; gamma/filter
state stays on the ordinary shader/sampler paths. No CPU readback is required.
Backends leaving the appended callback NULL retain their existing behaviour.

Metal records a full-size colour copy between existing draw records. Encoding
finishes earlier render work, blits the source storage without conversion, then
opens the next draw's pass with Load for colour, depth and stencil unless an
explicit intervening clear requests otherwise. Each copy has independent
storage, so subsequent copies cannot overwrite an earlier queued reader.
Retired numeric resource/view handles remain available until their records
have acquired retained Metal objects; resize and shutdown release the last
copy. Record/allocation failures are checked failures, not successful fallback
textures.

The implemented aliases have the same location, offset, entire physical width
and height, compatible native storage format, and effective source/texture row
pitch. Eligible formats are A8R8G8B8, W16_Z16_Y16_X16_FLOAT,
W32_Z32_Y32_X32_FLOAT and X32_FLOAT. Only linear 2D, non-cube, one-level layouts
are copied. A zero pitch denotes packed rows; explicit matching padded pitches
are accepted. AA-expanded storage remains at physical sample dimensions: an
AA3 64x64 pixel surface is copied as 128x64, AA4/5 as 128x128. This preserves
the existing sample-resolution approximation, not hardware MSAA coverage.

For a backend providing the callback, incompatible owned aliases or failed
copy/view creation reject the consuming draw with a bounded diagnostic and
checked failure. Subrectangles, swizzled reinterpretation, other formats,
mips, cubes and differing pitches require separate conversion contracts.
Non-owned colour/depth/guest binding paths are unchanged.

Synthetic validation drives the production engine and Metal translator with
queued producer and consumer draws, all four AA layouts, asymmetric component
remapping, alpha, EQUAL depth/stencil and a long queued retirement chain.
CPU controls exercise descriptor rejection, MRT, repeated units, allocation
location, depth priority, callback/view failures, resize and legacy NULL
behaviour. Ordinary engine regression stdout and stderr remain byte-identical
to the parent. These establish the generic feedback boundary; they do not
establish complete game rendering, visible object identity or hardware RSX
feedback hazard semantics within a single overlapping primitive.

The existing repository's `rsx_live_draw.c` own-target snapshot path documents
the compatibility behaviour (same-target GPU content, not stale guest VRAM).
This implementation is independently written. PR213 also describes snapshots
in a broad renderer change; this focused delta intentionally does not copy it.
