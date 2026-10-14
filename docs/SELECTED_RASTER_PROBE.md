# Selected raster diagnostic (private experiment)

This opt-in experiment tests whether culling suppresses already submitted
geometry. It is not a renderer correctness fix and carries no title profile.
The normal path is unchanged when `RSX_ENGINE_RASTER_PROBE` is absent or empty.

Set the following before launch:

- `RSX_ENGINE_RASTER_PROBE=observe` records state without changing it, or
  `cull-off` changes only `cull_enable` to zero.
- `RSX_ENGINE_RASTER_PROBE_TARGET=0:00040000` selects one synthetic example
  target. The syntax is one location digit (`0` or `1`), colon, eight hex
  digits. Supply the actual private target in the launch profile.
- Optional `RSX_ENGINE_RASTER_PROBE_TRIGGER=/absolute/private/file` keeps
  ordinary culling until this file exists. It is checked before each eligible
  draw's pipeline lookup, rather than at a later texture consumer. Once armed,
  the probe remains armed. An absent/empty trigger enables an explicitly
  selected probe immediately.
- `RSX_ENGINE_RASTER_PROBE_FRAMES=16` sets the active frame window; default 16,
  supported range 1 through 64. Every eligible draw within the window is
  affected; there is no per-draw budget that can expire halfway through a pass.

Eligibility requires exactly one color attachment at the selected location and
offset, enabled depth test and depth writes, and a nonzero color mask. Matching
uses the resolved active attachment, including B-only target selection; stale
A fields cannot match a B-only draw. Other
targets, MRT draws, depth-read-only draws, depth-disabled draws and zero color
masks retain ordinary behavior. Vertex/index source and primitive are not
selectors. Array and indexed triangles share the same final production path.

The state change happens before ordinary pipeline hashing and creation. The
original cull face, front-face winding, depth, stencil, blend, masks, viewport,
scissor, shaders, textures and geometry are retained. A changed pipeline has a
different ordinary render-state key. Once the frame window expires, the engine
uses the original state and can reuse its cached pipeline.

There is one bounded log line at the first selected draw of each active frame,
plus one expiry line. Each line records the first activation frame, active
window, target, cumulative selected/changed counts and original raster state.
No GPU readback, additional submit/wait, breakpoint or target call is added.

For quiet audio followed by a rendering experiment, use two independent file
triggers. Arm this probe only after the quiet audio result is flushed and all
debuggers are detached. Arm the normal/depth binding capture after observing
the probe's second active frame. Require the captured frame to be strictly
greater than `first_frame` and less than `first_frame + frames`, and verify a
positive `changed` count. This avoids interpreting a partial first prepass or
an expired probe as a negative result. A negative result still cannot separate
missing submission, shader discard, clipping, depth/stencil rejection or draws
outside the selected scope. Do not infer an implementation fix merely from
improved pixels when culling is disabled.

`test_raster_probe_engine.c` exercises the real public engine path, trigger
activation, both geometry sources, target/state misses, frame-window switchback
and unchanged remaining render-state bytes. `test_raster_probe_metal.m` uses
neutral synthetic triangles through the actual engine and Metal backend:
cull-off exposes a back-facing red triangle, while a later green triangle
still fails the unchanged depth test and the stencil comparison remains active.
