# Bounded Metal presentation diagnostics

Set `PS3RECOMP_METAL_PRESENT_DIAGNOSTICS=1` to emit at most eight presentation evidence records. Default rendering, submission, capture and logging are unchanged. This diagnostic adds no GPU fence or readback.

The first and 120th attempts before any queued draw may produce two records. Six additional records are reserved for the first presentation after a draw, then every 120 attempts. A draw remains remembered across report-triggered submission, so early black heartbeat frames cannot consume the post-draw budget.

Each `[rsx present diagnostic]` record identifies the selected backend surface handle, currently queued draw count, number naming that handle, and number encoded in this submission. Counts are not fragment/pixel counts. Already flushed draws are absent from the current queue counts.

Presentation reasons distinguish `no_surface`, `no_drawable`, `blit_skipped`, and `gpu_error`. When existing frame capture performs its normal synchronized readback, the independent `readback` field additionally reports `black` or `nonblack`, the count of pixels with any nonzero RGB channel, and maximum RGB byte. Alpha does not qualify as color. Unsupported readback formats/sizes, allocation failure, periodic skips and an already saved first-nonblack capture have explicit reasons. With capture disabled, `readback=not_requested` and `rgb_available=0` are expected; diagnostics never enable capture themselves.

`gpu_status` is Metal's command-buffer status observed at logging, or -1 when no command buffer was submitted. Status4 means completed; status5 means error, with the Metal error code in `gpu_error`. Existing capture fences make status available before readback. Without a preexisting wait the status may still be pending; the diagnostic does not wait just to improve its evidence. A successful encoded blit is not itself proof of onscreen display. Pixel statistics refer to the selected source surface, not the compositor drawable.

## Verification

`python3 tools/test_metal_present_diagnostics.py` exercises the actual backend with real Metal textures: opt-out silence, RGB-black versus nonblack, missing source, skipped display blit, no drawable, eight-record budget, post-draw sampling after report-style flushes, and a periodic black readback that must not hide a missing drawable. The first-nonblack regression remains separately runnable with `python3 tools/test_metal_first_nonblack.py`. Both explicitly skip outside macOS. The fixture does not inject a GPU execution fault, so the error-status reporting branch is not claimed as hardware-fault tested.
