# Native WWS job manager

A native implementation of the SPURS WWS job manager policy module, run through the native
policy-module seam (`docs/SPURS_NATIVE_PM.md`). It has two parts:

- **The pure core:** `runtime/spu/wwsjm/wwsjm_core.{h,c}`. This is the guest-visible contract as
  pure functions over byte buffers, with no `spu_context`, MFC, guest memory or globals.
- **The run loop:** `runtime/spu/wwsjm/wwsjm_run.inc`, plus the shadow comparison in
  `wwsjm_shadow.inc`. Both are included by `spurs_native_pm.inc`, so they compile inside
  `spurs_policy.c` with their own copy of the core (`WWSJM_API` is `static inline` there). A
  separately compiled `wwsjm_core.c` object therefore never clashes with it.

**Status:** opt-in, off by default. With `PS3RECOMP_SPURS_NATIVE_PM` unset, nothing here is
registered or runs.

| `PS3RECOMP_SPURS_NATIVE_PM` | Effect for the job manager's workloads |
|---|---|
| unset, `0` | The lifted policy module (default). |
| `1`, `wwsjm` | The native run loop. |
| `check` | The native run loop plus self-checks: every line transaction changes only what the contract allows, other lanes' markers are untouched, finish is called exactly once per job, the stack is restored, and each staged GET has the right shape. A violation stops the process (exit 86) with the workload, lane and line. |
| `shadow` | The lifted module. Its runs' events are compared with the core's decisions (see "Shadow comparison"). |

The implementation is registered for one policy image identity: fingerprint
`0xAD16923605435AD0` and the SHA-256 of the image the specification was written for. It takes
only workloads of a five-SPU instance, whose job-context constant and three services the
specification establishes. Other instances keep the lifted module (spec U11, U12).

**Method:** the core implements a behavioural specification written from the outside: the guest
memory the job manager reads and writes, the jobs it starts and the services it gives them. It is
not derived from the original program's code. It reuses none of the original's local-store
layout, register use or step order. Where the core chooses LS addresses (the job context, the
userdata copy), it receives them as arguments.

## The contract, in the core's terms

| Area | Functions | Behaviour |
|---|---|---|
| Job list line (128 bytes, big-endian) | `wwsjm_line_decode`, `wwsjm_cursor_of`, `wwsjm_header_ea` | Up to four header arrays. Each has a guest EA, its capacity − 1, and a marker block: submitted count `S`, next claim index `N`, and one marker per lane (`0xFFFF` = none). Only the current array (`+0x0A`) is serviced. The header of index `i` is at `EA + 8·(i mod capacity)` for the u16 `i`; capacities need not be powers of two. A current array with no EA or a capacity of 2³² is refused. `N` wrapping at `0x10000` was never observed; a check mode should report `N ≥ WWSJM_INDEX_WRAP_WATCH`. |
| Entry headers (8 bytes) | `wwsjm_header_decode`, `wwsjm_judge` | Type 0 means nothing published (stop). Type 1 is a job, whose EA is a record. Types 2 and 5 are gates: claimable when the u16 at their EA is 0, otherwise stop. Type 3 is a no-op. Type 4 switches to the next array. Any other type is refused. |
| Claim | `wwsjm_claim` | One line transaction: `N+1`. `S` is raised to `N+1` when it was behind (16-bit modular). For a type-1 entry, the lane's marker becomes `N` if the lane held no job. For type 4, the current array advances mod 4 and the switch counter, the u32 at `+0x0C`, is decremented (its low halfword borrows from the high one, as recorded in game). No other byte changes. |
| Retire | `wwsjm_retire` | After the job returns, the lane's marker on that job's array becomes the next job the lane still holds there (the one claimed during finish), or `0xFFFF`. |
| Ready count | `wwsjm_ready_value`, `wwsjm_ready_apply` | The workload's byte in the first 16 bytes of the SPURS instance line becomes `busy` while `S − N > 0` and the entry at `N` is claimable, otherwise 0. It becomes 8 on a flag entry. These are scheduling hints (see "What the replay showed"). |
| Dependency counters | `wwsjm_decrement`, `wwsjm_wake_target` | Each dependency command decrements a u16, with wrap (odd or out-of-line offsets are refused). When it reaches 0, the following u16 (`wid << 8 \| value`) names a workload whose ready count is set. |
| Command portion of a record | `wwsjm_plan_commands` | Decodes reserve, use, unreserve, dependency and run commands. No-op commands are skipped and counted (`nop_count`); a set 0 away from page 16 is allowed and noted (`set0_moved`). End and invalid commands, flag bits outside the contract, unreserved sets, overlapping sets and sets outside `[ls_low, ls_high)` are refused without a partial plan. The parameter data starts at the 16-byte boundary after the run command. |
| Buffer sets | `wwsjm_plan_place`, `wwsjm_plan_relocate`, `wwsjm_plan_held`, `wwsjm_buffer_span` | A set is `count` buffers of `pages` KiB, and its buffers form a ring. Placement starts each ring at its lowest physical buffer that overlaps no busy LS (the buffers an earlier job still holds). Logical buffer `b` then occupies physical `(b + start) mod count`. Set 0 (the position-dependent module) and any set holding a shareable copy never rotate. Loads and descriptors follow the placement. `wwsjm_plan_held` refuses rather than truncates. |
| Staging | the plan's loads, `wwsjm_gather_layout` | Copies split into GETs of at most 16 KiB. A gather list of (size, EA) pairs lands element by element under the MFC list rule: each element keeps its EA's offset within the quadword, and the next starts on the quadword after its last byte. Every byte must fit the buffer, an element is at most 16 KiB, the output has a capacity, and a stall-and-notify flag is refused. |
| Residency | `wwsjm_residency_*`, `wwsjm_plan_residency` | Within one run, a shareable copy whose LS, source EA and length match the last load there is not loaded again. Anything else the job reserves over it replaces it. The table resets every run. |
| Get buffer | `wwsjm_buffer_descriptor`, `wwsjm_job_tag` | Returns 16 bytes: the set, the logical buffer, a DMA tag, LS/4, length/4, and the use command's EA and byte length when the buffer was filled by a read. A job that asks for a tag gets the next one, counting down from 31, per job. |
| Job context | `wwsjm_context_build`, `wwsjm_finish_result` | 0x80 bytes: lane, kernel DMA tag, SPURS EA, `wid << 16 \| index`, dependency count, `list EA + per-instance constant \| array`, `wid << 16 \| 0xFFFF`, four copies of the userdata address, then the service slots. Finish returns the claimed job's header words, or `{0, 0xFFFFFFFF}`. |

## The run loop

One dispatch on a slot's resident context, on its worker thread:

- **Entry.** On a flag entry (poll status 4), the workload's ready count becomes 8. The job
  list line is read with GETLLAR.
- **Claiming.** Entries at N are claimed with one PUTLLC each. A lost reservation re-reads and
  retries. Non-job entries are claimed and passed, with a kernel poll after each; an answer
  naming another workload ends the run at the next entry boundary. The loop stops when the
  entry at N is not claimable.
- **Staging a job.** The record's command portion is read from guest memory and planned by the
  core. Each set's ring starts at its first buffer, because no job runs beside this one (spec
  §12.1). Module copies go through the context's MFC GETs (16 KiB at most), except where the
  per-run residency table says the module is already in place. A gather copies its (size, EA) list into the last `16·L` bytes of
  the buffer, where the job reads it. Each element is then one GET from the buffer's start,
  under MFC list rules (spec §17.1): zero-size elements transfer nothing and do not advance,
  and no element may reach the list copy. Header and gate reads go to guest memory directly;
  the manager uses no local store below `0x200`. The loop then waits for the staging tag.
- **Entering a job.** The command portion is copied to LS `0xC00`, so the parameter data the
  context points at lands at `0xC00` plus the commands' aligned length. The job API context is
  built at `0x1330`. Before each job the loop also plants the fixed low local store that jobs
  were observed to read (spec §13):
  - `0x000–0x00F`: four copies of an absolute branch (`bra`) to a window slot whose handler
    stops the process. The native run raises no SPU events, so that slot is never entered.
  - `0x010–0x07F`: zero.
  - The SPURS kernel context at `0x100–0x1E7`, every word the spec lists:
    - `0x100–0x1BF` zero;
    - `0x1C0` = 0 and `0x1C4` = instance EA;
    - `0x1C8` = lane and `0x1CC` = kernel DMA tag 8;
    - `0x1D0` = 0 and `0x1D4` = `0xA00`;
    - `0x1D8` = `0x1DC` = workload id;
    - `0x1E0`/`0x1E4` = the exit and select-workload addresses.

  Every job is entered with SPU interrupts enabled and the event mask zero (spec §5.1, §14).
  Jobs disable and re-enable interrupts around their service calls and may require them
  enabled. With no event enabled, none is ever taken. The job's own toggles stand, services
  never touch the bit, and the run restores the interrupt state it began with when it ends.

  These are job-facing contract addresses, not the lifted module's layout.
  `0x080–0x17F` belongs to the running job (its reservation buffers), and the manager keeps
  nothing there; static assertions keep the manager's own areas (`0x1000–0x117F`) clear of
  every job-facing range. The job is entered through the seam with r1 = {`0x3F990`, `0x1980`, 0, 0}.
  Word 1 is the stack bytes available, which the SPU ABI stack check in job code tests
  when it allocates a frame (spec §15). The job stack `[0x3E010, 0x3F990)` is reserved,
  and static assertions keep every manager area and the job window out of it. The other
  entry registers are r3 = the context (all four words), r4 = 0, r5 = the entry and
  r6 = `0x2C000000`. The lifted manager's leftovers in r2 and r7–r127 are not reproduced
  (spec §15).
- **Services, through the native-service window:**
  - **get buffer** returns the core's descriptor. A set's DMA tag is allocated at its first
    request with `r6 ≠ 0` (31 downward), and a repeat request for the set returns the same tag;
    `r6 = 0` gives 0 (spec §5.3, §17.3).
  - **release** returns all ones.
  - **finish** polls the kernel, claims the next job if one is claimable (the lane's marker
    stays on the running job), and publishes the ready count.
  - The other context slots are left empty, so a job that calls one stops the process. Window
    slot 7 is the interrupt-vector landing slot and stops the process too.
- **After the job.** The retire is one PUTLLC, then the dependency decrements (one PUTLLC
  each), then a ready-count write for each counter that reached zero, then the ready rule.
  The loop continues with the job finish claimed, or claims again.

Guest reads of headers, gate counters, records and gather lists go to guest memory directly.
When a policy-run observer is armed they are reported as GETs, so recordings and the shadow
comparison see them. Every refusal by the core (entry type, header, command, flags, ranges,
gather list) stops the process loudly; unsupported behaviour is never skipped. Counters print
as one `[wwsjm]` line at exit.

## Shadow comparison

With `PS3RECOMP_SPURS_NATIVE_PM=shadow`, the lifted module runs and owns every effect. The
policy-run recorder hands each event of its runs to `wwsjm_shadow.inc`, which keeps the bytes
each run observed. At every guest-visible effect it asks the core what the native manager would
have published, and compares:

- claims and retires (exact line bytes) and stops;
- dependency decrements, and the wakes they cause (exact);
- the run's own ready-count writes;
- plans, module loads and resident skips;
- gathered pieces;
- context words and the parameter data offset;
- get-buffer, release and finish results.

Allowed differences are counted by class (spec §12):

- `placement`: the recorded ring start is adopted (§12.1);
- `ready_preclaim`, `ready_gate_unread`, `ready_zero_after_job_claim`,
  `ready_after_uncounted_claim` and `ready_1_4`: ready-count hints (§12.2).

Anything else is a mismatch: counted, and the first 40 printed. Missing effects count too: a
dependency not decremented, a wake not published, or a job entered without a successful claim.
A `[wwsjm-shadow]` summary prints every 5 s and at exit.

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

`python3 tools/test_wwsjm_run.py [--mutants]` (POSIX) builds `tests/test_wwsjm_run.c` with the
real SPURS initializer, coordinator, five workers, policy runner, resolver, MFC engine and drain.

- **The list.** A manufactured list of 61 jobs, on two workloads and two header arrays. One
  array's capacity is not a power of two, and its indices cross the slot wrap. The list also
  has a no-op, a gate opened by the jobs' dependency decrements, a wake that alone starts the
  second workload, a report gate and an array switch.
- **Lost reservations.** A PPU-side writer keeps changing a word of the line the manager never
  reads, so store-conditionals are lost and retried.
- **Each job** checks what the specification says it is handed: registers, context words,
  parameter data, module bytes, gathered inputs, get-buffer descriptors and DMA tags.
- **A monitor thread** checks the PPU's completion predicate against the outputs.
- **The arms:** switch unset (inert), `1`, `check`, and `1` with
  `PS3RECOMP_SPURS_NATIVE_PM_SELF_SHADOW=1`. The last must leave every run identical: no
  mismatch, no unverifiable event, and every comparison class exercised.
- **`--mutants`:** nine run-loop breaks. They are a reservation loss ignored, a retire that
  drops the held job, decrements or the wake skipped, the parameter data pointer at the command
  portion, modules never reloaded, the tag counter kept across jobs, finish claiming nothing,
  and the context's array dropped. Each must fail the fixture, and must also show as mismatches
  in the shadow comparison.

`python3 tools/test_wwsjm_core.py [--mutants]` builds `runtime/spu/tests/test_wwsjm_core.c`
against the core. The test covers:
- the specification's vectors (job list claim, counted claim, retire, gates, array switch,
  no-op, index wrap, dependency decrement, flag entry, idle run, geometry-shaped command list,
  reordered command list, refusals, residency);
- generated property cases (the `S` rule, lane rule and byte preservation on 20,000 random
  lines);
- placement, tags and gather layout.

`--mutants` applies 32 mutations of the core and requires each to fail the fixture. They include
a retire without the lane, a lane marker of `N + 1`, a gate passing a non-zero counter, residency
that always or never reloads, gathers rounded down, a stale or unaligned userdata pointer, and the
`S`, switch, slot, malformed-capacity, ready, tag, descriptor, placement, pinned-set, no-op, decrement-offset, held-capacity and gather (quadword offset, element limit, output capacity) rules. CI runs the fixture on
Linux, macOS and Windows through the `tools/test_*.py` loop, and runs the mutants on Linux (clang
and gcc) and macOS.

`PS3RECOMP_WWSJM_STATS=<seconds>` (off when unset) prints the run loop's counters line
(`[wwsjm] runs=… gather_overlaps=…`) every `<seconds>`, for processes that are killed
rather than exited, which never reach the at-exit print.
