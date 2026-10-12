# Bounded texture descriptor capture

This diagnostic records the register-file draw engine's selected fragment
texture bindings. It leaves resource selection and image contents unchanged.
It emits each distinct descriptor and binding source once, with a default
budget of 128 records and an absolute maximum of 512 for the process.

Set `PS3RECOMP_RSX_TEXTURE_DIAGNOSTICS=1`. Optional settings:

- `PS3RECOMP_RSX_TEXTURE_DIAGNOSTICS_LIMIT`: 1–512 unique records.
- `PS3RECOMP_RSX_TEXTURE_DIAGNOSTICS_START_FRAME`: first eligible presented-frame
  count (default 0).
- `PS3RECOMP_RSX_TEXTURE_DIAGNOSTICS_TRIGGER`: an initially absent marker file.
  Create this file after the affected menu appears to arm capture. The engine
  checks for the file once per eligible frame, until it exists.

Each `[rsx texture descriptor]` record includes the frame, first observed unit,
binding source (`upload`, `picture`, `surface`, or `depth`), selected handle,
location/offset, complete format flags, dimensions, registered pitch, advertised
and calculated mip counts, compression class, filter/LOD control, remap, wrap,
surface dimensions/host format when applicable, picture identity, cached content
hash/generation, and every mip's `level:source_offset:source_row_pitch`.

For surface/depth bindings the mip array describes the hypothetical guest-memory
layout; the selected resource is the live GPU surface or depth snapshot. That
distinction is necessary before diagnosing a decoder error.

Capture original and corrected runs at the same menu. An uncompressed linear
upload with multiple sampled mips and a lower source pitch that shrinks from the
registered pitch exercises the separate linear-mip-pitch correction. Compressed
linear data currently ignores the pitch and needs separate evidence. A surface
binding with dimensions or format unrelated to the requested image suggests an
address alias; preserve the relevant surface creation and reuse history.

This first capture identifies affected resources. To distinguish a decoder
error from overwritten data or resource lifetime, a follow-up must save the
same source span before decoding, decoded mip bytes, and synchronized Metal
resource bytes, alongside draw order and handle retirement/reuse. It must also
save vertices/UVs for the incorrectly shaped player-card panel.
