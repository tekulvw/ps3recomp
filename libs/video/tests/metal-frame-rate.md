Set `PS3RECOMP_METAL_FPS=1` for a windowed macOS run. The original title is
preserved and gains a once-per-second display FPS, guest submission FPS,
average/p95/maximum display interval in milliseconds, and seconds since the
last displayed frame. The same fields are emitted as `[RSX FPS]` log lines.

Display FPS counts `MTLDrawable` presentation callbacks with a positive,
increasing `presentedTime`. Skipped drawables, GPU flushes, and headless
submissions do not count. Host boot redraws can count as display frames; they
are kept separate from guest submissions. Hosts should call
`rsx_metal_backend_note_guest_frame()` once at each guest FLIP submission.
Without that hook, guest FPS stays zero.

FPS is presentations/submissions divided by actual wall-clock sample duration.
Intervals retain continuity across sample boundaries, so a resumed frame
exposes the preceding stall. A sample with no new frames reports zero FPS and
zero interval statistics, while age keeps increasing. The p95 uses nearest
rank over at most the most recent 256 intervals in the sample; average and
maximum include every interval. The first frame has no preceding interval.

The main queue timer updates independently of the frame submission thread.
If the main queue itself blocks, the title cannot update; log timestamps make
that gap visible. No image readback, GPU wait, rendering change, or per-frame
statistics allocation is introduced. Drawable callbacks retain their own
session so shutdown/reinitialization cannot contaminate a new run.

Add `PS3RECOMP_METAL_FPS_DIAGNOSTICS=1` to an FPS-enabled windowed run to
append a separately labeled **GPU completed FPS / age** to the title and emit
one `[RSX FPS signals]` line per sample. With this option absent, title/log
format is unchanged and no extra command-buffer completion handler is added.
The existing display FPS, display intervals and age continue to use only
accepted compositor `presentedTime` values. GPU completion never supplies a
replacement display timestamp or display FPS.

The signal line contains per-sample admitted host FLIP submissions
(`guest_admitted`), successful frame command-buffer completions
(`gpu_completed` / `gpu_fps`), failed frame command buffers (`gpu_failed`),
and the last successful GPU completion's monotonic host timestamp and age.
These count at most one completion per frame command buffer, independently of
the number of draws or render passes. Report flushes, texture captures,
offscreen submissions and headless work are excluded. In the engine path,
the display blit must have encoded successfully before completion is tracked.
Successful completion establishes GPU work completion, not correct rendering,
visible scanout, or hardware performance.

`callbacks` counts every received drawable presentation callback. `positive`
counts all finite positive timestamps, including the separately rejected
`duplicate` and `out_of_order` timestamps; `zero` is separate from `invalid`
(negative or nonfinite). `display_last_s` is the last accepted compositor
timestamp. `drawable_attempts`, `nil_drawables` and `skipped_blits` distinguish
drawable acquisition failure and skipped engine display blits. All event
counters reset per sample, while the last timestamps retain continuity.
Callbacks retain their original session across shutdown/reinitialization.

For example, positive GPU completion rate with zero display rate and no
callbacks points to absent presentation callback delivery. A nonzero `zero`
count instead proves callbacks arrived without a positive presentation time.
Nil drawables or failed command buffers identify earlier failures. If both
GPU and admitted guest frame rates stop, check the guest/render submission
path. These signals diagnose telemetry and submission boundaries; they do
not establish the cause of compositor behavior by themselves.

Run `python3 tools/test_metal_frame_rate_signals.py` for portable math checks
and macOS protocol fixtures. It opens no window and submits no GPU work. It
checks success/error completions, callback classifications, nil drawables,
skipped blits, exclusion of a successful non-frame flush, session lifetime,
disabled mode, and the existing deterministic frame-rate scenarios.

Run `python3 tools/test_metal_frame_rate.py` on macOS with a display. It checks
deterministic rates/stalls/duplicate timestamps/bounded p95, a native title
timer without a rendering thread, actual drawable callbacks versus GPU-only
completion, reset, and disabled mode.
