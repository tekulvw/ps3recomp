# Selected PPU sleep and workload notes

This private, default-off diagnostic extends the selected PPU call-span capture.
Enable `AUDIO_CHRONOLOGY_PPU_WAIT_NOTES=1` with a valid
`AUDIO_CHRONOLOGY_PPU_TARGET` and the existing audio chronology trigger. No guest
address is built into the diagnostic. The selected span still covers the actual
indirect function and its complete trampoline drain.

The new `[ppu-call-note-summary]` and `[ppu-call-note]` prefixes share the existing
monotonic clock, run ID, and window (at most ten seconds). Existing audio and
call-span schemas are unchanged. Notes print after call-span records and before
old audio records, only when the existing audio poll/finish flushes the window.
Keep the entire prefix through the audio summary and records when freezing logs.

The separate static ring is limited to the configured `AUDIO_CHRONOLOGY_LIMIT`
(maximum 32768 records; 96 bytes each on the verified host, 3 MiB total).
Overflow saturates the dropped counter. `pending_recorded_sleeps` counts stored
begin records without stored ends; it is not the number of physical sleeping
threads. Cap loss, a boundary crossing, abnormal unwind, or missing callback end
makes the affected intervals incomplete.

Each row includes `run_id`, selected call `span` token, host context identity,
and guest thread ID copied from the admitted span. A scalar thread-local scope
supports nested selected calls; normal and C++ abnormal exit restore the outer
scope. Signals, aborts, and longjmp can leave an unmatched span and stale scalar
scope. Reject notes associated with spans lacking a matched normal end; do not
infer physical completion from a pending bracket. No context or guest argument
is dereferenced by a note hook.

* `kind=1 phase=0/1` brackets the already-existing platform sleep region in
  `sys_timer_usleep`. `token` pairs the begin/end. `arg0` is the existing requested
  microseconds. The end `rc` is the actual POSIX `nanosleep` return; `arg1` is
  its errno only when rc is -1. Windows reports INT32_MIN because its existing
  multi-API sleep path has no single equivalent return. A zero-usec request
  still brackets the existing no-sleep path. Guest return remains CELL_OK,
  including EINTR; there is no new retry, sleep, or altered scheduling policy.
  The interval includes note hook/metadata contention, and is not a pure kernel
  sleep duration or CPU-time measurement.
* `kind=2 phase=2` reports an already-executed `cellSpursGetWorkloadInfo` result.
  `arg0/arg1/arg2` are its existing SPURS address, workload ID, and output address.
  `rc` is its actual result. On success `arg3` packs the exact already-loaded
  contention, readyCount, and idleSpuRequest bytes at bits 0, 8, and 16. They
  retain their original load/store order and are not an atomic snapshot. On
  error arg3 is UINT64_MAX, with no output or guest reads added. These rows
  describe inclusive calls within the selected span, not a proven guest caller
  or one particular completion predicate.

No additional guest polls, guest-memory reads, API invocations, or PCM inspection
are introduced. Existing workload output writes, timer result, and host error
state are preserved. Outside active capture, the added hooks perform no clocks,
metadata locks, or output after initialization. The first discovery of expiry
can clock and lock once to close atomic admission; subsequent hooks are quiet.
The registry/chronology paths already present retain their existing behavior.

Compare complete normal spans' sleep intervals and actual workload-info values
by raw entry flags, including bit 0x2. Absence of workload-info rows can identify
an earlier boundary only when the span and note stream are complete and the
original caller/predicate chain is separately verified. It cannot distinguish
an unfinished marker minimum from an unstable terminal scan. No SPU-job ownership,
SPU completion, audio intent, or absence of CPU stores follows from this stream.

Run `python3 tools/test_ppu_wait_notes.py` and repeat with `--sanitize` for actual
source-derived POSIX sleep and whole workload-info fixtures. They cover quiet
admission, nesting, abnormal restoration, interrupted and zero sleeps, exact
workload errors and aliasing, concurrency, cap loss, and expiry. Source extraction
keeps the original platform sleep instructions and workload-info body under test;
synthetic VM and sleep replace device/game activity. `--source-root PATH` allows
an exact-parent RED check, and `--functional-only` compares unchanged functional
results and RAM digests. Native game capture remains a separate root-owned step.
