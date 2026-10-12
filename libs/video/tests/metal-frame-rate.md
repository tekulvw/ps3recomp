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

Run `python3 tools/test_metal_frame_rate.py` on macOS with a display. It checks
deterministic rates/stalls/duplicate timestamps/bounded p95, a native title
timer without a rendering thread, actual drawable callbacks versus GPU-only
completion, reset, and disabled mode.
