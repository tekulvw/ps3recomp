# Joining a legacy taskset releases its workload

## What a title sees

A SPURS instance has 16 workload slots. A legacy taskset (created by
`cellSpursCreateTaskset` or `cellSpursCreateTasksetWithAttribute`) takes one of
them for its whole life. A title ends that life with `cellSpursShutdownTaskset`
followed by `cellSpursJoinTaskset`. After a successful Join the taskset is gone:
no task of it can run again, nothing refers to its memory, and the title may
free that memory or create a new taskset in it. A title that loads levels
creates and joins a taskset for every level, so its slot use stays flat however
many levels it loads.

Before this change the HLE Join only waited for the tasks to exit and for the
task workers to let go. It never gave the slot back. Each Shutdown and Join cycle
left one slot taken by a taskset that no longer existed. MAG keeps 12 workloads
for the whole session (its inflate taskset and 11 job workloads), which leaves 4
for per-match tasksets. On the fifth match joined in one session there was no
slot left. `CreateTaskset2` and `CreateTasksetWithAttribute` failed with
`CELL_SPURS_CORE_ERROR_NOMEM`. The title does not check that result. It then
asked for its level-loading tasks in the taskset it had failed to create, those
calls failed too, no task ever started, and the primary PPU thread polled forever
for a job that never ran. The game froze on the loading screen with audio still
playing. A Windows player hit this; the leak itself does not depend on the host
or on which SPURS policy path is active, because tasksets never go through the
policy coordinator.

## Behaviour

1. `cellSpursJoinTaskset` keeps its current wait: it returns only once every task
   of the taskset has exited and no worker holds a reference, and it returns
   TASK_BUSY (`0x8041090A`) when that wait runs out. When it returns BUSY,
   nothing is released.
2. When the wait succeeds and the taskset's workload id names a live slot that a
   legacy taskset creation registered for this same taskset address and SPURS
   instance, Join releases that slot. It takes the same path as workload removal
   does: the slot's enabled bit, state byte and workload-info data are cleared in
   guest memory, any workload-flag receiver ownership is dropped, the scheduler
   forgets the slot, and the host record is cleared. It then sets the taskset's
   workload id field to 32, an id no slot has, so that the dead taskset can no
   longer be mistaken for whatever takes the slot next.
3. Join does not release:
   - a generation-2 taskset (`cellSpursCreateTaskset2`). Those are released by
     `cellSpursDestroyTaskset2`, which must still find its registration.
   - a slot that is not registered to this taskset, for example after another
     workload took a slot that a previous Join released. Join then only waits, as
     it does today, and says so.
   - a slot that a SPURS worker is in at that moment. Join leaves it registered,
     reports it, and still returns success, because the tasks have finished.
4. Joining a taskset a second time finds workload id 32. It waits (there is
   nothing left to wait for) and returns success without touching any slot.
5. `cellSpursDestroyTaskset`, the runtime's own legacy removal entry, returns
   success for a taskset whose slot Join already released (workload id 32). It
   must never remove a slot that now belongs to a different workload.
6. Running out of workload slots stays an error, but it is now loud: taskset
   creation and `cellSpursAddWorkload` print what holds each of the 16 slots
   when they return NOMEM. A task creation that fails prints its taskset, its
   result and, when the taskset has no live workload, that fact too. These lines
   are rate-limited.

## Tests

`tests/test_spurs_taskset_join_release.c`, run by
`tools/test_spurs_taskset_join_release.py`, drives the HLE entry points against
synthetic guest bytes. No game file is needed.

- It fills 12 slots with ordinary workloads, as MAG does. Then it creates,
  shuts down and joins a legacy taskset 40 times, through both creation entry
  points and at a mix of reused and fresh addresses. Every creation must succeed
  and every Join must give back the slot. Before this change the fifth creation
  failed.
- After a Join, a new workload that takes the released slot must survive a
  Destroy and a second Join of the dead taskset.
- A Join that times out on a held worker reference must keep the slot.
- A generation-2 taskset that is joined must still be released by
  `cellSpursDestroyTaskset2`.
- With every slot taken, creation must still return NOMEM.
