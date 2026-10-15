# SPU interpreter decode cache

`spu_step` still fetches every instruction from current local-store bytes. A
host-thread-local cache reuses only the decoded operands, keyed by the full PC
and current 32-bit instruction word. DMA overlays, STQD self-modification and
context switches therefore cannot reuse a decode for different live code.
The decoder itself, arithmetic helpers, lifted lookup/rejoin, observer, channel
operations, stop handling and scheduling are unchanged.

This is a focused adaptation of the interpreter cache in PR213 commit
[8a10b5448c1633405d28930d46587e9d59e2dcc8](https://github.com/sp00nznet/ps3recomp/commit/8a10b5448c1633405d28930d46587e9d59e2dcc8).
No PPU memory fast paths, histograms, DMA diagnostics, float rewrite or lookup
cache from that aggregate commit is included. Unlike the original retained
calloc pointer, a fixed TLS array has the host thread's automatic lifetime.
There are 65,536 entries: 2 MiB per interpreter host thread on the tested ABI.
There is no per-context allocation or heap-allocation fallback. TLS allocation
and reclamation follow the host runtime; its allocation failure is not converted
into an interpreter fallback. Threads that access other TLS in the same module
may reserve the module's TLS block even when this experiment is disabled.

The cache is enabled by default. Start a fresh process with
`SPU_INTERP_DECODE_CACHE=0` to use the original decoder, or `=1` to enable it.
Only the exact string `0` disables it. The setting is read once per host thread;
changing the environment during execution is unsupported. Both modes retain the
same instruction-byte reads. This switch does not disable the existing ARM SHUFB
or accepted FNMS behavior and does not suppress any runtime checks.

Run `python3 tools/test_spu_decode_cache.py`; set `UBSAN=1` for the sanitized
variant. The fixture compiles the actual interpreter with a test-only decode
entry counter inserted into a temporary source copy, never into production.
It checks repeated decode reuse; modified/restored overlays; actual STQD code
replacement; same-address context reuse; PC-relative branch targets; last LS
word and zero-opcode validity; simultaneous host threads; all decoded fields for
8,192 deterministic words, repeated at varying PCs; and identical GPR/LS/PC/status
digests with caching disabled, enabled and default. Unsupported words are decoded
without executing them. Negative LQD/STQD displacements are excluded from the
raw-word UBSan oracle because the inherited decoder shifts a negative signed
integer there; this optimization does not change or repair that existing UB.
Parent code fails the repeated-decode assertion. Removing word/PC/valid checks
or disabling the cache is rejected by the retained negative controls.

This is not a speedup guarantee or complete SPU conformance proof. Compare fresh
native processes at the same scene with the switch on/off, no GPU readback or
PC observer, and matched sound/FPS/diagnostic settings. Measure actual frame
submission/completion and per-thread CPU; test audio chronology separately.
Do not combine a lifted-lookup cache or integer/float rewrite into that A/B.
