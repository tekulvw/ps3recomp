# Diagnostic elapsed time on POSIX

`ps3_ms_now()` provides elapsed milliseconds for SPU DMA range rates, task
signal rates, task execution timing, and RSX timing diagnostics. It previously
returned zero on non-Windows hosts, preventing timed diagnostic windows from
reporting and making elapsed intervals appear zero on macOS and Linux.

The helper now uses `GetTickCount64()` on every platform. The existing POSIX
compatibility implementation uses `CLOCK_MONOTONIC` and returns milliseconds;
Windows keeps its original system implementation. This change does not alter
the guest system-time import or its microsecond units.

Run `python3 tools/test_diagnostic_milliseconds.py` to check the production
helper against independent monotonic bounds and verify that elapsed time
advances. The fixture compiles the helper from `cellGcmSys.c` with the real
platform compatibility header. It does not need a GPU or a running title.

For audio investigation, `SPU_DMA_RANGE=58000000-58100008` can report transfer
sites and five-second byte rates for an observed ring in MAG retail v1.0.
That address is title-specific. A nonzero PUT rate establishes transfers, not
nonzero PCM samples or audible output. This timer correction alone does not
establish or repair an audio producer failure.
