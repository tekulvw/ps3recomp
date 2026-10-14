# CPU-only producer geometry diagnostic

This opt-in diagnostic reuses the existing binding-capture writer, program
decoder, structural FP/control selector, and successful-draw preparation point.
It records producer inputs even when the fragment program samples no textures.
It adds no backend callback, GPU readback, submit, wait, raster override or
shader modification. It is an investigation tool, not a rendering fix.

Set these before the engine's first draw:

- `RSX_ENGINE_PRODUCER_CAPTURE_DIR`: an existing fresh private directory.
  An absent or empty value preserves the ordinary path.
- `RSX_ENGINE_PRODUCER_CAPTURE_TARGET`: `location:offset`, using a location
  digit of 0 or 1 and up to eight hexadecimal offset digits. It matches any
  resolved active color attachment, including B-only and nonfirst MRT members.
- `RSX_ENGINE_PRODUCER_CAPTURE_SELECTOR`: one required structural FP hash and
  raw shader control, in the existing `hash:control` hexadecimal syntax.
  Wildcards are rejected so an accidental unfiltered dump cannot occur.
- `RSX_ENGINE_PRODUCER_CAPTURE_LIMIT`: 1 through 32, default 32 attempts.
  Failed writes and unsupported geometry spend attempts; nonmatching shaders
  do not. The vertex and index arrays are each limited to 1 MiB. Successful
  capture reservations, including 8 KiB per-record metadata allowance, are
  limited to 64 MiB; failed attempts can additionally emit bounded metadata.
- `RSX_ENGINE_PRODUCER_CAPTURE_TRIGGER`: optional file path. While absent,
  nothing is captured. After it is first observed, capture waits for the next
  engine frame, then observes only that full frame and at most the count cap.
  Without a trigger, the explicitly selected diagnostic also waits for the
  next frame after configuration. It never follows later frames or rearms.

For a quiet-audio run, arm immediately after the complete quiet summary has
flushed and any debugger is detached. Keep GPU dump flags disabled. Use a
bounded external wait for files. If there is no new full frame, a shader does
not match, or writing fails, report capture unavailable; absence of files does
not prove geometry or asset absence. Environment is cached at first draw, so
enabling this on an existing process without launch flags is unsupported.

Each matching draw retains decoded vertex bytes in compact attribute order,
rebuilt host indices (an empty file for a nonindexed draw), raw VP and FP
programs, actual uploaded constant blocks, the entire register array, effective
render state, decoded surface declaration and viewport. The metadata records
frame/draw, the arm frame, original primitive and geometry source counts,
program identity, attribute order, raw target selector and every resolved
attachment's location, offset, pitch, physical dimensions and format. Raw and
resolved MRT counts remain distinct. Native binary structures and arrays use
the producing build's ABI and byte order; retain the matching source/build pin.

The capture occurs before backend draw binding/emission. It establishes the
CPU inputs prepared for that draw, not successful GPU encoding, visibility or
object identity. Original raw guest vertex bytes are not substituted for the
decoded bytes. Full registers retain their addresses/formats for later source
qualification. Texture images are deliberately excluded.

Tests drive the public engine with neutral zero-sampler programs, compare exact
serialized geometry/program/constant data, cover arrays, indices, B-only and
MRT selection, check trigger and single-frame/count bounds, and assert that no
backend readback or synchronization callback was added. The ordinary engine
suite is compared byte-for-byte with the parent while capture is disabled.
