# Guest publication of FIFO reports

`NV4097_GET_REPORT` previously updated only the host report cache. A guest
polling a mapped 16-byte report could therefore wait forever after its FIFO
request had been consumed. The FIFO now tracks `SET_CONTEXT_DMA_REPORT` (byte
method `0x1A8`) and publishes reports for these contexts:

| Context | Address resolution |
| --- | --- |
| `0xBAD68000` report main | IO map at `0x0E000000 + (argument & 0xFFFFFF)` |
| `0xFEED0001` host buffer | IO map at `argument & 0xFFFFFF` |
| `0x66626660` report local | Existing host report cache only |

Local guest report memory remains unimplemented. The current HLE allocation
contains labels, the control register and IO tables; inventing a local report
window beside the labels would overlap those structures. Unknown contexts,
misaligned records, unmapped or invalid EA pages, and records beyond the guest
VM are rejected with bounded diagnostics. Guest DMA offsets are not limited by
the smaller legacy host-cache capacity. `ppu_vm_size == 0` means 4 GiB.

For a valid report, both enabled draw backends are flushed before publication.
`rsx_draw_engine_flush()` uses the backend's synchronous `submit_and_wait`
contract. Metal's implementation invokes `eng_encode_and_commit(nil)` and
waits for command-buffer completion, including in windowed mode. The D3D12
live path's flush likewise waits for queued work. This boundary matters because
guest completion may release buffers referenced by earlier draws.

The record contains a big-endian 64-bit monotonic timestamp, 32-bit value and
32-bit zero completion/padding word. The whole record is written while holding
the SPU lock-line lock. A release fence precedes the final completion-word
store; each written word breaks PPU reservations when the existing plain-store
reservation tracking is enabled, and the complete record is
followed by an SPU coherence notification. This uses the existing notification
contract and does not call guest code or synthesize SPURS scheduling events.

The value semantics are deliberately unchanged: ZPASS type 1 reports a
conservative `0xFFFF`; other types report zero. These are **not measured GPU
occlusion results**. This fix supplies address resolution, ordering and guest
publication, not accurate query emulation. Existing host getters and host-only
local reports keep their existing cache behavior.

## Evidence and verification

Public RPCS3 sources were consulted for interface facts, not copied:

- [gcm_enums.h](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/gcm_enums.h): context handles and method number.
- [common.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/NV47/HW/common.cpp): the report uses the selected DMA context.
- [RSXThread.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/RSXThread.cpp): dedicated report-main IO window versus raw host-buffer IO.
- [GCM.h](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/GCM.h): 16-byte guest report layout.
- [nv4097.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/NV47/HW/nv4097.cpp): GET_REPORT and completion padding.

Run `python3 tools/test_gcm_report_publication.py` on macOS or Linux. It compiles
the real GCM FIFO walker with recording backend and reservation hooks, requiring
no GPU. The baseline failed because the first report's guest completion remained
`0x00010000`. The regression checks report-main and host-buffer contexts,
context persistence and an unrelated context register, both backend flushes
before stores, every record word and completion-last order, per-word PPU
reservation breaks, SPU notification, neighboring memory, local host-only
behavior, rejected mappings, offsets beyond the host cache, guest page zero,
and the last valid record in a sparse 4-GiB guest VM. Actual Metal execution and
SPU scheduling integration are separate runtime validation steps.
