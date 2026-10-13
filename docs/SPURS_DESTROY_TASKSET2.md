# Bounded Taskset2 destruction

`cellSpursDestroyTaskset2` is NID `0x1EBCF459` and takes one guest
`CellSpursTaskset2*`. The MAG retail caller at `0x00124C64` sets r3 from its
owner's taskset pointer and calls import stub `0x01528514`; LR is `0x00124C68`.
The other GPR values in the unresolved-call report are not API arguments.
The observed taskset `0x47010400` was created with size `0x2900`, with no
CreateTask or WakeUp admission for that taskset before destruction.

The supported HLE operation is removal of an HLE-created, never-submitted,
empty generation-2 taskset on an ordinary SPURS instance. It removes the real
workload registration, clears the owning instance's workload enabled/state and
info data, releases workload-flag receiver ownership through the existing
RemoveWorkload path, prepares owned host/info metadata before publishing the
workload slot free, and invalidates the taskset workload id to 32. It does not
free the game's allocation.

A host lifetime directory is authoritative. CreateTask records admission before
reading its optional arguments or writing task bytes/output. The async worker
keeps a counted reference from before host thread creation through its final
local-store release and job cleanup. WakeUp records the actual workload's
owner, even if its policy image has no lifted implementation, and dispatches
with that explicit owner rather than the last task's global selection. Admission
and the six empty task bitset checks use the same lock. A closed generation
rejects subsequent CreateTask calls without guest mutation. A valid creation
resets its generation only after no previously tracked references remain;
legacy removal cannot permit reuse while an admitted Taskset2 worker is active.
Fresh Taskset1 addresses preserve their existing admission behavior.

The directory retains at most 256 distinct taskset-address tombstones. A valid
recreation at an existing address reuses its entry. Capacity exhaustion, or an
attempt to recreate an address with tracked references, returns TASK_BUSY before
clearing the guest allocation. This intentionally makes the bounded resource
visible rather than silently discarding lifetime history.

## Supported errors

These are task-facility values, not the older task macros currently present in
cellSpurs.h:

| Condition | Return |
| --- | --- |
| NULL destroy pointer | `0x80410911` (NULL_POINTER) |
| Pointer not aligned to 128 bytes | `0x80410910` (ALIGN) |
| Wide EA or allocation beyond the guest range | `0x8041090D` (FAULT) |
| Invalid workload id, including double destruction | `0x80410902` (INVAL) |
| Wrong generation size or invalid/unregistered SPURS owner | `0x8041090F` (STAT) |
| No matching HLE workload registration | `0x80410905` (SRCH) |
| Foreign nonzero event semaphore ids | `0x80410903` (NOSYS) |
| Any task bits, task/policy admission, active reference or policy-start marker | `0x8041090A` (BUSY) |
| CreateTask on a closed generation | `0x8041090F` (STAT) |
| CreateTask with a wide EA | `0x8041090D` (FAULT) |

An ever-submitted set continues to return BUSY after its workers finish and its
enabled bits become empty. Full cooperative Taskset2 shutdown/join, persistent
worker cancellation, firmware event semaphore teardown and the SPURS2
32-workload namespace remain unsupported. The legacy DestroyTaskset,
ShutdownTaskset and JoinTaskset APIs have not been promoted to complete shutdown
implementations. In particular, legacy DestroyTaskset must not be used as a
Taskset2 destruction alias.

## ABI and lifecycle evidence

The NID was independently computed with the repository's salted name-hash tool.
The local libsre export table identifies the DestroyTaskset2 descriptor
`0x3003177C`, whose first big-endian word identifies code `0x30014FF4` (load base
`0x30000000`). The locally lifted function validates the task pointer, alignment,
workload id and generation size, uses errors in facility `0x804109xx`, claims a
destruction bit, calls ShutdownTaskset (`0x30014974`), destroys both event
semaphores, waits for workload shutdown (`0x3000A304`), removes the workload
(`0x3000A50C`), and invalidates its id to 32. The source firmware image SHA-256
is `115f9df9596dd0b358e5a8c4eaaf77ee44c1f3ec16f9056b3abc517b739140c9`.
Firmware and generated title/firmware files remain outside this commit.

[RPCS3's primary cellSpurs source](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSpurs.cpp)
registers the same API name, but its DestroyTaskset2 and JoinTaskset bodies are
unimplemented. Its return-success stub is not evidence of safe destruction.
The HLE currently creates no LV2 event semaphores for Taskset2, so this change
supports the known empty HLE-owned lifecycle and explicitly rejects foreign
semaphore objects and submitted workloads.

## Verification

The new asset-free fixture invokes production CreateTaskset2, DestroyTaskset2,
CreateTask, WakeUp, the taskset builder and real async dispatch/runtime. It
checks invalid inputs and canaries, all six pending/active bitsets, workload
cleanup, double destruction, closed and wide submission pointers, valid address
reuse, 64 concurrent submission/destruction races, a blocked real worker,
completed-worker history, legacy removal preventing active address reuse,
Taskset1 admission and WakeUp ownership with deliberately stale globals. The
runner independently scans the NID and checks production generated registration.
The fixture supplied a synthetic lifted callback and a tiny synthetic SPU ELF;
it contains no firmware or game assets.

Before implementation, the runner failed because DestroyTaskset2 was absent.
A separate investigation fixture also demonstrated that the old DestroyTaskset
returned success and removed its workload while a real async worker was still
blocked. That unsafe behavior was not reused by the new API.

The focused checks are `tools/test_spurs_destroy_taskset2.py`,
`tools/test_spurs_taskset2.py`, `tools/test_spurs_taskset_registration.py`,
`tools/test_spurs_task_signal.py`, `tools/test_spurs_workload_flag.py` and
`tools/test_spurs_flag_receiver.py`. The isolated runtime Release build and
CTest sync_stress/fiber_switch are also run. This is SDK fixture validation;
confirmation that retail training advances belongs to the next native replay.
