# Native WWS job manager: the pure core

`runtime/spu/wwsjm/wwsjm_core.{h,c}` is the guest-visible contract of the SPURS WWS job manager
policy module, written as pure functions over byte buffers. It has no `spu_context`, no MFC, no
guest memory and no globals. A later run loop reads guest memory, asks the core what to publish,
and performs the atomics and transfers through the runtime's MFC path.

**Status:** the core and its synthetic tests only. Nothing in the runtime links or calls it yet,
and there is no switch: build outputs and behaviour are unchanged. The run loop, the
native-service window and the opt-in switch (`PS3RECOMP_SPURS_NATIVE_PM`) are later changes.

**Method:** the core implements a behavioural specification written from the outside: the guest
memory the job manager reads and writes, the jobs it starts and the services it gives them. It is
not derived from the original program's code. It reuses none of the original's local-store
layout, register use or step order. Where the core chooses LS addresses (the job context, the
userdata copy), it receives them as arguments.

## The contract, in the core's terms

| Area | Functions | Behaviour |
|---|---|---|
| Job list line (128 bytes, big-endian) | `wwsjm_line_decode`, `wwsjm_cursor_of`, `wwsjm_header_ea` | Up to four header arrays. Each has a guest EA, its capacity − 1, and a marker block: submitted count `S`, next claim index `N`, and one marker per lane (`0xFFFF` = none). Only the current array (`+0x0A`) is serviced. The header of index `i` is at `EA + 8·(i mod capacity)`; capacities need not be powers of two. |
| Entry headers (8 bytes) | `wwsjm_header_decode`, `wwsjm_judge` | Type 0 means nothing published (stop). Type 1 is a job, whose EA is a record. Types 2 and 5 are gates: claimable when the u16 at their EA is 0, otherwise stop. Type 3 is a no-op. Type 4 switches to the next array. Any other type is refused. |
| Claim | `wwsjm_claim` | One line transaction: `N+1`. `S` is raised to `N+1` when it was behind (16-bit modular). For a type-1 entry, the lane's marker becomes `N` if the lane held no job. For type 4, the current array advances mod 4 and the switch counter at `+0x0E` is decremented. No other byte changes. |
| Retire | `wwsjm_retire` | After the job returns, the lane's marker on that job's array becomes the next job the lane still holds there (the one claimed during finish), or `0xFFFF`. |
| Ready count | `wwsjm_ready_value`, `wwsjm_ready_apply` | The workload's byte in the first 16 bytes of the SPURS instance line becomes `busy` while `S − N > 0` and the entry at `N` is claimable, otherwise 0. It becomes 8 on a flag entry. These are scheduling hints (see "What the replay showed"). |
| Dependency counters | `wwsjm_decrement`, `wwsjm_wake_target` | Each dependency command decrements a u16, with wrap. When it reaches 0, the following u16 (`wid << 8 \| value`) names a workload whose ready count is set. |
| Command portion of a record | `wwsjm_plan_commands` | Decodes reserve, use, unreserve, dependency and run commands. No-op, end and invalid commands, flag bits outside the contract, unreserved sets, overlapping sets and sets outside `[ls_low, ls_high)` are refused without a partial plan. The parameter data starts at the 16-byte boundary after the run command. |
| Buffer sets | `wwsjm_plan_place`, `wwsjm_plan_relocate`, `wwsjm_plan_held`, `wwsjm_buffer_span` | A set is `count` buffers of `pages` KiB, and its buffers form a ring. Placement starts each ring at its lowest physical buffer that overlaps no busy LS (the buffers an earlier job still holds). Logical buffer `b` then occupies physical `(b + start) mod count`. Loads and descriptors follow the placement. |
| Staging | the plan's loads, `wwsjm_gather_layout` | Copies split into GETs of at most 16 KiB. A gather list of (size, EA) pairs lands element by element under the MFC list rule: each element starts on the next quadword, keeps its EA's offset within the quadword, and its length rounds up to 16. A stall-and-notify flag is refused. |
| Residency | `wwsjm_residency_*`, `wwsjm_plan_residency` | Within one run, a shareable copy whose LS, source EA and length match the last load there is not loaded again. Anything else the job reserves over it replaces it. The table resets every run. |
| Get buffer | `wwsjm_buffer_descriptor`, `wwsjm_job_tag` | Returns 16 bytes: the set, the logical buffer, a DMA tag, LS/4, length/4, and the use command's EA and byte length when the buffer was filled by a read. A job that asks for a tag gets the next one, counting down from 31, per job. |
| Job context | `wwsjm_context_build`, `wwsjm_finish_result` | 0x80 bytes: lane, kernel DMA tag, SPURS EA, `wid << 16 \| index`, dependency count, `list EA + per-instance constant \| array`, `wid << 16 \| 0xFFFF`, four copies of the userdata address, then the service slots. Finish returns the claimed job's header words, or `{0, 0xFFFFFFFF}`. |

## What the replay showed

A private harness (game-derived inputs, kept outside the repositories) replayed recorded policy
runs through this core and compared every claim, retire, decrement, ready-count write, staged
transfer, get-buffer result, finish result and context. It established these contract details:
- the header slot rule for non-power-of-two capacities;
- the get-buffer tag counter and the descriptor's buffer byte;
- the source fields on copy-filled buffers;
- several use commands per set, and zero-page sets;
- ring placement beside held buffers.

Two kinds of difference remain, and both are allowed:

- **Ready-count values at some decision points.** The original sometimes publishes the value it
  decided before a claim, or before reading a gate's counter, and occasionally 1–4. Ready counts
  are scheduling hints that the kernel consumes. The core implements the specification's rule.
- **Ring placement after unrecorded history.** With LS kept across dispatches, the original's
  placement also depends on buffers left allocated by jobs before the recording started. Jobs
  reach their buffers only through get-buffer and their staged inputs, so the start is the
  manager's choice. `wwsjm_plan_relocate` lets a caller choose it.

## Tests

`python3 tools/test_wwsjm_core.py [--mutants]` builds `runtime/spu/tests/test_wwsjm_core.c`
against the core. The test covers:
- the specification's vectors (job list claim, counted claim, retire, gates, array switch,
  no-op, index wrap, dependency decrement, flag entry, idle run, geometry-shaped command list,
  reordered command list, refusals, residency);
- generated property cases (the `S` rule, lane rule and byte preservation on 20,000 random
  lines);
- placement, tags and gather layout.

`--mutants` applies 22 mutations of the core and requires each to fail the fixture. They include
a retire without the lane, a lane marker of `N + 1`, a gate passing a non-zero counter, residency
that always or never reloads, gathers rounded down, a stale or unaligned userdata pointer, and the
`S`, switch, slot, ready, tag, descriptor, placement and gather rules. CI runs the fixture on
Linux, macOS and Windows through the `tools/test_*.py` loop, and runs the mutants on Linux (clang
and gcc) and macOS.
