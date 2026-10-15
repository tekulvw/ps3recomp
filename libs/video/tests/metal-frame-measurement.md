# Correlated guest-frame wall measurements

This is opt-in diagnostic metadata. It does not change guest audio, shader,
geometry, FIFO, report/reference completion, command encoding, or presentation
pacing. No report-flush optimization is included.

A coherent `CellGcmFlipReceipt` identifies an accepted guest flip boundary and
its selected buffer. MAG tags the existing host presentation call with that ID.
The buffer ID is metadata; the wrapper does not change surface selection. Boot
redraws and engine-internal sink presentations have ID zero and cannot establish
verified guest FPS. The receipt lock coherently publishes/takes ID, buffer,
legacy flip count and pending state; it does not queue additional presentations.
Several pending boundaries retain the existing coalescing behavior. Their IDs
are never recreated merely to match a host presentation count.

Enable `PS3RECOMP_METAL_FPS=1`, and set
`PS3RECOMP_METAL_FPS_WINDOW_CONTROL` to an owned control-file path at launch.
Guest audio processing remains enabled. For the requested muted host condition,
retain `SDL_AUDIODRIVER=dummy` in both paired native runs.

Write a line containing a strictly increasing positive uint64 request ID and a
duration in seconds, e.g. `1 60`. The existing main-queue FPS timer consumes it
and emits `[RSX FPS window]` JSON `started`, `complete`, or `rejected` events.
A busy request is rejected; it never resets the current window. Malformed or
out-of-range input is ignored. Supported durations are >0 and <=300 seconds.
The fixed ledger holds 16384 submissions, including callback grace arrivals;
capacity exhaustion explicitly invalidates qualification. Use shorter windows
at high rates (60 seconds supports the 120 FPS stretch target with headroom).

The optional collector has no app/process control:

```sh
python3 tools/metal_fps_window.py --control /owned/run/fps-control.txt \
  --log /owned/run/native/engine-console.log --request-id 1 --seconds 60 \
  --output /owned/run/fps-window-1.json
```

The SDK records `t0` and `t1=t0+duration` using `CACurrentMediaTime`. Every rate
uses `t1-t0`, including leading/trailing stalls and delayed main-queue timers.
Callbacks may arrive for two seconds after t1. Accepted presentation timestamps
must lie in `[t0,t1)`, and commits must also lie in `[t0,t1)`. A guest ID already
admitted when the window starts cannot enter its verified numerator. This is a
conservative lower bound: leading in-flight frames, and frames committed before
t1 but actually displayed at or after t1, are excluded.

Each actual drawable submission has a session-local submission ID. Admission,
commit, GPU completion, and drawable presentation remain distinct events. The
commit marker follows the actual Metal `commit` call, so callbacks can arrive
first. Immutable tickets retain their original session and request generation.
Late callbacks cannot enter a later window in the same or a replacement session.
Report flushes, headless work and GPU-only offscreen submissions have no frame
drawable and remain excluded from frame FPS.

`unique_guest_displayed` deduplicates guest IDs across submissions only after
successful GPU completion plus a finite positive presentation time. Presentation
arrival order is diagnostic only; records are sorted by timestamp when the window
closes. Equal positive timestamps across different submissions count as one
distinct display event. `duplicate_presented_time` counts extra submissions in
these groups. If a group names different guest IDs, `ambiguous_presented_time`
counts that group and it contributes no verified guest ID. This preserves a
conservative lower bound even when delivery is reordered.
`repeated_guest_displayed` counts additional distinct display timestamps for
the same guest ID after timestamp deduplication. `positive_presentations` counts
eligible positive per-submission records before that deduplication. `displayed`
counts distinct positive presentation timestamps inside the interval; it can
include uncorrelated or failed guest frames, so it is not verified guest FPS.

`qualified=false` if no presentation is accepted, any zero/invalid
presentation timestamp is seen, a GPU/backend failure occurs, a committed frame
has unresolved completion, an event lacks its commit marker, metadata IDs
conflict, ledger capacity is exceeded, a submission/blit fails, or a frame is
uncorrelated. Arrival reordering and cross-submission timestamp ties
remain explicit diagnostics without invalidating this conservative lower bound.
A repeated callback for one submission never creates another event; if it claims
a different positive timestamp it is an identity error and invalidates qualification.
Nil drawables and missing presentation callbacks are reported as losses; they never create a display
event. `meets_30_displayed_guest_fps` requires both qualification and >=30 unique
correlated successfully completed guest frames per requested wall second. A
locked surface returning zero timestamps always fails acceptance, even with a
positive GPU completion rate. This telemetry does not independently prove correct
scene content or replace bounded visual validation outside benchmark windows.

Validation: `python3 tools/test_frame_measurement.py` exercises portable receipt
coherence/concurrent publication, frame correlation, actual engine commit hooks
through protocol stand-ins, callback races, errors, exact interval boundaries,
zero timestamps, reordered delivery, nonconsecutive equal-timestamp ties,
conflicting timestamp identities, and session/request lifecycle without windows
or GPU work.
`python3 tools/test_metal_fps_window.py` checks exact-wall collector validation.
Existing no-GPU diagnostics regressions are `python3
tools/test_metal_frame_rate_signals.py`. Full native display verification remains
the native-run owner's responsibility.
