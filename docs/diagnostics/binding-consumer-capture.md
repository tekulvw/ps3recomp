# Selecting a diagnostic texture consumer

The existing bounded texture-binding capture can optionally select the consuming
fragment program separately for each requested texture offset. This changes only
capture selection; it does not change texture bindings, shader compilation,
render state, draw emission or guest memory.

Set `PS3RECOMP_RSX_BINDING_CAPTURE_CONSUMERS` to a comma-separated list aligned
with `PS3RECOMP_RSX_BINDING_CAPTURE_OFFSETS`. Each entry is either `*` (the legacy
first consumer) or `HASH:CONTROL`, with 1–16 hexadecimal digits for HASH and 1–8
for the exact raw shader control word. No prefixes, whitespace or signs are
accepted. HASH must be nonzero. The list must contain exactly one entry per
offset, at most eight. Malformed configuration disables capture with a diagnostic;
rendering proceeds normally. Missing or empty configuration retains legacy behavior.

HASH is the resident FP's `rsx_fp_structural_hash` with seed
`1469598103934665603`. It includes instruction structure and excludes inline
constant values, so constant updates and guest program relocation do not spend a
request. It is the existing production hash representation, not a cryptographic
identity or a promise of cross-endian portability. CONTROL matches every raw bit.
A matching program does not promise equal constants, geometry, targets or frames:
inspect the saved program, constants, descriptors and target metadata too.
Only an actually bound sampled unit at the requested offset/location can capture.
Nonmatches do not spend the attempt or image-byte budget and do not call the
backend capture callback. A matched request is attempted once even if capture
fails, preserving existing bounds. Filtered metadata records the matched hash and
control. Unfiltered metadata and files are unchanged.

Existing limits remain: eight distinct offsets, each source and decoded image at
most 8 MiB, source/decoded total at most 64 MiB, vertices/indices individually at
most 1 MiB, and Metal conversion at most 4 Mi pixels. The existing location,
start-frame, start-draw and trigger options still apply. Configuration is read once
per process. Captures from different requests may occur at different draws/frames.

# Optional original 1x1 float storage

`PS3RECOMP_RSX_BINDING_CAPTURE_RAW_1X1=1` additionally writes `.metal.rgba.raw`
and `.metal.rgba.raw.meta` for selected 1x1 floating-point textures. Supported
physical Metal formats are RGBA32Float, RGBA16Float, R32Float and R16Float, with
16, 8, 4 and 2 bytes respectively. Other dimensions produce no raw sidecar; other
1x1 formats record `unsupported_format` without a fabricated payload.

The normal pre-consumer queue drain happens first. A checked texture-to-buffer
blit reads level zero, slice zero into a 256-byte aligned shared buffer; only the
physical texel bytes are written. Sampling/view swizzles do not transform these
bytes. Metadata records the actual physical pixel format, actual swizzle,
`swizzle_applied=0`, host endianness, GPU status and save status. Apply that swizzle
when relating the physical bytes to shader-visible values. This does not claim
MSAA coverage or copy arbitrary texture dimensions/formats. Allocation, GPU or
file failure affects only the diagnostic sidecar. The existing RGBA8 capture and
its return status are unchanged. Missing, empty or any value other than exact `1`
adds no readback commands, payloads or metadata.

Synthetic tests use no original title programs or assets:

- `test_binding_capture_filter.c`: strict bounded parser, exact matching and real
  structural-hash controls for changed instructions versus changed constants.
- `test_binding_capture_engine.c`: wrong early program/control, matching late
  consumer, one attempt, invalid configuration and legacy/wildcard behavior through
  production dispatch/draw code. Run each mode in a separate process.
- `test_binding_capture_raw_metal.m`: all four float storage formats, out-of-range
  values, nonidentity swizzle, queued GPU producer/fence and disabled behavior.

The portable parser/matcher and CPU fixtures are generic. Wiring in this branch
uses the native engine's existing diagnostic capture facility; this patch does
not add that facility wholesale to a source branch that lacks it.
