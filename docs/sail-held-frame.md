# Explicit held THX picture diagnostic

With the optional FFmpeg build enabled, PS3RECOMP_SAIL_HELD_FRAME=1 selects
one held picture, not timed video. Only an opened local thx.pam with the
retained selected MPEG-2 PES is accepted. Decode actual ordinal30 and require
1280x720, pitch5120, progressive BT709/limited source, 30000/1001, valid
90kHz PTS. Other sources fail through real stream close before an error event;
no unsupported intro readiness or fake EOS is reported.

Pin graphics configuration before borrowed OPD callbacks. Original title
memory allocation supplies aligned 128-byte scratch; original makeup and
allocFrame supply one guest frame buffer. Validate bounds and the entire
existing contiguous GCM mapping using public configuration/offset tables,
without implicit AddressToOffset auto-mapping. This GCM translation does not
assert that the title's separate VRAM texture table/draw works: native evidence
must establish that. Copy immutable actual RGBA, invoke formatChanged with the
same session as GetFrame, and report RUNNING only after successful publication.
Start preserves command EA and full64-bit user token. The held picture never
reports natural completion; audio, clocks, seek and buffer recycling are absent.

Stop/Close hide publication before release. The active configuration pin and
live pointer array survive freeFrame, cleanup and allocator free. A callback
that never dispatched can retry. A negative completed free, nonlocal unwind,
or invalid allocation address quarantines ownership instead of guessing or
calling free again. Reentrant descriptor mutations and adapter/player teardown
are rejected while setup/release owns its pin. FreeFrame uses the original
deferred VRAM-reference release path. UpdateAvSync cannot authorize overwrite.

Optional PS3RECOMP_SAIL_HELD_FRAME_DUMP writes the copied guest RGBA bytes for
local checksum evidence. Logs explicitly identify the diagnostic, source PTS,
actual dimensions, original allocation, contiguous GCM translation and FNV1a.
Neither the dumped source pixels nor these logs prove final native presentation.

Tests use the real generated NID/PPU bridge and a deterministic decoder boundary
fixture; the decoder itself has separate actual FFmpeg/reference tests. Scenarios
cover callback order/session, copied bytes, full tokens, teardown reentry,
never-dispatched free retry, direct Close, decode/allocation/unsupported failures,
unmapped tail pages, invalid buffer quarantine and interrupted free without
redispatch. Dependency fixtures include real filesystem/PAMF symbols and the
correct allocator argument. Native acceptance remains a separate required test.
