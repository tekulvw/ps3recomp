# Watching a DMA producer's source

Set `SPU_DMA_SOURCE_F32BE=1` together with `SPU_DMA_RANGE=<lo>-<hi>`
(hexadecimal guest addresses, upper endpoint exclusive) to interpret the source
of accepted PUTs touching that range as big-endian IEEE binary32 values. This
is an explicit numeric interpretation; a DMA payload can contain code or other
data, and nonzero statistics alone do not prove that it contains audible PCM.
The flag is off by default and requires exactly `1` and a valid watch range.

The observer runs immediately before a validated PUT's copy. It also sees PUTL
list elements through the same transfer path. It reports the issuing context's
image ID and local-store PC, command, masked local-store address, destination,
transfer size, source byte offset, inspected word count, finite/nonzero/nonfinite
counts and maximum finite absolute value. It emits no sample values or dumps.
The image ID identifies the runtime context; dynamically loaded overlays may
contain a different ELF's executable code.

Only complete four-byte words inside the watched intersection are inspected.
Negative zero counts as zero. NaNs and infinities count as nonfinite and do not
contribute to the peak or finite-nonzero count. At most 4,096 words (one valid
16 KiB transfer) are read. Existing transfer size, local-store and effective
address bounds are checked before calling the observer; it repeats source read
bounds defensively. Neither source nor destination bytes are changed.

An atomic window admits one observation per five seconds per emitting
translation unit, including an immediate first observation. Concurrent issuing
threads share that window. Reports describe one accepted transfer per window,
not every producer write; a zero observation cannot prove that every transfer
in that window was zero. Transfers outside the watch, GETs, invalid transfers
and partial words do not consume the window. The observer's clock/scan/output
work is skipped when disabled or outside the watch.

Use the source observation alongside the destination's own bounded observations:
nonzero source with zero destination narrows investigation to copying/ownership;
zero source narrows it to producer state or processing. A paused local-store
snapshot may contain a different overlay by the time it is captured, so do not
assume that an earlier PUT's local-store address still holds the same payload.

Run the synthetic direct/list DMA fixture with:

```sh
python3 tools/test_spu_dma_source_f32be.py
```

It checks explicit opt-in, invalid/missing ranges, negative zero, finite peaks,
NaN/infinity accounting, partial watch boundaries, five-second rate limiting,
a concurrent eight-thread window claim, masked local-store addresses, maximum-size last-word coverage and rejected
local-store/EA overruns. Accepted DMA byte copies and source preservation are
asserted through the real direct/list transfer paths.
