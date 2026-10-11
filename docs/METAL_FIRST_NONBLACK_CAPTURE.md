# First nonblack Metal frame capture

Set both `PS3RECOMP_METAL_FRAME_DUMP=/absolute/path/frame.ppm` and
`PS3RECOMP_METAL_FRAME_DUMP_FIRST_NONBLACK=1` to retain the first nonblack
supported game surface submitted for presentation by the Metal draw engine.
The existing PPM path and RGB encoding are unchanged. The option is intended
for startup diagnostics where a transient image might fall between periodic
captures. It does not generate or substitute an image.

Until a capture succeeds, this mode reads every submitted RGBA8Unorm or
BGRA8Unorm presentation surface. Any nonzero RGB byte qualifies; opaque alpha
alone does not. A windowed presentation skipped because no drawable was
available does not qualify. Missing display helpers, pipeline creation failure,
or unavailable render encoders also leave capture armed. Windowed readbacks fence the command queue, as the
existing periodic diagnostic does. Headless presentation already waits for
its submitted work. This can slow startup significantly while frames remain
black; it is opt-in and intended for bounded diagnostic runs.

Black and unsupported surfaces create no file. After a complete successful
PPM write and close, the process keeps that first snapshot and performs no
further capture fence or texture readback in this mode. A failed save leaves
capture armed for a later presentation. State lasts for the process lifetime.
Host heartbeat presentations of opaque black do not qualify. A supported
nonblack surface presented by a heartbeat does qualify: this diagnostic
observes submitted surfaces, not inferred game loading state or draw counts.

Without the exact value `1`, capture retains the existing every-120th-call
cadence, including its behavior of capturing black surfaces and overwriting
the path on each periodic capture. The existing frame counter remains shared
with that mode; changing modes during a process is not a supported diagnostic
workflow.

## Verification

`python3 tools/test_metal_first_nonblack.py` builds an Objective-C fixture on
macOS and uses real Metal textures and `getBytes` readback. It checks early
black-to-colored capture, all-black frames through the old cadence boundary,
RGBA/BGRA conversion, the windowed queue-fence path, an actual Metal shader
display pass and its missing-resource gates, rejected unsupported or
unsubmitted surfaces, retry after file-open failure, retention through later
frames, and default/disabled mode captures at calls 120 and 240. A post-capture
sentinel rejects any further texture messaging. Each scenario runs in its own
process. Other operating systems skip this Metal-only fixture.

The fixture's windowed case exercises the real Metal queue fence without
creating a Cocoa window; it does not claim end-to-end compositor validation.
The initial regression failed on the pinned implementation because a colored
sixth frame produced no PPM. No MAG assets or fabricated screenshots are
included.

All 18 SDK test scripts passed with `CI=1` on the development Mac; the
expanded eight-mode Metal fixture was rerun after the final display-pass
success propagation and passed.
