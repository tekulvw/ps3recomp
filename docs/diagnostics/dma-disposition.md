# Optional DMA copy-disposition receipts

This diagnostic observes the existing ordinary DMA core and list elements. It
changes no transfer, queue, reservation notification, list park/resume, tag or
return behavior. Reservation atomic commands are outside this observer.

Enable a hexadecimal half-open EA window with `SPU_DMA_DISPOSITION=lo-hi`.
Optional controls are `SPU_DMA_DISPOSITION_WORDS` (1–16, default 16) and
`SPU_DMA_DISPOSITION_LIMIT` (1–32768, default 128). Invalid configuration disables
this observer. With no window it reads no transfer payload or context, polls no
clock/path, and emits nothing. Lazy diagnostic initialization preserves errno.

`SPU_DMA_DISPOSITION_TRIGGER` optionally names an existing regular file, with a
maximum path length of 128 bytes. Until the file exists, no capture quota is
consumed. Watched candidates poll at most once per 250 ms globally. The observer
never creates/deletes the file and does not read its contents. Arming remains
latched; remove a prior trigger before launching a new diagnostic process.

Each admitted operation has one globally bounded atomic reservation sequence and
one final `[dma-disposition]` line: actual host SPU context, image/SPU/PC, r0/r1,
requested and actual addresses/size/command, unchanged return code, disposition,
and bounded before-source/after-destination BE32 samples. Source/destination
reads occur only at the already validated real memcpy sites. Post-copy sampling
happens immediately after memcpy, before later notifications or callbacks. No
logging, clock or filesystem operation occurs under the coherent PUT lock.

`copied-get` and `copied-put` identify execution of a real memcpy. Malformed
ordinary commands can retain the existing zero return without copying; those
records explicitly say `malformed-no-copy`. Rejections and compatibility LS zeroing
are distinct and sample no payload. A completed
tag alone is not a copy certificate; the existing failed ordinary-submit tag
behavior is deliberately preserved.

Sequence numbers order admission reservations, not frames or completion. A
sample is bounded and instantaneous, not a promise of unchanged later memory,
resource ownership, or audible output. Concurrent unreserved destinations can
change around the sample. In lifted code, the context PC may describe a coarse
runtime boundary. Out-of-window, wrapped/high-EA or exhausted operations are
unobserved; absence of a record does not imply absence of a transfer.

Synthetic checks: `python3 tools/test_spu_dma_disposition.py`, optionally with
`DMA_DISPOSITION_UBSAN=1`, and `python3 tools/check_spu_dma_disposition_preservation.py`.
