# Bounded source / binding / GPU capture

This is diagnostic instrumentation, not a MAG rendering correction. It is based
on `67eb98df` (bounded descriptors) and does not include the separate linear-mip
pitch correction. The corrupted MAG descriptor run contains no linear textures
with more than one mip and no unnormalized textures.

Create an existing empty output directory, then configure:

```sh
PS3RECOMP_RSX_BINDING_CAPTURE_DIR=/absolute/path/to/capture
PS3RECOMP_RSX_BINDING_CAPTURE_OFFSETS=04316380,04391800,04CA8100,046FA880,04585880,02620000,00030000,0E60F980
PS3RECOMP_RSX_BINDING_CAPTURE_LOCATION=0
PS3RECOMP_RSX_BINDING_CAPTURE_TRIGGER=/absolute/path/to/initially-absent-marker
```

Offsets are hexadecimal and location is decimal (0 local, 1 main). At most
eight offsets are accepted; a longer, duplicate or malformed list disables capture.
Create the marker only after the affected menu appears. Optional
`PS3RECOMP_RSX_BINDING_CAPTURE_START_FRAME` and `_START_DRAW` defer eligibility
by presented-frame count and accepted draw count respectively. The trigger is
checked once per eligible presented frame. Each configured offset captures only
its first enabled, nonzero selected binding after arming. Failed captures also
consume that offset, preventing retries and repeated writes.

Each prefix identifies frame, draw, unit and guest offset. Its files are:

- `.source.bin`: current guest source span copied into immutable memory.
- `.decoded-mip0.bin`: a **fresh decode of that copy**, before the component
  remap. BC1/2/3 remain compressed blocks because that is the production decoder's
  output. This is not falsely labelled as the cached upload's original input.
- `.metal.rgba` and `.metal.rgba.meta`: the actual selected Metal texture/view,
  point sampled at mip 0, including hardware BC decoding and the real remap.
  Rows are RGBA8 in texture coordinates; metadata gives actual host dimensions
  and pixel format, which may differ from the requested descriptor.
- `.vertices.bin`, optional `.indices.bin`: actual compacted float4 attributes
  and rebuilt little-endian u32 triangle indices submitted by the selected draw.
  The metadata lists the attribute register indices in stride order.
- `.vp-constants.bin`, `.fp-constants.bin`: actual upload constant blocks,
  including the viewport epilogue and buffered fragment constants.
- `.vp-program.bin`, `.fp-program.bin`: current guest program words when present.
  The pipeline key and shader control are recorded separately. Cached compiled
  host shader binaries are not captured.
- `.registers.bin`, `.render-state.bin`: current register file and decoded host
  render-state struct, interpreted with the same SDK headers/host ABI.
- `.meta`: descriptor, binding source, handle, current/cached/live hashes,
  geometry, target identities, viewport and raw scissor.

`cache_matches_current=1` means the immutable copy has the same content hash as
the selected upload cache entry. `source_stable=1` means a second live hash after
copy/decode still matches it. These are mutation evidence, not proof that another
thread cannot write after the second hash. Surface/depth bindings have no upload
cache entry; their guest bytes may be stale because the authoritative image is
on the GPU. In particular, do not call a surface's hypothetical guest decode its
actual GPU data.

The Metal callback submits and waits for preceding recorded work, then copies
the selected resource's sampled values before the consuming draw. This is a
deliberate diagnostic stall, bounded to eight selected bindings. It preserves
the resource and its handle and does not refresh the cache. A timing-sensitive
failure can change under this fence, so compare the resulting visible screenshot
with the known baseline corruption. A clean sampled texture with malformed
vertices/transforms points to geometry or shader handling; malformed source
payload and matching GPU image point upstream; a stable guest copy that disagrees
with the cached hash proves a stale binding; good source and wrong GPU sampling
with a matching hash directs investigation to upload/remap/resource selection.

Bounds are 8 MiB each for source and fresh decoded bytes, 64 MiB total across all
source/decoded captures, 1 MiB each for vertices and indices per capture, and
4 million pixels (16 MiB) per GPU sample. Temporary CPU pixel/source allocations
are released after each binding. There is no per-frame file stream. Cubemaps are
not sampled by the Metal callback; source capture still includes their guest span
when it fits the limit. RGBA8 sampling clamps HDR values, so it is suitable for
the menu's BC/ordinary color resources but not precise HDR analysis.

## Actual descriptor leads

In `retail-visible-managed-playerretail-np-renderdiag-20261012`, frame 6660 binds
local BC1 `04316380` (1280x720, pitch 2560) and BC3 `04391800` (1280x720, pitch
5120); frame 6670 binds BC3 `04CA8100` (1280x136). Their pitches equal packed
base-level block row sizes. Frame 6650 binds texture `B0` (DEPTH24_D8 | LN) at
`02620000`, 2048x1024/pitch8192, through an RGBA8 color surface at the same
address. This is a concrete format-class mismatch requiring investigation; it
does not by itself establish that the menu stripes come from this binding.
Frame 6420 samples RGBA8 `00030000`, 1280x720/pitch10240, from a 1280x720 host
surface. The doubled guest pitch can reflect sample storage or alias shape;
plain width*4 arithmetic is insufficient to conclude which.

RSX surface reuse needs compatibility and typeless-format handling. The primary
comparison [RPCS3 texture cache](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Common/texture_cache.h)
declares `render_target_format_is_compatible` around line 651 and handles
cross-aspect/typeless transfers around lines 2493–2500. This supports checking
format compatibility before calling the current offset-only selection correct;
it does not authorize translating their implementation into this SDK.

## Verification

The CPU production fixture failed before instrumentation (no capture), then
passed disabled/delayed/triggered capture, padded-row decode, compressed byte
preservation, single-offset deduplication and a deliberate stale-cache mismatch.
The real Metal fixture failed before the callback existed, then passed preceding
queued-clear synchronization, exact nonuniform pixel coordinates, hardware BC1
decode and B8 alpha remapping. Related descriptor, immediate-vertex and checked
engine fixtures passed. No live MAG control or integrated renderer change was
performed by this diagnostic branch.
